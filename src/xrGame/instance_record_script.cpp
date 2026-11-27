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
#include "weapon_part_devices.h"
#include "weapon_part_tree.h"
#include "Weapon.h"
#include "player_hud.h"
#include "script_light_inline.h"
#include "script_attachment_manager.h"
#include "part_items.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
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
	// alife_inventory_item(se_obj | id): the server entity as cse_alife_inventory_item, or nil. luabind gives an
	// entity the type the function that returned it declares (alife():create gives cse_abstract, alife():object
	// cse_alife_dynamic_object) unless it has a script class from se_item.script, so a plain item (a bandage)
	// otherwise has no condition, upgrades or record methods in Lua (5a).
	CSE_ALifeInventoryItem* alife_inventory_item_obj(CSE_Abstract* e)
	{
		return e ? e->cast_inventory_item() : nullptr;
	}

	CSE_ALifeInventoryItem* alife_inventory_item_id(u16 id)
	{
		CSE_Abstract* e = ai().get_alife() ? ai().alife().objects().object(id, true) : nullptr;
		return alife_inventory_item_obj(e);
	}

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
		// Keeps the script's light alive while the part has it: registry[PART_LIGHTS_KEY][light pointer] = light
		lua_State* L = light.lua_state();
		lua_getfield(L, LUA_REGISTRYINDEX, CWeaponPartTree::PART_LIGHTS_KEY);
		if (!lua_istable(L, -1))
		{
			lua_pop(L, 1);
			lua_newtable(L);
			lua_pushvalue(L, -1);
			lua_setfield(L, LUA_REGISTRYINDEX, CWeaponPartTree::PART_LIGHTS_KEY);
		}
		lua_pushlightuserdata(L, *l);
		light.pushvalue();
		lua_rawset(L, -3);
		lua_pop(L, 1);
		return t->attach_light(id, *l, 1, bone);
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

	// ---- 5b roles -----------------------------------------------------------

	::luabind::object go_get_roles(lua_State* L, CScriptGameObject* self)
	{
		::luabind::object t = ::luabind::newtable(L);
		CWeapon* w = self ? smart_cast<CWeapon*>(&self->object()) : nullptr;
		if (!w)
			return t;
		for (int s = 0; s < CWeapon::PART_SUBSYSTEMS; ++s)
		{
			::luabind::object ids = ::luabind::newtable(L);
			int i = 1;
			for (u16 id : w->role_parts(CWeapon::EPartSubsystem(s)))
				ids[i++] = id;
			t[CWeapon::part_subsystem_role(CWeapon::EPartSubsystem(s))] = ids;
		}
		t["active_sight"] = w->active_sight();
		return t;
	}

	bool go_has_part_role(CScriptGameObject* self, LPCSTR role)
	{
		CInventoryItem* item = game_item(self);
		if (!item || !role)
			return false;
		for (const SInstancePart& p : item->instance_record().parts)
			if (weapon_parts::has_role(p, role))
				return true;
		return false;
	}

	::luabind::object go_get_part_roles(lua_State* L, CScriptGameObject* self, u16 id)
	{
		::luabind::object t = ::luabind::newtable(L);
		CInventoryItem* item = game_item(self);
		const SInstancePart* p = item ? item->instance_record().find_part(id) : nullptr;
		if (!p)
			return t;
		xr_vector<shared_str> roles;
		weapon_parts::part_roles(*p, roles);
		int i = 1;
		for (const shared_str& r : roles)
			t[i++] = r.c_str();
		return t;
	}

	// ---- 5a fit and remove --------------------------------------------------
	// On a game object, or on a server entity; an entity whose object is online goes through the object (4.3).

	// The item being edited: a gun, or a loose part item
	struct SHolder
	{
		CInventoryItem* game = nullptr;
		CSE_ALifeInventoryItem* se = nullptr;

		bool valid() const { return game || se; }
		LPCSTR name() const { return game ? game->object().cNameSect().c_str() : se->base()->s_name.c_str(); }
		u16 id() const { return game ? game->object().ID() : se->base()->ID; }
		// The inventory spawned items go to: the one that holds this item, 0xffff on the ground
		u16 owner() const
		{
			if (game)
				return game->object().H_Parent() ? game->object().H_Parent()->ID() : u16(-1);
			return se->base()->ID_Parent;
		}
		const CSE_InstanceRecord& record() const { return game ? game->instance_record() : se->m_instance; }
		bool commit(const CSE_InstanceRecord& r) const
		{
			if (game)
				return game->set_instance_record(r);
			if (se->m_instance.is_opaque())
				return false;
			se->m_instance = r;
			return true;
		}
	};

	static SHolder holder(CScriptGameObject* go)
	{
		SHolder h;
		h.game = game_item(go);
		return h;
	}

	static SHolder holder(CSE_ALifeInventoryItem* se)
	{
		SHolder h;
		h.game = online_item(se);
		if (!h.game)
			h.se = se;
		return h;
	}

	// The item a part comes from
	struct SSource
	{
		shared_str section;
		float condition = 1.f;
		xr_vector<shared_str> upgrades;
		const CSE_InstanceRecord* record = nullptr;
		u16 id = u16(-1);
	};

	static bool source(CInventoryItem* item, SSource& s)
	{
		if (!item)
			return false;
		s.section = item->object().cNameSect();
		s.condition = item->GetCondition();
		s.upgrades = item->get_upgrades();
		s.record = &item->instance_record();
		s.id = item->object().ID();
		return true;
	}

	static bool source(CSE_ALifeInventoryItem* se, SSource& s)
	{
		if (!se)
			return false;
		if (CInventoryItem* item = online_item(se))
			return source(item, s);
		s.section = se->base()->s_name;
		s.condition = se->m_fCondition;
		s.upgrades = se->m_upgrades;
		s.record = &se->m_instance;
		s.id = se->base()->ID;
		return true;
	}

	// The props a script passes, through the same checks as set_part_prop. false: a refused value (logged)
	static bool apply_props(CSE_InstanceRecord& r, u16 id, const ::luabind::object& props)
	{
		if (!props.is_valid() || props.type() == LUA_TNIL)
			return true;
		if (props.type() != LUA_TTABLE)
		{
			Msg("! [weapon parts] fit_part: props must be a table");
			return false;
		}
		for (::luabind::object::iterator it = props.begin(); it != props.end(); ++it)
		{
			::luabind::object key = it.key();
			if (key.type() != LUA_TSTRING)
				continue;
			::luabind::object value(*it);
			if (!set_part_prop(r, id, ::luabind::object_cast<LPCSTR>(key), value))
				return false;
		}
		return true;
	}

	// from: the item whose state and contents move into the new part, or null for a fresh part of section
	static u16 fit(const SHolder& target, const SSource* from, LPCSTR section, u16 parent, LPCSTR mount, const ::luabind::object* props)
	{
		if (!target.valid() || !section || !*section)
			return 0;
		if (from && from->id == target.id())
		{
			Msg("! [weapon parts] fit_part: an item can't be fitted into itself");
			return 0;
		}
		if (!pSettings->section_exist(section))
		{
			Msg("! [weapon parts] fit_part: no section [%s]", section);
			return 0;
		}
		CSE_InstanceRecord r = target.record();
		const u16 id = r.add_part(parent, mount, section);
		if (!id)
		{
			Msg("! [weapon parts] fit_part: [%s] can't go on part %u of %s (missing parent, a cap, or a read-only record)",
				section, u32(parent), target.name());
			return 0;
		}
		SInstancePart* part = r.find_part(id);
		if (from)
		{
			const CSE_InstanceRecord& own = *from->record;
			part->set_condition(from->condition);
			part->upgrades = from->upgrades;
			part->props = own.self_props;
			part->blob = own.self_blob;
			part->paintjob = own.self_paintjob;
			if (own.has_config_overrides() || !own.modifiers.empty())
				Msg("~ [weapon parts] fit_part: [%s] has config overrides or modifiers of its own; a part doesn't keep them", section);
			if (!r.graft(id, own))
			{
				Msg("! [weapon parts] fit_part: what [%s] holds doesn't fit in the record (a cap); nothing fitted", section);
				return 0;
			}
		}
		if (props && !apply_props(r, id, *props))
			return 0;
		if (!target.commit(r))
			return 0;
		if (from)
			part_items::release(from->id);
		return id;
	}

	// Placement props describe where a part sat on the gun; an item off the gun has none
	static void strip_placement(InstanceProps& props)
	{
		for (auto it = props.begin(); it != props.end();)
		{
			if (weapon_parts::is_placement_prop(it->key.c_str()) && xr_strcmp(it->key, weapon_parts::PROP_VISUAL))
				it = props.erase(it);
			else
				++it;
		}
	}

	static bool loose_opt(const ::luabind::object& opts)
	{
		if (!opts.is_valid() || opts.type() != LUA_TTABLE)
			return false;
		::luabind::object v = opts["loose"];
		return v.type() == LUA_TBOOLEAN && ::luabind::object_cast<bool>(v);
	}

	static ::luabind::object take_off(lua_State* L, const SHolder& target, u16 id, bool loose)
	{
		if (!target.valid())
			return ::luabind::object();
		const CSE_InstanceRecord before = target.record();
		if (!before.find_part(id))
		{
			Msg("! [weapon parts] remove_part: %s has no part %u", target.name(), u32(id));
			return ::luabind::object();
		}

		// What to spawn: the part with its contents inside, or each part of the subtree on its own
		struct SSpawn
		{
			shared_str section;
			float condition;
			xr_vector<shared_str> upgrades;
			CSE_InstanceRecord record;
		};
		xr_vector<SSpawn> spawns;
		xr_vector<u16> ids;
		before.subtree(id, ids);
		for (u16 pid : ids)
		{
			if (!loose && pid != id)
				break; // the contents travel inside the first item
			const SInstancePart* p = before.find_part(pid);
			SSpawn s;
			s.section = p->section;
			s.condition = p->condition_f();
			s.upgrades = p->upgrades;
			if (loose)
			{
				s.record.self_props = p->props;
				s.record.self_blob = p->blob;
				s.record.self_paintjob = p->paintjob;
			}
			else
				before.extract(pid, s.record);
			strip_placement(s.record.self_props);
			if (!s.section.size())
				continue; // a mount emptied on purpose: nothing to give back
			if (!pSettings->section_exist(s.section))
			{
				Msg("! [weapon parts] remove_part: part %u has section [%s], which doesn't exist; it is dropped", u32(pid), s.section.c_str());
				continue;
			}
			spawns.push_back(s);
		}

		// Commit the record first: if that fails nothing is spawned
		CSE_InstanceRecord after = before;
		after.remove_part(id);
		if (!target.commit(after))
			return ::luabind::object();

		::luabind::object out = ::luabind::newtable(L);
		int n = 1;
		for (const SSpawn& s : spawns)
		{
			const u16 spawned = part_items::spawn(s.section.c_str(), target.owner(), target.id(), s.condition, s.upgrades, s.record);
			if (spawned != u16(-1))
				out[n++] = spawned;
		}
		return out;
	}

	// Game objects
	u16 go_fit_part_item_props(CScriptGameObject* self, CScriptGameObject* item, u16 parent, LPCSTR mount, const ::luabind::object& props)
	{
		SSource s;
		if (!source(item ? smart_cast<CInventoryItem*>(&item->object()) : nullptr, s))
		{
			Msg("! [weapon parts] fit_part: not an inventory item");
			return 0;
		}
		return fit(holder(self), &s, s.section.c_str(), parent, mount, &props);
	}

	u16 go_fit_part_item(CScriptGameObject* self, CScriptGameObject* item, u16 parent, LPCSTR mount)
	{
		return go_fit_part_item_props(self, item, parent, mount, ::luabind::object());
	}

	u16 go_fit_part_section_props(CScriptGameObject* self, LPCSTR section, u16 parent, LPCSTR mount, const ::luabind::object& props)
	{
		return fit(holder(self), nullptr, section, parent, mount, &props);
	}

	u16 go_fit_part_section(CScriptGameObject* self, LPCSTR section, u16 parent, LPCSTR mount)
	{
		return fit(holder(self), nullptr, section, parent, mount, nullptr);
	}

	::luabind::object go_take_off_part_opts(lua_State* L, CScriptGameObject* self, u16 id, const ::luabind::object& opts)
	{
		return take_off(L, holder(self), id, loose_opt(opts));
	}

	::luabind::object go_take_off_part(lua_State* L, CScriptGameObject* self, u16 id)
	{
		return take_off(L, holder(self), id, false);
	}

	// Server entities (offline items; an online one goes through its object)
	u16 se_fit_part_item_props(CSE_ALifeInventoryItem* self, CSE_ALifeInventoryItem* item, u16 parent, LPCSTR mount, const ::luabind::object& props)
	{
		SSource s;
		if (!source(item, s))
		{
			Msg("! [weapon parts] fit_part: not an inventory item");
			return 0;
		}
		return fit(holder(self), &s, s.section.c_str(), parent, mount, &props);
	}

	u16 se_fit_part_item(CSE_ALifeInventoryItem* self, CSE_ALifeInventoryItem* item, u16 parent, LPCSTR mount)
	{
		return se_fit_part_item_props(self, item, parent, mount, ::luabind::object());
	}

	u16 se_fit_part_section_props(CSE_ALifeInventoryItem* self, LPCSTR section, u16 parent, LPCSTR mount, const ::luabind::object& props)
	{
		return fit(holder(self), nullptr, section, parent, mount, &props);
	}

	u16 se_fit_part_section(CSE_ALifeInventoryItem* self, LPCSTR section, u16 parent, LPCSTR mount)
	{
		return fit(holder(self), nullptr, section, parent, mount, nullptr);
	}

	::luabind::object se_take_off_part_opts(lua_State* L, CSE_ALifeInventoryItem* self, u16 id, const ::luabind::object& opts)
	{
		return take_off(L, holder(self), id, loose_opt(opts));
	}

	::luabind::object se_take_off_part(lua_State* L, CSE_ALifeInventoryItem* self, u16 id)
	{
		return take_off(L, holder(self), id, false);
	}

	// ---- 4d place_parts hooks ---------------------------------------------

	// The hooks live in a named registry table (an array, registration order), not behind luaL_ref numbers: a
	// numbered reference that something else frees by mistake gets reused for an unrelated value, and the engine
	// then calls that value as a hook.
	static LPCSTR const PLACE_PARTS_KEY = "mw_weapon_place_parts";
	static int s_place_parts_count = 0;

	// Pushes registry[key], creating the table when it is missing
	static void push_registry_table(lua_State* L, LPCSTR key)
	{
		lua_getfield(L, LUA_REGISTRYINDEX, key);
		if (lua_istable(L, -1))
			return;
		lua_pop(L, 1);
		lua_newtable(L);
		lua_pushvalue(L, -1);
		lua_setfield(L, LUA_REGISTRYINDEX, key);
	}

	// 1-based position of the function at stack index arg in the hook table at stack index tbl; 0 when absent
	static int find_place_parts(lua_State* L, int tbl, int arg)
	{
		const int n = int(lua_objlen(L, tbl));
		for (int i = 1; i <= n; ++i)
		{
			lua_rawgeti(L, tbl, i);
			const bool same = !!lua_rawequal(L, -1, arg);
			lua_pop(L, 1);
			if (same)
				return i;
		}
		return 0;
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
		push_registry_table(L, PLACE_PARTS_KEY);
		const int tbl = lua_gettop(L);
		if (find_place_parts(L, tbl, 1))
		{
			lua_settop(L, tbl - 1);
			lua_pushboolean(L, 0);
			return 1;
		}
		lua_pushvalue(L, 1);
		lua_rawseti(L, tbl, int(lua_objlen(L, tbl)) + 1);
		s_place_parts_count = int(lua_objlen(L, tbl));
		lua_settop(L, tbl - 1);
		lua_pushboolean(L, 1);
		return 1;
	}

	// unregister_place_parts(func): true when it was registered
	int lua_unregister_place_parts(lua_State* L)
	{
		if (!lua_isfunction(L, 1))
		{
			lua_pushboolean(L, 0);
			return 1;
		}
		push_registry_table(L, PLACE_PARTS_KEY);
		const int tbl = lua_gettop(L);
		const int i = find_place_parts(L, tbl, 1);
		if (i)
		{
			const int n = int(lua_objlen(L, tbl));
			for (int k = i; k < n; ++k)
			{
				lua_rawgeti(L, tbl, k + 1);
				lua_rawseti(L, tbl, k);
			}
			lua_pushnil(L);
			lua_rawseti(L, tbl, n);
			s_place_parts_count = n - 1;
		}
		lua_settop(L, tbl - 1);
		lua_pushboolean(L, i != 0);
		return 1;
	}

	// ---- 5d devices ---------------------------------------------------------

	// Callbacks as named registry fields, for the reason the place_parts hooks are (above)
	static LPCSTR const LIGHT_TOGGLE_KEY = "mw_weapon_light_toggle";
	static LPCSTR const NPC_LIGHT_RULE_KEY = "mw_weapon_npc_light_rule";
	static bool s_light_toggle = false;
	static bool s_npc_light_rule = false;

	static int set_callback(lua_State* L, bool& set, LPCSTR key, LPCSTR name)
	{
		if (!lua_isfunction(L, 1) && !lua_isnil(L, 1) && !lua_isnone(L, 1))
		{
			Msg("! [weapon parts] %s: needs a function or nil", name);
			lua_pushboolean(L, 0);
			return 1;
		}
		set = !!lua_isfunction(L, 1);
		if (set)
			lua_pushvalue(L, 1);
		else
			lua_pushnil(L);
		lua_setfield(L, LUA_REGISTRYINDEX, key);
		lua_pushboolean(L, 1);
		return 1;
	}

	int lua_set_part_light_toggle_callback(lua_State* L) { return set_callback(L, s_light_toggle, LIGHT_TOGGLE_KEY, "set_part_light_toggle_callback"); }
	int lua_set_part_npc_light_rule(lua_State* L) { return set_callback(L, s_npc_light_rule, NPC_LIGHT_RULE_KEY, "set_part_npc_light_rule"); }

	// Calls registry[key](a, b): 1 true, 0 false, -1 nil or an error
	static int call_slot(bool set, LPCSTR key, CScriptGameObject* a, CScriptGameObject* b, LPCSTR name)
	{
		if (!set || !a)
			return -1;
		lua_State* L = ai().script_engine().lua();
		const int top = lua_gettop(L);
		lua_getfield(L, LUA_REGISTRYINDEX, key);
		if (!lua_isfunction(L, -1))
		{
			lua_settop(L, top);
			return -1;
		}
		::luabind::object(L, a).pushvalue();
		int n = 1;
		if (b)
		{
			::luabind::object(L, b).pushvalue();
			n = 2;
		}
		int out = -1;
		if (lua_pcall(L, n, 1, 0))
			Msg("! [weapon parts] %s failed: %s", name, lua_isstring(L, -1) ? lua_tostring(L, -1) : "?");
		else if (lua_isboolean(L, -1))
			out = lua_toboolean(L, -1) ? 1 : 0;
		lua_settop(L, top);
		return out;
	}

	bool run_light_toggle(CScriptGameObject* gun)
	{
		return call_slot(s_light_toggle, LIGHT_TOGGLE_KEY, gun, nullptr, "the light toggle callback") == 1;
	}

	int run_npc_light_rule(CScriptGameObject* npc, CScriptGameObject* gun)
	{
		return call_slot(s_npc_light_rule, NPC_LIGHT_RULE_KEY, npc, gun, "the NPC light rule");
	}

	bool go_set_part_on(CScriptGameObject* self, u16 id, bool on)
	{
		CWeapon* w = self ? smart_cast<CWeapon*>(&self->object()) : nullptr;
		return w && CWeaponPartDevices::set_on(*w, id, on);
	}

	bool go_is_part_on(CScriptGameObject* self, u16 id)
	{
		CWeapon* w = self ? smart_cast<CWeapon*>(&self->object()) : nullptr;
		return w && w->part_devices() && w->part_devices()->is_lit(id);
	}

	bool go_is_part_powered(CScriptGameObject* self, u16 id)
	{
		CWeapon* w = self ? smart_cast<CWeapon*>(&self->object()) : nullptr;
		return w && w->part_devices() && w->part_devices()->is_powered(id);
	}

	bool go_toggle_part_lights(CScriptGameObject* self)
	{
		CWeapon* w = self ? smart_cast<CWeapon*>(&self->object()) : nullptr;
		return w && CWeaponPartDevices::toggle(*w);
	}

	::luabind::object go_get_hud_aim_offset(lua_State* L, CScriptGameObject* self, bool alt)
	{
		CWeapon* w = self ? smart_cast<CWeapon*>(&self->object()) : nullptr;
		attachable_hud_item* h = w ? w->HudItemData() : nullptr;
		if (!h)
			return ::luabind::object();
		Fvector pos, rot;
		w->hud_aim_offset(alt ? 3 : 1, h, pos, rot);
		::luabind::object t = ::luabind::newtable(L);
		t["pos"] = pos;
		t["rot"] = rot;
		return t;
	}

	bool has_place_parts()
	{
		return s_place_parts_count > 0;
	}

	bool run_place_parts(CScriptGameObject* owner, CSE_InstanceRecord& record, LPCSTR owner_name)
	{
		if (s_place_parts_count <= 0 || !owner || record.is_opaque() || record.parts.empty())
			return false;
		lua_State* L = ai().script_engine().lua();
		bool changed = false;
		// A snapshot of the hooks, since a hook may register or unregister hooks
		const int base = lua_gettop(L);
		push_registry_table(L, PLACE_PARTS_KEY);
		const int live = lua_gettop(L);
		const int count = int(lua_objlen(L, live));
		lua_createtable(L, count, 0);
		const int snap = lua_gettop(L);
		for (int k = 1; k <= count; ++k)
		{
			lua_rawgeti(L, live, k);
			lua_rawseti(L, snap, k);
		}
		for (int k = 1; k <= count; ++k)
		{
			const int top = lua_gettop(L);
			lua_rawgeti(L, snap, k);
			if (!lua_isfunction(L, -1))
			{
				lua_settop(L, top);
				continue;
			}
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
		lua_settop(L, base);
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
	::luabind::module(L)[
		::luabind::def("alife_inventory_item", &alife_inventory_item_obj),
		::luabind::def("alife_inventory_item", &alife_inventory_item_id)
	];
	lua_register(L, "register_place_parts", &instance_record_script::lua_register_place_parts);
	lua_register(L, "set_part_light_toggle_callback", &instance_record_script::lua_set_part_light_toggle_callback);
	lua_register(L, "set_part_npc_light_rule", &instance_record_script::lua_set_part_npc_light_rule);
	lua_register(L, "unregister_place_parts", &instance_record_script::lua_unregister_place_parts);
}
