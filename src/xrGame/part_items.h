////////////////////////////////////////////////////////////////////////////
//	Module 		: part_items.h
//	Description : Spawning and releasing the items that parts come from and go back to
//	              (WEAPON_ENGINE_PLAN.md 10.1, change 5a)
////////////////////////////////////////////////////////////////////////////

#pragma once

class CSE_InstanceRecord;

namespace part_items
{
	// Spawns an item of section with the given condition, upgrades and instance record. owner_id: the inventory
	// it goes into (0xffff: on the ground). place_id: the object whose position and graph vertices a ground
	// spawn uses. The state is written before the item reaches the client, so it never exists without it.
	// Returns the new item's id, or 0xffff when section isn't an item or the spawn failed (logged).
	u16 spawn(LPCSTR section, u16 owner_id, u16 place_id, float condition, const xr_vector<shared_str>& upgrades,
		const CSE_InstanceRecord& record);

	// Removes an item from the game (online: through the destroy event, offline: from ALife). false when there is
	// no such object.
	bool release(u16 id);
}
