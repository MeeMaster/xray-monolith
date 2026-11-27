////////////////////////////////////////////////////////////////////////////
//	Module 		: weapon_part_tree.cpp
//	Description : Engine-drawn weapon parts (WEAPON_ENGINE_PLAN.md 8.2, change 4b)
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "pch_script.h"
#include "weapon_part_tree.h"
#include "weapon_parts.h"
#include "script_attachment_manager.h"
#include "../xrServerEntities/InstanceRecord.h"
#include "../Include/xrRender/Kinematics.h"

void CWeaponPartTree::clear()
{
	// A child attachment is owned by its parent attachment, so only roots are deleted here
	for (SNode& n : m_nodes)
		if (n.att && !n.parent)
			xr_delete(n.att);
	m_nodes.clear();
}

u32 CWeaponPartTree::drawn() const
{
	u32 n = 0;
	for (const SNode& node : m_nodes)
		if (node.att)
			++n;
	return n;
}

void CWeaponPartTree::build(const CSE_InstanceRecord& record, IKinematics* gun, LPCSTR owner)
{
	clear();
	if (record.is_opaque() || record.parts.empty())
		return;

	auto find = [&](u16 id) -> SNode* {
		for (SNode& n : m_nodes)
			if (n.id == id)
				return &n;
		return nullptr;
	};

	// Parents first, whatever order the record keeps them in
	xr_vector<const SInstancePart*> todo;
	for (const SInstancePart& p : record.parts)
		todo.push_back(&p);
	bool progress = true;
	while (!todo.empty() && progress)
	{
		progress = false;
		for (auto it = todo.begin(); it != todo.end();)
		{
			const SInstancePart& part = **it;
			SNode* parent = part.parent ? find(part.parent) : nullptr;
			if (part.parent && !parent)
			{
				++it;
				continue;
			}
			progress = true;
			it = todo.erase(it);

			SNode node;
			node.id = part.id;
			node.parent = part.parent;
			node.section = part.section;

			weapon_parts::SPlacement pl;
			if (parent && !parent->att)
				node.reason = "its parent part is not drawn";
			else if (!weapon_parts::read_placement(part, pl))
				node.reason = "no placement (bone) yet";
			else
			{
				node.bone = pl.bone;
				LPCSTR model = pl.visual.size() ? pl.visual.c_str()
					: (part.section.size() && pSettings->section_exist(part.section) && pSettings->line_exist(part.section, "visual"))
					? pSettings->r_string(part.section, "visual") : nullptr;
				xr_string resolved;
				if (!model)
					node.reason = "no model: set the visual prop, or give the part section a visual";
				else if (!weapon_parts::resolve_model(model, resolved))
					node.reason = xr_string("model not found: ") + model;
				else if (!weapon_parts::model_bones(resolved.c_str()))
					node.reason = xr_string("model has no skeleton: ") + resolved;
				else
				{
					node.model = resolved.c_str();
					IKinematics* parent_model = parent ? parent->att->Kinematics() : gun;
					if (!parent_model || parent_model->LL_BoneID(pl.bone) == BI_NONE)
						node.reason = xr_string("bone not on the ") + (parent ? "parent part: " : "gun: ") + pl.bone.c_str();
					else
					{
						string32 name;
						xr_sprintf(name, "mw_part:%u", u32(part.id));
						script_attachment* att = xr_new<script_attachment>(name, node.model.c_str());
						att->SetPosition(pl.pos);
						att->SetRotation(pl.hpb);
						if (parent)
						{
							att->SetParent(parent->att);
							att->SetParentBone(pl.bone.c_str()); // looked up on the parent part's model
						}
						node.att = att;
					}
				}
			}
			if (!node.att)
				Msg("~ [weapon parts] %s: part %u [%s] not drawn: %s", owner, u32(part.id), part.section.c_str(), node.reason.c_str());
			m_nodes.push_back(node);
		}
	}
	for (const SInstancePart* p : todo)
		Msg("! [weapon parts] %s: part %u [%s] has a parent that isn't in the record; not drawn", owner, u32(p->id), p->section.c_str());
}

void CWeaponPartTree::render(IKinematics* model, const Fmatrix& xform)
{
	if (m_nodes.empty() || !model)
		return;

	const bool update = m_last_update_frame != Device.dwFrame;
	m_last_update_frame = Device.dwFrame;

	for (SNode& n : m_nodes)
	{
		if (!n.att || n.parent)
			continue;
		const u16 bone = model->LL_BoneID(n.bone);
		if (bone == BI_NONE)
			continue;
		if (update)
			n.att->Update(); // the part's own bones, once per frame (children too)

		// Roots hang from a bone of whichever model this view draws; children follow their parent part
		Fmatrix m;
		m.mul_43(xform, model->LL_GetTransform(bone));
		n.att->UpdateXFORM(nullptr, &m);
		n.att->renderable_Render();
	}
}
