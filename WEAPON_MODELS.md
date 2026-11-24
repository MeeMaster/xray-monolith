# Weapon models in xray-monolith: HUD model vs. world model

This note covers how a weapon is drawn in the engine, why every weapon ships two
models (a first-person "HUD" model and a third-person "world" model), and what it
would take to get by with one.

All paths are relative to `xray-monolith/src/`.

---

## 1. TL;DR

* At runtime a weapon is **two independent skeletal model instances**:
  * the **world model** is the `CGameObject` visual (`visual = ...` in the weapon
    section). It is what NPCs hold, what lies on the ground, what the physics shell
    is built from, and what you see in third person.
  * the **HUD model** (`item_visual = ...` in the `hud` section) is owned by an
    `attachable_hud_item`. It is attached to the first-person hands model
    (`player_hud`) and drawn only in the actor's first-person HUD pass.
* They share **no state**. Transform, animation, bone visibility (addons, bullets)
  and fire points are computed separately for each one.
* They are drawn by **different render paths**: different projection (HUD FOV),
  a compressed depth range, separate dsgraph buckets, and, on DX11 with SSS
  (`ssfx_core`), **different compiled shaders** (`model_hud` / `base_hud`).
* The "HUD-ness" of a visual is **baked in when the `.ogf` is first loaded**, and
  the model pool caches visuals **by file name only**. So today one `.ogf` file
  cannot correctly serve as both a HUD and a world model at the same time. That
  is the main *engine-side* reason the two must be separate files. The other
  reasons are about content: animation rigs, polygon and texture budget, and
  authoring frame.
* A single model is achievable. The minimum engine work is to make the HUD
  variant a **per-draw choice** instead of a load-time property (or at least to
  key the pool by `name + hud flag`). After that, one asset can serve both roles
  if it meets the HUD requirements (animated skeleton, `idle` motion, HUD bones).
  LODs keep the cost of the world copy down.

---

## 2. Where each model comes from (config)

Typical weapon (Anomaly-style configs):

```ini
[wpn_ak74]
visual        = dynamics\weapons\wpn_ak74\wpn_ak74.ogf   ; WORLD model  (CGameObject)
hud           = wpn_ak74_hud                             ; -> HUD section
position      = ...                                      ; world grip offset (m_Offset)
orientation   = ...
strap_position / strap_orientation / strap_bone0 / strap_bone1  ; world "on back" pose

[wpn_ak74_hud]
item_visual       = dynamics\weapons\wpn_ak74\wpn_ak74_hud.ogf  ; HUD model
attach_place_idx  = 0                       ; which hand anchor (0 = right/main, 1 = left, 2 = scope slot)
hands_position / hands_orientation (+ _16x9 variants)
item_position / item_orientation            ; HUD item offset relative to hand bone
fire_bone / fire_point / fire_bone2 / fire_point2 / shell_bone / shell_point
anm_idle = <hand motion> [, <item motion>] [, speed]   ; animation aliases
...
```

* `CHudItem::Load` reads `hud_sect = r_string(section, "hud")`. See `xrGame/HudItem.cpp:86`.
* `attachable_hud_item::load` reads `item_visual`, `attach_place_idx`, the
  measures and the motion aliases. See `xrGame/player_hud.cpp:619`.
* `hud_item_measures::load` reads hands/item offsets and fire/shell bones
  **resolved against the HUD model's skeleton**. See `xrGame/player_hud.cpp:391`.
* The world offsets (`position`, `orientation`, `strap_*`) are read in
  `CWeapon` at `xrGame/Weapon.cpp:2404-2434`.

---

## 3. Runtime objects

| | World model | HUD model |
|---|---|---|
| Owner | `CGameObject::Visual()` of the `CWeapon` | `attachable_hud_item::m_model` (`xrGame/player_hud.h`) |
| Created | at spawn, with the game object | lazily, in `CHudItem::HudItemData()` (`xrGame/HudItem.cpp:953`), the first time the item is needed on the HUD |
| Loaded with `Render->hud_loading` | `false` | `true` (`xrGame/player_hud.cpp:625-627`) |
| Parent transform | NPC/actor **body** bones (`g_WeaponBones`) | **hands** model bone (`player_hud::m_ancors`) |
| Animation | normally none (static pose) | full: plays item motion in sync with hand motion |
| Bone visibility | `CWeapon::UpdateAddonsVisibility` | `CWeapon::UpdateHUDAddonsVisibility`, plus bullet bones (`Weapon.cpp:1949-2068`) |
| Fire point / shell point | world bones (`UpdateFireDependencies_internal`, non-HUD branch) | HUD bones (`attachable_hud_item::setup_firedeps`) |
| Physics shell | built from this visual (`CWeapon::create_physic_shell` -> `CPhysicsShellHolder`) | none |
| Script attachments | `eSA_World` | `eSA_HUD` (`attachable_hud_item::update_attachments_xform/render`) |

