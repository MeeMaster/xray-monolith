# Modular weapon objects: engine plan

How to change xray-monolith so that every weapon is an independent object: one base model, parts as separate models fitted at runtime, and a per-weapon record that rebuilds the gun on save and load. It builds on [WEAPON_MODELS.md](WEAPON_MODELS.md), [WEAPON_OBJECTS.md](WEAPON_OBJECTS.md) and the Part_bench mod (`../part_bench`, its `AGENTS.md` and `mw_*` scripts).

Engine paths are relative to `xray-monolith/src/`. Status: **plan**. Nothing here is implemented. Sizes are estimates from reading the code.

---

## 1. Decisions

Agreed on 2026-11-24:

| Question | Decision |
|---|---|
| Where per-weapon data lives | A **record on the weapon's server entity**, written to the save. At spawn the engine turns it into a **generated in-memory section** `[<base>_i<id>]:<base>` that the client object loads. An `.ltx` dump exists for debugging only. |
| What a fitted part keeps | **Embedded state**: section, condition, installed upgrades, and a small per-part data blob. Detaching respawns a real item with that state. No entity exists while the part is fitted. |
| Existing content | **Replace everywhere.** Every firearm moves to the new model. Baked combo sections (`wpn_ak74_kobra`), addon flags and `alife_clone_weapon` swaps are mapped onto base + parts, for new spawns and for old saves. |
| Engine vs. Lua | **Engine core, Lua edges.** The engine owns the record, persistence, mount resolving, part drawing, stats, aim and gameplay effects. Lua keeps the bench UI, hand poses and shims for other mods, through new engine APIs. |
| How changes apply | **In-place refresh only** (agreed 2026-11-24). Every part change updates the record, then `ReloadConfig` recomputes the same live object from base + record (section 6). There is no incremental "add onto the live object" path, so the live gun always matches what a reload would rebuild. |

Assumptions made without asking (change them if wrong):

- **Part stats are recomputed, not installed.** A part's stat effects use the existing upgrade property syntax, but they are applied on every rebuild and never added to the weapon's installed-upgrade list. Removing a part is just a rebuild without it. This drops Part_bench's "Apply = clone with a new upgrade list".
- **"One model" means one file and one instance** (revised 2026-11-24). First person and third person draw the same model instance with the same pose, bone visibility, parts and shaders. Only the transform and the HUD pass draw state (FOV, depth range) differ per view (section 7).
- **Part_bench's data format is the starting schema.** Keys such as `mw_mounts`, `mw_defaults`, `mount_type`, `accepts`, `positions`, `blocks`, `long`, `pair`, `canted` and `aim_bone` are kept, so the existing part `.ltx` files and meshes keep working.
- **Single-player only.** Network sync is kept just correct enough for the local client/server pair.
- **Every `CWeapon` gets a record.** Only firearms have mounts. Knives, grenades and binoculars carry an empty record.

---

## 2. Target model

### 2.1 What a weapon is

```
Weapon object (one server entity, one ID for its whole life)
  base section        wpn_ak74                    unchanged, from the .ltx
  instance record     saved with the entity       (section 4)
    parts             mount path -> embedded part
    overrides         key -> value                engine or script keys
  derived at spawn
    generated section [wpn_ak74_i10323]:wpn_ak74  base + part effects + overrides
    live object       CWeapon loaded from the generated section
    models            1 gun model instance, drawn in both views + 1 model per part, nested on mounts
```

Nothing about a weapon's look or behaviour depends on its section name any more, except the base it starts from.

### 2.2 Config schema (data side)

Taken from Part_bench, with engine additions marked **new**.

```ini
[wpn_ak74]
visual        = mw\wpn_ak74            ; the only gun model, HUD and world
hud           = wpn_ak74_hud           ; placement, motions, fire bones; no item_visual
mw_mounts     = scope, stock, muzzle, mag, near_bottom
mw_defaults   = stock:part_stock_ak74, mag:part_mag_ak74_30
mw_sight_line = 0.0, 0.0864            ; iron-sight line in model space (aim maths)
mw_icon_scope = 61, -2                 ; icon anchor per mount

[wpn_ak74_mount_scope]
bone     = wpn_body                    ; or a mnt_* bone in the gun model (section 7, 1f)
offset   = 0,0.035,0.033 | 0,0,0
accepts  = dovetail, picatinny
blocks   = near_top
hides    = iron_sights                 ; new: gun bones hidden while the mount is filled

[mw_part_kobra]
mount_type    = dovetail
visual        = mw\kobra               ; one file; visual_hud kept as an optional override
role          = sight                  ; new: sight | muzzle | launcher | magazine | stock | cosmetic
modifiers     = mw_part_kobra_stats    ; new name for Part_bench's `upgrade`; same property syntax
sight_section = mw_part_kobra          ; zoom, scope texture, 3DSS lens, Mark Switch
inv_item      = mw_part_kobra          ; new: the inventory item it comes from and returns to
```

A part can provide mounts of its own (`mw_mounts` on the part), so the fitted parts form a tree addressed by paths such as `handguard/near_bottom`.

### 2.3 Runtime pipeline

