////////////////////////////////////////////////////////////////////////////
//	Module 		: weapon_part_devices.cpp
//	Description : Lights, lasers and batteries on weapon parts (WEAPON_ENGINE_PLAN.md 9.1a, change 5d)
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "weapon_part_devices.h"
#include "Weapon.h"
#include "weapon_parts.h"
#include "weapon_part_tree.h"
#include "player_hud.h"
#include "Actor.h"
#include "Level.h"
#include "CustomMonster.h"
#include "memory_manager.h"
#include "enemy_manager.h"
#include "script_light_inline.h"
#include "instance_record_script.h"
#include "../xrServerEntities/InstanceRecord.h"
#include "../xrCDB/xr_area.h"

int g_wpn_part_power = 1;

namespace
{
	LPCSTR const PROP_ON = "on";
	LPCSTR const PROP_SCRIPT_LIGHT = "script_light";

	bool prop_true(const SInstancePart& p, LPCSTR key)
	{
		for (const SInstanceProp& pr : p.props)
			if (!xr_strcmp(pr.key, key))
				return pr.value.size() && (pr.value == "1" || !xr_strcmp(pr.value, "true"));
		return false;
	}

	template <class T>
	T sect_read(LPCSTR sect, LPCSTR key, T def, T (CInifile::*fn)(LPCSTR, LPCSTR) const)
	{
		return pSettings->line_exist(sect, key) ? (pSettings->*fn)(sect, key) : def;
	}

	bool sect_bool(LPCSTR sect, LPCSTR key, bool def)
	{
		return pSettings->line_exist(sect, key) ? !!pSettings->r_bool(sect, key) : def;
	}
}

CWeaponPartDevices::~CWeaponPartDevices()
{
	for (SDevice& d : m_devices)
	{
		xr_delete(d.lamp);
		d.dot.destroy();
	}
}

bool CWeaponPartDevices::is_device(const SInstancePart& p)
{
	if (prop_true(p, PROP_SCRIPT_LIGHT))
		return false;
	return weapon_parts::has_role(p, "light") || weapon_parts::has_role(p, "laser");
}

bool CWeaponPartDevices::switched_on(const SInstancePart& p) { return prop_true(p, PROP_ON); }

CWeaponPartDevices::SDevice* CWeaponPartDevices::find(u16 id)
{
	for (SDevice& d : m_devices)
		if (d.id == id)
			return &d;
	return nullptr;
}

const CWeaponPartDevices::SDevice* CWeaponPartDevices::find(u16 id) const
{
	for (const SDevice& d : m_devices)
		if (d.id == id)
			return &d;
	return nullptr;
}

bool CWeaponPartDevices::is_lit(u16 id) const
{
	const SDevice* d = find(id);
	return d && d->lit;
}

bool CWeaponPartDevices::is_powered(u16 id) const
{
	const SDevice* d = find(id);
	return d && d->powered;
}

void CWeaponPartDevices::release(CWeapon& w, SDevice& d)
{
	CWeaponPartTree* t = w.part_tree_rw_nobuild();
	if (d.lamp)
	{
		if (t && t->light_of(d.id) == d.lamp)
			t->detach_light(d.id);
		xr_delete(d.lamp);
	}
	d.dot.destroy();
}

