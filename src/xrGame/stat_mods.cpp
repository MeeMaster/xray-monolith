////////////////////////////////////////////////////////////////////////////
//	Module 		: stat_mods.cpp
//	Description : Stacked modifiers (WEAPON_ENGINE_PLAN.md 6.3, change 3d)
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "stat_mods.h"
#include "../xrServerEntities/InstanceRecord.h"

namespace
{
	LPCSTR const LIMITS_FILE = "misc\\stat_mods.ltx";
	LPCSTR const LIMITS_SECTION = "stat_mod_limits";
	LPCSTR const MODS_LINE = "stat_mods";

	struct SLimit
	{
		float min = -flt_max;
		float max = flt_max;
		xr_string def;             // empty: no default
	};

	struct SLimits
	{
		bool loaded = false;
		float pct_floor = -0.9f;
		xr_map<shared_str, SLimit> keys;
	};
	SLimits g_limits;

	// A number filling the whole string, spaces around it allowed
	bool parse_number(LPCSTR s, float& out)
	{
		if (!s)
			return false;
		while (*s == ' ' || *s == '\t')
			++s;
		if (!*s)
			return false;
		char* end = nullptr;
		const double d = strtod(s, &end);
		if (end == s)
			return false;
		while (*end == ' ' || *end == '\t')
			++end;
		if (*end)
			return false;
		out = float(d);
		return _valid(out);
	}

	xr_string format_number(float v)
	{
		string64 buf;
		const double d = v;
		if (d == floor(d) && _abs(d) < 1e15)
			xr_sprintf(buf, "%.0f", d);
		else
			xr_sprintf(buf, "%.7g", d);
		return buf;
	}

	void read_limits(const CInifile& ini)
	{
		CInifile::Sect& S = ini.r_section(LIMITS_SECTION);
		for (const CInifile::Item& item : S.Data)
		{
			LPCSTR v = item.second.c_str();
			if (!xr_strcmp(item.first, "pct_floor"))
			{
				float f;
				if (parse_number(v, f))
					g_limits.pct_floor = f;
				else
					Msg("! [stat mods] %s: pct_floor = %s is not a number", LIMITS_SECTION, v ? v : "");
				continue;
			}
			string256 a, b, c;
			const int n = v ? _GetItemCount(v) : 0;
			SLimit lim;
			float f;
			bool ok = n >= 2 && parse_number(_GetItem(v, 0, a), lim.min) && parse_number(_GetItem(v, 1, b), lim.max);
			if (ok && n >= 3)
			{
				_GetItem(v, 2, c);
				ok = parse_number(c, f);
				lim.def = c;
			}
			if (!ok || lim.min > lim.max)
			{
				Msg("! [stat mods] %s: %s = %s must be <min>, <max> [, <default>]", LIMITS_SECTION, item.first.c_str(), v ? v : "");
				continue;
			}
			g_limits.keys[item.first] = lim;
		}
	}

	const SLimits& limits()
	{
		if (g_limits.loaded)
			return g_limits;
		g_limits.loaded = true;

		// pSettings first, so a DLTX mod can carry the table; else the file next to the other misc configs
		if (pSettings->section_exist(LIMITS_SECTION))
		{
			read_limits(*pSettings);
			return g_limits;
		}
		string_path path;
		if (FS.exist(path, "$game_config$", LIMITS_FILE))
		{
			CInifile ini(path, TRUE, TRUE, FALSE);
			if (ini.section_exist(LIMITS_SECTION))
				read_limits(ini);
		}
		return g_limits;
	}

	const SLimit* limit_of(const shared_str& key)
	{
		const SLimits& L = limits();
		auto it = L.keys.find(key);
		return it == L.keys.end() ? nullptr : &it->second;
	}

	// Each problem is logged once per item section and key, not on every rebuild
	void log_once(LPCSTR section, LPCSTR key, LPCSTR text)
	{
		static xr_set<xr_string> logged;
		xr_string id = section;
		id += ':';
		id += key;
		if (logged.insert(id).second)
			Msg("! [stat mods] [%s] %s: %s", section, key, text);
	}

	// One stat_mods section: `rpm = 0.1, pct` or `rpm = 25` (flat when no mode is given)
	void read_mods_section(LPCSTR mods, const shared_str& source, stat_mods::Contributions& out)
	{
		if (!pSettings->section_exist(mods))
		{
			log_once(mods, MODS_LINE, "the stat_mods section does not exist");
			return;
		}
		CInifile::Sect& S = pSettings->r_section(mods);
		for (const CInifile::Item& item : S.Data)
		{
			LPCSTR v = item.second.c_str();
			const int n = v ? _GetItemCount(v) : 0;
			string256 num, mode;
			stat_mods::SContribution c;
			c.source = source;
			c.key = item.first;
			bool ok = (n == 1 || n == 2) && parse_number(_GetItem(v, 0, num), c.value);
			if (ok && n == 2)
			{
				_GetItem(v, 1, mode);
				if (!xr_strcmp(mode, "pct"))
					c.mode = SInstanceModifier::PCT;
				else if (xr_strcmp(mode, "flat"))
					ok = false;
			}
			if (ok)
				out.push_back(c);
			else
				log_once(mods, item.first.c_str(), "must be <number> [, flat | pct]; ignored");
		}
	}

