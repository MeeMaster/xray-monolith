////////////////////////////////////////////////////////////////////////////
//	Module 		: weapon_part_tree.h
//	Description : The parts of one weapon, drawn by the engine from the placement
//	              the script resolver stores in the record (WEAPON_ENGINE_PLAN.md 8.2, change 4b)
////////////////////////////////////////////////////////////////////////////

#pragma once

class CSE_InstanceRecord;
class script_attachment;
class IKinematics;
class CGameObject;
class AttachmentScriptLight;

// One tree per weapon, built from the record's parts. Each part is a script_attachment owned by the tree (not
// in the object's script attachment list, so scripts can't remove it). The same tree is drawn in the world
// pass (the gun's XFORM) and in the HUD pass (the HUD item transform): the transform is set again right
// before each draw, and the renderer copies it at insert time, so both views in one frame are correct.
class CWeaponPartTree
{
public:
	struct SNode
	{
		u16 id = 0;
		u16 parent = 0;                // part id, 0 = the gun
		shared_str section;
		shared_str model;
		shared_str bone;               // on the parent model
		script_attachment* att = nullptr; // nullptr: not drawn, see reason
		xr_vector<shared_str> hides;   // gun bones hidden while this part is drawn (4c)
		xr_string reason;
	};

	~CWeaponPartTree();

	// Rebuilds every attachment from the record. gun: the gun's kinematics, to check root bones. owner: for log lines.
	// holder: the weapon, whose physics shell takes the parts' collision while the gun lies in the world (4c).
	void build(const CSE_InstanceRecord& record, IKinematics* gun, LPCSTR owner, CGameObject* holder);
	void clear();

	// The holder's physics shell was created (true) or is about to go (false): add or remove the parts' collision
	void shell(bool create);

	// The gun bones that drawn parts hide, sorted, no repeats
	void hidden_bones(xr_vector<shared_str>& out) const;

	// Draws the parts on a model drawn with xform (the world visual, or the shared model in the HUD pass)
	void render(IKinematics* model, const Fmatrix& xform);

	const xr_vector<SNode>& nodes() const { return m_nodes; }
	u32 drawn() const;

	// ---- script hooks (4c, WEAPON_ENGINE_PLAN.md 8.2). Kept per part id across rebuilds, dropped with the tree
	// and when the part leaves the record. Not saved: scripts set them again after a load.
	enum EView { VIEW_BOTH = 0, VIEW_HUD = 1, VIEW_WORLD = 2 };

	// Hides or shows a bone of a part while the gun is in the given view (HUD = first person). false: no such part
	bool set_bone_visible(u16 id, LPCSTR bone, bool visible, EView view);
	// Applies the bone visibility of the current view; cheap when nothing changed
	void apply_view(bool hud);

	// Hands a light to a part. lua_ref is a registry reference that keeps the script's light alive; the tree
	// releases it when the light is detached or replaced, the part goes, or the tree is destroyed.
	bool attach_light(u16 id, AttachmentScriptLight* light, int lua_ref, LPCSTR bone);
	bool detach_light(u16 id);

	// A drawn part's bone (or its origin when bone is empty) in world space, for a gun model drawn with gun_xform
	bool part_xform(u16 id, LPCSTR bone, IKinematics* gun, const Fmatrix& gun_xform, Fmatrix& out) const;

private:
	struct SOverride
	{
		xr_vector<shared_str> hidden[3]; // by EView
		xr_vector<shared_str> touched;   // every bone ever named, shown again when no list hides it
		AttachmentScriptLight* light = nullptr;
		int light_ref = -2;              // LUA_NOREF
		shared_str light_bone;
	};

	const SNode* find(u16 id) const;
	void release_light(SOverride& o);
	void hook_up(SNode& n);               // gives a freshly built part its light

	xr_vector<SNode> m_nodes;      // parents before children
	u32 m_last_update_frame = 0;
	xr_map<u16, SOverride> m_overrides;
	int m_view = -1;               // the view whose bone visibility is applied; -1: apply again
};
