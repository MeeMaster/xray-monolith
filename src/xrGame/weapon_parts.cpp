////////////////////////////////////////////////////////////////////////////
//	Module 		: weapon_parts.cpp
//	Description : Part placement data and model bind poses (WEAPON_ENGINE_PLAN.md 8.1, change 4a)
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "weapon_parts.h"
#include "../xrServerEntities/InstanceRecord.h"
#include "../Include/xrRender/Kinematics.h"
#include "../xrEngine/bone.h"

namespace weapon_parts
{
	LPCSTR const PROP_BONE = "bone";
	LPCSTR const PROP_OFFSET = "offset";
	LPCSTR const PROP_HIDES = "hides";
	LPCSTR const PROP_VISUAL = "visual";
	LPCSTR const PROP_ROLE = "role";
}

namespace
{
	bool parse_number(LPCSTR s, float& out)
	{
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

	// Comma-separated names, trimmed, empty items dropped
	void split_names(LPCSTR value, xr_vector<shared_str>& out)
	{
		out.clear();
		const int n = _GetItemCount(value);
		for (int i = 0; i < n; ++i)
		{
			string256 item;
			_GetItem(value, i, item);
			_Trim(item);
			if (*item && std::find(out.begin(), out.end(), shared_str(item)) == out.end())
				out.push_back(item);
		}
	}

	bool parse_offset(LPCSTR value, float (&v)[6], int& count)
	{
		count = _GetItemCount(value);
		if (count != 3 && count != 6)
			return false;
		for (int i = 0; i < 6; ++i)
			v[i] = 0.f;
		for (int i = 0; i < count; ++i)
		{
			string64 item;
			if (!parse_number(_GetItem(value, i, item), v[i]))
				return false;
		}
		return true;
	}

	xr_map<shared_str, weapon_parts::BindBones> g_bind_cache;
	xr_set<shared_str> g_bind_failed;
}

namespace weapon_parts
{
	bool is_placement_prop(LPCSTR key)
	{
		return key && (!xr_strcmp(key, PROP_BONE) || !xr_strcmp(key, PROP_OFFSET) || !xr_strcmp(key, PROP_HIDES) ||
			!xr_strcmp(key, PROP_VISUAL));
	}

	bool normalise_prop(LPCSTR key, LPCSTR value, xr_string& out, xr_string& error)
	{
		out = value ? value : "";
		if (key && !xr_strcmp(key, PROP_ROLE) && !out.empty())
		{
			xr_vector<shared_str> names;
			split_names(value, names);
			out.clear();
			xr_vector<xr_string> seen;
			for (const shared_str& n : names)
			{
				string256 low;
				xr_strcpy(low, n.c_str());
				xr_strlwr(low);
				if (std::find(seen.begin(), seen.end(), xr_string(low)) != seen.end())
					continue;
				seen.push_back(low);
				if (!out.empty())
					out += ", ";
				out += low;
			}
			if (out.empty())
			{
				error = "role must list role names";
				return false;
			}
			return true;
		}
		if (!is_placement_prop(key) || out.empty())
			return true;

		if (!xr_strcmp(key, PROP_BONE) || !xr_strcmp(key, PROP_VISUAL))
		{
			string_path name;
			xr_strcpy(name, value);
			_Trim(name);
			if (!*name || strchr(name, ','))
			{
				error = !xr_strcmp(key, PROP_BONE) ? "bone must be one bone name" : "visual must be one mesh path";
				return false;
			}
			out = name;
			return true;
		}
		if (!xr_strcmp(key, PROP_OFFSET))
		{
			float v[6];
			int count;
			if (!parse_offset(value, v, count))
			{
				error = "offset must be x, y, z or x, y, z, h, p, b";
				return false;
			}
			string256 buf;
			xr_sprintf(buf, "%g, %g, %g, %g, %g, %g", v[0], v[1], v[2], v[3], v[4], v[5]);
			out = buf;
			return true;
		}
		xr_vector<shared_str> names;
		split_names(value, names);
		out.clear();
		for (const shared_str& n : names)
		{
			if (!out.empty())
				out += ", ";
			out += n.c_str();
		}
		if (out.empty())
		{
			error = "hides must list bone names";
			return false;
		}
		return true;
	}

