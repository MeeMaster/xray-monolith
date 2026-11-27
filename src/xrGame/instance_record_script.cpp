////////////////////////////////////////////////////////////////////////////
//	Module 		: instance_record_script.cpp
//	Description : Script access to an item's instance record (WEAPON_ENGINE_PLAN.md 14, change 2h)
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "instance_record_script.h"
#include "../xrServerEntities/InstanceRecord.h"
#include "../xrServerEntities/xrServer_Objects_ALife_Items.h"
#include "script_game_object.h"
#include "inventory_item.h"
#include "GameObject.h"
#include "Level.h"
#include "ai_space.h"
#include "script_engine.h"
#include "stat_mods.h"
#include "weapon_parts.h"
#include "weapon_part_tree.h"
#include "Weapon.h"
#include "player_hud.h"
#include "script_light_inline.h"
#include "script_attachment_manager.h"
#include "../Include/xrRender/Kinematics.h"

namespace
{

	CInventoryItem* game_item(CScriptGameObject* self)
	{
		CInventoryItem* item = self ? smart_cast<CInventoryItem*>(&self->object()) : nullptr;
		if (!item)
			Msg("! [instance record] %s is not an inventory item", self ? self->object().cName().c_str() : "nil");
		return item;
	}

	// The game object of a server item, while it has one; edits must go through it (4.3)
	CInventoryItem* online_item(CSE_ALifeInventoryItem* se)
	{
		if (!g_pGameLevel || !se)
			return nullptr;
		CObject* O = Level().Objects.net_Find(se->base()->ID);
		return O ? smart_cast<CInventoryItem*>(O) : nullptr;
	}

	// Override value from a script value: strings, numbers and booleans; nil means clear
	enum EValue { eInvalid, eClear, eSet };
	EValue script_value(const ::luabind::object& v, xr_string& out)
	{
		if (!v.is_valid())
			return eClear;
		switch (v.type())
		{
		case LUA_TNIL: return eClear;
		case LUA_TSTRING: out = ::luabind::object_cast<LPCSTR>(v); return eSet;
		case LUA_TBOOLEAN: out = ::luabind::object_cast<bool>(v) ? "true" : "false"; return eSet;
		case LUA_TNUMBER:
			{
				const double d = ::luabind::object_cast<double>(v);
				string64 buf;
				if (d == floor(d) && _abs(d) < 1e15)
					xr_sprintf(buf, "%.0f", d);
				else
					xr_sprintf(buf, "%.9g", d);
				out = buf;
				return eSet;
			}
		default:
			Msg("! [instance record] an override value must be a string, number, boolean or nil");
			return eInvalid;
		}
	}

	// Applies an edit to a copy of the game-side record and sends it on success
	// defer: a state value edit (3c); the item sends it later unless the config changed too
	template <typename F>
	bool edit_game(CInventoryItem* item, F&& edit, bool defer = false)
	{
		CSE_InstanceRecord record = item->instance_record();
		if (!edit(record))
			return false;
		return item->set_instance_record(record, defer);
	}

	// Server-side edit: through the game object while there is one, else on the server copy
	template <typename F>
	bool edit_server(CSE_ALifeInventoryItem* se, F&& edit, bool defer = false)
	{
		if (!se)
			return false;
		if (CInventoryItem* item = online_item(se))
			return edit_game(item, edit, defer);
		if (se->m_instance.is_opaque())
		{
			Msg("! [instance record] [%u] has a record from a newer build; it is read-only", se->base()->ID);
			return false;
		}
		return edit(se->m_instance);
	}

	// Reads: the game-side copy is the current one while the item is online
	const CSE_InstanceRecord& server_view(CSE_ALifeInventoryItem* se)
	{
		if (CInventoryItem* item = online_item(se))
			return item->instance_record();
		return se->m_instance;
	}

	// nullptr reaches Lua as nil (lua_pushstring). Never return an empty luabind::object:
	// copying one on the way out dereferences a missing reference.
	LPCSTR get_override(const CSE_InstanceRecord& r, LPCSTR key)
	{
		return key ? r.get_override(key) : nullptr;
	}

	// filter: nullptr = both kinds, "config" or "state" (3c)
	::luabind::object get_overrides(lua_State* L, const CSE_InstanceRecord& r, LPCSTR filter = nullptr)
	{
		const bool want_state = filter && !xr_strcmp(filter, "state");
		const bool want_config = filter && !xr_strcmp(filter, "config");
		if (filter && !want_state && !want_config)
			Msg("! [instance record] get_overrides filter must be \"config\" or \"state\", not \"%s\"", filter);
		::luabind::object t = ::luabind::newtable(L);
		for (const SInstanceProp& p : r.overrides)
			if ((!want_state && !want_config) || want_state == p.is_state_value())
				t[p.key.c_str()] = p.value.c_str();
		return t;
	}

