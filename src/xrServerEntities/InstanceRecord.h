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

// 3d (WEAPON_ENGINE_PLAN.md 6.3): one script contribution to a numeric config key. Upgrade and part
// contributions are not stored; they are worked out from the upgrades and the parts list.
struct SInstanceModifier
{
	enum : u8 { FLAT = 0, PCT = 1 };

	shared_str source;         // the caller's tag, such as "my_mod:overheat"
	shared_str key;
	u8 mode = FLAT;
	float value = 0.f;

	bool operator==(const SInstanceModifier& o) const
	{
		return source == o.source && key == o.key && mode == o.mode && value == o.value;
	}
};

class CSE_InstanceRecord
{
public:
	// Format version this build writes. Records with a higher version are kept as raw bytes and
	// written back unchanged, so a save made by a newer build survives a round trip here.
	// 1: first format. 2: a flags byte per override (3c); a version 1 record reads every override as config.
	// 3: script modifiers (3d); older records have none.
	enum : u8 { VERSION = 4 };

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
		MAX_MODIFIERS = 256,
		MAX_MODIFIERS_PER_SOURCE = 64,
	};

	InstanceProps overrides;
	u16 next_part_id = 1;
	xr_vector<SInstancePart> parts;
	SInstanceRuntime runtime;
	xr_vector<SInstanceModifier> modifiers;   // script modifiers (3d), in the order they were added

	// 5a: the item's own part-level data, for an item that is a part (a flashlight off the gun). When it is
	// fitted these become the part's props, blob and paintjob; when the part is removed they come back here.
	InstanceProps self_props;
	xr_vector<u8> self_blob;
	shared_str self_paintjob;

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
	// True when both records give the same generated section: the same config overrides in the same order, the
	// same script modifiers, and the same parts that declare stat_mods (3d). State overrides and the rest are ignored.
	bool same_config(const CSE_InstanceRecord& o) const;

	// Script modifiers (3d). mode is SInstanceModifier::FLAT or PCT. Adding again with the same source and key
	// replaces the entry. false when a cap, an invalid value or the opaque state forbids it.
	bool add_modifier(LPCSTR source, LPCSTR key, float value, u8 mode);
	// Removes the entry for that key, or every entry of the source when key is null; returns the count
	u32 remove_modifier(LPCSTR source, LPCSTR key = nullptr);

	// Parts
	SInstancePart* find_part(u16 id);
	const SInstancePart* find_part(u16 id) const;
	u16 add_part(u16 parent, LPCSTR mount, LPCSTR section, float pos = 0.f);   // 0 on failure
	u32 remove_part(u16 id);                                                       // removes its children too; returns the count
	// 5a subtrees. graft: adds every part of src under parent (0 = the item itself), keeping their structure,
	// with new ids; src's top-level parts take mount names and positions as they are. All or nothing: false (and
	// no change) when parent is missing, a cap would be passed, or either record is opaque. first_id: the id src's
	// first top-level part got, 0 when src has no parts.
	bool graft(u16 parent, const CSE_InstanceRecord& src, u16* first_id = nullptr);
	// extract: out becomes a record for part id as an item: its props, blob and paintjob as out's self data and
	// everything mounted on it (recursively) as out's parts, renumbered from 1. Doesn't change this record.
	bool extract(u16 id, CSE_InstanceRecord& out) const;
	// The ids of a part and everything below it, parents before children
	void subtree(u16 id, xr_vector<u16>& out) const;

	// Part props (4a): placement and mod state. A null or empty value clears the key. false when the part is
	// missing, the key is empty, a cap or the opaque state forbids it, or clearing a key that isn't set.
	LPCSTR get_part_prop(u16 id, LPCSTR key) const;
	bool set_part_prop(u16 id, LPCSTR key, LPCSTR value);

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
