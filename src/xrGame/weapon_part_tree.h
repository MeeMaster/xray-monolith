////////////////////////////////////////////////////////////////////////////
//	Module 		: weapon_part_tree.h
//	Description : The parts of one weapon, drawn by the engine from the placement
//	              the script resolver stores in the record (WEAPON_ENGINE_PLAN.md 8.2, change 4b)
////////////////////////////////////////////////////////////////////////////

#pragma once

class CSE_InstanceRecord;
class script_attachment;
class IKinematics;

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
		xr_string reason;
	};

	~CWeaponPartTree() { clear(); }

	// Rebuilds every attachment from the record. gun: the gun's kinematics, to check root bones. owner: for log lines.
	void build(const CSE_InstanceRecord& record, IKinematics* gun, LPCSTR owner);
	void clear();

	// Draws the parts on a model drawn with xform (the world visual, or the shared model in the HUD pass)
	void render(IKinematics* model, const Fmatrix& xform);

	const xr_vector<SNode>& nodes() const { return m_nodes; }
	u32 drawn() const;

private:
	xr_vector<SNode> m_nodes;      // parents before children
	u32 m_last_update_frame = 0;
};