```
save file --> server entity (base section + record)
                  |  spawn, or ReloadConfig after a change
                  v
          resolver: record + mount data --> ordered part nodes, errors
                  |
                  v
          generated section = base + part role data + overrides
                  |
                  v
          CWeapon::Load(generated) -> replay mechanic upgrades -> apply part modifiers
                  -> restore runtime state
                  |
                  v
          part renderer: nodes -> one attachment tree on the gun's single model instance
```

---

## 3. Workstreams

Each workstream below lists what changes, where, what Part_bench already has, and how to know it works. Sizes: **S** = days, **M** = 1–3 weeks, **L** = more.

| # | Workstream | Size | Depends on |
|---|---|---|---|
| W1 | One model instance per gun, one treatment in both views | M | — |
| W2 | Instance record and save format | M | — |
| W3 | Generated sections and base identity | M | W2 |
| W4 | `ReloadConfig`: rebuild the same object | L | W3 |
| W5 | Mount resolver in C++ | M | W2 |
| W6 | Engine-owned part rendering | L | W1, W5 |
| W7 | Gameplay from parts, replacing the three addon flags | L | W4, W5 |
| W8 | Aim from the mount chain | M | W5, W6 |
| W9 | Fit and remove as state changes; inventory | M | W4, W7 |
| W10 | Icons | M | W5 |
| W11 | ALife, NPCs, trade | S–M | W2, W5 |
| W12 | Migration of existing content and saves | L | W2–W9 |
| W13 | Script API, callbacks, debug tools | M | grows with each workstream |

---

## 4. W2: Instance record and save format

**Goal:** a weapon's full configuration survives save/load, going offline and changing owner, on the same entity and ID.

### 4.1 The record

A new struct, `CSE_InstanceRecord`, held by `CSE_ALifeInventoryItem` so other item kinds can use it later:

```
u8   version
u16  override count, then (stringZ key, stringZ value) each
u8   part count, then per part:
       stringZ path          "scope", "handguard/near_bottom"
       stringZ section       "" = emptied on purpose (Part_bench's `false`)
       u8     condition      quantised like m_fCondition in UPDATE packets
       u8     upgrade count, then stringZ ids
       u16    blob size, then bytes   part-owned data: battery charge, reticle mark, light on/off
runtime block (for rebuilds and save):
       u8 fire mode, u8 GL mode, per-round ammo type list
```

Paths that are absent fall back to `mw_defaults`, as in `mw_resolve`. An unmodified gun has an empty record and costs a few bytes.

### 4.2 Writing it into the save

Follow WEAPON_OBJECTS.md §12.5:

- Write the record **at the end of the state block**, after `STATE_Write`, inside the size frame in `CSE_Abstract::Spawn_Write` (`xrServerEntities/xrServer_Object_Base.cpp:198`). Read it in `Spawn_Read` (`:275`) from the remaining bytes when `m_wVersion > 128`.
- Bump `SPAWN_VERSION` to 129 (`xrServerEntities/xrServer_Objects.h:166`).
- Existing fields keep their offsets, so Anomaly's byte-level packet scripts (`utils_stpk`) keep working. Check that they tolerate trailing bytes (open question in WEAPON_OBJECTS.md §12.11).
- Keep the record well under the 16 KB packet limit. Cap the part count and blob sizes and refuse writes that exceed them.

### 4.3 Client and server copies

- The client `CWeapon` holds a copy (`m_instance`). Every change goes client to server through a new event `GE_INSTANCE_RECORD` carrying the whole record, handled next to `GE_INSTALL_UPGRADE` in `xrGame/xrServer_process_event.cpp`.
- `net_Spawn` reads the record from the server entity before `Load` (section 5). It never lives in client data, which `switch_offline` wipes.
- `alife():clone_weapon` copies the record, for as long as anything still clones.

### 4.4 Bugs fixed along the way

From WEAPON_OBJECTS.md §10:

- **Bugs 2 and 5** (scope index lost offline, index 0 overloaded) disappear. The record stores the sight's section, not an index.
- **Bugs 7 and 8** (mixed magazines collapse; fire mode, GL mode and shotgun ammo not in STATE) are fixed by the record's runtime block.

**Done when:** a gun with parts survives save/load, an NPC carrying it going offline and back, and a level change, with the same ID. An old save loads with empty records.

---

## 5. W3: Generated sections and base identity

**Goal:** every existing `pSettings->r_*(section, key)` read in the weapon code sees the instance's values, with no per-field refactor.

### 5.1 Storage

`CInifile` keeps sections by value in one sorted vector (`xrCore/xr_ini.h:76`, `typedef xr_vector<Sect> Root`). Inserting at runtime can move every section. So:

- Add a **dynamic section store** to `CInifile`: node-based (pointer-stable), behind a lock, consulted by `r_section`, `section_exist`, `line_exist` and the `r_*` readers after the main lookup misses.
- `add_dynamic_section(name, base, items)` builds a **flattened** section (base items copied, then overridden). Inheritance is resolved at build time, as the loader already does for static sections.
- `remove_dynamic_section(name)` on going offline or on destroy.
- Only names with the reserved suffix (`_i<id>`) may be dynamic, so a typo can never shadow a real section.

