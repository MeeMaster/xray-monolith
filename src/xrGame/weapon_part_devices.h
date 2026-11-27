////////////////////////////////////////////////////////////////////////////
//	Module 		: weapon_part_devices.h
//	Description : Lights, lasers and batteries on weapon parts, run by the engine
//	              from section data (WEAPON_ENGINE_PLAN.md 9.1a, change 5d)
////////////////////////////////////////////////////////////////////////////

#pragma once

class CWeapon;
class AttachmentScriptLight;
struct SInstancePart;
class CSE_InstanceRecord;

// Console wpn_part_power: 1 = light and laser parts with needs_power run on their batteries; 0 = always powered
extern int g_wpn_part_power;

// The devices of one gun: every part with the role light or laser, unless its prop script_light = 1 (a script
// drives it through the 4c hooks). On and off is the part prop `on` (saved with the part); an NPC's gun follows
// a rule instead. A part with needs_power = true works while a battery part inside it has charge, and drains it.
class CWeaponPartDevices
{
public:
	~CWeaponPartDevices();

	// After the part tree was built or the parts changed: creates, configures and drops devices
	void sync(CWeapon& w);
	// Every frame while the gun is online: on state, power, battery drain, laser beam and dot
	void update(CWeapon& w);
	bool empty() const { return m_devices.empty(); }

	// Lit now: switched on (prop or NPC rule) and powered. false for a part that isn't a device.
	bool is_lit(u16 id) const;
	bool is_powered(u16 id) const;

	// The part is a light or laser the engine runs
	static bool is_device(const SInstancePart& p);
	// The switch: every device part of the gun on when any is off, else all off. Returns the new state.
	static bool toggle(CWeapon& w);
	static bool set_on(CWeapon& w, u16 id, bool on);
	static bool switched_on(const SInstancePart& p);

private:
	struct SDevice
	{
		u16 id = 0;
		bool light = false;
		bool laser = false;
		bool needs_power = false;
		bool lit = false;              // applied last frame
		bool powered = true;
		AttachmentScriptLight* lamp = nullptr;
		shared_str light_bone;
		shared_str laser_bone;
		xr_vector<shared_str> beam_bones;
		float laser_range = 100.f;
		Fcolor laser_color;
		bool laser_dot = true;
		ref_light dot;
		float drain = 0.f;             // battery charge used and not yet written
	};
	SDevice* find(u16 id);
	const SDevice* find(u16 id) const;
	void release(CWeapon& w, SDevice& d);
	bool owner_rule(CWeapon& w, bool laser); // an NPC's gun: the script rule, else night and combat
	u16 battery_of(const CSE_InstanceRecord& r, u16 id) const;

	xr_vector<SDevice> m_devices;
	u32 m_rule_time = 0;
	int m_rule[2] = { -1, -1 };      // the NPC rule's last answer for lights, lasers; -1: none
};
