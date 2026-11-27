////////////////////////////////////////////////////////////////////////////
//	Module 		: stat_mods.h
//	Description : Stacked modifiers: upgrades, parts and scripts each nudge a numeric
//	              config key; the sum is written into the generated section
//	              (WEAPON_ENGINE_PLAN.md 6.3, change 3d)
////////////////////////////////////////////////////////////////////////////

#pragma once

class CSE_InstanceRecord;

namespace stat_mods
{
	// One contribution. source is "upgrade:<upgrade id>", "part:<part id>" or a script tag.
	struct SContribution
	{
		shared_str source;
		shared_str key;
		u8 mode = 0;               // SInstanceModifier::FLAT or PCT
		float value = 0.f;
	};
	typedef xr_vector<SContribution> Contributions;

	// Every contribution to an item: upgrades in install order, then parts, then script modifiers.
	// upgrades may be null (no upgrades known).
	void collect(const CSE_InstanceRecord& record, const xr_vector<shared_str>* upgrades, Contributions& out);

	// True when the upgrade's property section (the one its `section =` names) declares stat_mods
	bool upgrade_has_mods(LPCSTR upgrade_id);

	// How one key's final value comes about
	struct SExplain
	{
		bool modified = false;     // true: the key has contributions and a usable base, final is what Load reads
		xr_string base;            // after overrides; the limits default when the section lacks the key
		bool base_is_default = false;
		float flat = 0.f;          // sum of flat contributions
		float pct = 0.f;           // sum of pct contributions
		float pct_used = 0.f;      // pct after the pct floor
		bool clamped = false;      // some element hit the key's limits
		xr_string final_value;
		Contributions sources;
		xr_string error;           // why the contributions were skipped, when modified is false and there are some
	};

	// Explains key for an item with base section `section`
	void explain(LPCSTR section, const CSE_InstanceRecord& record, const xr_vector<shared_str>* upgrades, LPCSTR key,
	             SExplain& out);

	// Applies every contribution to items: a key already in items (a config override) gets its value replaced,
	// others are appended. Returns the number of keys changed.
	u32 apply(LPCSTR section, const CSE_InstanceRecord& record, const Contributions& all, CInifile::DynamicItems& items);

	// Limits table (configs/misc/stat_mods.ltx, [stat_mod_limits]); read once
	float pct_floor();
}