	void part_roles(const SInstancePart& part, xr_vector<shared_str>& out)
	{
		out.clear();
		LPCSTR value = nullptr;
		for (const SInstanceProp& p : part.props)
			if (!xr_strcmp(p.key, PROP_ROLE))
				value = p.value.c_str();
		if (!value && part.section.size() && pSettings->section_exist(part.section) && pSettings->line_exist(part.section, "role"))
			value = pSettings->r_string(part.section, "role");
		if (!value || !*value)
			return;
		xr_vector<shared_str> names;
		split_names(value, names);
		for (const shared_str& n : names)
		{
			string256 low;
			xr_strcpy(low, n.c_str());
			xr_strlwr(low);
			const shared_str s = low;
			if (std::find(out.begin(), out.end(), s) == out.end())
				out.push_back(s);
		}
	}

	bool has_role(const SInstancePart& part, LPCSTR role)
	{
		xr_vector<shared_str> roles;
		part_roles(part, roles);
		for (const shared_str& r : roles)
			if (!xr_strcmp(r, role))
				return true;
		return false;
	}

	u16 active_sight(const CSE_InstanceRecord& record)
	{
		if (record.is_opaque())
			return 0;
		auto depth = [&](const SInstancePart& part) {
			u32 d = 0;
			for (const SInstancePart* p = &part; p && p->parent; p = record.find_part(p->parent))
				++d;
			return d;
		};
		LPCSTR chosen = record.get_override("active_sight");
		const u16 chosen_id = chosen ? u16(atoi(chosen)) : 0;
		u16 best = 0;
		u32 best_depth = 0;
		for (const SInstancePart& p : record.parts)
		{
			if (!has_role(p, "sight"))
				continue;
			if (chosen_id && p.id == chosen_id)
				return p.id;
			const u32 d = depth(p);
			if (!best || d < best_depth)
			{
				best = p.id;
				best_depth = d;
			}
		}
		return best;
	}

	shared_str sight_data_section(const SInstancePart& part)
	{
		if (!part.section.size() || !pSettings->section_exist(part.section))
			return shared_str();
		if (pSettings->line_exist(part.section, "sight_section"))
		{
			LPCSTR s = pSettings->r_string(part.section, "sight_section");
			if (s && *s && pSettings->section_exist(s))
				return shared_str(s);
			Msg("! [weapon parts] [%s] sight_section [%s] does not exist; its own keys are used", part.section.c_str(), s ? s : "");
		}
		return part.section;
	}

	shared_str active_sight_data(const CSE_InstanceRecord& record)
	{
		const SInstancePart* p = record.find_part(active_sight(record));
		return p ? sight_data_section(*p) : shared_str();
	}

	static bool is_sight_key(LPCSTR k)
	{
		if (!xr_strcmp(k, "scope_status") || !xr_strcmp(k, "scope_name") || !xr_strcmp(k, "scope_x") || !xr_strcmp(k, "scope_y"))
			return false; // the gun's own: addon status, and the attachable scope's name and icon place
		return !strncmp(k, "scope_", 6) || !strncmp(k, "s3ds_", 5) || !xr_strcmp(k, "min_scope_zoom_factor") ||
			!xr_strcmp(k, "zoom_step_count") || !xr_strcmp(k, "holder_range_modifier") ||
			!xr_strcmp(k, "holder_fov_modifier") || !xr_strcmp(k, "zoom_enabled");
	}

	void sight_items(LPCSTR data_section, CInifile::DynamicItems& out)
	{
		if (!data_section || !*data_section || !pSettings->section_exist(data_section))
			return;
		xr_vector<shared_str> extra;
		if (pSettings->line_exist(data_section, "sight_keys"))
			split_names(pSettings->r_string(data_section, "sight_keys"), extra);
		for (const CInifile::Item& it : pSettings->r_section(data_section).Data)
		{
			if (!it.first.size() || !it.second.size())
				continue;
			if (is_sight_key(it.first.c_str()) || std::find(extra.begin(), extra.end(), it.first) != extra.end())
				out.push_back({ it.first, it.second });
		}
		out.push_back({ shared_str("scope_status"), shared_str("1") });
	}

