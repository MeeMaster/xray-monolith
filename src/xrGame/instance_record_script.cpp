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
}
