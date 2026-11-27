////////////////////////////////////////////////////////////////////////////
//	Module 		: instance_section.cpp
//	Description : Generated config section of one item (WEAPON_ENGINE_PLAN.md 5.2, change 2f)
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "instance_section.h"
#include "../xrServerEntities/InstanceRecord.h"

namespace instance_section
{
	shared_str name(LPCSTR base, u16 id)
	{
		string256 buf;
		xr_sprintf(buf, "%s_i%u", base, u32(id));
		return shared_str(buf);
	}

	bool needed(const CSE_InstanceRecord& record)
	{
		// A record from a newer build can't be read, so the item stays on its base section. State values (3c)
		// never reach the section, so a record with only those needs none.
		return !record.is_opaque() && record.has_config_overrides();
	}

	shared_str build(LPCSTR base, u16 id, const CSE_InstanceRecord& record)
	{
		const shared_str sect = name(base, id);
		if (!needed(record))
		{
			pSettings->remove_dynamic_section(sect.c_str());
			return shared_str();
		}

		// Layers, later ones winning (5.2): the base section, then part role data (phase 5), then overrides
		CInifile::DynamicItems items;
		items.reserve(record.overrides.size());
		for (const SInstanceProp& p : record.overrides)
			if (!p.is_state_value())
				items.push_back({ p.key, p.value });

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