### 5.2 What goes into a generated section

Built in this order, later layers overriding earlier ones:

1. The base section.
2. **Role data from parts** (section 9): the main sight's zoom and scope keys, the muzzle's silencer flags, fire point and sounds, the launcher's GL keys.
3. **Overrides** from the record.

Part **modifiers** are not written here. They are applied after `Load` through the upgrade handlers (section 6.2), so additive keys add instead of overwrite.

A matching **generated HUD section** `[<hud>_i<id>]` carries the computed aim offsets (W8) and, later, hand-pose animation keys if poses ever move into the engine.

### 5.3 Identity

- `cNameSect()` / `section()` keep returning the **base** section. Trade lists, ammo checks, repair kits, upgrade roots and most scripts key on it.
- `instance_section()` (new) returns the generated name. `CWeapon::Load` and `reload` receive it instead of the base.
- The upgrade manager keeps looking up roots by base section (`xrGame/inventory_upgrade_manager.cpp:304`, `get_root(item.m_section_id)`).
- `GetScopeName()` and the other functions that re-read the INI at runtime (WEAPON_OBJECTS.md §12.1, point 4) read the generated section.

**Done when:** a weapon whose record overrides `rpm` fires at the overridden rate after save/load, and `section()` still returns the base.

---

## 6. W4: `ReloadConfig`, rebuilding the same object

**Goal:** fitting or removing a part changes the live gun in place: same ID, same inventory slot, no release and respawn.

Follow WEAPON_OBJECTS.md §12.7. In short:

1. Run only while the gun is idle or hidden. Otherwise queue the request until it is.
2. Save the runtime state: ammo count and per-round types, ammo type, fire mode, GL mode and grenades, condition, weapon state, zoom.
3. Make `Load` safe to re-run: clear `m_ammoTypes`, `m_scopes`, `m_aFireModes`, `bullets_bones`, `m_ammoTypes2`, sound collections and `m_UIScope` before reloading.
4. Rebuild the generated section (W3) and run `Load` + `reload` on it.
5. Replay mechanic upgrades (`net_Spawn_install_upgrades`), then apply part modifiers (6.2).
6. Restore the runtime state, clamped to the new config. If the magazine got smaller or the calibre changed, unload the extra rounds into the owner's inventory.
7. `InitAddons()` and bone visibility, now driven by parts (W6, W7).
8. Rebuild the HUD binding if the HUD section changed: delete `m_attachable`, then `player_hud::attach_item` again while the gun is shown.
9. Rebuild the part attachments (W6).

### 6.1 Classes to touch

18 classes override `Load`. The real work is in `CWeapon`, `CWeaponMagazined`, `CWeaponMagazinedWGrenade`, `CWeaponShotgun`/`CWeaponAutomaticShotgun`, `CWeaponStatMgun` and `CHudItem`. About 300–500 lines.

### 6.2 Part modifiers

Each part's `modifiers` sections are applied with the existing `install_upgrade_impl` chain (`xrGame/WeaponUpgrade.cpp:23`), with `loading = true`, but **not** added to `m_upgrades`. Part_bench's `upgrade =` sections work unchanged. While here, fix bug 1 (`hit_power_critical` writes zeros, `WeaponUpgrade.cpp:180`) and bug 4 (`scopes_sect` appended without a clear).

### 6.3 Bench preview

The bench previews a configuration by applying a **preview record** to the gun through `ReloadConfig` while the bench holds it (the gun is not in hands), and reverting on cancel. The bench then reads stats from the real object, so they always match what Apply gives. This replaces Part_bench's Lua stat prediction.

**Done when:** fitting a suppressor and then removing it leaves the gun's ID, slot, ammo and upgrades unchanged, and its stats equal a fresh gun's.

---

## 7. W1: One model instance, one treatment

**Goal:** a weapon has one model instance. First person and third person draw that same instance, with the same animated pose, bone visibility, parts and shaders. The only per-view differences are the transform and the HUD pass draw state.

Revised 2026-11-24. The earlier version kept two instances and compiled HUD shader variants per draw. Reading the code shows that neither is needed.

### 7.1 What separates the two views today

| Difference | Where | Needed? |
|---|---|---|
| Two instances: the world visual, plus a second copy that `attachable_hud_item::load` creates from `item_visual` | `xrGame/player_hud.cpp:619` | **No.** Share one instance (7.3) |
| The world copy comes apart when the model is animated | `CKinematicsAnimated::LL_BoneMatrixBuild` | **No.** It is a bug with a small fix (7.2) |
| HUD shader variants, baked in when the model loads | `ResourceManager.cpp:200` and `:263`, via `hud_loading` | **No.** Drop them for weapons and parts (7.4) |
| Two bone-visibility functions, one per copy | `CWeapon::UpdateAddonsVisibility` / `UpdateHUDAddonsVisibility` (`Weapon.cpp:2010`, `:2071`) | **No.** Merge into one |
| Two attachment trees (`eSA_HUD` and `eSA_World`) | `script_attachment` | **No.** One tree that follows the view (W6) |
| Transform: hand anchors of the arms rig in first person, NPC or actor body bones in third person | `player_hud::calc_transform`, `CWeapon::UpdateXForm` | **Yes.** A gun has to sit in the hands it is drawn with |
| HUD pass: its own FOV, depth squeezed to the near range, HUD buckets | `r_dsgraph_render_hud`, `rmNear` | **Yes.** Without it the held gun clips into walls and changes size with the world FOV. It is draw state, not a shader |