	bool read_placement(const SInstancePart& part, SPlacement& out)
	{
		out = SPlacement();
		for (const SInstanceProp& p : part.props)
		{
			if (!p.value.size())
				continue;
			if (!xr_strcmp(p.key, PROP_BONE))
				out.bone = p.value;
			else if (!xr_strcmp(p.key, PROP_OFFSET))
			{
				float v[6];
				int count;
				if (parse_offset(p.value.c_str(), v, count))
				{
					out.pos.set(v[0], v[1], v[2]);
					out.hpb.set(v[3], v[4], v[5]);
				}
			}
			else if (!xr_strcmp(p.key, PROP_HIDES))
				split_names(p.value.c_str(), out.hides);
			else if (!xr_strcmp(p.key, PROP_VISUAL))
				out.visual = p.value;
		}
		return out.bone.size() != 0;
	}

	bool resolve_model(LPCSTR model_or_section, xr_string& out)
	{
		out.clear();
		if (!model_or_section || !*model_or_section)
			return false;
		LPCSTR path = model_or_section;
		if (pSettings->section_exist(model_or_section))
		{
			if (pSettings->line_exist(model_or_section, "visual"))
				path = pSettings->r_string(model_or_section, "visual");
			else if (pSettings->line_exist(model_or_section, "item_visual"))
				path = pSettings->r_string(model_or_section, "item_visual");
			else
				return false;
		}
		string_path buf;
		xr_strcpy(buf, path);
		for (char* c = buf; *c; ++c)
			if (*c == '/')
				*c = '\\';
		const u32 len = xr_strlen(buf);
		if (len > 4 && !stricmp(buf + len - 4, ".ogf"))
			buf[len - 4] = 0;
		string_path file;
		xr_strconcat(file, buf, ".ogf");
		if (!FS.exist("$game_meshes$", file))
			return false;
		out = buf;
		return true;
	}

	const BindBones* model_bones(LPCSTR model_or_section)
	{
		xr_string model;
		if (!resolve_model(model_or_section, model))
		{
			const shared_str key = model_or_section ? model_or_section : "";
			if (g_bind_failed.insert(key).second)
				Msg("! [weapon parts] no model for [%s]: not a mesh path, or a section without visual / item_visual",
					key.c_str());
			return nullptr;
		}
		const shared_str key = model.c_str();
		auto it = g_bind_cache.find(key);
		if (it != g_bind_cache.end())
			return &it->second;
		if (g_bind_failed.find(key) != g_bind_failed.end())
			return nullptr;

		// No HUD flag: the model pool keys on the file name only (see MotionExists, level_script.cpp)
		IRenderVisual* vis = ::Render->model_Create(model.c_str());
		IKinematics* K = vis ? vis->dcast_PKinematics() : nullptr;
		if (!K)
		{
			if (vis)
				::Render->model_Delete(vis);
			g_bind_failed.insert(key);
			Msg("! [weapon parts] [%s] has no skeleton; bind poses need a skinned model", model.c_str());
			return nullptr;
		}

		xr_vector<Fmatrix> matrices;
		K->LL_GetBindTransform(matrices);
		BindBones& bones = g_bind_cache[key];
		bones.reserve(matrices.size());
		for (u16 id = 0; id < u16(matrices.size()); ++id)
		{
			SBindBone b;
			b.name = K->LL_BoneName_dbg(id);
			const u16 parent = K->LL_GetData(id).GetParentID();
			b.parent = parent != BI_NONE ? K->LL_BoneName_dbg(parent) : "";
			b.xform = matrices[id];
			b.pos = b.xform.c;
			b.xform.getHPB(b.hpb);
			b.hpb.mul(180.f / PI);
			bones.push_back(b);
		}
		::Render->model_Delete(vis);
		return &bones;
	}

	const SBindBone* find_bone(const BindBones& bones, LPCSTR name)
	{
		for (const SBindBone& b : bones)
			if (!xr_strcmp(b.name, name))
				return &b;
		return nullptr;
	}
}
