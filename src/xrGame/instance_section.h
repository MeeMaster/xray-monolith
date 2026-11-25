////////////////////////////////////////////////////////////////////////////
//	Module 		: instance_section.h
//	Description : Generated config section of one item: its base section plus
//	              its instance record (WEAPON_ENGINE_PLAN.md 5.2, change 2f)
////////////////////////////////////////////////////////////////////////////

#pragma once

class CSE_InstanceRecord;

namespace instance_section
{
	// "<base>_i<id>"; a dynamic section name (CInifile::is_dynamic_name)
	shared_str name(LPCSTR base, u16 id);

	// True when the record changes config values, so the item needs its own section. Parts join in phase 5.
	bool needed(const CSE_InstanceRecord& record);

	// Builds or replaces [<base>_i<id>] from the record, or removes it when the record needs none.
	// Returns the section name, or an empty string when the item reads its base section.
	shared_str build(LPCSTR base, u16 id, const CSE_InstanceRecord& record);

	void remove(LPCSTR base, u16 id);
}