So the world depiction can be handled exactly like the HUD one. Only "where" and "in which pass" differ.

### 7.2 Why child bones collapse, and the fix

The pose of an animated model is built per bone from the motions playing on it. `CKinematicsAnimated::LL_BoneMatrixBuild` (`Layers/xrRender/SkeletonAnimated.cpp`) always processes channel 0, even when it has no blends:

```cpp
for (u16 j = 0; MAX_CHANNELS > j; ++j)
{
    if (j != 0 && keys.chanel_blend_conts[j] == 0)
        continue;                       // channel 0 is never skipped
    ...
}
MixChannels(Result, ...);               // with no blends: Q = (0,0,0,0), T = (0,0,0)
RES.mk_xform(Result.Q, Result.T);       // identity
bi.mTransform.mul_43(*parent, RES);     // child bone lands on its parent's origin
```

With nothing playing, every bone gets the identity instead of its offset from its parent, so the bolt, magazine and anything mounted on them collapse onto `wpn_body`. The first-person copy never shows this, because `attachable_hud_item::anim_play` always plays a cycle and falls back to `idle`. A rigid model never shows it, because `CKinematics::BuildBoneMatrix` uses the bone's bind transform.

**Fix (1a):** when channel 0 has no blends, use the bone's bind transform for that channel, exactly as the rigid path does. When other channels (additive ones) have blends, mix them on top of the bind key instead of the identity. About 15–30 lines. Bones driven by physics or callbacks set `callback_overwrite` and never reach this code, so ragdolls and IK are unaffected.

After 1a, an animated gun model with no motion playing shows its parts in their relative positions, on NPCs, on the ground and on the back. With the shared instance (7.3), the actor's gun in third person even shows the same animation as in first person.

### 7.3 One instance for both views

- **No second model.** `attachable_hud_item::load` stops creating a model. It takes the owner's world visual as `m_model`, without owning it: the destructor (`player_hud.cpp:610`) and the script-animation cleanup must not delete it. `item_visual` is no longer read.
- **Drawing it twice in a frame.** In first person the world copy is still drawn in the actor-shadow phase (`CActor::renderable_Render`, `Actor.cpp:2249`), and the HUD pass draws it again with the HUD transform. The renderer skips a visual already added in the current pass unless its skip marker is set (`r__dsgraph_build.cpp:57`). `script_attachment::LoadModel` already sets this marker for the same reason (`script_attachment_manager.cpp:1686`). Set it on every weapon visual.
- **One pose per frame.** Bones are calculated once per frame and are relative to the model, so the shadow and the HUD pass share one pose. That also halves the bone work for the actor's gun.
- **The root bone.** `anim_play` pins the gun's root bone to the identity with `callback_overwrite` (`player_hud.cpp:694`). On a shared instance this also applies to the world draw. That is harmless while held, because Part_bench's gun meshes have an identity `wpn_body` bind pose. When the gun is dropped, clear the override before the physics shell installs its own callbacks, and set it again on pickup.
- **Bone visibility.** Merge `UpdateAddonsVisibility` and `UpdateHUDAddonsVisibility` into one function on one instance. Bullet bones then show the rounds in the magazine in third person too.
- **Fire and shell points.** The bones are the same. `setup_firedeps` keeps choosing the transform by view: the HUD transform in first person, `XFORM()` otherwise.
- **NPC guns** have no HUD binding. They show the bind pose (1a), or a short motion on shots (1g).
- **Physics.** The single model is the world export with bone shapes, so dropping works. The HUD view ignores the shapes.

### 7.4 One shader set

The load-time HUD flag (`hud_loading`) has exactly two effects, both in `Layers/xrRender/ResourceManager.cpp`, and both only matter with Screen Space Shaders (SSS) on:

- `:200` sets `HudElement`, which compiles `model_hud` / `base_hud` with HUD raindrops, and plain glass instead of `ssfx_glass`.
- `:263` marks lens shaders `hud_disabled`, which keeps them out of the HUD mask.

The `hud` field on a visual (`FBasicVisual.cpp:48`, `:149`) only re-applies that flag when shaders are created. Nothing else reads it. So:

- **Weapons and parts stop loading with `hud_loading`.** With one instance there is no separate HUD load anyway. `MotionExists` in `level_script.cpp:1775` also stops poisoning the cache, and the model pool problem in WEAPON_MODELS.md §6 goes away with no pool change.
- **The lens exclusion moves to draw time.** In the HUD mask pass, skip shaders flagged as lenses when they are drawn, instead of compiling a separate pass for them.
- **The hands model keeps its HUD shaders.** It is never drawn in the world. This is optional and can be dropped too, for uniformity.

What this costs, with SSS on only:

