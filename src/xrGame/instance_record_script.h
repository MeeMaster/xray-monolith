////////////////////////////////////////////////////////////////////////////
//	Module 		: instance_record_script.h
//	Description : Script access to an item's instance record, the first slice of the
//	              W13 script API (WEAPON_ENGINE_PLAN.md 14, change 2h)
//	              Include after pch_script.h (needs luabind).
////////////////////////////////////////////////////////////////////////////

#pragma once

class CScriptGameObject;
class CSE_ALifeInventoryItem;
class CSE_InstanceRecord;

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
	// 3d: stacked modifiers. mode is "flat" (default) or "pct".
	bool go_add_modifier(CScriptGameObject* self, LPCSTR source, LPCSTR key, float value);
	bool go_add_modifier_mode(CScriptGameObject* self, LPCSTR source, LPCSTR key, float value, LPCSTR mode);
	u32 go_remove_modifier(CScriptGameObject* self, LPCSTR source);
	u32 go_remove_modifier_key(CScriptGameObject* self, LPCSTR source, LPCSTR key);
	::luabind::object go_get_modifiers(lua_State* L, CScriptGameObject* self);
	::luabind::object go_get_modifiers_key(lua_State* L, CScriptGameObject* self, LPCSTR key);
	::luabind::object go_explain_stat(lua_State* L, CScriptGameObject* self, LPCSTR key);
	// 4a: part props; bone, offset and hides are checked and normalised (weapon_parts.h)
	LPCSTR go_get_part_prop(CScriptGameObject* self, u16 id, LPCSTR key);
	bool go_set_part_prop(CScriptGameObject* self, u16 id, LPCSTR key, const ::luabind::object& value);
	// 4b: the weapon's drawn part tree: {{id, parent, section, model, bone, drawn, reason}, ...}
	::luabind::object go_get_part_tree(lua_State* L, CScriptGameObject* self);
	// 4c script hooks (WEAPON_ENGINE_PLAN.md 8.2)
	bool go_set_part_bone_visible(CScriptGameObject* self, u16 id, LPCSTR bone, bool visible);
	bool go_set_part_bone_visible_view(CScriptGameObject* self, u16 id, LPCSTR bone, bool visible, LPCSTR view);
	bool go_attach_part_light(CScriptGameObject* self, u16 id, const ::luabind::object& light);
	bool go_attach_part_light_bone(CScriptGameObject* self, u16 id, const ::luabind::object& light, LPCSTR bone);
	bool go_detach_part_light(CScriptGameObject* self, u16 id);
	::luabind::object go_get_part_tree_stats(lua_State* L, CScriptGameObject* self);
	// 5b roles: {sight = {ids}, muzzle = {ids}, launcher = {ids}, active_sight = id}; any item's part roles
	::luabind::object go_get_roles(lua_State* L, CScriptGameObject* self);
	bool go_has_part_role(CScriptGameObject* self, LPCSTR role);
	::luabind::object go_get_part_roles(lua_State* L, CScriptGameObject* self, u16 id);

	// 5a fit and remove (WEAPON_ENGINE_PLAN.md 10.1), on any item with a record: a gun or a loose part item.
	// fit_part(item | section, parent_id, mount [, props]): the item's state and contents move into a new part,
	// the item goes; returns the part id, 0 on failure (nothing changed).
	u16 go_fit_part_item(CScriptGameObject* self, CScriptGameObject* item, u16 parent, LPCSTR mount);
	u16 go_fit_part_item_props(CScriptGameObject* self, CScriptGameObject* item, u16 parent, LPCSTR mount, const ::luabind::object& props);
	u16 go_fit_part_section(CScriptGameObject* self, LPCSTR section, u16 parent, LPCSTR mount);
	u16 go_fit_part_section_props(CScriptGameObject* self, LPCSTR section, u16 parent, LPCSTR mount, const ::luabind::object& props);
	// remove_part(id [, {loose = true}]): the part comes off as an item that keeps its contents (or, loose, every
	// part as its own item); returns a table of the spawned item ids, nil on failure (nothing changed).
	::luabind::object go_take_off_part(lua_State* L, CScriptGameObject* self, u16 id);
	::luabind::object go_take_off_part_opts(lua_State* L, CScriptGameObject* self, u16 id, const ::luabind::object& opts);

	// 4d: the place_parts hooks (WEAPON_ENGINE_PLAN.md 8.1), registered with the global register_place_parts(func).
	// run_place_parts calls each in registration order with (weapon, parts) and writes the bone, offset and hides
	// they return into record. true when record changed. owner_name: for log lines.
	bool has_place_parts();
	bool run_place_parts(CScriptGameObject* owner, CSE_InstanceRecord& record, LPCSTR owner_name);
	::luabind::object go_get_part_transform(lua_State* L, CScriptGameObject* self, u16 id, LPCSTR bone, bool hud);
	::luabind::object go_get_part_transform_origin(lua_State* L, CScriptGameObject* self, u16 id, bool hud);

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
	bool se_add_modifier(CSE_ALifeInventoryItem* self, LPCSTR source, LPCSTR key, float value);
	bool se_add_modifier_mode(CSE_ALifeInventoryItem* self, LPCSTR source, LPCSTR key, float value, LPCSTR mode);
	u32 se_remove_modifier(CSE_ALifeInventoryItem* self, LPCSTR source);
	u32 se_remove_modifier_key(CSE_ALifeInventoryItem* self, LPCSTR source, LPCSTR key);
	::luabind::object se_get_modifiers(lua_State* L, CSE_ALifeInventoryItem* self);
	::luabind::object se_get_modifiers_key(lua_State* L, CSE_ALifeInventoryItem* self, LPCSTR key);
	::luabind::object se_explain_stat(lua_State* L, CSE_ALifeInventoryItem* self, LPCSTR key);
	LPCSTR se_get_part_prop(CSE_ALifeInventoryItem* self, u16 id, LPCSTR key);
	bool se_set_part_prop(CSE_ALifeInventoryItem* self, u16 id, LPCSTR key, const ::luabind::object& value);
	// 5a fit and remove on server entities: offline items; an online one goes through its object
	u16 se_fit_part_item(CSE_ALifeInventoryItem* self, CSE_ALifeInventoryItem* item, u16 parent, LPCSTR mount);
	u16 se_fit_part_item_props(CSE_ALifeInventoryItem* self, CSE_ALifeInventoryItem* item, u16 parent, LPCSTR mount, const ::luabind::object& props);
	u16 se_fit_part_section(CSE_ALifeInventoryItem* self, LPCSTR section, u16 parent, LPCSTR mount);
	u16 se_fit_part_section_props(CSE_ALifeInventoryItem* self, LPCSTR section, u16 parent, LPCSTR mount, const ::luabind::object& props);
	::luabind::object se_take_off_part(lua_State* L, CSE_ALifeInventoryItem* self, u16 id);
	::luabind::object se_take_off_part_opts(lua_State* L, CSE_ALifeInventoryItem* self, u16 id, const ::luabind::object& opts);
}

// 4a: global functions for the script resolver (WEAPON_ENGINE_PLAN.md 8.1): model_bones, bone_bind_transform
struct weapon_parts_registrator
{
	static void script_register(lua_State* L);
};
