////////////////////////////////////////////////////////////////////////////
//	Module 		: weapon_parts.h
//	Description : Part placement data written by the script resolver, and model
//	              bind poses for it (WEAPON_ENGINE_PLAN.md 8.1, change 4a)
////////////////////////////////////////////////////////////////////////////

#pragma once

struct SInstancePart;
class CSE_InstanceRecord;

namespace weapon_parts
{
	// Reserved part props. The script resolver writes them; the engine draws the part from them (4b).
	//   bone   = <bone on the parent model>
	//   offset = x, y, z [, h, p, b]     metres; degrees, as script_attachment:set_rotation takes them
	//   hides  = bone, bone, ...         gun bones hidden while the part is fitted
	//   visual = <mesh path>             optional (4b): the model to draw; default the part section's `visual`
	extern LPCSTR const PROP_BONE;
	extern LPCSTR const PROP_VISUAL;
	extern LPCSTR const PROP_OFFSET;
	extern LPCSTR const PROP_HIDES;

	bool is_placement_prop(LPCSTR key);

	// Roles (5b, WEAPON_ENGINE_PLAN.md 9.1): tags saying which engine subsystem a part feeds (sight, muzzle,
	// launcher, ...). A part can have several and several parts can share one. The part's `role` prop replaces
	// its section's `role` key. Lower case, no blanks or repeats.
	extern LPCSTR const PROP_ROLE;
	void part_roles(const SInstancePart& part, xr_vector<shared_str>& out);
	bool has_role(const SInstancePart& part, LPCSTR role);

	// Sight (5b-2, WEAPON_ENGINE_PLAN.md 9.1). The active sight: the state value active_sight when it names a
	// fitted sight, else the sight with the fewest mounts between it and the item, then record order. 0: none.
	u16 active_sight(const CSE_InstanceRecord& record);
	// Where a sight's scope data is: its section's `sight_section`, else its section. Empty when neither exists.
	shared_str sight_data_section(const SInstancePart& part);
	// The data section of the active sight; empty when there is none. A change here changes the item's config.
	shared_str active_sight_data(const CSE_InstanceRecord& record);
	// The generated-section layer for a sight: its scope keys (scope_*, s3ds_*, min_scope_zoom_factor,
	// zoom_step_count, holder_range_modifier, holder_fov_modifier, zoom_enabled, and the names in its
	// `sight_keys`), then scope_status = 1, so the gun reads them as a built-in scope.
	void sight_items(LPCSTR data_section, CInifile::DynamicItems& out);

	// Muzzle (5b-3): every muzzle part, the ones nearest the gun first, so the outermost wins a shared key. Data:
	// its section's `muzzle_section`, else its section. Copied: silencer_* (not the gun's status, name, icon
	// place), snd_silncer_*, flame_particles, smoke_particles, and its `muzzle_keys`. A muzzle part is a
	// silencer when its data has `silencer = true` or is a vanilla silencer item (class W_SILENC): then
	// silencer_status = 1 and silencer_koef_section = the outermost silencer's data.
	void muzzle_parts(const CSE_InstanceRecord& record, xr_vector<const SInstancePart*>& out);
	shared_str muzzle_data_section(const SInstancePart& part);
	bool is_silencer_data(LPCSTR data_section);
	// Launcher (5b-4): the launcher part nearest the gun, then record order. Data: `launcher_section`, else its
	// section. Works only on a gun of a launcher class whose base can have a launcher (grenade_launcher_status
	// 1 or 2: it has the launcher animations). Copied: grenade_vel, grenade_class, grenade_flame_particles,
	// snd_shoot_grenade, launcher_* and its `launcher_keys`; then grenade_launcher_status = 1 and
	// launcher_koef_section.
	u16 active_launcher(const CSE_InstanceRecord& record);
	shared_str launcher_data_section(const SInstancePart& part);
	bool launcher_capable(LPCSTR base);

	// Every role layer of the generated section, in order: sight, muzzle, launcher (5.2 "part role data")
	void role_items(LPCSTR base, const CSE_InstanceRecord& record, CInifile::DynamicItems& out);
	// What the role layers are built from; a change here is a config change. Empty: no role layer.
	shared_str role_signature(const CSE_InstanceRecord& record);

	// Checks a placement prop value and writes its normal form (numbers as %g, names trimmed, no repeats).
	// Other keys pass through unchanged. false with a reason when the value is invalid.
	bool normalise_prop(LPCSTR key, LPCSTR value, xr_string& out, xr_string& error);

	struct SPlacement
	{
		shared_str bone;               // empty: the part has no placement and isn't drawn
		Fvector pos = { 0.f, 0.f, 0.f };
		Fvector hpb = { 0.f, 0.f, 0.f }; // degrees
		xr_vector<shared_str> hides;
		shared_str visual;             // empty: the part section's `visual`
	};
	// Reads the placement props of a part. false when it has no bone (the props are written by the resolver)
	bool read_placement(const SInstancePart& part, SPlacement& out);

	// Bind pose of one bone in model space (composed with its parents), as the model was exported
	struct SBindBone
	{
		shared_str name;
		shared_str parent;             // empty for the root
		Fvector pos;
		Fvector hpb;                   // degrees; set_rotation(hpb) reproduces the bone's rotation
		Fmatrix xform;
	};
	typedef xr_vector<SBindBone> BindBones;

	// A mesh path (as in the LTX: with or without .ogf, either slash) or a section with `visual` or
	// `item_visual`. Writes the mesh path without .ogf. false when there is none or the file doesn't exist.
	bool resolve_model(LPCSTR model_or_section, xr_string& out);

	// Every bone of a model, in bone id order. Cached per model; nullptr when the model can't be loaded or
	// has no skeleton (logged once).
	const BindBones* model_bones(LPCSTR model_or_section);
	const SBindBone* find_bone(const BindBones& bones, LPCSTR name);
}
