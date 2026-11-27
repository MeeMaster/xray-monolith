////////////////////////////////////////////////////////////////////////////
//	Module 		: InstanceRecord.h
//	Description : Per-item configuration record: parts, overrides and runtime
//	              state of one weapon (WEAPON_ENGINE_PLAN.md 4.1, change 2a)
////////////////////////////////////////////////////////////////////////////

#pragma once

class NET_Packet;

struct SInstanceProp
{
	enum : u8
	{
		// 3c (WEAPON_ENGINE_PLAN.md 6.2): a state value. Stored and saved, but never in the generated section,
		// so changing it rebuilds nothing. Only overrides use flags; part props always keep 0.
		STATE = 1 << 0,
	};

	shared_str key;
	shared_str value;
	u8 flags = 0;

	bool is_state_value() const { return (flags & STATE) != 0; }
	bool operator==(const SInstanceProp& o) const { return key == o.key && value == o.value && flags == o.flags; }
};

typedef xr_vector<SInstanceProp> InstanceProps;

// One fitted part. Its id is stable for the part's life on this item and never reused.
struct SInstancePart
{
	u16 id = 0;
	u16 parent = 0;            // 0 = the item itself
	shared_str mount;          // mount name on the parent: "scope", "top"
	float pos = 0.f;           // position along the mount in metres; 0 on a point mount
	InstanceProps props;       // placement and mod state; unknown keys are kept as they are
	shared_str section;        // empty = mount emptied on purpose (Part_bench's `false`)
	u8 condition = 255;        // quantised like m_fCondition in UPDATE packets
	xr_vector<shared_str> upgrades;
	xr_vector<u8> blob;        // part-owned data: battery charge, reticle mark, light on/off
	shared_str paintjob;       // empty = none

	float condition_f() const { return float(condition) / 255.f; }
	void set_condition(float c) { clamp(c, 0.f, 1.f); condition = u8(iFloor(c * 255.f + .5f)); }

	bool operator==(const SInstancePart& o) const;
};

// State a rebuild or a save must carry over: fire mode, grenade launcher mode, and the ammo
// type index of each round in the magazine (bugs 7 and 8 in WEAPON_OBJECTS.md)
struct SInstanceRuntime
{
	bool valid = false;        // false = nothing stored; the item keeps its own state
	u8 fire_mode = 0;
	u8 gl_mode = 0;
	xr_vector<u8> rounds;      // ammo type index per round, first fired first

	bool operator==(const SInstanceRuntime& o) const
	{
		return valid == o.valid && fire_mode == o.fire_mode && gl_mode == o.gl_mode && rounds == o.rounds;
	}
};

class CSE_InstanceRecord
{
public:
	// Format version this build writes. Records with a higher version are kept as raw bytes and
	// written back unchanged, so a save made by a newer build survives a round trip here.
	// 1: first format. 2: a flags byte per override (3c); a version 1 record reads every override as config.
	enum : u8 { VERSION = 2 };

	// Hard caps, so a record can never push a spawn packet near its 16 KB limit
	enum : u32
	{
		MAX_BYTES = 8 * 1024,
		MAX_PARTS = 64,
		MAX_PROPS = 32,          // per part
		MAX_OVERRIDES = 128,
		MAX_UPGRADES = 32,       // per part
		MAX_BLOB = 1024,         // per part
		MAX_ROUNDS = 1024,
		MAX_STRING = 255,        // per key, value or name
	};

	InstanceProps overrides;
	u16 next_part_id = 1;
	xr_vector<SInstancePart> parts;
	SInstanceRuntime runtime;

	bool empty() const;
	void clear();

	// True when this record came from a newer format; the engine must not edit it
	bool is_opaque() const { return !m_opaque.empty(); }

	// Packet form: u8 version (0 = empty record, nothing follows), then u16 body size and the body.
	// read() always consumes the whole frame. A damaged body leaves an empty record and logs it.
	void write(NET_Packet& P) const;
	void read(NET_Packet& P);

	// Size write() would produce
	u32 byte_size() const;

	// Overrides. One namespace: setting a key with the other kind moves it (3c).
	LPCSTR get_override(LPCSTR key) const;
	bool set_override(LPCSTR key, LPCSTR value, bool state = false);   // false when a cap or the opaque state forbids it
	bool clear_override(LPCSTR key);
	bool is_state_override(LPCSTR key) const;
	bool has_config_overrides() const;
	// True when both records give the same generated section: the same config overrides in the same order.
	// State overrides and everything else are ignored.
	bool same_config(const CSE_InstanceRecord& o) const;

	// Parts
	SInstancePart* find_part(u16 id);
	const SInstancePart* find_part(u16 id) const;
	u16 add_part(u16 parent, LPCSTR mount, LPCSTR section, float pos = 0.f);   // 0 on failure
	u32 remove_part(u16 id);                                                       // removes its children too; returns the count

	bool operator==(const CSE_InstanceRecord& o) const;
	bool operator!=(const CSE_InstanceRecord& o) const { return !(*this == o); }

	// Round trip and robustness checks; writes a line per check to the log. True when all pass.
	static bool self_test();

private:
	xr_vector<u8> m_opaque;    // version byte, size and body of a record from a newer format

	void write_body(xr_vector<u8>& out) const;
	bool read_body(const u8* data, u32 size, u8 version);
	bool fits() const;
};
