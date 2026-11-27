////////////////////////////////////////////////////////////////////////////
//	Module 		: instance_record_script.h
//	Description : Script access to an item's instance record, the first slice of the
//	              W13 script API (WEAPON_ENGINE_PLAN.md 14, change 2h)
//	              Include after pch_script.h (needs luabind).
////////////////////////////////////////////////////////////////////////////

#pragma once

class CScriptGameObject;
class CSE_ALifeInventoryItem;

namespace instance_record_script
{
	// Methods of game_object (an online item). Edits go through the game-side copy, which
	// sends the record to the server (4.3).
	LPCSTR go_instance_section(CScriptGameObject* self);
	LPCSTR go_get_override(CScriptGameObject* self, LPCSTR key);
	bool go_set_override(CScriptGameObject* self, LPCSTR key, const ::luabind::object& value);
	// 3c: set_override(key, value, reload); reload = false stores a state value
	bool go_set_override_reload(CScriptGameObject* self, LPCSTR key, const ::luabind::object& value, bool reload);
	bool go_clear_override(CScriptGameObject* self, LPCSTR key);
	bool go_is_state_override(CScriptGameObject* self, LPCSTR key);
	::luabind::object go_get_overrides(lua_State* L, CScriptGameObject* self);
	::luabind::object go_get_overrides_filter(lua_State* L, CScriptGameObject* self, LPCSTR filter);
	::luabind::object go_record_counters(lua_State* L, CScriptGameObject* self);
	::luabind::object go_get_parts(lua_State* L, CScriptGameObject* self);
	u16 go_add_part(CScriptGameObject* self, u16 parent, LPCSTR mount, LPCSTR section);
	u16 go_add_part_pos(CScriptGameObject* self, u16 parent, LPCSTR mount, LPCSTR section, float pos);
	u32 go_remove_part(CScriptGameObject* self, u16 id);
	u32 go_record_size(CScriptGameObject* self);

	// Methods of cse_alife_inventory_item. While the item has a game object, edits are
	// forwarded to it; otherwise they change the server copy, which the next spawn reads.
	LPCSTR se_get_override(CSE_ALifeInventoryItem* self, LPCSTR key);
	bool se_set_override(CSE_ALifeInventoryItem* self, LPCSTR key, const ::luabind::object& value);
	bool se_set_override_reload(CSE_ALifeInventoryItem* self, LPCSTR key, const ::luabind::object& value, bool reload);
	bool se_clear_override(CSE_ALifeInventoryItem* self, LPCSTR key);
	bool se_is_state_override(CSE_ALifeInventoryItem* self, LPCSTR key);
	::luabind::object se_get_overrides(lua_State* L, CSE_ALifeInventoryItem* self);
	::luabind::object se_get_overrides_filter(lua_State* L, CSE_ALifeInventoryItem* self, LPCSTR filter);
	::luabind::object se_get_parts(lua_State* L, CSE_ALifeInventoryItem* self);
	u16 se_add_part(CSE_ALifeInventoryItem* self, u16 parent, LPCSTR mount, LPCSTR section);
	u16 se_add_part_pos(CSE_ALifeInventoryItem* self, u16 parent, LPCSTR mount, LPCSTR section, float pos);
	u32 se_remove_part(CSE_ALifeInventoryItem* self, u16 id);
	u32 se_record_size(CSE_ALifeInventoryItem* self);
}
