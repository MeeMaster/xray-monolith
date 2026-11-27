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
#include "GameObject.h"
#include "script_light_inline.h"
#include "ai_space.h"
#include "script_engine.h"

// The placement props of a part, as one string to compare (4d)
static xr_string placement_key(const CSE_InstanceRecord& r, u16 id)
{
	xr_string k;
	for (LPCSTR key : { "bone", "offset", "hides", "visual" })
	{
		LPCSTR v = r.get_part_prop(id, key);
		k += v ? v : "\x01";
		k += '\x02';
	}
	return k;
}

bool CWeaponPartTree::update_placement(const CSE_InstanceRecord& record, IKinematics* gun)
{
	if (record.is_opaque() || record.parts.size() != m_nodes.size() || m_nodes.empty())
		return false;

	struct SMove
	{
		SNode* node;
		weapon_parts::SPlacement pl;
		xr_string key;
		u16 root_bone;
	};
	xr_vector<SMove> moves;
	for (const SInstancePart& part : record.parts)
	{
		SNode* node = nullptr;
		for (SNode& n : m_nodes)
			if (n.id == part.id)
				node = &n;
		if (!node || node->parent != part.parent || node->section != part.section)
			return false;
		xr_string key = placement_key(record, part.id);
		if (key == node->key)
			continue;
		if (!node->att)
			return false; // an undrawn part may draw now: the full build works that out
		LPCSTR vis = record.get_part_prop(part.id, "visual");
		if (xr_strcmp(vis ? vis : "", node->visual_prop.c_str()))
			return false; // a new model: rebuild

		SMove m;
		m.node = node;
		m.key = key;
		m.root_bone = BI_NONE;
		if (!weapon_parts::read_placement(part, m.pl))
			return false;
		IKinematics* parent_model = gun;
		if (node->parent)
		{
			parent_model = nullptr;
			for (SNode& n : m_nodes)
				if (n.id == node->parent && n.att)
					parent_model = n.att->Kinematics();
		}
		if (!parent_model)
			return false;
		const u16 bid = parent_model->LL_BoneID(m.pl.bone);
		if (bid == BI_NONE)
			return false; // the full build logs why it isn't drawn
		m.root_bone = bid;
		moves.push_back(m);
	}

	for (SMove& m : moves)
	{
		SNode& n = *m.node;
		n.att->SetPosition(m.pl.pos);
		n.att->SetRotation(m.pl.hpb);
		if (n.bone != m.pl.bone)
		{
			if (n.parent)
				n.att->SetParentBone(m.pl.bone.c_str());
			else
				n.att->SetParentBone(m.root_bone);
			n.bone = m.pl.bone;
		}
		n.hides = m.pl.hides;
		n.key = m.key;
	}
	++m_moves;
	return true;
}

CWeaponPartTree::~CWeaponPartTree()
{
	clear();
	for (auto& it : m_overrides)
		release_light(it.second);
}

void CWeaponPartTree::release_light(SOverride& o)
{
	o.light = nullptr;
	if (o.light_ref != LUA_NOREF && o.light_ref != LUA_REFNIL)
		luaL_unref(ai().script_engine().lua(), LUA_REGISTRYINDEX, o.light_ref);
	o.light_ref = LUA_NOREF;
}

const CWeaponPartTree::SNode* CWeaponPartTree::find(u16 id) const
{
	for (const SNode& n : m_nodes)
		if (n.id == id)
			return &n;
	return nullptr;
}

void CWeaponPartTree::hook_up(SNode& n)
{
	auto it = m_overrides.find(n.id);
	if (it == m_overrides.end() || !n.att || !it->second.light)
		return;
	n.att->AttachLight(it->second.light);
	n.att->SetScriptLightBone(it->second.light_bone.size() ? it->second.light_bone.c_str() : "");
}

bool CWeaponPartTree::set_bone_visible(u16 id, LPCSTR bone, bool visible, EView view)
{
	if (!bone || !*bone || !find(id))
		return false;
	SOverride& o = m_overrides[id];
	const shared_str b = bone;
	if (std::find(o.touched.begin(), o.touched.end(), b) == o.touched.end())
		o.touched.push_back(b);
	for (int v = VIEW_BOTH; v <= VIEW_WORLD; ++v)
	{
		if (view != VIEW_BOTH && v != view)
			continue;
		xr_vector<shared_str>& list = o.hidden[v];
		auto at = std::find(list.begin(), list.end(), b);
		if (!visible && at == list.end())
			list.push_back(b);
		else if (visible && at != list.end())
			list.erase(at);
		// "both" covers either view, so showing a bone in one view also lifts a hide set for both
		if (visible && view != VIEW_BOTH)
		{
			xr_vector<shared_str>& both = o.hidden[VIEW_BOTH];
			auto bt = std::find(both.begin(), both.end(), b);
			if (bt != both.end())
			{
				both.erase(bt);
				const int other = view == VIEW_HUD ? VIEW_WORLD : VIEW_HUD;
				if (std::find(o.hidden[other].begin(), o.hidden[other].end(), b) == o.hidden[other].end())
					o.hidden[other].push_back(b);
			}
		}
	}
	m_view = -1;
	return true;
}