| Lost or changed | Impact |
|---|---|
| Raindrops on the held gun | Cosmetic. Can come back later through a per-draw "is HUD" shader constant, which needs a change in the SSS shader pack, outside this repo |
| The held gun's glass uses `ssfx_glass` refraction | Needs a visual test in the HUD pass, because HUD depth is squeezed into the near range. If it looks wrong, skip refraction per draw in the HUD pass |

Without SSS, nothing changes visually.

### 7.5 Changes

| # | Change | Where | Size |
|---|---|---|---|
| 1a | Bind-pose fallback for bones with no blend | `Layers/xrRender/SkeletonAnimated.cpp`, `LL_BoneMatrixBuild` | S |
| 1b | One shared instance; `item_visual` retired; skip marker; root override cleared on drop | `xrGame/player_hud.cpp` (`attachable_hud_item::load`, destructor, `anim_play`), `HudItem.cpp`, `Weapon.cpp` drop and pickup | M |
| 1c | No `hud_loading` for weapons and parts; lens mask exclusion at draw time | `player_hud.cpp`, `level_script.cpp:1775`, `ResourceManager.cpp:263`, the HUD mask pass | S |
| 1d | One bone-visibility function | `Weapon.cpp:2010`, `:2071` | S |
| 1e | Force LOD0 in the HUD pass; world draws use progressive LOD | `r__dsgraph_build.cpp`, `rimp_select_sh_dynamic` | S |
| 1f | Optional: tolerate bones that a motion file doesn't animate, so gun models can carry `mnt_*` bones (Part_bench spike 6 crashed on this) | `xrEngine/SkeletonMotions.cpp:120-150` and motion loading in `CKinematicsAnimated` | M, spike first |
| 1g | Optional: a short world motion (bolt cycle) on NPC shots if the model has it | `CWeaponMagazined` shot path | S |

Notes:

- **Do 1a first.** It is small, it can ship on its own, and it is what makes a single animated file usable in the world at all.
- **1f changes a mesh convention.** Part_bench placed gun mounts as LTX offsets because a new bone broke the shared motion file. If 1f works, mounts can be bones in the gun model, which is easier to author. Both forms stay supported. Release builds skip the bone-count check (it is a `VERIFY3`), so the crash likely comes from later motion indexing. Spike it before committing.
- **Content requirement.** Each gun model is one animated file with bone shapes (for dropping), the motion reference (for first person), and ideally LODs. Part_bench's `wpn_ak74_n_one.ogf` from spike 8 is exactly this. `tools\ogf_hud_from_world.ps1` makes it from a world export.
- **What the HUD view still needs from config:** the `hud` section's hand and item placement, motions and fire bones. Those describe placement in the hands, not a separate model.

**Done when:** `wpn_ak74_n_one.ogf` from Part_bench spike 8 is the only model of its gun, and it looks right in first person, in third person (bolt and magazine in place), dropped, and in an NPC's hands. Test with SSS on and off. In first person with actor shadows on, the shadow and the held gun show the same pose.

---

## 8. W5 + W6: Mounts and part rendering in the engine

### 8.1 W5: Resolver

Port `mw_resolve.script` to C++ as `CWeaponPartResolver`. Input: base section plus record. Output: an ordered node list (path, part, parent, transform for HUD and world, role) and a list of errors.

Keep Part_bench's rules exactly, and port `mw_resolve_test.script` as a test suite run from a console command:

- `accepts` by `mount_type`; `positions` with `*`; `blocks` on mounts and parts; the earlier mount wins a clash
- defaults from `mw_defaults`, explicit empties
- `long` / `far_*`, `pair` / `pair_only`, `canted` and `canted_on_canted`
- `align_bone` and `fix`

**Bind poses:** `mw_ogf.script` parses OGF files to read bone bind poses. The engine already loads the model, so it reads them from the kinematics' bone data. The Lua OGF parser, and its guessed rotation order, go away.

### 8.2 W6: Rendering

The weapon owns **one part tree**, built from the resolver's nodes on its single model instance (W1). It exists while the gun is online, whoever holds it and in whichever view.

Implementation:

