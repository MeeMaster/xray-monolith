////////////////////////////////////////////////////////////////////////////
//	Module 		: instance_section.cpp
//	Description : Generated config section of one item (WEAPON_ENGINE_PLAN.md 5.2, change 2f)
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "instance_section.h"
#include "../xrServerEntities/InstanceRecord.h"
#include "stat_mods.h"
#include "weapon_parts.h"

namespace instance_section
{
	shared_str name(LPCSTR base, u16 id)
	{
		string256 buf;
		xr_sprintf(buf, "%s_i%u", base, u32(id));
		return shared_str(buf);
	}

	bool needed(const CSE_InstanceRecord& record, const xr_vector<shared_str>* upgrades)
	{
		// A record from a newer build can't be read, so the item stays on its base section. State values (3c)
		// never reach the section, so a record with only those needs none.
		if (record.is_opaque())
			return false;
		if (record.has_config_overrides() || weapon_parts::role_signature(record).size())
			return true;
		stat_mods::Contributions mods;
		stat_mods::collect(record, upgrades, mods);
		return !mods.empty();
	}

	shared_str build(LPCSTR base, u16 id, const CSE_InstanceRecord& record, const xr_vector<shared_str>* upgrades)
	{
		const shared_str sect = name(base, id);
		stat_mods::Contributions mods;
		if (!record.is_opaque())
			stat_mods::collect(record, upgrades, mods);
		const shared_str roles = record.is_opaque() ? shared_str() : weapon_parts::role_signature(record);
		if (record.is_opaque() || (!record.has_config_overrides() && mods.empty() && !roles.size()))
		{
			pSettings->remove_dynamic_section(sect.c_str());
			return shared_str();
		}

		// Layers, later ones winning (5.2): the base section, then part role data (5b: sight, muzzle, launcher),
		// then overrides, then stacked modifiers (3d), which start from the overridden value
		CInifile::DynamicItems items;
		items.reserve(record.overrides.size() + mods.size());
		weapon_parts::role_items(base, record, items);
		for (const SInstanceProp& p : record.overrides)
			if (!p.is_state_value())
				items.push_back({ p.key, p.value });
		stat_mods::apply(base, record, mods, items);
		if (items.empty())
		{
			// Every modifier was skipped (logged): nothing differs from the base section
			pSettings->remove_dynamic_section(sect.c_str());
			return shared_str();
		}

		if (!pSettings->add_dynamic_section(sect.c_str(), base, items))
		{
			Msg("! [instance section] could not build [%s] from [%s]", sect.c_str(), base);
			return shared_str();
		}
		return sect;
	}

	void remove(LPCSTR base, u16 id)
	{
		pSettings->remove_dynamic_section(name(base, id).c_str());
	}
}