	bool set_override(CSE_InstanceRecord& r, LPCSTR key, const ::luabind::object& value, bool state = false)
	{
		if (!key || !*key)
			return false;
		xr_string s;
		switch (script_value(value, s))
		{
		case eClear: return r.clear_override(key);
		case eSet: return r.set_override(key, s.c_str(), state);
		default: return false;
		}
	}

	// ---- 3d: stacked modifiers --------------------------------------------

	// "flat" or "pct"; anything else is refused with a log line
	bool modifier_mode(LPCSTR mode, u8& out)
	{
		if (!mode || !xr_strcmp(mode, "flat"))
			out = SInstanceModifier::FLAT;
		else if (!xr_strcmp(mode, "pct"))
			out = SInstanceModifier::PCT;
		else
		{
			Msg("! [stat mods] modifier mode must be \"flat\" or \"pct\", not \"%s\"", mode);
			return false;
		}
		return true;
	}

	LPCSTR mode_name(u8 mode)
	{
		return mode == SInstanceModifier::PCT ? "pct" : "flat";
	}

	::luabind::object contribution(lua_State* L, const stat_mods::SContribution& c)
	{
		::luabind::object m = ::luabind::newtable(L);
		m["source"] = c.source.c_str();
		m["key"] = c.key.c_str();
		m["mode"] = mode_name(c.mode);
		m["value"] = c.value;
		return m;
	}

	// Every contribution, upgrades and parts included: {{source, key, mode, value}, ...}
	::luabind::object get_modifiers(lua_State* L, const CSE_InstanceRecord& r, const xr_vector<shared_str>& upgrades, LPCSTR key)
	{
		stat_mods::Contributions all;
		stat_mods::collect(r, &upgrades, all);
		::luabind::object t = ::luabind::newtable(L);
		int i = 1;
		for (const stat_mods::SContribution& c : all)
			if (!key || !xr_strcmp(c.key, key))
				t[i++] = contribution(L, c);
		return t;
	}

	// {base, flat, pct, pct_used, clamped, final, modified, error, sources = {...}}
	::luabind::object explain_stat(lua_State* L, LPCSTR section, const CSE_InstanceRecord& r, const xr_vector<shared_str>& upgrades,
	                               LPCSTR key)
	{
		::luabind::object t = ::luabind::newtable(L);
		if (!key || !*key)
			return t;
		stat_mods::SExplain e;
		stat_mods::explain(section, r, &upgrades, key, e);
		t["base"] = e.base.c_str();
		t["base_is_default"] = e.base_is_default;
		t["flat"] = e.flat;
		t["pct"] = e.pct;
		t["pct_used"] = e.pct_used;
		t["clamped"] = e.clamped;
		t["final"] = e.final_value.c_str();
		t["modified"] = e.modified;
		if (!e.error.empty())
			t["error"] = e.error.c_str();
		::luabind::object sources = ::luabind::newtable(L);
		int i = 1;
		for (const stat_mods::SContribution& c : e.sources)
			sources[i++] = contribution(L, c);
		t["sources"] = sources;
		return t;
	}

	// ---- 4a: part props ----------------------------------------------------

	bool set_part_prop(CSE_InstanceRecord& r, u16 id, LPCSTR key, const ::luabind::object& value)
	{
		if (!key || !*key)
			return false;
		xr_string s;
		switch (script_value(value, s))
		{
		case eClear: return r.set_part_prop(id, key, nullptr);
		case eSet:
			{
				xr_string normal, error;
				if (!weapon_parts::normalise_prop(key, s.c_str(), normal, error))
				{
					Msg("! [weapon parts] part %u: %s = %s refused: %s", u32(id), key, s.c_str(), error.c_str());
					return false;
				}
				return r.set_part_prop(id, key, normal.c_str());
			}
		default: return false;
		}
	}

	::luabind::object bind_bone(lua_State* L, const weapon_parts::SBindBone& b)
	{
		::luabind::object t = ::luabind::newtable(L);
		t["name"] = b.name.c_str();
		t["parent"] = b.parent.size() ? b.parent.c_str() : "";
		t["pos"] = b.pos;
		t["hpb"] = b.hpb;
		return t;
	}