The two are linked only through `CHudItem`. `CHudItem::GetHUDmode()`
(`xrGame/HudItem.cpp:816`) returns true when the item is held by the actor, the
actor is in first-person view (`HUDview()`), and the item is attached to
`g_player_hud`.

---

## 4. World-model render path

1. `CWeapon::UpdateXForm()` (`xrGame/Weapon.cpp:184`) takes the parent's
   kinematics, gets the left/right weapon bones (`E->g_WeaponBones`), builds a
   basis from the two hand positions, and calls
   `UpdatePosition(mRes)`. That computes `XFORM = mRes * (m_strapped_mode ? m_StrapOffset : m_Offset)`
   (`Weapon.cpp:1560`).
2. `CWeapon::renderable_Render()` -> `CHudItem::renderable_Render()`
   (`xrGame/HudItem.cpp:126`). If we are **not** in the HUD pass, or the
   item is not in HUD mode, the item adds its world visual through the normal
   `CGameObject` path (`on_renderable_Render`). In the HUD pass the world visual
   is skipped.
3. In the renderer it goes into the normal dsgraph (`r__dsgraph_build.cpp`):
   main projection, normal depth range, shadows, wallmarks, LOD selection
   (`rimp_select_sh_dynamic`), SSA culling.

## 5. HUD-model render path

1. `CHUDManager::Render_Last` (`xrGame/HUDManager.cpp:179`) sets
   `::Render->set_HUD(TRUE)` and calls `O->OnHUDDraw()`. That leads to
   `player_hud::render_hud()` (`xrGame/player_hud.cpp:1313`).
2. `render_hud()` adds the **two hands models** (`m_model`, `m_model_2`), then each
   attached item (`attachable_hud_item::render`, `player_hud.cpp:355`) with
   `m_item_transform`, plus HUD script attachments.
3. `m_item_transform` comes from
   `player_hud::calc_transform()` (`player_hud.cpp:2459`):
   `result = hands_transform * hand_anchor_bone_transform * item_attach_offset`.
   The weapon is glued to a bone of the animated **arms** rig, not to the
   body. The scope slot (`SCOPE_ATTACH_IDX`) chains off item 0's transform
   (`player_hud.cpp:254`).
4. Animation is driven through aliases. `attachable_hud_item::anim_play`
   (`player_hud.cpp:662`) plays the hand motion on the arms model **and** the
   matching item motion (`m_additional_name`, or the same name, falling back to
   `idle`) on the HUD weapon model. The engine `R_ASSERT`s that the HUD model is
   an `IKinematicsAnimated` and has at least `idle`.
5. Renderer side, with `val_bHUD == TRUE` (`r__dsgraph_build.cpp:137`), the visual
   goes into `mapHUD`, `mapHUDSorted`, `mapHUDEmissive`, `mapHUDDistort` (or the
   custom-FOV / cam-attached / 3D-scope buckets) instead of the world maps.
6. `r_dsgraph_render_hud()` (`r__dsgraph_render.cpp:637`):
   * swaps in `Device.mProjectHud`, which is built from `psHUD_FOV`
     (`xrEngine/CameraManager.cpp:503`), so the HUD has its **own FOV**;
   * calls `rmNear()`, which squeezes depth into the viewport range
     **[0, 0.02]** (e.g. `xrRenderPC_R4/r4.cpp:908`), so the gun never clips into
     walls;
   * HUD objects are excluded from some world features (R1 shadows in
     `LightShadows.cpp:95`, wallmarks in `WallmarksEngine.cpp:348`).

## 6. Load-time HUD flag, and why one `.ogf` can't be both today