- Reuse `script_attachment` (`xrGame/script_attachment_manager.*`) for each node. It already nests, follows animated bones, carries physics geometry and lights. Add an **engine-owned** flag and a reserved name prefix so scripts can read these attachments but not remove them.
- **A view-following attachment type.** Today an attachment is either `eSA_HUD` or `eSA_World`. Add a third type that is drawn in whichever pass its parent is drawn in, with the parent's transform for that pass: from `attachable_hud_item::render` with the HUD transform, and from the world render with `XFORM()`. Its transform is recomputed before each draw, because the shadow phase and the HUD pass use different parent transforms in the same frame. Its visual gets the skip marker, like the gun's.
- **Parts use world shaders too** (W1, 7.4). They load without `hud_loading`, so the existing problem of HUD parts getting the wrong shader variant disappears.
- Build the trees in `net_Spawn`, in `ReloadConfig`, and when the HUD item is attached. This replaces Part_bench's `mw_render` diffing, its one-frame-late redraw from `game_object_on_net_spawn`, and its active-item polling.
- **Nested mounts on part bones.** Part_bench spike 2b found that a nested attachment's parent bone was looked up on the gun. The current source looks it up on the parent attachment (`script_attachment::SetParentBone`, `xrGame/script_attachment_manager.cpp:1551`). Re-test on the current build. If it works, part mounts use the part's bones directly and the offset workaround goes away.
- **Bone hiding on the gun:** while a mount holds a part, hide the gun bones in its `hides` list. This generalises Part_bench's `iron_sights` rule and replaces the three addon bones (`wpn_scope`, `wpn_silencer`, `wpn_launcher`).
- **Lens bones:** the engine handles `lens_show_bone` / `lens_hide_bone` per view and aim state, as Part_bench's `mw_aim` does today.
- **Physics:** a dropped gun's part attachments add collision geometry through the existing attachment physics, so a long scope doesn't sink into the ground.
- **Lights and lasers:** the engine creates the part's `attachment_script_light` from its `light` section. With one tree, a part has one light. It is placed from the HUD transform in first person and from the world transform otherwise, which replaces `mw_light`'s pair of HUD and world lights. The laser beam's `models_laserbeam` surfaces draw only in the HUD pass today; check that this still holds once parts use world shaders. Switching on and off and batteries stay in Lua, through an engine on/off call and the part's data blob.
- **Performance:** NPC guns now draw 1 + N models. Measure early (section 16). If needed, skip cosmetic parts beyond a distance, and later merge a gun's static parts into one draw.

**Done when:** Part_bench's spike 9 checklist passes with no `mw_render` script loaded: one tree of parts showing in first and third person, nested parts, dropped guns, NPC guns, offline and back, save/load.

---

## 9. W7 + W8: Gameplay from parts

### 9.1 W7: Replace the three addon flags

Today the engine knows only scope, silencer and grenade launcher bits (`m_flagsAddOnState`). Each part gets a `role`, and each role feeds one hook:

| Role | Feeds | Replaces |
|---|---|---|
| `sight` | Main sight (fewest mounts deep, then mount order): `scope_zoom_factor`, dynamic zoom, `scope_texture`, night vision, alive detector, 3DSS `s3ds_*` and Mark Switch keys via `sight_section`. Second sight: alternative aim and zoom | `m_scopes`, `m_cur_scope`, `scopes_sect`, modular scope groups, Part_bench's shared `mw_scope_*` items and `mw_scope.script` |
| `muzzle` | Silencer state, sounds, flame and smoke particles, light, `fire_point` from the part's `muzzle` bone | `silencer_name`, `fire_point_silencer`, `LoadSilencerKoeffs` |
| `launcher` | GL mode, its ammo and sounds | `grenade_launcher_name` and the GL flag |
| `magazine` | `ammo_mag_size` and allowed ammo, through modifiers | — |
| `stock`, `grip`, `handguard`, `cosmetic` | Stats through modifiers; hand pose in Lua | — |

Rules:

- **Keep the old queries working.** `IsScopeAttached()`, `IsSilencerAttached()` and `IsGrenadeLauncherAttached()` become "a part with that role is fitted". `m_addon_flags` stays in the server entity and the UPDATE packet, derived from parts, so scripts and other mods that read it see the right bits.
- **The weapon class is fixed per base section.** A gun that can take a launcher must use the GL class (`CWeaponMagazinedWGrenade`) in its base section. The launcher part only enables what the class supports. Check this for every base during migration.
- **`scope_status`, `silencer_status`, `grenade_launcher_status`** are derived from whether the gun has a mount for that role. "Permanent" becomes a default part that cannot be removed (`fixed = true`, new part key).
- **Bug 3** (modular scope koeffs stale after load) disappears, since koeffs come from the generated section on every rebuild.

### 9.2 W8: Aim from the mount chain

Port `mw_aim.script` into the generated HUD section:

- Aim position = section aim + `mw_sight_line` − the sight's aim bone on the gun, followed through the mount chain, plus `aim_adjust`.
- Canted sights roll the gun about the eye. The aim that centres the sight's point *p* is `-R (p - line - base)`, with the roll *R* read from the mount chain.
- Alternative aim comes from the second sight, or from the main sight's `secondary_aim` bone.
- The result is computed at rebuild time and written as `aim_hud_offset_pos/rot` (and the alt keys) into the generated HUD section. No `hud_adjust` claim is needed, so 3DSS and other users of `hud_adjust_owner` no longer compete with it.

**Done when:** every Part_bench sight lines up on the AK-74N and M4 test guns without a Lua aim script, including canted and alternative aim.

---

## 10. W9 + W10: Fitting, inventory and icons

### 10.1 W9: Fit and remove

- **Part items.** A part item is any inventory item whose section has `mount_type`, or that a part names in `inv_item`.
- **Fit:** `CWeapon::FitPart(path, item)` validates through the resolver, moves the item's condition, upgrades and per-item data into the record, destroys the item entity, and runs `ReloadConfig`.
- **Remove:** `CWeapon::RemovePart(path, spawn)` spawns an item of the part's section in the owner's inventory with its saved state, then rebuilds. Parts mounted on the removed part come off too, each returned as its own item.
- **Drag and drop.** Generalise `CWeaponMagazined::CanAttach` / `Attach` / `Detach` (`xrGame/WeaponMagazined.cpp:1287`) to try a part fit: the first free compatible mount, else refuse with the resolver's reason. The context menu lists fitted parts for removal.
- **The bench** (Part_bench UI, Lua) calls the same functions, plus the preview record from 6.3.
- **Mechanic upgrades** no longer unload and detach everything (`pre_install_upgrade`, `xrGame/inventory_item_upgrade.cpp:260`). Parts stay fitted, and the rebuild applies the new upgrade.