void CWeaponPartTree::apply_view(bool hud)
{
	const int view = hud ? VIEW_HUD : VIEW_WORLD;
	if (m_view == view)
		return;
	m_view = view;
	for (SNode& n : m_nodes)
	{
		auto it = m_overrides.find(n.id);
		if (!n.att || it == m_overrides.end())
			continue;
		const SOverride& o = it->second;
		IKinematics* K = n.att->Kinematics();
		auto hidden = [&](const shared_str& b) {
			return std::find(o.hidden[VIEW_BOTH].begin(), o.hidden[VIEW_BOTH].end(), b) != o.hidden[VIEW_BOTH].end()
				|| std::find(o.hidden[view].begin(), o.hidden[view].end(), b) != o.hidden[view].end();
		};
		// Shown first, then hidden, so hiding a parent wins over showing its child
		for (int pass = 0; pass < 2; ++pass)
			for (const shared_str& b : o.touched)
			{
				const bool hide = hidden(b);
				if (hide != (pass == 1))
					continue;
				const u16 bid = K->LL_BoneID(b);
				if (bid != BI_NONE && !!K->LL_GetBoneVisible(bid) == hide)
					K->LL_SetBoneVisible(bid, !hide, TRUE);
			}
		K->CalculateBones_Invalidate();
	}
}

bool CWeaponPartTree::attach_light(u16 id, AttachmentScriptLight* light, int lua_ref, LPCSTR bone)
{
	if (!light || !find(id))
	{
		if (lua_ref != LUA_NOREF && lua_ref != LUA_REFNIL)
			luaL_unref(ai().script_engine().lua(), LUA_REGISTRYINDEX, lua_ref);
		return false;
	}
	detach_light(id);
	SOverride& o = m_overrides[id];
	o.light = light;
	o.light_ref = lua_ref;
	o.light_bone = bone && *bone ? bone : "";
	for (SNode& n : m_nodes)
		if (n.id == id)
			hook_up(n);
	return true;
}

bool CWeaponPartTree::detach_light(u16 id)
{
	auto it = m_overrides.find(id);
	if (it == m_overrides.end() || !it->second.light)
		return false;
	for (SNode& n : m_nodes)
		if (n.id == id && n.att && n.att->GetLight() == it->second.light)
			n.att->DetachLight();
	release_light(it->second);
	return true;
}

bool CWeaponPartTree::part_xform(u16 id, LPCSTR bone, IKinematics* gun, const Fmatrix& gun_xform, Fmatrix& out) const
{
	const SNode* node = find(id);
	if (!node || !node->att || !gun)
		return false;

	// The chain from the top-level part down to this one
	xr_vector<const SNode*> chain;
	for (const SNode* n = node; n; n = n->parent ? find(n->parent) : nullptr)
	{
		if (!n->att)
			return false;
		chain.push_back(n);
	}
	std::reverse(chain.begin(), chain.end());

	Fmatrix m = gun_xform;
	IKinematics* model = gun;
	for (const SNode* n : chain)
	{
		const u16 b = model->LL_BoneID(n->bone);
		if (b == BI_NONE)
			return false;
		m.mulB_43(model->LL_GetTransform(b));
		m.mulB_43(n->att->GetOffset());
		model = n->att->Kinematics();
	}
	if (bone && *bone)
	{
		const u16 b = model->LL_BoneID(bone);
		if (b == BI_NONE)
			return false;
		m.mulB_43(model->LL_GetTransform(b));
	}
	out = m;
	return true;
}

void CWeaponPartTree::clear()
{
	// Lights stay with the tree (m_overrides); the attachments only point at them
	for (SNode& n : m_nodes)
		if (n.att && n.att->GetLight())
			n.att->DetachLight();
	// A child attachment is owned by its parent attachment, so only roots are deleted here
	for (SNode& n : m_nodes)
		if (n.att && !n.parent)
		{
			n.att->OnParentDestroyPhysicsShell(); // its collision leaves the gun's shell (children too)
			xr_delete(n.att);
		}
	m_nodes.clear();
}