	// model_bones(model_or_section): bone name -> {name, parent, pos, hpb}, or nil
	int lua_model_bones(lua_State* L)
	{
		LPCSTR model = lua_tostring(L, 1);
		const weapon_parts::BindBones* bones = weapon_parts::model_bones(model);
		if (!bones)
		{
			lua_pushnil(L);
			return 1;
		}
		::luabind::object t = ::luabind::newtable(L);
		for (const weapon_parts::SBindBone& b : *bones)
			t[b.name.c_str()] = bind_bone(L, b);
		t.pushvalue();
		return 1;
	}

	// bone_bind_transform(model_or_section, bone): {name, parent, pos, hpb}, or nil
	int lua_bone_bind_transform(lua_State* L)
	{
		LPCSTR model = lua_tostring(L, 1);
		LPCSTR bone = lua_tostring(L, 2);
		const weapon_parts::BindBones* bones = model && bone ? weapon_parts::model_bones(model) : nullptr;
		const weapon_parts::SBindBone* b = bones ? weapon_parts::find_bone(*bones, bone) : nullptr;
		if (!b)
		{
			lua_pushnil(L);
			return 1;
		}
		bind_bone(L, *b).pushvalue();
		return 1;
	}

	// weapon_bone_pose(id, bone, hud): the bone's current (animated) transform in model space, as model_bones
	// gives the bind pose: {pos, hpb}. hud: the first-person model (the world model when the gun shares it).
	// nil when the object, model or bone is missing. For checking placement against the animated pose.
	int lua_weapon_bone_pose(lua_State* L)
	{
		const u16 id = u16(lua_tointeger(L, 1));
		LPCSTR bone = lua_tostring(L, 2);
		const bool hud = !!lua_toboolean(L, 3);
		CObject* obj = bone ? Level().Objects.net_Find(id) : nullptr;
		CHudItem* item = obj ? smart_cast<CHudItem*>(obj) : nullptr;
		IKinematics* K = nullptr;
		if (hud)
			K = item && item->HudItemData() ? item->HudItemData()->m_model : nullptr;
		else
			K = obj && obj->Visual() ? obj->Visual()->dcast_PKinematics() : nullptr;
		const u16 b = K ? K->LL_BoneID(bone) : BI_NONE;
		if (b == BI_NONE)
		{
			lua_pushnil(L);
			return 1;
		}
		const Fmatrix& m = K->LL_GetTransform(b);
		Fvector hpb;
		m.getHPB(hpb);
		hpb.mul(180.f / PI);
		::luabind::object t = ::luabind::newtable(L);
		t["pos"] = m.c;
		t["hpb"] = hpb;
		t.pushvalue();
		return 1;
	}

	// The upgrades a server item has: the game object's list while it is online
	xr_vector<shared_str> server_upgrades(CSE_ALifeInventoryItem* se);

	// "scope", "handguard/bottom"; a part sharing its parent and mount with another
	// (several parts on one rail) is named "<mount>#<id>" (4.1)
	xr_string part_path(const CSE_InstanceRecord& r, const SInstancePart& part, u32 depth = 0)
	{
		bool shared = false;
		for (const SInstancePart& o : r.parts)
			if (o.id != part.id && o.parent == part.parent && o.mount == part.mount)
				shared = true;

		xr_string name = part.mount.size() ? part.mount.c_str() : "?";
		if (shared)
		{
			string16 id;
			xr_sprintf(id, "#%u", u32(part.id));
			name += id;
		}

		const SInstancePart* parent = part.parent ? r.find_part(part.parent) : nullptr;
		if (!parent || depth > CSE_InstanceRecord::MAX_PARTS)
			return name;
		return part_path(r, *parent, depth + 1) + "/" + name;
	}