```cpp
// xrRender/FBasicVisual.cpp:48
hud = ::Render->hud_loading;          // stored on the visual

// xrRender/FBasicVisual.cpp:149  (shader creation for that visual)
::Render->hud_loading = hud;
shader.create(*dbg_shader, *dbg_texture);

// xrRender/ResourceManager.cpp:200
if (::Render->hud_loading && RImplementation.o.ssfx_core)
    C.HudElement = true;              // -> blender picks "model_hud"/"base_hud" + s_hud_rain
// ResourceManager.cpp:263: lens shaders on HUD get ps->hud_disabled (excluded from HUD mask)
```

`blender_deffer_model.cpp:119` and `Blender_Model_EbB.cpp:241` (R4) select the
`model_hud` vertex shader and `base_hud` pixel shader when `HudElement` is set.
Those are the SSS HUD shaders (HUD rain droplets, HUD masking, etc.; their sources
live in the shader pack, not in this repo).

Now the model pool (`xrRender/ModelPool.cpp:235`):

```cpp
Base = Instance_Find(low_name);           // keyed ONLY by lower-cased file name
if (!Base) Base = Instance_Load(...);     // reads hud_loading HERE, once
Model = Instance_Duplicate(Base);         // dxRender_Visual::Copy() copies `shader` and `hud`
```

Released instances also go back into `Pool`, keyed by the same name.

**Result:** if `item_visual` and `visual` point to the same file, whichever
is loaded first decides the shaders for **both** uses:

* world first (the usual case, since NPCs spawn carrying the gun): the
  first-person gun gets world shaders. It has no HUD raindrops, its glass uses
  `ssfx_glass` screen-space refraction, and its lens parts are not flagged
  `hud_disabled`;
* HUD first: every NPC copy gets the HUD variants (raindrops meant for the
  first-person gun, no `ssfx_glass` on its glass).

(`level_script.cpp:1775` `MotionExists()` also loads models with
`hud_loading = true`, so even a script probe can "poison" a world model this
way.)

Without SSS (`ssfx_core` off) the shader difference disappears and the pool
issue mostly goes away. The other differences listed below still apply.

---

### 6.1 What the HUD shader variant actually changes

The only differences are SSS features, and only when `ssfx_core` is on.
Without SSS, a HUD visual and a world visual compile to identical shaders.

| Where | HUD visual | World visual |
|---|---|---|
| Opaque deferred pass (`blender_deffer_model.cpp:119`, `Blender_Model_EbB.cpp:241`) | `model_hud` / `base_hud` + `s_hud_rain` texture: raindrops on the player's gun (`ssfx_hud_raindrops`, which is enabled when `deffer_base_hud_bump.ps` exists, `r4.cpp:482`) | `model` / `base` |
| Transparent glass, forward pass (`Blender_Model_EbB.cpp:210`) | plain `model_env_lq` | `ssfx_glass` (screen-space refraction that samples the scene buffer) |
| Lens shaders (`ResourceManager.cpp:263`) | `hud_disabled` (kept out of the HUD mask) | n/a |

None of this needs a separate *asset*. It is a variant choice that the engine
happens to make at load time instead of draw time (see 9.3).

## 7. How the two models differ in practice

> Note: this table describes the *traditional* GSC/vanilla content split. The
> engine itself enforces very little of it. Many weapon mods use the same mesh
> for both roles, which works (see section 10).

| Aspect | HUD model | World model |
|---|---|---|
| Seen from | ~10-60 cm, one at a time, full screen | metres away, many instances (every armed NPC) |
| Polycount / textures | high (often 20k-80k+ tris, 2k-4k textures) | low/mid, often with LODs |
| Skeleton | many animated bones: bolt, mag, trigger, selector, bullets, slide, `wpn_body`, plus addon bones; motions referencing the arms rig | few bones: `wpn_body`, addon bones (`wpn_scope`, `wpn_silencer`, `wpn_launcher`), usually no motions |
| Animations | required (`idle` at minimum, plus every alias item motion) | not used by the engine for weapons in hands |
| Authoring frame | origin and scale chosen to line up with the arms rig and `item_position` | origin chosen for NPC hand bones (`position`/`orientation`), strap pose, ground/physics |
| Hidden geometry | often omits or simplifies surfaces never seen from first person, or exaggerates proportions to look right at HUD FOV | must look right from all angles |
| Collision | none needed | bone shapes used for the physics shell when dropped |
| Shaders | compiled as HUD (DX11 + SSS) | normal world shaders |
| Render pass | HUD projection, near depth range, no world shadows | normal |

## 8. Why they are stored separately