### 10.2 W10: Icons

- Extend the weapon cell item (`CUIWeaponCellItem`) from three fixed addon layers to one layer per fitted part: the part's icon drawn at the gun's `mw_icon_<mount>` anchor minus the part's `icon_anchor`.
- Base icons must show the stripped gun. Part_bench's plan (render the stripped gun and every part from Blender with one fixed camera per gun) produces matching anchors.
- Later option: render the icon from the 3D model at runtime (`UI3dStatic` exists), which removes anchors entirely.

---

## 11. W11: ALife, NPCs and trade

- **Loadouts:** when spawning a gun for an NPC or a stash, roll parts per mount from loadout data, instead of picking a combo section.
- **Offline weapons** are covered already: the record is on the server entity.
- **Value and weight:** base plus fitted parts (part item cost and weight, cost scaled by condition). Traders buy and sell the assembled gun.
- **Death and loot:** the gun drops with its parts.

---

## 12. W12: Migrating existing content and saves

This is the largest risk, because GAMMA, 3DSS and many scripts treat combo section names as the weapon's identity.

### 12.1 Combo map

At startup, build a table **combo section → base section + parts**:

- **Automatic:** a combo is a weapon section whose `parent_section` differs from itself (`wpn_ak74_kobra` with `parent_section = wpn_ak74`). The suffix names the addon item (`kobra`), which maps to a part through `inv_item`.
- **Explicit:** `[mw_combo_map]` entries for anything the rule gets wrong (several addons, odd names).
- **Unconverted combos:** if no part model exists yet for a combo's addon, map it to base + **overrides** of `visual` and `hud` pointing at the combo's baked model, plus the combo's own stat keys. The gun is still one object with a record, and the baked look stays until the part is authored. Content can then migrate gun by gun without a second engine path.

### 12.2 Where the map is applied