	// path -> {path, id, parent, mount, pos, section, condition, paintjob, data_size, upgrades = {...}, props = {...}}
	::luabind::object get_parts(lua_State* L, const CSE_InstanceRecord& r)
	{
		::luabind::object t = ::luabind::newtable(L);
		for (const SInstancePart& part : r.parts)
		{
			::luabind::object p = ::luabind::newtable(L);
			const xr_string path = part_path(r, part);
			p["path"] = path.c_str();
			p["id"] = part.id;
			p["parent"] = part.parent;
			p["mount"] = part.mount.size() ? part.mount.c_str() : "";
			p["pos"] = part.pos;
			p["section"] = part.section.size() ? part.section.c_str() : "";
			p["condition"] = part.condition_f();
			p["paintjob"] = part.paintjob.size() ? part.paintjob.c_str() : "";
			p["data_size"] = u32(part.blob.size());

			::luabind::object upgrades = ::luabind::newtable(L);
			int i = 1;
			for (const shared_str& u : part.upgrades)
				upgrades[i++] = u.c_str();
			p["upgrades"] = upgrades;

			::luabind::object props = ::luabind::newtable(L);
			for (const SInstanceProp& pr : part.props)
				props[pr.key.c_str()] = pr.value.size() ? pr.value.c_str() : "";
			p["props"] = props;

			// 4a: the placement the engine draws from, parsed; absent until the resolver sets a bone
			weapon_parts::SPlacement pl;
			if (weapon_parts::read_placement(part, pl))
			{
				::luabind::object placement = ::luabind::newtable(L);
				placement["bone"] = pl.bone.c_str();
				placement["pos"] = pl.pos;
				placement["hpb"] = pl.hpb;
				::luabind::object hides = ::luabind::newtable(L);
				int h = 1;
				for (const shared_str& name : pl.hides)
					hides[h++] = name.c_str();
				placement["hides"] = hides;
				p["placement"] = placement;
			}

			t[path.c_str()] = p;
		}
		return t;
	}
}

namespace instance_record_script
{
	// ---- game object ----------------------------------------------------

	LPCSTR go_instance_section(CScriptGameObject* self)
	{
		CInventoryItem* item = self ? smart_cast<CInventoryItem*>(&self->object()) : nullptr;
		if (item)
			return item->config_section().c_str();
		return self ? self->object().cNameSect().c_str() : "";
	}

	LPCSTR go_get_override(CScriptGameObject* self, LPCSTR key)
	{
		CInventoryItem* item = game_item(self);
		return item ? get_override(item->instance_record(), key) : nullptr;
	}

	// reload = false stores a state value (3c). Clearing a state value needs no section either, so it is
	// deferred like setting one.
	bool go_set_override_reload(CScriptGameObject* self, LPCSTR key, const ::luabind::object& value, bool reload)
	{
		CInventoryItem* item = game_item(self);
		const bool defer = !reload || (item && key && item->instance_record().is_state_override(key));
		return item && edit_game(item, [&](CSE_InstanceRecord& r) { return set_override(r, key, value, !reload); }, defer);
	}

	bool go_set_override(CScriptGameObject* self, LPCSTR key, const ::luabind::object& value)
	{
		return go_set_override_reload(self, key, value, true);
	}

	bool go_clear_override(CScriptGameObject* self, LPCSTR key)
	{
		CInventoryItem* item = game_item(self);
		const bool defer = item && key && item->instance_record().is_state_override(key);
		return item && key && edit_game(item, [&](CSE_InstanceRecord& r) { return r.clear_override(key); }, defer);
	}

	bool go_is_state_override(CScriptGameObject* self, LPCSTR key)
	{
		CInventoryItem* item = game_item(self);
		return item && key && item->instance_record().is_state_override(key);
	}

	::luabind::object go_get_overrides(lua_State* L, CScriptGameObject* self)
	{
		CInventoryItem* item = game_item(self);
		return item ? get_overrides(L, item->instance_record()) : ::luabind::newtable(L);
	}

	::luabind::object go_get_overrides_filter(lua_State* L, CScriptGameObject* self, LPCSTR filter)
	{
		CInventoryItem* item = game_item(self);
		return item ? get_overrides(L, item->instance_record(), filter) : ::luabind::newtable(L);
	}

	// Test counters, for every item: records sent to the server copy and sections built at runtime
	::luabind::object go_record_counters(lua_State* L, CScriptGameObject* self)
	{
		::luabind::object t = ::luabind::newtable(L);
		t["sends"] = CInventoryItem::s_instance_sends;
		t["builds"] = CInventoryItem::s_instance_builds;
		return t;
	}

	::luabind::object go_get_parts(lua_State* L, CScriptGameObject* self)
	{
		CInventoryItem* item = game_item(self);
		return item ? get_parts(L, item->instance_record()) : ::luabind::newtable(L);
	}

	u16 go_add_part_pos(CScriptGameObject* self, u16 parent, LPCSTR mount, LPCSTR section, float pos)
	{
		CInventoryItem* item = game_item(self);
		u16 id = 0;
		if (item)
			edit_game(item, [&](CSE_InstanceRecord& r) { id = r.add_part(parent, mount, section, pos); return id != 0; });
		return id;
	}

	u16 go_add_part(CScriptGameObject* self, u16 parent, LPCSTR mount, LPCSTR section)
	{
		return go_add_part_pos(self, parent, mount, section, 0.f);
	}