void CWeaponPartDevices::sync(CWeapon& w)
{
	const CSE_InstanceRecord& r = w.instance_record();
	CWeaponPartTree* t = w.part_tree_rw_nobuild();

	// Devices whose part went or stopped being one
	for (u32 i = 0; i < m_devices.size();)
	{
		const SInstancePart* p = r.find_part(m_devices[i].id);
		if (p && is_device(*p))
		{
			++i;
			continue;
		}
		release(w, m_devices[i]);
		m_devices.erase(m_devices.begin() + i);
	}

	for (const SInstancePart& p : r.parts)
	{
		if (!is_device(p) || !p.section.size() || !pSettings->section_exist(p.section))
			continue;
		LPCSTR s = p.section.c_str();
		SDevice* d = find(p.id);
		if (!d)
		{
			m_devices.push_back(SDevice());
			d = &m_devices.back();
			d->id = p.id;
			d->light = weapon_parts::has_role(p, "light");
			d->laser = weapon_parts::has_role(p, "laser");
			d->needs_power = sect_bool(s, "needs_power", false);
			if (d->light)
			{
				d->lamp = xr_new<AttachmentScriptLight>();
				d->lamp->Enable(false);
				d->lamp->SetType(IRender_Light::SPOT);
				d->lamp->SetColor(pSettings->line_exist(s, "light_color") ? pSettings->r_fcolor(s, "light_color") : Fcolor().set(1, 1, 1, 1));
				d->lamp->SetRange(sect_read<float>(s, "light_range", 30.f, &CInifile::r_float));
				d->lamp->SetCone(deg2rad(sect_read<float>(s, "light_cone", 45.f, &CInifile::r_float)));
				if (pSettings->line_exist(s, "light_texture"))
					d->lamp->SetTexture(pSettings->r_string(s, "light_texture"));
				d->lamp->SetShadow(sect_bool(s, "light_shadow", true));
				if (sect_bool(s, "light_volumetric", false))
					d->lamp->SetVolumetric(true);
				d->light_bone = pSettings->line_exist(s, "light_bone") ? pSettings->r_string(s, "light_bone") : "";
			}
			if (d->laser)
			{
				d->laser_bone = pSettings->line_exist(s, "laser_bone") ? pSettings->r_string(s, "laser_bone") : "";
				d->laser_range = sect_read<float>(s, "laser_range", 100.f, &CInifile::r_float);
				d->laser_color = pSettings->line_exist(s, "laser_color") ? pSettings->r_fcolor(s, "laser_color") : Fcolor().set(1, 0, 0, 1);
				d->laser_dot = sect_bool(s, "laser_dot", true);
				if (pSettings->line_exist(s, "laser_beam_bones"))
				{
					LPCSTR list = pSettings->r_string(s, "laser_beam_bones");
					string128 b;
					for (int i = 0, n = _GetItemCount(list); i < n; ++i)
						d->beam_bones.push_back(_Trim(_GetItem(list, i, b)));
				}
				if (d->laser_dot)
				{
					d->dot = ::Render->light_create();
					d->dot->set_type(IRender_Light::POINT);
					d->dot->set_shadow(false);
					d->dot->set_range(.12f);
					d->dot->set_color(d->laser_color);
					d->dot->set_active(false);
				}
			}
			d->lit = true; // forces the first update to apply the state
		}

		// A freshly built tree has no light on the part: give it ours, unless a script gave it one (4c hook)
		if (d->lamp && t && t->has_node(p.id) && !t->light_of(p.id))
			t->attach_light(p.id, d->lamp, -2 /* LUA_NOREF: ours, not the script's */, d->light_bone.c_str());
	}
}

u16 CWeaponPartDevices::battery_of(const CSE_InstanceRecord& r, u16 id) const
{
	// The first charged battery inside the part (at any depth), in record order
	for (const SInstancePart& p : r.parts)
	{
		if (!weapon_parts::has_role(p, "battery") || p.condition == 0)
			continue;
		for (const SInstancePart* q = &p; q && q->parent; q = r.find_part(q->parent))
			if (q->parent == id)
				return p.id;
	}
	return 0;
}

bool CWeaponPartDevices::owner_rule(CWeapon& w, bool laser)
{
	CCustomMonster* npc = smart_cast<CCustomMonster*>(w.H_Parent());
	if (!npc)
		return false;
	if (Device.dwTimeGlobal >= m_rule_time)
	{
		m_rule_time = Device.dwTimeGlobal + 1000;
		const int answer = instance_record_script::run_npc_light_rule(npc->lua_game_object(), w.lua_game_object());
		if (answer >= 0)
			m_rule[0] = m_rule[1] = answer;
		else
		{
			const bool combat = !!npc->memory().enemy().selected();
			const float hour = Level().GetGameDayTimeSec() / 3600.f;
			const bool night = hour >= 21.f || hour < 5.f;
			m_rule[0] = night && combat;
			m_rule[1] = combat;
		}
	}
	return m_rule[laser ? 1 : 0] > 0;
}