- **Spawning:** `alife():create` / `spawn_item` of a combo section creates the base with the mapped parts. Trader lists, loadouts, stashes, quest rewards and scripts that spawn by name keep working without edits.
- **Loading old saves:** in `Spawn_Read`, an entity of version 128 or older whose section is a combo is rewritten to its base with the mapped record. Its addon flags become parts too (the silencer bit becomes the gun's `silencer_name` part, and so on). If client data holds `m_cur_scope`, it picks which scope.
- **Scripts asking for the section:** `section()` returns the base. For scripts that need the old name, add `weapon:combo_section()`, which returns the combo matching the current parts, or the base. Lua shims wrap the known readers: 3DSS's `get_sight_section` / `parse_parameters` and Mark Switch (as `mw_compat` does), Borksy's `has_laser`, and the `item_weapon.attach_scope` / `detach_scope` entry points.
- **`alife_clone_weapon`:** the engine function stays (it copies the record), but `item_weapon` stops calling it for addons. Other mods that still call it to switch to a combo get the mapped parts fitted in place.

### 12.3 Per-gun data from other mods

The ID no longer changes on attach and detach. Part_bench's list of per-gun data to carry across a rebuild (WPO parts, 3DSS marks, laser state, showcase and companion entries) therefore only matters for legacy callers of `alife_clone_weapon`.

### 12.4 Content work this implies

This is not engine work, but the engine plan only pays off with it:

- A stripped single model per gun: bone shapes, motion reference, LODs, no baked addons.
- Mount data per gun (offsets, or `mnt_*` bones if 1f lands).
- Part models and part sections for every addon in use. Part_bench's `tools\mw_part_ltx.ps1` generates sections from meshes.
- Stripped base icons and part icon anchors.

Size this per gun family before promising "everywhere" in one release. The unconverted-combo fallback in 12.1 allows a gradual rollout.

---

## 13. W13: Script API

On the weapon's game object unless noted:

| Function | Purpose |
|---|---|
| `instance_section()` | Generated section name |
| `get_parts()` | Table of path → `{section, condition, upgrades, data}` |
| `get_mounts([path])` | Mounts and what each accepts, for the bench |
| `can_fit(path, section)` | `nil` / `"clash", reason` / `true`, the same contract as `mw_resolve.check` |
| `fit_part(path, item_or_section [, state])`, `remove_part(path [, spawn])` | Commit changes |
| `set_preview(record)`, `clear_preview()` | Bench preview (6.3) |
| `get_part_data(path)`, `set_part_data(path, string)` | Per-part blob (battery, reticle mark) |
| `get_override(key)`, `set_override(key, value)`, `iterate_overrides()` | Persistent per-gun keys, and persistent versions of `SetRPM` and the other setters |
| `part_attachment(path, hud)` | Read-only handle to a part's attachment (bone queries, lights) |
| `combo_section()` | Compatibility name (12.2) |
| `alife():weapon_record(se_obj)` | Read and write the record of an offline weapon |

Callbacks: `on_weapon_parts_changed(wpn)`, `on_weapon_rebuilt(wpn)`.

Console commands (with `-dbg`): `mw_dump <id>` writes the generated section to an `.ltx` in `appdata` for inspection; `mw_resolve_test`; `mw_fit`; `mw_remove`.

---

## 14. Where Part_bench's code goes

| Part_bench | Becomes |
|---|---|
| `mw_resolve.script` | `CWeaponPartResolver` in C++ (W5). Lua keeps only the bench's listing helpers, through `get_mounts` / `can_fit` |
| `mw_state.script` (`m_data.mw_parts`) | The instance record (W2) |
| `mw_render.script` (separate `mw_h:` and `mw_w:` trees) | One engine part tree on the shared instance (W6) |
| `mw_apply.script` (clone + upgrade list) | `fit_part` / `remove_part` + `ReloadConfig` (W4, W9) |
| `mw_ogf.script` | Bind poses from loaded kinematics (W5) |
| `mw_aim.script` | Generated HUD section aim (W8) |
| `mw_scope.script` and the `mw_scope_*` items | The sight role (W7) |
| `mw_compat.script` | Stays in Lua, keyed on the main sight from `get_parts()` |
| `mw_pose.script` | Stays in Lua. Later option: write pose keys into the generated HUD section |
| `mw_light.script`, `mw_laser.script` | Light creation and view switching move to the engine; on/off and batteries stay in Lua |
| `part_bench_mcm.script` | Stays in Lua, on the new API |
| `tools\*.ps1`, Blender script | Content pipeline, unchanged |

---

## 15. Order and estimates

Rough figures for one developer who knows the codebase, including testing.

| Phase | Workstreams | Result | Estimate |
|---|---|---|---|
| 1 | W1 (1a–1e), spike for 1f | One model instance per gun, same pose and shaders in both views | 2–3 weeks |
| 2 | W2, W3 | Per-gun record saved; generated sections; overrides persist | 2–3 weeks |
| 3 | W4 | Same-object rebuild; mechanic upgrades without detaching | 2–4 weeks |
| 4 | W5, W6 | Parts drawn by the engine from the record | 3–5 weeks |
| 5 | W7, W8, W9 | Parts drive gameplay and aim; fit and remove from inventory and bench | 4–6 weeks |
| 6 | W10, W11 | Icons, NPC loadouts, trade | 2–3 weeks |
| 7 | W12 | Combo map, old-save migration, mod shims | 3–6 weeks, plus content |

Engine total: about **4–7 months**. Content conversion runs in parallel from phase 4 and is the open-ended part.

Each phase ships on its own. After phase 3, Part_bench can stop cloning. After phase 4, it can drop `mw_render`.

---

## 16. Testing

- **Resolver unit tests:** port `mw_resolve_test.script` to C++, run from a console command, plus a case for every error code.
- **Save round trip:** for guns with deep part trees, save, load and compare records byte for byte. Load a save made by the current exe and check that every combo gun becomes base + parts.
- **Regression checklist:** Part_bench spikes 1, 2, 3a–c, 4, 8, 9, 11, 12 and 13, run without the scripts they replace.
- **Performance:** a scene with 30 armed NPCs; frame time before and after, with 0, 3 and 6 parts per gun.
- **Mod compatibility:** a GAMMA profile run with 3DSS, WPO, Mark Switch, Borksy's lasers and Mags Redux enabled. Log every script error that mentions a weapon section.

---

## 17. Risks and open questions

- **Old exes can't read new saves.** Bumping `SPAWN_VERSION` is one-way. Warn users and keep a save backup on first load.
- **Mod breakage.** "Replace everywhere" means any script that parses combo section names breaks until shimmed. The combo map and `combo_section()` cover the common readers. The long tail is unknown until a full GAMMA run.
- **NPC render cost.** HUD-detail models plus parts on every NPC gun may cost noticeable frame time without LODs. Measure in phases 1 and 4.
- **HUD rebind while held** (W4 step 8) is the riskiest code path. Gate it so it only runs while the gun is hidden.
- **Threads and the dynamic section store.** Check whether any worker thread reads `pSettings` while sections are added or removed. Use a lock.
- **Shared instance edge cases** (W1, 7.3): the root-bone override on drop and pickup, script animations that swap the HUD model (`script_anim_item_model`), and mods that call `HudItemData().m_model` expecting a separate model. Test drop, pickup, holster and script animations early.
- **Lost SSS HUD effects** (W1, 7.4): no raindrops on the held gun until the SSS shaders take a per-draw flag, and the held gun's glass needs a visual check.
- **Motion files and extra bones** (W1 1f) are unproven. Without them, gun mounts stay LTX offsets.
- **Nested bone lookup** (W6) behaves differently between Part_bench's tested build (MT-TEST 2026.10.02) and the current source. Re-test before relying on part bones.
- **Magazine mods** (Mags Redux) keep their own magazine state. A `magazine` part may need an adapter for them.