1. **Engine constraint (hard, today):** the HUD/world distinction is a property
   of the loaded visual and its compiled shaders, and the pool shares one base per
   file name (section 6). Two roles need two files.
2. **Animation rig:** the HUD model must be an `IKinematicsAnimated` with motions
   matching the hands' alias table (`anim_play` asserts). Its bone set is driven
   by the first-person animation pipeline. The world model hangs off a body
   skeleton and is never animated.
3. **Performance budget:** the HUD gun is drawn once. World guns are drawn for
   every NPC in view, cast shadows, and are skinned. Shipping the HUD mesh as the
   world mesh multiplies vertex and skinning cost across all NPCs.
4. **Different coordinate frames and tuning:** HUD placement is tuned against
   the arms rig (`hands_position`, `item_position`, `_16x9` variants, aim
   offsets). World placement is tuned against NPC skeletons (`position`,
   `orientation`, `strap_*`). Each config is specific to the bone layout of its
   own model (fire, shell and addon bones are looked up by name on each model).
5. **Art reasons:** HUD meshes are sometimes intentionally "wrong" in world
   space (stretched or shortened for a low HUD FOV, culled back sides), and world
   meshes need collision shapes and LODs.
6. **History:** the GSC SDK/actor-editor pipeline (and the mod ecosystem built
   on it, with thousands of existing assets) assumes `visual` and `item_visual`
   are separate files.

---

## 9. Can the engine be changed so a single model suffices?

Yes. Here are the options, from least to most invasive.

### 9.1 Config-only (no code change, limited)

Set `item_visual` to the world `.ogf`. This works only if the model:

* is a skinned `IKinematicsAnimated` with an `idle` motion (and any item motions
  the aliases reference, or every alias falls back to `idle`);
* contains every bone the `hud` section references (`fire_bone`, `shell_bone`,
  addon bones, bullet bones);
* is tuned via `item_position/orientation` to sit in the hands.

It **still breaks on DX11 + SSS** because of the pool/shader issue in section 6.

### 9.2 Fix 1: key the model pool by role (small, low risk)

Make the HUD and world instances of the same file separate bases:

```cpp
// ModelPool::Create / Instance_Find / Pool / Registry
string_path key;
xr_strcpy(key, low_name);
if (::Render->hud_loading) xr_strcat(key, "#hud");
// look up / insert using `key`; load the file using `low_name`
```