	u32 go_remove_part(CScriptGameObject* self, u16 id)
	{
		CInventoryItem* item = game_item(self);
		u32 n = 0;
		if (item)
			edit_game(item, [&](CSE_InstanceRecord& r) { n = r.remove_part(id); return n != 0; });
		return n;
	}

	u32 go_record_size(CScriptGameObject* self)
	{
		CInventoryItem* item = game_item(self);
		return item ? item->instance_record().byte_size() : 0;
	}

	bool go_add_modifier_mode(CScriptGameObject* self, LPCSTR source, LPCSTR key, float value, LPCSTR mode)
	{
		CInventoryItem* item = game_item(self);
		u8 m;
		return item && modifier_mode(mode, m) &&
			edit_game(item, [&](CSE_InstanceRecord& r) { return r.add_modifier(source, key, value, m); });
	}

	bool go_add_modifier(CScriptGameObject* self, LPCSTR source, LPCSTR key, float value)
	{
		return go_add_modifier_mode(self, source, key, value, nullptr);
	}

	u32 go_remove_modifier_key(CScriptGameObject* self, LPCSTR source, LPCSTR key)
	{
		CInventoryItem* item = game_item(self);
		u32 n = 0;
		if (item)
			edit_game(item, [&](CSE_InstanceRecord& r) { n = r.remove_modifier(source, key); return n != 0; });
		return n;
	}

	u32 go_remove_modifier(CScriptGameObject* self, LPCSTR source)
	{
		return go_remove_modifier_key(self, source, nullptr);
	}

	::luabind::object go_get_modifiers_key(lua_State* L, CScriptGameObject* self, LPCSTR key)
	{
		CInventoryItem* item = game_item(self);
		return item ? get_modifiers(L, item->instance_record(), item->get_upgrades(), key) : ::luabind::newtable(L);
	}

	::luabind::object go_get_modifiers(lua_State* L, CScriptGameObject* self)
	{
		return go_get_modifiers_key(L, self, nullptr);
	}

	::luabind::object go_explain_stat(lua_State* L, CScriptGameObject* self, LPCSTR key)
	{
		CInventoryItem* item = game_item(self);
		return item ? explain_stat(L, item->object().cNameSect().c_str(), item->instance_record(), item->get_upgrades(), key)
		            : ::luabind::newtable(L);
	}

	LPCSTR go_get_part_prop(CScriptGameObject* self, u16 id, LPCSTR key)
	{
		CInventoryItem* item = game_item(self);
		return item ? item->instance_record().get_part_prop(id, key) : nullptr;
	}

	bool go_set_part_prop(CScriptGameObject* self, u16 id, LPCSTR key, const ::luabind::object& value)
	{
		CInventoryItem* item = game_item(self);
		return item && edit_game(item, [&](CSE_InstanceRecord& r) { return set_part_prop(r, id, key, value); });
	}

	::luabind::object go_get_part_tree(lua_State* L, CScriptGameObject* self)
	{
		::luabind::object t = ::luabind::newtable(L);
		CWeapon* w = self ? smart_cast<CWeapon*>(&self->object()) : nullptr;
		if (!w)
			return t;
		w->parts_update();
		const CWeaponPartTree* tree = w->part_tree();
		if (!tree)
			return t;
		int i = 1;
		for (const CWeaponPartTree::SNode& n : tree->nodes())
		{
			::luabind::object node = ::luabind::newtable(L);
			node["id"] = n.id;
			node["parent"] = n.parent;
			node["section"] = n.section.size() ? n.section.c_str() : "";
			node["model"] = n.model.size() ? n.model.c_str() : "";
			node["bone"] = n.bone.size() ? n.bone.c_str() : "";
			node["drawn"] = n.att != nullptr;
			node["reason"] = n.reason.c_str();
			xr_string hides;
			for (const shared_str& b : n.hides)
				hides += (hides.empty() ? "" : ",") + xr_string(b.c_str());
			node["hides"] = hides.c_str();
			// 4c: the part's own bones that are hidden now (set_part_bone_visible, in the current view)
			xr_string hidden;
			if (n.att && n.att->Kinematics())
			{
				IKinematics* K = n.att->Kinematics();
				for (u16 b = 0; b < K->LL_BoneCount(); ++b)
					if (!K->LL_GetBoneVisible(b))
						hidden += (hidden.empty() ? "" : ",") + xr_string(K->LL_BoneName_dbg(b));
			}
			node["hidden"] = hidden.c_str();
			t[i++] = node;
		}
		return t;
	}

	// ---- 4c script hooks ---------------------------------------------------