	// The value a modifier starts from: a config override, else the section's own line, else the limits default
	bool base_value(LPCSTR section, const CSE_InstanceRecord& record, const shared_str& key, xr_string& out, bool& is_default)
	{
		is_default = false;
		for (const SInstanceProp& p : record.overrides)
			if (!p.is_state_value() && p.key == key)
			{
				out = p.value.c_str() ? p.value.c_str() : "";
				return true;
			}
		if (pSettings->line_exist(section, key))
		{
			LPCSTR v = pSettings->r_string(section, key.c_str());
			out = v ? v : "";
			return true;
		}
		if (const SLimit* lim = limit_of(key))
			if (!lim->def.empty())
			{
				out = lim->def;
				is_default = true;
				return true;
			}
		return false;
	}

	// final = clamp((base + flat) * (1 + max(pct, pct_floor))), per element of a list value
	void combine(LPCSTR section, const CSE_InstanceRecord& record, const shared_str& key, const stat_mods::Contributions& all,
	             stat_mods::SExplain& out)
	{
		for (const stat_mods::SContribution& c : all)
			if (c.key == key)
			{
				out.sources.push_back(c);
				(c.mode == SInstanceModifier::PCT ? out.pct : out.flat) += c.value;
			}
		if (out.sources.empty())
			return;

		if (!base_value(section, record, key, out.base, out.base_is_default))
		{
			out.error = "the section has no such key and the limits table gives no default";
			return;
		}
		out.pct_used = _max(out.pct, limits().pct_floor);

		const int n = _GetItemCount(out.base.c_str());
		if (n <= 0)
		{
			out.error = "the base value is empty";
			return;
		}
		const SLimit* lim = limit_of(key);
		xr_string result;
		for (int i = 0; i < n; ++i)
		{
			string256 item;
			float b;
			if (!parse_number(_GetItem(out.base.c_str(), i, item), b))
			{
				out.error = "the base value is not numeric: ";
				out.error += out.base;
				return;
			}
			float v = (b + out.flat) * (1.f + out.pct_used);
			if (lim && (v < lim->min || v > lim->max))
			{
				clamp(v, lim->min, lim->max);
				out.clamped = true;
			}
			if (i)
				result += ", ";
			result += format_number(v);
		}
		out.final_value = result;
		out.modified = true;
	}
}

namespace stat_mods
{
	float pct_floor()
	{
		return limits().pct_floor;
	}

	bool upgrade_has_mods(LPCSTR upgrade_id)
	{
		if (!upgrade_id || !pSettings->section_exist(upgrade_id) || !pSettings->line_exist(upgrade_id, "section"))
			return false;
		LPCSTR prop = pSettings->r_string(upgrade_id, "section");
		return prop && pSettings->section_exist(prop) && pSettings->line_exist(prop, MODS_LINE);
	}

	void collect(const CSE_InstanceRecord& record, const xr_vector<shared_str>* upgrades, Contributions& out)
	{
		out.clear();
		if (record.is_opaque())
			return;

		string256 source;
		if (upgrades)
			for (const shared_str& id : *upgrades)
				if (upgrade_has_mods(id.c_str()))
				{
					LPCSTR prop = pSettings->r_string(id, "section");
					xr_sprintf(source, "upgrade:%s", id.c_str());
					read_mods_section(pSettings->r_string(prop, MODS_LINE), source, out);
				}

		for (const SInstancePart& part : record.parts)
			if (part.section.size() && pSettings->section_exist(part.section) && pSettings->line_exist(part.section, MODS_LINE))
			{
				xr_sprintf(source, "part:%u", u32(part.id));
				read_mods_section(pSettings->r_string(part.section, MODS_LINE), source, out);
			}

		for (const SInstanceModifier& m : record.modifiers)
		{
			SContribution c;
			c.source = m.source;
			c.key = m.key;
			c.mode = m.mode;
			c.value = m.value;
			out.push_back(c);
		}
	}

	void explain(LPCSTR section, const CSE_InstanceRecord& record, const xr_vector<shared_str>* upgrades, LPCSTR key,
	             SExplain& out)
	{
		out = SExplain();
		Contributions all;
		collect(record, upgrades, all);
		const shared_str k = key;
		combine(section, record, k, all, out);
		if (!out.modified && out.sources.empty())
		{
			// No contributions: the base is still worth showing
			base_value(section, record, k, out.base, out.base_is_default);
			out.final_value = out.base;
		}
	}

	u32 apply(LPCSTR section, const CSE_InstanceRecord& record, const Contributions& all, CInifile::DynamicItems& items)
	{
		// Keys in first-seen order, so the result does not depend on how sources are stored
		xr_vector<shared_str> keys;
		for (const SContribution& c : all)
			if (std::find(keys.begin(), keys.end(), c.key) == keys.end())
				keys.push_back(c.key);

		u32 changed = 0;
		for (const shared_str& key : keys)
		{
			SExplain e;
			combine(section, record, key, all, e);
			if (!e.modified)
			{
				xr_string text = e.error;
				text += "; modifier skipped";
				log_once(section, key.c_str(), text.c_str());
				continue;
			}
			bool replaced = false;
			for (CInifile::DynamicItem& it : items)
				if (it.key == key)
				{
					it.value = e.final_value.c_str();
					replaced = true;
				}
			if (!replaced)
				items.push_back({ key, shared_str(e.final_value.c_str()) });
			++changed;
		}
		return changed;
	}
}
