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