Also apply it to `CModelPool::DeleteInternal`/`Discard` (`ModelPool.cpp:307`,
`:367`), which find the name via `Registry`, and to `Prefetch_One` (`:432`). Cost: geometry is loaded twice for a file used
in both roles (a second VB/IB copy, no worse than today's two files). Benefit:
one asset on disk, correct shaders for both roles, and no other behaviour change.

### 9.3 Fix 2: pick HUD shaders per draw, not per load (cleaner)

Instead of baking `HudElement` into the visual's `shader` at load:

* compile a second `ref_shader shader_hud` on `dxRender_Visual` lazily (same
  `dbg_shader`/`dbg_texture`, `HudElement = true`);
* in `r__dsgraph_build.cpp` (`add_leafs_Dynamic` / `insert_dynamic`) pick
  `pVisual->shader_hud` when `RI.val_bHUD` is true (and `ssfx_core` is on),
  otherwise `pVisual->shader`;
* remove the `hud` field and `hud_loading` from load-time paths (keep it only as
  a hint for prefetching).

With this, one loaded base can be drawn as both HUD and world, sharing geometry
in VRAM. Pool keying is no longer needed, and the `MotionExists` "poisoning"
goes away.

### 9.4 Fix 3: unified asset with LOD, both roles from one file

For a single `.ogf` to be a good world model and a good HUD model:

* **LOD:** use the existing progressive/SWI mesh support (`FProgressive`,
  `CSkeletonX_PM`, `rimp_select_sh_dynamic` + SSA) so NPC copies drop to low detail.
  The HUD copy always renders LOD0. This needs the art pipeline to export LODs for
  skinned weapon meshes, and possibly a forced-LOD0 override when
  `val_bHUD` is set.
* **Skeleton:** world instances can keep the full bone set and simply never play
  motions (cost: per-instance bone matrices, small). Optionally hide HUD-only bones
  on world instances (`LL_SetBoneVisible`) or add a per-instance "static pose"
  flag that skips `CalculateBones` after the first frame.
* **Placement:** keep both config sets (`position/orientation` for the world,
  `item_position/orientation` for the HUD). They are independent of the mesh. A
  convention like a `grip` bone could later replace one of them.
* **Physics:** the unified model needs collision bone shapes (ignored on the HUD).
* **Fallback:** make `item_visual` optional and default it to the weapon's `visual`
  (`attachable_hud_item::load`, `player_hud.cpp:622`), so authors opt in per
  weapon and existing content keeps working.

### 9.5 Things that do *not* need to change

* The HUD projection, the `rmNear` depth range, the HUD dsgraph buckets and
  `player_hud`'s transform chain are **per draw**, keyed on `val_bHUD` and the
  transform passed to `set_Transform`. They work with any visual.
* Separate **instances** (HUD and world) are still desirable even with one asset.
  They have different bone poses, bone-visibility states and callbacks at the
  same time. Instances share the base's geometry, so this costs little.

### 9.6 Recommendation

1. Implement **9.3** (or 9.2 as a quick interim). It removes the only hard engine
   blocker.
2. Add the **`item_visual` -> `visual` fallback** (9.4, last point).
3. Leave the choice to content. Weapons whose world model is good enough (or
   whose HUD model has LODs) can ship one file. Everything existing keeps working.

The trade-off to state clearly: a single model either costs world-render
performance (HUD-detail mesh on every NPC) or needs LOD authoring. It also gives
up the art freedom of a first-person-only mesh. The engine changes are small; the
content work is where the real cost is.

---

## 10. Follow-up clarifications

**Does the world model care about animations?** No. The world model only needs
to be an `IKinematics`. It may carry any number of motions (it can be the HUD
`.ogf` itself), and nothing in `CWeapon` plays cycles on it. The stationary
machine gun (`WeaponStatMgun*.cpp`) is the only weapon class that animates its
own visual. Motions are shared data of the loaded model, so the extra
per-instance cost of the unused motions is negligible. Only the HUD side has hard
requirements: `IKinematicsAnimated`, an `idle` motion, and the bones referenced by
the `hud` section.

**Many mods already use the same model for both roles.** That works, because
placement is entirely config-driven (`position`/`orientation`/`strap_*` versus
`hands_*`/`item_*`). So the "separate assets" in sections 7 and 8 are a content
convention, not an engine requirement. There are two caveats:

* If `visual` and `item_visual` are the **same path**, the load-time HUD flag
  problem in section 6 applies under SSS: whichever role loads first decides the
  shaders for both (usually the world role wins, so the first-person gun loses
  HUD raindrops). Two **copies** of the same mesh under different file names avoid
  this, which may be why some mods ship a duplicate file.
* With a HUD-detail mesh as the world model, NPCs render the full-detail gun.
  That is a performance cost, not a correctness issue.

So for such mods, the only engine change needed to make "one file" fully correct
is 9.3 (or 9.2). Section 11 describes one more requirement: the world copy needs a
correct rest pose.

---

## 11. Part_bench spike 8: why the world copy of an animated mesh comes apart

### What was observed

Part_bench (`D:\Code\Stalker mods\Part_bench\AGENTS.md`, spike 8) used one file,
`wpn_ak74_n_one.ogf`, for both roles. It is the rigid world mesh with its header
type switched from 10 (rigid) to 3 (animated) and the HUD's motion reference
(chunk 0x13) appended. First person was fine. In third person the body and stock
were fine, but the bolt, the magazine and the dovetail mount were out of place.
Calling `play_cycle("idle")` on the dropped gun did not visibly change anything.

### Cause: an animated skeleton with no motion playing is not in its rest pose

The header type selects the skeleton class: type 10 loads as `CKinematics`,
type 3 as `CKinematicsAnimated`. Both compute a bone the same way
(`CKinematics::CLBone`, `Layers/xrRender/SkeletonRigid.cpp:219`):

```cpp
BuildBoneMatrix(bd, bi, parent);                          // local pose -> bi.mTransform
bi.mRenderTransform.mul_43(bi.mTransform, bd->m2b_transform); // matrix used for skinning
```

`m2b_transform` is the inverse of the bone's rest (bind) pose in model space,
built from the OGF's bind data (`CBoneData::CalculateM2B`, `xrEngine/bone.cpp:48`).
Skinning is correct only when `mTransform` equals the bind pose, or an animated
pose based on it.

The two classes differ in `BuildBoneMatrix`:

* **Rigid** (`SkeletonRigid.cpp:204`): `mTransform = parent * bind_transform`.
  Each bone sits at its rest offset from its parent, and the mesh is drawn as
  exported.
* **Animated** (`SkeletonAnimated.cpp:1021` → `LL_BoneMatrixBuild:963`): the local
  transform is the blend of the motions currently playing on that bone. With **no
  blend**, `MixInterlerp` (`AnimationKeyCalculate.h:190`) returns
  `Q = (0,0,0,0), T = (0,0,0)`, which `mk_xform` turns into the **identity**. So
  `mTransform = parent * identity`, and every child bone collapses onto its
  parent's origin.

The bind pose is never used as a fallback. Every sub-mesh skinned to a non-root
bone is therefore drawn displaced by the inverse of its bone's offset from its
parent, and the gun comes apart.

### Matching the observation with the actual files

The bind data in the Part_bench files (chunk 0x10, read with a small parser) is
identical in `wpn_ak74_n_hud_modified.ogf`, `wpn_ak74_n_world_modified.ogf` and
`wpn_ak74_n_one.ogf`. Every bone has zero bind rotation and a non-zero offset:

| Bone | Parent | Bind position (x, y, z) | Displacement with no motion |
|---|---|---|---|
| `wpn_body` | root | 0, 0, 0 | none, so the **body is fine** |
| `zatvor` (bolt) | `wpn_body` | 0.030, 0.050, 0.176 | ~19 cm, mostly backward |
| `magazin` | `wpn_body` | 0.000, -0.043, 0.135 | ~14 cm back and up |
| `bullets` | `magazin` | 0.000, 0.068, -0.009 | adds to the magazine's displacement |
| `wpn_scope` | `wpn_body` | -0.024, 0.086, 0.045 | ~10 cm |
| `muzzle`, `wpn_silencer` | `wpn_body` | ~0, 0.037, 0.635-0.657 | ~64 cm (if anything is weighted to them) |

This explains each part of the report:

* **Body and stock are fine:** they are skinned to `wpn_body`, the root, whose
  bind pose is the identity. Identity-instead-of-bind changes nothing there.
* **Bolt and magazine move:** they are skinned to child bones.
* **The dovetail mount moves:** a script attachment is placed on a bone's
  `mTransform` (`LL_GetTransform`). A child bone's `mTransform` is now
  `wpn_body`'s origin, so the attachment loses its bone's offset. Attachments on
  `wpn_body` itself are unaffected, which is why the stock part was fine.
* **First person is fine:** the HUD instance always has a motion playing.
  `attachable_hud_item::anim_play` falls back to `idle` and asserts it exists
  (`xrGame/player_hud.cpp:662`). The animated poses are based on the bind pose.

Part_bench's spike 6 also used a type-3 HUD mesh as the world `visual` and
reported that it "lined up". That fits the same mechanism if only the body and
stock (both on `wpn_body`) were checked, since those are the parts this bug
leaves alone. The bolt and magazine would have moved there as well.

### Why the physics shell doesn't hide it on a dropped gun

On a dropped gun, `CPHShell::ResetCallbacksRecursive` (`xrPhysics/PHShell.cpp:1036`)
gives `callback_overwrite` only to bones that have their own physics element
(bones with a shape). Shapeless or rigid-joint bones such as the bolt and the
magazine keep going through `BuildBoneMatrix`, so they still get the identity
local pose. The physics shell places the root correctly, and the children still
come apart.

### The `play_cycle("idle")` result is still open

According to the code, a playing cycle should fix the pose. `PlayCycle` adds a
blend, and `CalculateBones` → `OnCalculateBones` → `UpdateTracks`
(`SkeletonAnimated.cpp:1091`) runs on every bone update, world weapons included.
After that, `BuildBoneMatrix` uses the motion's keys instead of the identity. The
note's guess that "nothing updates animations on a world weapon" is therefore not
what the code shows. Things worth checking:

* whether the call reached the world visual of the right object;
* whether the motion's bone part covers the child bones;
* whether a blend was still active when the gun was looked at (log
  `bone_position("zatvor")` before and after the call).

Even if it works, it is a workaround: the pose would have to be re-applied after
every spawn, pickup and drop.

### Fixes

1. **Content (what Part_bench does now):** keep the world file rigid (type 10)
   and generate the HUD file from it (`tools\ogf_hud_from_world.ps1`). This is
   correct and costs nothing at runtime.
2. **Engine (small, removes the problem for every animated model):** when a bone
   has no blends at all, use the rigid path, i.e. the bind pose:

   ```cpp
   // SkeletonAnimated.cpp, CKinematicsAnimated::BuildBoneMatrix
   SKeyTable keys;
   LL_BuldBoneMatrixDequatize(bd, channel_mask, keys);

   bool any_blend = false;
   for (u16 j = 0; j < MAX_CHANNELS; ++j)
       if (keys.chanel_blend_conts[j]) { any_blend = true; break; }

   if (!any_blend)
   {
       CKinematics::BuildBoneMatrix(bd, bi, parent, channel_mask); // parent * bind_transform
       return;
   }
   LL_BoneMatrixBuild(bd->GetSelfID(), bi, parent, keys);
   ```

   Bones driven by physics or by script callbacks set `callback_overwrite` and
   never reach this code, so ragdolls, the HUD root and IK are unaffected. The
   only behaviour that changes is an animated model with nothing playing, whose
   current result (children collapsed onto their parents) is never what was
   intended. This makes an animated OGF usable as a world model with no motion,
   and with the shader fix (9.2/9.3) a single file could serve both roles.

   A further refinement is to substitute the bind key for an **empty channel 0**
   when other channels (for example additive ones) do have blends. This patch
   does not cover that case.
3. **Game-side workaround:** start a looped `idle` on world weapons whose visual
   is animated (on spawn, pickup and drop). It is more code, it costs an
   `UpdateTracks` per weapon, and it depends on the open question above. Fix 2 is
   better.

Even with fix 2, the world file still needs its **bone shapes** (chunk 0x10).
Spike 6 showed that a HUD export without them crashes when dropped
(`CPhysicsShellHolder::correct_spawn_pos`). A single file must therefore be the
world export (shapes included) with the HUD's motion reference added, which is
exactly what `wpn_ak74_n_one.ogf` is.

---

## 12. Target design and how much change it needs

**Target:**
* **One model per gun.** It carries the collision shapes (used only when the gun
  is dropped) and the motions (used in first person, and optionally for NPC
  firing).
* **One model per attachment**, fitted at runtime, with no baked
  gun × attachment combinations.

The effort splits into two parts that can be delivered separately.

### 12.1 Part A: one model per gun. Small: engine changes of a few hundred lines

| # | Change | Where | Size |
|---|---|---|---|
| A1 | Bones with no motion playing fall back to the rest pose (section 11, fix 2) | `SkeletonAnimated.cpp` `BuildBoneMatrix` | ~15 lines |
| A2 | Choose HUD vs. world shader variant per draw (9.3), or key the model cache by role (9.2) as a stopgap | `FBasicVisual`, `ResourceManager`, `r__dsgraph_build.cpp`, `ModelPool.cpp` | 9.2: ~30 lines; 9.3: ~100-150 lines |
| A3 | `item_visual` optional, falling back to the weapon's `visual` | `attachable_hud_item::load` (`player_hud.cpp:619`) | ~5 lines |
| A4 | (Optional) NPC firing animation: on a shot fired outside HUD mode, play a world motion (e.g. `shoot_world`) on the world model if it exists (`ID_Cycle_Safe`) | `CWeaponMagazined` shot path | ~20-30 lines |

Notes:
* **A4:** keep it to the bolt cycling on a shot. Reload and other motions would
  not match the NPC's body animations, which are a separate system. The world
  model only animates while it is drawn, because motions advance only when its
  bones are updated, so this costs nothing for off-screen NPCs.
* **Performance:** NPCs then draw a HUD-detail, skinned mesh. Without LODs on the
  single model (the engine can draw lower-detail skinned meshes, but each gun
  needs them exported), crowded scenes get slower. This is content work, not
  engine work.
* **Content:** each gun is exported once from the world mesh (with shapes), and
  the motion reference is added. Part_bench's `ogf_hud_from_world.ps1` already
  produces this file. A1 is what makes that same file usable as `visual` too.

### 12.2 Part B: runtime attachments. Much larger, mostly because the game logic assumes three addon flags

**What already exists:**
* **`script_attachment`** (`xrGame/script_attachment_manager.*`, ~3k lines): any
  model on any bone of the HUD or world model. It supports nesting, offset,
  rotation, scale, bone hiding, lights and physics. Part_bench's spikes show it
  is enough to *draw* parts on both models.
* **MAS** (`modular_attachments`): one engine-drawn scope on the HUD (attach slot
  2, `attach_base/mount_hud_offset`). It handles only a scope, only in first
  person, and only the scope in the `scopes` list at the current index.
* **The upgrade system** can already change weapon stats from config sections.
  It could be reused as each part's stat effects.

**What is missing.** Size: S ≈ days, M ≈ 1-3 weeks, L ≈ more.

| # | Gap | Today | Needed | Size |
|---|---|---|---|---|
| B1 | **Saving which parts are fitted** | Weapon state saves 3 addon bits + `m_cur_scope` (`CSE_ALifeItemWeapon`) | A list of (mount, part section) per weapon: either in the server object packet (save-format change) or in script storage (Part_bench `mw_state`) | M (engine) / S (script) |
| B2 | **Drawing parts automatically** | Script creates HUD and world attachments and re-creates them on spawn, pickup and NPC equip (Part_bench `mw_render`) | Engine-side: create parts on spawn, follow HUD/world mode, and nest parts on parts | M-L (or keep in script) |
| B3 | **HUD shaders on attachments** | `script_attachment::LoadModel` never sets `hud_loading`, so HUD parts get world shaders under SSS | Solved by A2 (9.3) | — |
| B4 | **Gameplay tied to the 3 flags** | `IsScopeAttached` drives zoom, scope texture, aim and scope zoom factors; `IsSilencerAttached` drives sounds, flame and the silencer fire point; the grenade launcher is a separate class (`CWeaponMagazinedWGrenade`) | Derive these from fitted parts: "has suppressor" from any part with `silencer = true`; zoom from the main sight's section; a launcher part enables GL behaviour on guns whose class supports it | L |
| B5 | **Aim per combination** | Per baked section (`aim_hud_offset_*`), or `hud_adjust` from script | Compute from the mount chain plus the sight's aim bone (Part_bench `mw_aim` does this in Lua; port it or keep it) | M |
| B6 | **Fire point from muzzle parts** | `fire_point` / `fire_point_silencer` from config | Read the fitted muzzle part's `muzzle` bone | S |
| B7 | **Hiding replaced pieces of the base gun** | Bone visibility for three bones (`wpn_scope`, etc.) | A convention: every removable piece on its own bone (iron sights, stock, handguard), hidden while a part occupies that mount. This is content plus a few lines | S engine + content |
| B8 | **Icons** | One icon per baked section (vanilla overlays only scope, silencer and launcher icons) | Overlay N part icons at offsets, or render the icon in 3D (`UI3dStatic` already exists) | M |
| B9 | **Part items and the fit/remove UI** | Addon items attach by drag-and-drop for the 3 types | Each part an inventory item; a fit/remove action (Part_bench's bench) | M (mostly UI/script) |
| B10 | **NPC loadouts** | Baked sections in trade and spawn lists | Pick parts on spawn | S |
| B11 | **Compatibility with the mod ecosystem** | GAMMA/3DSS scripts, trader lists, upgrade trees and ammo scripts key off baked section names | A compatibility layer or migration | L, and the hardest to estimate |

### 12.3 Overall estimate

These are rough figures for one developer who knows the codebase.

* **Part A alone** (A1-A3, plus A4 if wanted): **about 1-2 weeks** including
  testing. It is low-risk and independent of everything else. It would let any
  mod ship one file per gun.
* **Part B, engine-native** (B1-B10 in C++): **about 2-4 months**, mostly B4
  (replacing the three-flag logic), B2 and B8.
* **Part B, hybrid** (keep Part_bench's Lua layer for B1, B2, B5 and B9; add
  small engine hooks for B4, B6 and B7: "set zoom from section", "set
  silencer state", "fire point from attachment bone"): **about 1-2 months**.
  This is the shortest path because Part_bench already has working versions of
  B1, B2, B5 and B7.
* **B11** (existing content) is open-ended. It depends on how many of the pack's
  scripts assume baked sections.

**Suggested order:**
1. A1 + A2 (9.3) + A3: one file per gun, correct shaders on HUD attachments.
2. The engine hooks for B4 (scope zoom and silencer state from parts) and B6. These
   remove the current workarounds in Part_bench (shared `mw_scope_*` items,
   first-index-only scope).
3. Decide whether B1/B2 stay in Lua or move to the engine, based on performance
   with many NPCs.
