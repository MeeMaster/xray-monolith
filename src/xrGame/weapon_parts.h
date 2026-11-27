////////////////////////////////////////////////////////////////////////////
//	Module 		: weapon_parts.h
//	Description : Part placement data written by the script resolver, and model
//	              bind poses for it (WEAPON_ENGINE_PLAN.md 8.1, change 4a)
////////////////////////////////////////////////////////////////////////////

#pragma once

struct SInstancePart;

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
