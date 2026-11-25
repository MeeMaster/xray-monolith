////////////////////////////////////////////////////////////////////////////
//	Module 		: xrServer_Objects_ALife_Items_script.cpp
//	Created 	: 19.09.2002
//  Modified 	: 04.06.2003
//	Author		: Dmitriy Iassenev
//	Description : Server items for ALife simulator, script export
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "xrServer_Objects_ALife_Items.h"
#include "xrServer_script_macroses.h"
#include "../xrGame/instance_record_script.h"

void add_upgrade_script(CSE_ALifeInventoryItem* ta, LPCSTR str)
{
	ta->add_upgrade(str);
}

bool has_upgrade_script(CSE_ALifeInventoryItem* ta, LPCSTR str)
{
	return ta->has_upgrade(str);
}

float get_condition_script(CSE_ALifeInventoryItem* item)
{
	return item->m_fCondition;
}

// the net packet quantises condition over zero to one, anything else corrupts the stored value
void set_condition_script(CSE_ALifeInventoryItem* item, float value)
{
	if (value < 0.f || value > 1.f)
	{
		Msg("!cse_alife_inventory_item.condition set to %f, must be between 0 and 1", value);
		return;
	}

	item->m_fCondition = value;
}

using namespace luabind;

#pragma optimize("s",on)
void CSE_ALifeInventoryItem::script_register(lua_State* L)
{
	module(L)[
		class_<CSE_ALifeInventoryItem>
		("cse_alife_inventory_item")
		//			.def(		constructor<LPCSTR>())
		.def("has_upgrade", &has_upgrade)
		.def("add_upgrade", &add_upgrade)
		.property("condition", &get_condition_script, &set_condition_script)
		// WEAPON_ENGINE_PLAN.md 14 (2h): the instance record; edits go through the game object while there is one
		.def("get_override", &instance_record_script::se_get_override)
		.def("set_override", &instance_record_script::se_set_override)
		.def("set_override", &instance_record_script::se_clear_override) // set_override(key) clears
		.def("clear_override", &instance_record_script::se_clear_override)
		.def("get_overrides", &instance_record_script::se_get_overrides, raw<1>())
		.def("get_parts", &instance_record_script::se_get_parts, raw<1>())
		.def("record_add_part", &instance_record_script::se_add_part)
		.def("record_add_part", &instance_record_script::se_add_part_pos)
		.def("record_remove_part", &instance_record_script::se_remove_part)
		.def("record_size", &instance_record_script::se_record_size)
	];
}

void CSE_ALifeItem::script_register(lua_State* L)
{
	module(L)[
		luabind_class_item2(
			//		luabind_class_abstract2(
			CSE_ALifeItem,
			"cse_alife_item",
			CSE_ALifeDynamicObjectVisual,
			CSE_ALifeInventoryItem
		)
	];
}

void CSE_ALifeItemTorch::script_register(lua_State* L)
{
	module(L)[
		luabind_class_item1(
			CSE_ALifeItemTorch,
			"cse_alife_item_torch",
			CSE_ALifeItem
		)
	];
}

void CSE_ALifeItemAmmo::script_register(lua_State* L)
{
	module(L)[
		luabind_class_item1(
			CSE_ALifeItemAmmo,
			"cse_alife_item_ammo",
			CSE_ALifeItem
		)
	];
}

void CSE_ALifeItemWeapon::script_register(lua_State* L)
{
	module(L)[
		luabind_class_item1(
			CSE_ALifeItemWeapon,
			"cse_alife_item_weapon",
			CSE_ALifeItem
		)
		.def("clone_addons", &CSE_ALifeItemWeapon::clone_addons)
		.def("set_ammo_elapsed", &CSE_ALifeItemWeapon::set_ammo_elapsed)
		.def("get_ammo_elapsed", &CSE_ALifeItemWeapon::get_ammo_elapsed)
		.def("get_ammo_magsize", &CSE_ALifeItemWeapon::get_ammo_magsize)
	];
}

void CSE_ALifeItemWeaponShotGun::script_register(lua_State* L)
{
	module(L)[
		luabind_class_item1(
			CSE_ALifeItemWeaponShotGun,
			"cse_alife_item_weapon_shotgun",
			CSE_ALifeItemWeapon
		)
	];
}

void CSE_ALifeItemWeaponAutoShotGun::script_register(lua_State* L)
{
	module(L)[
		luabind_class_item1(
			CSE_ALifeItemWeaponAutoShotGun,
			"cse_alife_item_weapon_auto_shotgun",
			CSE_ALifeItemWeapon
		)
	];
}

void CSE_ALifeItemDetector::script_register(lua_State* L)
{
	module(L)[
		luabind_class_item1(
			CSE_ALifeItemDetector,
			"cse_alife_item_detector",
			CSE_ALifeItem
		)
	];
}

void CSE_ALifeItemArtefact::script_register(lua_State* L)
{
	module(L)[
		luabind_class_item1(
			CSE_ALifeItemArtefact,
			"cse_alife_item_artefact",
			CSE_ALifeItem
		)
	];
}