	static CWeaponPartTree* tree_of(CScriptGameObject* self)
	{
		CWeapon* w = self ? smart_cast<CWeapon*>(&self->object()) : nullptr;
		return w ? w->part_tree_rw() : nullptr;
	}

	bool go_set_part_bone_visible_view(CScriptGameObject* self, u16 id, LPCSTR bone, bool visible, LPCSTR view)
	{
		CWeaponPartTree::EView v = CWeaponPartTree::VIEW_BOTH;
		if (view && !xr_strcmp(view, "hud"))
			v = CWeaponPartTree::VIEW_HUD;
		else if (view && !xr_strcmp(view, "world"))
			v = CWeaponPartTree::VIEW_WORLD;
		else if (view && *view && xr_strcmp(view, "both"))
		{
			Msg("! [weapon parts] set_part_bone_visible: view must be \"hud\", \"world\" or \"both\", not \"%s\"", view);
			return false;
		}
		CWeaponPartTree* t = tree_of(self);
		if (!t || !t->set_bone_visible(id, bone, visible, v))
			return false;
		smart_cast<CWeapon*>(&self->object())->parts_update(); // applies it now
		return true;
	}

	bool go_set_part_bone_visible(CScriptGameObject* self, u16 id, LPCSTR bone, bool visible)
	{
		return go_set_part_bone_visible_view(self, id, bone, visible, nullptr);
	}

	bool go_attach_part_light_bone(CScriptGameObject* self, u16 id, const ::luabind::object& light, LPCSTR bone)
	{
		std::optional<AttachmentScriptLight*> l = ::luabind::object_cast_nothrow<AttachmentScriptLight*>(light);
		if (!l || !*l)
		{
			Msg("! [weapon parts] attach_part_light: the light must be an attachment_script_light");
			return false;
		}
		CWeaponPartTree* t = tree_of(self);
		if (!t)
			return false;
		lua_State* L = light.lua_state();
		light.pushvalue();
		const int ref = luaL_ref(L, LUA_REGISTRYINDEX); // keeps the script's light alive while the part has it
		return t->attach_light(id, *l, ref, bone);
	}

	bool go_attach_part_light(CScriptGameObject* self, u16 id, const ::luabind::object& light)
	{
		return go_attach_part_light_bone(self, id, light, nullptr);
	}

	bool go_detach_part_light(CScriptGameObject* self, u16 id)
	{
		CWeaponPartTree* t = tree_of(self);
		return t && t->detach_light(id);
	}

	::luabind::object go_get_part_transform(lua_State* L, CScriptGameObject* self, u16 id, LPCSTR bone, bool hud)
	{
		CWeapon* w = self ? smart_cast<CWeapon*>(&self->object()) : nullptr;
		CWeaponPartTree* t = tree_of(self);
		if (!w || !t)
			return ::luabind::object();
		IKinematics* model = nullptr;
		const Fmatrix* xform = nullptr;
		if (hud)
		{
			attachable_hud_item* h = w->HudItemData();
			if (h)
			{
				model = h->m_model;
				xform = &h->m_item_transform;
			}
		}
		else if (w->Visual())
		{
			model = w->Visual()->dcast_PKinematics();
			xform = &w->XFORM();
		}
		Fmatrix m;
		if (!model || !t->part_xform(id, bone, model, *xform, m))
			return ::luabind::object();
		return ::luabind::object(L, m);
	}

	::luabind::object go_get_part_transform_origin(lua_State* L, CScriptGameObject* self, u16 id, bool hud)
	{
		return go_get_part_transform(L, self, id, nullptr, hud);
	}

	::luabind::object go_get_part_tree_stats(lua_State* L, CScriptGameObject* self)
	{
		::luabind::object t = ::luabind::newtable(L);
		CWeaponPartTree* tree = tree_of(self);
		t["builds"] = tree ? tree->builds() : 0;
		t["moves"] = tree ? tree->moves() : 0;
		return t;
	}

	// ---- 4d place_parts hooks ---------------------------------------------

	static xr_vector<int> s_place_parts; // registry references, in registration order

	static int find_place_parts(lua_State* L, int arg)
	{
		for (u32 i = 0; i < s_place_parts.size(); ++i)
		{
			lua_rawgeti(L, LUA_REGISTRYINDEX, s_place_parts[i]);
			const bool same = !!lua_rawequal(L, -1, arg);
			lua_pop(L, 1);
			if (same)
				return int(i);
		}
		return -1;
	}

