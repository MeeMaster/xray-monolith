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

	// True when the record or the upgrades change config values, so the item needs its own section:
	// config overrides, or any stacked modifier (3d) from an upgrade, a part or a script.
	// upgrades: the item's installed upgrade ids, or null.
	bool needed(const CSE_InstanceRecord& record, const xr_vector<shared_str>* upgrades = nullptr);

	// Builds or replaces [<base>_i<id>] from the record and the upgrades, or removes it when nothing needs one.
	// Returns the section name, or an empty string when the item reads its base section.
	shared_str build(LPCSTR base, u16 id, const CSE_InstanceRecord& record, const xr_vector<shared_str>* upgrades = nullptr);

	void remove(LPCSTR base, u16 id);
}
