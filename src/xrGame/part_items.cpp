////////////////////////////////////////////////////////////////////////////
//	Module 		: part_items.cpp
//	Description : Spawning and releasing the items that parts come from and go back to
//	              (WEAPON_ENGINE_PLAN.md 10.1, change 5a)
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "part_items.h"
#include "alife_simulator.h"
#include "ai_space.h"
#include "alife_object_registry.h"
#include "xrServer.h"
#include "Level.h"
#include "../xrServerEntities/xrServer_Objects_ALife_Items.h"
#include "../xrServerEntities/InstanceRecord.h"

namespace part_items
{
	static void setup(CSE_Abstract* e, float condition, const xr_vector<shared_str>& upgrades, const CSE_InstanceRecord& record)
	{
		CSE_ALifeInventoryItem* item = smart_cast<CSE_ALifeInventoryItem*>(e);
		if (!item)
			return;
		item->m_fCondition = condition;
		item->m_upgrades = upgrades;
		item->m_instance = record;
	}

	u16 spawn(LPCSTR section, u16 owner_id, u16 place_id, float condition, const xr_vector<shared_str>& upgrades,
		const CSE_InstanceRecord& record)
	{
		if (!section || !*section || !pSettings->section_exist(section) || !ai().get_alife())
		{
			Msg("! [weapon parts] can't spawn [%s]: no such section, or no ALife", section ? section : "");
			return u16(-1);
		}
		CALifeSimulator& sim = const_cast<CALifeSimulator&>(ai().alife());

		// Where: the owner's place for an inventory spawn, else the place of place_id
		const u16 where_id = owner_id != u16(-1) ? owner_id : place_id;
		CSE_ALifeDynamicObject* where = ai().alife().objects().object(where_id, true);
		if (!where)
		{
			Msg("! [weapon parts] can't spawn [%s]: object %u is not in ALife", section, u32(where_id));
			return u16(-1);
		}
		const Fvector pos = where->o_Position;
		const u32 lvid = where->m_tNodeID;
		const GameGraph::_GRAPH_ID gvid = where->m_tGraphID;

		CSE_ALifeDynamicObject* owner = owner_id != u16(-1) ? ai().alife().objects().object(owner_id, true) : nullptr;
		if (!owner || !owner->m_bOnline)
		{
			// Offline owner or the ground: the entity is registered now and goes online later with its state
			CSE_Abstract* e = sim.spawn_item(section, pos, lvid, gvid, owner_id);
			if (!e)
				return u16(-1);
			setup(e, condition, upgrades, record);
			return e->ID;
		}

		// Online owner: as alife():create does, write the spawn packet with the state in it and let the server
		// spawn it, so the client never sees the item without its state
		CSE_Abstract* e = sim.spawn_item(section, pos, lvid, gvid, owner_id, false);
		if (!e)
			return u16(-1);
		setup(e, condition, upgrades, record);
		NET_Packet packet;
		packet.w_begin(M_SPAWN);
		packet.w_stringZ(section);
		e->Spawn_Write(packet, FALSE);
		sim.server().FreeID(e->ID, 0);
		F_entity_Destroy(e);

		ClientID client;
		client.set(0xffff);
		u16 dummy;
		packet.r_begin(dummy);
		CSE_Abstract* spawned = sim.server().Process_spawn(packet, client);
		return spawned ? spawned->ID : u16(-1);
	}

	bool release(u16 id)
	{
		if (!ai().get_alife())
			return false;
		CALifeSimulator& sim = const_cast<CALifeSimulator&>(ai().alife());
		CSE_ALifeDynamicObject* e = ai().alife().objects().object(id, true);
		if (!e)
			return false;
		if (!e->m_bOnline)
		{
			sim.release(e, true);
			return true;
		}
		NET_Packet packet;
		packet.w_begin(M_EVENT);
		packet.w_u32(Level().timeServer());
		packet.w_u16(GE_DESTROY);
		packet.w_u16(id);
		Level().Send(packet, net_flags(TRUE, TRUE));
		return true;
	}
}