void CWeaponPartTree::shell(bool create)
{
	for (SNode& n : m_nodes)
		if (n.att && !n.parent)
		{
			if (create)
				n.att->OnParentCreatePhysicsShell();
			else
				n.att->OnParentDestroyPhysicsShell();
		}
}

void CWeaponPartTree::hidden_bones(xr_vector<shared_str>& out) const
{
	out.clear();
	for (const SNode& n : m_nodes)
		if (n.att)
			for (const shared_str& b : n.hides)
				if (std::find(out.begin(), out.end(), b) == out.end())
					out.push_back(b);
	std::sort(out.begin(), out.end(), [](const shared_str& a, const shared_str& b) { return xr_strcmp(a, b) < 0; });
}

u32 CWeaponPartTree::drawn() const
{
	u32 n = 0;
	for (const SNode& node : m_nodes)
		if (node.att)
			++n;
	return n;
}

// 5a: the model a part section gives a fitted part: part_visual (the part's own model, apart from the item's
// world visual), else visual. null when it has neither.
static LPCSTR section_part_visual(const shared_str& section)
{
	if (!section.size() || !pSettings->section_exist(section))
		return nullptr;
	if (pSettings->line_exist(section, "part_visual"))
		return pSettings->r_string(section, "part_visual");
	if (pSettings->line_exist(section, "visual"))
		return pSettings->r_string(section, "visual");
	return nullptr;
}

void CWeaponPartTree::build(const CSE_InstanceRecord& record, IKinematics* gun, LPCSTR owner, CGameObject* holder)
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

			// 5a: visual = none (the prop) or part_visual = none (the part section) marks a data-only part: never drawn, no log
			LPCSTR vis_prop = record.get_part_prop(part.id, "visual");
			LPCSTR vis_sect = section_part_visual(part.section);
			const bool data_only = !_stricmp(vis_prop ? vis_prop : (vis_sect ? vis_sect : ""), "none");

			weapon_parts::SPlacement pl;
			if (data_only)
				node.reason = "data only (visual = none)";
			else if (parent && !parent->att)
				node.reason = "its parent part is not drawn";
			else if (!weapon_parts::read_placement(part, pl))
				node.reason = "no placement (bone) yet";
			else
			{
				node.bone = pl.bone;
				LPCSTR model = pl.visual.size() ? pl.visual.c_str() : section_part_visual(part.section);
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
						// A part shows its bind pose: the attachment starts the model's "idle", which on a
						// single-model gun is the first-person idle and moves the whole part away from its bone
						att->StopMotions();
						att->SetPosition(pl.pos);
						att->SetRotation(pl.hpb);
						if (parent)
						{
							att->SetParent(parent->att); // takes the holder from its parent
							att->SetParentBone(pl.bone.c_str()); // looked up on the parent part's model
						}
						else
						{
							// Not parented to the object (the tree draws it), but the physics code needs the
							// gun bone and the holder to put its collision on the dropped gun's shell
							att->SetParentBone(parent_model->LL_BoneID(pl.bone));
							att->SetTopLevelParent(holder);
						}
						att->Kinematics()->CalculateBones_Invalidate();
						att->Kinematics()->CalculateBones(TRUE);
						att->SetUsePHShapes(true); // adds the collision now if the gun lies in the world
						node.att = att;
						node.hides = pl.hides;
					}
				}
			}
			node.key = placement_key(record, part.id);
			{
				LPCSTR vis = record.get_part_prop(part.id, "visual");
				node.visual_prop = vis ? vis : "";
			}
			if (!node.att && !data_only)
				Msg("~ [weapon parts] %s: part %u [%s] not drawn: %s", owner, u32(part.id), part.section.c_str(), node.reason.c_str());
			m_nodes.push_back(node);
		}
	}
	for (const SInstancePart* p : todo)
		Msg("! [weapon parts] %s: part %u [%s] has a parent that isn't in the record; not drawn", owner, u32(p->id), p->section.c_str());

	// Script hooks: drop those of parts that left the record, give the rest to the new attachments
	for (auto it = m_overrides.begin(); it != m_overrides.end();)
	{
		bool kept = false;
		for (const SInstancePart& p : record.parts)
			if (p.id == it->first)
				kept = true;
		if (kept)
			++it;
		else
		{
			release_light(it->second);
			it = m_overrides.erase(it);
		}
	}
	for (SNode& n : m_nodes)
		hook_up(n);
	m_view = -1;
	++m_builds;
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