	// register_place_parts(func): true when added, false when it isn't a function or is registered already
	int lua_register_place_parts(lua_State* L)
	{
		if (!lua_isfunction(L, 1))
		{
			Msg("! [weapon parts] register_place_parts: needs a function");
			lua_pushboolean(L, 0);
			return 1;
		}
		if (find_place_parts(L, 1) >= 0)
		{
			lua_pushboolean(L, 0);
			return 1;
		}
		lua_pushvalue(L, 1);
		s_place_parts.push_back(luaL_ref(L, LUA_REGISTRYINDEX));
		lua_pushboolean(L, 1);
		return 1;
	}

	// unregister_place_parts(func): true when it was registered
	int lua_unregister_place_parts(lua_State* L)
	{
		const int i = lua_isfunction(L, 1) ? find_place_parts(L, 1) : -1;
		if (i >= 0)
		{
			luaL_unref(L, LUA_REGISTRYINDEX, s_place_parts[i]);
			s_place_parts.erase(s_place_parts.begin() + i);
		}
		lua_pushboolean(L, i >= 0);
		return 1;
	}

	bool has_place_parts()
	{
		return !s_place_parts.empty();
	}

	bool run_place_parts(CScriptGameObject* owner, CSE_InstanceRecord& record, LPCSTR owner_name)
	{
		if (s_place_parts.empty() || !owner || record.is_opaque() || record.parts.empty())
			return false;
		lua_State* L = ai().script_engine().lua();
		bool changed = false;
		const xr_vector<int> hooks = s_place_parts; // a hook may register or unregister hooks
		for (int ref : hooks)
		{
			const int top = lua_gettop(L);
			lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
			::luabind::object(L, owner).pushvalue();
			get_parts(L, record).pushvalue(); // each hook sees what the ones before it returned
			if (lua_pcall(L, 2, 1, 0))
			{
				Msg("! [weapon parts] %s: a place_parts hook failed, stored placement kept: %s", owner_name,
					lua_isstring(L, -1) ? lua_tostring(L, -1) : "?");
				lua_settop(L, top);
				continue;
			}
			if (lua_istable(L, -1))
			{
				lua_pushnil(L);
				while (lua_next(L, -2))
				{
					// key at -2 (part id), value at -1 ({bone, offset, hides})
					if (lua_type(L, -2) == LUA_TNUMBER && lua_istable(L, -1))
					{
						const u16 id = u16(lua_tointeger(L, -2));
						for (LPCSTR key : { "bone", "offset", "hides" })
						{
							lua_getfield(L, -1, key);
							if (lua_type(L, -1) == LUA_TSTRING)
							{
								LPCSTR value = lua_tostring(L, -1);
								LPCSTR now = record.get_part_prop(id, key);
								xr_string normal, error;
								if (!*value)
								{
									if (now)
										changed |= record.set_part_prop(id, key, nullptr);
								}
								else if (!weapon_parts::normalise_prop(key, value, normal, error))
									Msg("! [weapon parts] %s: place_parts gave part %u %s = %s, refused: %s", owner_name, u32(id), key, value, error.c_str());
								else if (!now || xr_strcmp(now, normal.c_str()))
								{
									if (record.set_part_prop(id, key, normal.c_str()))
										changed = true;
									else
										Msg("! [weapon parts] %s: place_parts gave a placement for part %u, which isn't on the gun", owner_name, u32(id));
								}
							}
							lua_pop(L, 1);
						}
					}
					lua_pop(L, 1);
				}
			}
			lua_settop(L, top);
		}
		return changed;
	}

	// ---- server item ----------------------------------------------------

	LPCSTR se_get_override(CSE_ALifeInventoryItem* self, LPCSTR key)
	{
		return self ? get_override(server_view(self), key) : nullptr;
	}

	bool se_set_override_reload(CSE_ALifeInventoryItem* self, LPCSTR key, const ::luabind::object& value, bool reload)
	{
		const bool defer = !reload || (self && key && server_view(self).is_state_override(key));
		return edit_server(self, [&](CSE_InstanceRecord& r) { return set_override(r, key, value, !reload); }, defer);
	}

	bool se_set_override(CSE_ALifeInventoryItem* self, LPCSTR key, const ::luabind::object& value)
	{
		return se_set_override_reload(self, key, value, true);
	}

	bool se_clear_override(CSE_ALifeInventoryItem* self, LPCSTR key)
	{
		const bool defer = self && key && server_view(self).is_state_override(key);
		return key && edit_server(self, [&](CSE_InstanceRecord& r) { return r.clear_override(key); }, defer);
	}

	bool se_is_state_override(CSE_ALifeInventoryItem* self, LPCSTR key)
	{
		return self && key && server_view(self).is_state_override(key);
	}