void CWeaponPartDevices::update(CWeapon& w)
{
	if (m_devices.empty())
		return;
	const CSE_InstanceRecord& r = w.instance_record();
	CWeaponPartTree* t = w.part_tree_rw_nobuild();
	CObject* parent = w.H_Parent();
	const bool npc_gun = parent && !smart_cast<CActor*>(parent) && smart_cast<CCustomMonster*>(parent);
	const bool hud = parent && smart_cast<CActor*>(parent) && w.GetHUDmode() && w.HudItemData();

	CSE_InstanceRecord* edit = nullptr; // a battery to write
	for (SDevice& d : m_devices)
	{
		const SInstancePart* p = r.find_part(d.id);
		if (!p)
			continue;
		const bool wanted = npc_gun ? owner_rule(w, d.laser && !d.light) : switched_on(*p);

		// Power
		u16 battery = 0;
		d.powered = true;
		if (d.needs_power && g_wpn_part_power)
		{
			battery = battery_of(r, d.id);
			d.powered = battery != 0;
		}
		const bool lit = wanted && d.powered; // dropped guns too: a light left on stays on

		// Drain the battery while lit, in steps of the stored precision (1/255)
		if (lit && battery)
		{
			const SInstancePart* b = r.find_part(battery);
			const float hours = b && pSettings->section_exist(b->section) ?
				sect_read<float>(b->section.c_str(), "battery_capacity", 1.f, &CInifile::r_float) : 1.f;
			d.drain += Device.fTimeDelta / (_max(hours, .001f) * 3600.f);
			if (d.drain >= 1.f / 255.f)
			{
				if (!edit)
					edit = xr_new<CSE_InstanceRecord>(r);
				if (SInstancePart* eb = edit->find_part(battery))
					eb->set_condition(eb->condition_f() - d.drain);
				d.drain = 0.f;
			}
		}

		// Light: on and off; the attachment places it every frame
		if (d.lamp)
		{
			const bool attached = t && t->light_of(d.id) == d.lamp && t->light_attached(d.id);
			d.lamp->Enable(lit && attached);
			if (lit && attached)
				d.lamp->Update();
		}

		// Laser: beam surfaces while lit, and the dot where the beam hits
		if (d.laser)
		{
			if (lit != d.lit && t)
				for (const shared_str& b : d.beam_bones)
					t->set_bone_visible(d.id, b.c_str(), lit, CWeaponPartTree::VIEW_BOTH);
			bool dot_on = false;
			if (lit && d.dot && t)
			{
				IKinematics* model = nullptr;
				const Fmatrix* xform = nullptr;
				if (hud)
				{
					model = w.HudItemData()->m_model;
					xform = &w.HudItemData()->m_item_transform;
				}
				else if (w.Visual())
				{
					model = w.Visual()->dcast_PKinematics();
					xform = &w.XFORM();
				}
				Fmatrix m;
				if (model && t->part_xform(d.id, d.laser_bone.size() ? d.laser_bone.c_str() : nullptr, model, *xform, m))
				{
					Fvector dir = m.k;
					dir.normalize_safe();
					collide::rq_result R;
					if (Level().ObjectSpace.RayPick(m.c, dir, d.laser_range, collide::rqtBoth, R, parent ? parent : &w) && R.range > .05f)
					{
						Fvector at;
						at.mad(m.c, dir, R.range - .03f);
						d.dot->set_position(at);
						dot_on = true;
					}
				}
			}
			if (d.dot)
				d.dot->set_active(dot_on);
		}
		d.lit = lit;
	}
	if (edit)
	{
		w.set_instance_record(*edit, true); // a state change: sent with the next flush
		xr_delete(edit);
	}
}

bool CWeaponPartDevices::set_on(CWeapon& w, u16 id, bool on)
{
	CSE_InstanceRecord r = w.instance_record();
	const SInstancePart* p = r.find_part(id);
	if (!p || !is_device(*p))
		return false;
	if (switched_on(*p) == on)
		return true;
	r.set_part_prop(id, PROP_ON, on ? "1" : nullptr);
	return w.set_instance_record(r, true);
}

bool CWeaponPartDevices::toggle(CWeapon& w)
{
	CSE_InstanceRecord r = w.instance_record();
	bool any = false, any_off = false;
	for (const SInstancePart& p : r.parts)
		if (is_device(p))
		{
			any = true;
			any_off = any_off || !switched_on(p);
		}
	if (!any)
		return false;
	for (const SInstancePart& p : w.instance_record().parts)
		if (is_device(p))
			r.set_part_prop(p.id, PROP_ON, any_off ? "1" : nullptr);
	w.set_instance_record(r, true);
	return any_off;
}