	::luabind::object se_get_overrides(lua_State* L, CSE_ALifeInventoryItem* self)
	{
		return self ? get_overrides(L, server_view(self)) : ::luabind::newtable(L);
	}

	::luabind::object se_get_overrides_filter(lua_State* L, CSE_ALifeInventoryItem* self, LPCSTR filter)
	{
		return self ? get_overrides(L, server_view(self), filter) : ::luabind::newtable(L);
	}

	::luabind::object se_get_parts(lua_State* L, CSE_ALifeInventoryItem* self)
	{
		return self ? get_parts(L, server_view(self)) : ::luabind::newtable(L);
	}

	u16 se_add_part_pos(CSE_ALifeInventoryItem* self, u16 parent, LPCSTR mount, LPCSTR section, float pos)
	{
		u16 id = 0;
		edit_server(self, [&](CSE_InstanceRecord& r) { id = r.add_part(parent, mount, section, pos); return id != 0; });
		return id;
	}

	u16 se_add_part(CSE_ALifeInventoryItem* self, u16 parent, LPCSTR mount, LPCSTR section)
	{
		return se_add_part_pos(self, parent, mount, section, 0.f);
	}

	u32 se_remove_part(CSE_ALifeInventoryItem* self, u16 id)
	{
		u32 n = 0;
		edit_server(self, [&](CSE_InstanceRecord& r) { n = r.remove_part(id); return n != 0; });
		return n;
	}

	u32 se_record_size(CSE_ALifeInventoryItem* self)
	{
		return self ? server_view(self).byte_size() : 0;
	}

	LPCSTR se_get_part_prop(CSE_ALifeInventoryItem* self, u16 id, LPCSTR key)
	{
		return self ? server_view(self).get_part_prop(id, key) : nullptr;
	}

	bool se_set_part_prop(CSE_ALifeInventoryItem* self, u16 id, LPCSTR key, const ::luabind::object& value)
	{
		return edit_server(self, [&](CSE_InstanceRecord& r) { return set_part_prop(r, id, key, value); });
	}

	bool se_add_modifier_mode(CSE_ALifeInventoryItem* self, LPCSTR source, LPCSTR key, float value, LPCSTR mode)
	{
		u8 m;
		return modifier_mode(mode, m) && edit_server(self, [&](CSE_InstanceRecord& r) { return r.add_modifier(source, key, value, m); });
	}

	bool se_add_modifier(CSE_ALifeInventoryItem* self, LPCSTR source, LPCSTR key, float value)
	{
		return se_add_modifier_mode(self, source, key, value, nullptr);
	}

	u32 se_remove_modifier_key(CSE_ALifeInventoryItem* self, LPCSTR source, LPCSTR key)
	{
		u32 n = 0;
		edit_server(self, [&](CSE_InstanceRecord& r) { n = r.remove_modifier(source, key); return n != 0; });
		return n;
	}

	u32 se_remove_modifier(CSE_ALifeInventoryItem* self, LPCSTR source)
	{
		return se_remove_modifier_key(self, source, nullptr);
	}

	::luabind::object se_get_modifiers_key(lua_State* L, CSE_ALifeInventoryItem* self, LPCSTR key)
	{
		return self ? get_modifiers(L, server_view(self), server_upgrades(self), key) : ::luabind::newtable(L);
	}

	::luabind::object se_get_modifiers(lua_State* L, CSE_ALifeInventoryItem* self)
	{
		return se_get_modifiers_key(L, self, nullptr);
	}

	::luabind::object se_explain_stat(lua_State* L, CSE_ALifeInventoryItem* self, LPCSTR key)
	{
		return self ? explain_stat(L, self->base()->s_name.c_str(), server_view(self), server_upgrades(self), key)
		            : ::luabind::newtable(L);
	}
}

namespace
{
	xr_vector<shared_str> server_upgrades(CSE_ALifeInventoryItem* se)
	{
		if (CInventoryItem* item = online_item(se))
			return item->get_upgrades();
		return se->m_upgrades;
	}
}

void weapon_parts_registrator::script_register(lua_State* L)
{
	lua_register(L, "model_bones", &lua_model_bones);
	lua_register(L, "bone_bind_transform", &lua_bone_bind_transform);
	lua_register(L, "weapon_bone_pose", &lua_weapon_bone_pose);
	lua_register(L, "register_place_parts", &instance_record_script::lua_register_place_parts);
	lua_register(L, "unregister_place_parts", &instance_record_script::lua_unregister_place_parts);
}
