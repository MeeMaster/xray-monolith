# Modular weapon objects: engine plan

How to change xray-monolith so that every weapon is an independent object: one base model, parts as separate models fitted at runtime, and a per-weapon record that rebuilds the gun on save and load. It builds on [WEAPON_MODELS.md](WEAPON_MODELS.md), [WEAPON_OBJECTS.md](WEAPON_OBJECTS.md) and the Part_bench mod (`../part_bench`, its `AGENTS.md` and `mw_*` scripts).

Engine paths are relative to `src/`. Status: **plan**, with W1 change 1a implemented and tested in game (section 7.2), 1b implemented and tested in game (section 7.3), 1c implemented and run (section 7.4), 1d implemented and partly tested in game (section 7.3), 1e implemented and built (section 7.5), 1f skipped, and 1g implemented and tested in game (section 7.5). Phase 2: 2a (the record type) tested, 2b (saving it) tested, 2c (game-side copy and sync) tested (sections 4.1–4.3), 2d (runtime block) fire mode tested (section 4.4), 2e (dynamic sections) tested (section 5.1), 2f (generated sections) tested, 2g (reading them) tested (sections 5.2, 5.3), 2h (script API, first slice) and 2i (tests) tested (section 14). Phase 2 is complete. Sizes are estimates from reading the code.

---

## 1. Decisions

Agreed on 2026-11-24:

| Question | Decision |
|---|---|
| Where per-weapon data lives | A **record on the weapon's server entity**, written to the save. At spawn the engine turns it into a **generated in-memory section** `[<base>_i<id>]:<base>` that the client object loads. An `.ltx` dump exists for debugging only. |
| What a fitted part keeps | **Embedded state**: section, condition, installed upgrades, and a small per-part data blob. Detaching respawns a real item with that state. No entity exists while the part is fitted. |
| Existing content | **Replace everywhere.** Every firearm moves to the new model. Baked combo sections (`wpn_ak74_kobra`), addon flags and `alife_clone_weapon` swaps are mapped onto base + parts, for new spawns and for old saves. |
| Engine vs. Lua | **Engine core, Lua edges.** The engine owns the record, persistence, mount resolving, part drawing, stats, aim and gameplay effects. Lua keeps the bench UI, hand poses and shims for other mods, through new engine APIs. |
| How changes apply | **In-place refresh only** (agreed 2026-11-24). Every part change updates the record, then `ReloadConfig` recomputes the same live object from base + record (section 6). There is no incremental "add onto the live object" path, so the live gun always matches what a reload would rebuild. A change that only moves parts takes a short path inside `ReloadConfig`, which must give the same result as a full rebuild (section 8.3). |
| Rails and free placement | **Supported by the engine, not shipped as gameplay** (agreed 2026-11-25). Mounts can be rails with a length, parts sit at a position on them, and several parts can share one rail. The engine provides the data, record, resolver rules and script API. Moving parts along rails in the UI is left to mods (section 8.3). |

Assumptions made without asking (change them if wrong):

- **Part stats are recomputed, not installed.** A part's stat effects use the existing upgrade property syntax, but they are applied on every rebuild and never added to the weapon's installed-upgrade list. Removing a part is just a rebuild without it. This drops Part_bench's "Apply = clone with a new upgrade list".
- **"One model" means one file and one instance** (revised 2026-11-24). First person and third person draw the same model instance with the same pose, bone visibility, parts and shaders. Only the transform and the HUD pass draw state (FOV, depth range) differ per view (section 7).
- **Part_bench's data format is the starting schema.** Keys such as `mw_mounts`, `mw_defaults`, `mount_type`, `accepts`, `positions`, `blocks`, `long`, `pair`, `canted` and `aim_bone` are kept, so the existing part `.ltx` files and meshes keep working.
- **Single-player only.** Network sync is kept just correct enough for the local client/server pair.
- **Every `CWeapon` and every part item gets a record.** Only firearms have mounts. Knives, grenades and binoculars carry an empty record. A loose part item uses its record for state that must survive fitting and removal, such as its paintjob (section 13); the rest of a part's state still moves into the gun's record when it is fitted.

---

## 2. Target model

### 2.1 What a weapon is

```
Weapon object (one server entity, one ID for its whole life)
  base section        wpn_ak74                    unchanged, from the .ltx
  instance record     saved with the entity       (section 4)
    parts             list of embedded parts, each on a mount at a position (section 8.3)
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

[mw_m4_tac_mount_top]                  ; new: a rail (section 8.3)
bone     = mnt_top                     ; rail start, the end nearer the model origin
bone_end = mnt_top_end                 ; rail end; optional when the bone is named <bone>_end
accepts  = picatinny
stops    = near_top:mnt_near_top, far_top:mnt_far_top ; named positions, from bones or metres

[mount_type_picatinny]                 ; new: shared data per rail standard
pitch    = 0.01001                     ; slot spacing in metres; 0 = slide freely (dovetail)

[mw_part_aimpoint]
span     = -0.012, 0.012               ; new: where its clamps sit, rear and front of its origin; must fit on the rail
body     = -0.02, 0.025                ; new: its length for clashes; may overhang the rail; default = span
```

A part can provide mounts of its own (`mw_mounts` on the part), so the fitted parts form a tree addressed by paths such as `handguard/near_bottom`.

Schema rule for moddability: every extension point is its own section or its own key, never a position inside a value. DLTX can append to lists (`>mw_mounts = rail_left`), patch one key, and override or delete a section, but it can't edit one field inside a packed value. No part of the engine may cap the number of mounts, parts or stops.

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
| W14 | Paintjobs: per-gun and per-part texture sets | S–M | W1, W2, W4; W6 for parts |

---

## 4. W2: Instance record and save format

**Goal:** a weapon's full configuration survives save/load, going offline and changing owner, on the same entity and ID.

### 4.1 The record

A new struct, `CSE_InstanceRecord`, held by `CSE_ALifeInventoryItem` so other item kinds can use it later:

```
u8   version
u16  override count, then (stringZ key, stringZ value) each
u16  next part id
u8   part count, then per part:
       u16    id             stable for the part's life on this gun; never reused
       u16    parent id      0 = the gun itself
       stringZ mount         mount name on the parent, "scope", "top"
       f32    pos            position along the mount in metres; 0 on a point mount
       u8     prop count, then (stringZ key, stringZ value) each
                             placement and mod state the engine and resolve callbacks can read (8.3)
       stringZ section       "" = emptied on purpose (Part_bench's `false`)
       u8     condition      quantised like m_fCondition in UPDATE packets
       u8     upgrade count, then stringZ ids
       u16    blob size, then bytes   part-owned data: battery charge, reticle mark, light on/off
       stringZ paintjob      "" = none; the part's own paintjob (section 13)
runtime block (for rebuilds and save):
       u8 fire mode, u8 GL mode, per-round ammo type list
```

Mounts that hold no entry fall back to `mw_defaults`, as in `mw_resolve`. An unmodified gun has an empty record and costs a few bytes.

The record is a list keyed by part id, not a map keyed by mount path, because a rail can hold several parts (section 8.3). Paths such as `handguard/near_bottom` are derived from the parent chain for the API. A part on a rail is addressed as `top#4`, its mount name plus its id. Unknown props and a higher record version must survive a load and save, so mods can store their own placement state without a format change.

**Implemented 2026-11-25 (2a). Tested 2026-11-25 (`instance_record_test`, all checks pass).** `CSE_InstanceRecord` (`xrServerEntities/InstanceRecord.*`), held by `CSE_ALifeInventoryItem` as `m_instance`:
- **Layout** as above, with two additions: the body sits in a frame (u8 version, u16 size), and version 0 means an empty record of one byte. The runtime block has a presence flag, and rounds are a u8 ammo type index each.
- **Robust reading.** Every read is bounded by the frame. A damaged body is logged and dropped, and the frame is still consumed, so whatever follows in the packet reads correctly.
- **Newer formats** are kept as raw bytes, refuse edits, and are written back unchanged.
- **Caps:** 8 KB per record, 64 parts, 32 props and 32 upgrades per part, 1 KB blob per part, 128 overrides, 1024 rounds, 255 characters per string. The editing functions (`set_override`, `add_part`, `remove_part`, which also removes children) refuse anything over a cap. `write()` saves an empty record, with an error in the log, if code filled the vectors directly past a cap.
- **Part ids** come from `next_part_id` and are never reused, which is why a record whose parts were all removed still counts as not empty.
- **Test:** console `instance_record_test` runs the round-trip, byte-identity, newer-version, damaged-body and editing checks and logs each result.

### 4.2 Writing it into the save

Follow WEAPON_OBJECTS.md §12.5:

- Write the record **at the end of the state block**, after `STATE_Write`, inside the size frame in `CSE_Abstract::Spawn_Write` (`xrServerEntities/xrServer_Object_Base.cpp:198`). Read it in `Spawn_Read` (`:275`) from the remaining bytes when `m_wVersion > 128`.
- Bump `SPAWN_VERSION` to 129 (`xrServerEntities/xrServer_Objects.h:166`).
- Existing fields keep their offsets, so Anomaly's byte-level packet scripts (`utils_stpk`) keep working. Check that they tolerate trailing bytes (open question in WEAPON_OBJECTS.md §12.11).
- Keep the record well under the 16 KB packet limit. Cap the part count and blob sizes and refuse writes that exceed them.

**Implemented 2026-11-25 (2b). Tested in game 2026-11-25:** a version 128 save loads with empty records; overrides survive save/load and a level change.
- `CSE_Abstract::Spawn_Write` writes an inventory item's record right after `STATE_Write`, inside the state size frame. `SPAWN_VERSION` is 129.
- `Spawn_Read` reads it for version 129 from the bytes left in the frame, then sets the read position to the frame end. A damaged or newer record can't shift anything after it. Version 128 and older load with an empty record.
- **Script packets are unchanged.** The record is not part of `STATE_Write`/`STATE_Read`, so `utils_stpk` and every script that builds packets with them see the same bytes as before.
- **No item class reads leftover frame bytes** in its `STATE_Read` (checked in `xrServerEntities` and the GAMMA mods' scripts), so nothing else consumes the record.
- `spawn_packs.cpp` `peek_graph_vertex` accepts versions 128–129; the header it reads is the same.
- **Probably:** an older exe loading a 129 save ignores the record. The registry reads each spawn packet as its own buffer, so leftover bytes in it are skipped. Not tested.
- **Debug commands:** `instance_record_dump` prints the record of the server entity behind the item in hands. `instance_record_set <key> [value]` sets an override, and no value clears it.

### 4.3 Client and server copies

- The client `CWeapon` holds a copy (`m_instance`). Every change goes client to server through a new event `GE_INSTANCE_RECORD` carrying the whole record, handled next to `GE_INSTALL_UPGRADE` in `xrGame/xrServer_process_event.cpp`.
- `net_Spawn` reads the record from the server entity before `Load` (section 5). It never lives in client data, which `switch_offline` wipes.
- `alife():clone_weapon` copies the record, for as long as anything still clones.

**Implemented 2026-11-25 (2c). Tested in game 2026-11-25:** setting and clearing an override through the game side reaches the server copy, and both copies match after save/load and a level change.
- **Game-side copy.** `CInventoryItem::m_instance` holds a copy for every inventory item, not only weapons, matching where the server keeps it. `net_Spawn` fills it from the server entity, so going offline and back, a level change or a respawn always starts from the server's copy.
- **Sending changes.** `CInventoryItem::set_instance_record` replaces the local copy and sends the whole record as `GE_INSTANCE_RECORD`, added at the end of the `GE_` list so existing values don't move. The server handler (`xrServer_process_event.cpp`) replaces the entity's copy. A record from a newer build is never replaced.
- **Clones.** `alife():clone_weapon` copies the record.
- **One direction only.** A change made on the server side while the item is online doesn't reach the game copy until the next `net_Spawn`. The script API (2h) edits through the game side while an item is online and through the server side while it is offline.
- **Debug commands.** `instance_record_set` now goes through the game side and the event. `instance_record_dump` prints both copies and whether they match.

### 4.4 Bugs fixed along the way

From WEAPON_OBJECTS.md §10:

- **Bugs 2 and 5** (scope index lost offline, index 0 overloaded) disappear. The record stores the sight's section, not an index.
- **Bugs 7 and 8** (mixed magazines collapse; fire mode, GL mode and shotgun ammo not in STATE) are fixed by the record's runtime block.

**Implemented 2026-11-25 (2d). Tested in game 2026-11-25:** a non-default fire mode survives the gun going offline (dropped, level change, back). Mixed magazines not tested yet. The runtime block in use (`Weapon.cpp`, `WeaponMagazined.cpp`, `WeaponMagazinedWGrenade.cpp`):
- **What the bugs really are.** Fire mode and launcher mode are already kept in the weapon's client data (`save`/`load`), so they survive save/load on the actor. They are lost when an item goes offline, because `switch_offline` clears client data. Mixed magazines are lost everywhere, because `net_Spawn` refills the magazine with copies of one type.
- **Capture.** The per-round ammo type indices, oldest first (the bottom of `m_magazine`; in launcher mode the rifle rounds in `m_magazine2`), plus the fire mode and launcher mode.
- **Defaults aren't stored.** A weapon whose rounds all match its current ammo type, in the default fire mode (the last one listed) and not in launcher mode, keeps an empty block. Most weapons add nothing to the save.
- **Sync** (`CWeapon::instance_runtime_sync`) sends the block only when it no longer describes the weapon. Shots only remove rounds from the end, so after firing the rounds left are still a prefix of the stored list, and the saved round count says how many remain. Reloads, unloads, mode switches and script changes do trigger a send. Sync runs every 0.5 s from `UpdateCL` and once from `CWeapon::save`, which the client save calls before the server writes the file. Single player handles events on receipt, so the record arrives first.
- **Apply.** At the end of `CWeapon::net_Spawn`, the first `iAmmoElapsed` stored types replace the default cartridges, and `CWeaponMagazined` restores the fire mode. The queue size is only set for the actor, as in `OnH_A_Chield`.
- **Not covered:**
  - launcher mode itself is not restored on spawn; the client data still does that for the actor;
  - rounds are not applied when the weapon was saved in launcher mode, because the saved round count is then the grenades';
  - a magazine changed while the weapon is neither updating nor saved (for example unloaded in the inventory and then taken offline) keeps the previous block.
- **Debug:** `instance_record_dump` prints the runtime block.

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

**Implemented 2026-11-25 (2e). Tested 2026-11-25 (`ini_dynamic_test` passes; 100 000 missed lookups take 5.5 ms, about 55 ns each, with no dynamic sections live).** `xrCore/xr_ini.*`:
- **Store.** `CInifile::DynamicStore`, created on first use: an `xr_map` from lower-case name to a heap `Sect*` (pointer-stable), an `xrCriticalSection`, and an atomic count.
- **Two hooks only.** `r_section` and `section_exist` check the store after the normal lookup misses. Everything else (`r_string` and every typed reader, `line_exist`, `line_count`, `r_line`) goes through `r_section`. A hit in the loaded sections costs exactly what it did before. A miss costs a suffix check, and the lock is taken only for an `_i<digits>` name while dynamic sections exist.
- **API**, callable on `pSettings` (const):
  - `add_dynamic_section(name, base, items)` copies the loaded, already flattened base and replaces or adds the items; adding again replaces the section;
  - `remove_dynamic_section`, `dynamic_section_exist`, `dynamic_section_count`.
  - It refuses names without the `_i<digits>` suffix, a name that a loaded section already has, and a base that isn't a loaded section.
- **Lifetime.** A replaced or removed section is retired, not freed, so a `Sect&` handed out earlier stays valid. `CLevel::OnFrame` frees retired sections at the start of each frame (`dynamic_sections_collect`).
- **Not in `sections()`.** Scripts and code that iterate every section never see per-object sections.
- **Test:** console `ini_dynamic_test [base]` (default `wpn_ak74`) checks building, reading, overriding and adding keys, line counts, rebuilding, refusals and removal. It also times 100 000 missed lookups.

### 5.2 What goes into a generated section

Built in this order, later layers overriding earlier ones:

1. The base section.
2. **Role data from parts** (section 9): the main sight's zoom and scope keys, the muzzle's silencer flags, fire point and sounds, the launcher's GL keys.
3. **Overrides** from the record.

Part **modifiers** are not written here. They are applied after `Load` through the upgrade handlers (section 6.2), so additive keys add instead of overwrite.

A matching **generated HUD section** `[<hud>_i<id>]` carries the computed aim offsets (W8) and, later, hand-pose animation keys if poses ever move into the engine.

**Implemented 2026-11-25 (2f). Tested in game 2026-11-25:** the section is built from overrides, rebuilt after save/load, a level change and offline and back, and dropped when the overrides are cleared. `xrGame/instance_section.*`:
- **Builder.** `instance_section::build(base, id, record)` builds `[<base>_i<id>]` from the loaded base and the record's overrides. Part role data joins in phase 5. It removes the section instead when the record needs none: no overrides, or a record from a newer build. A runtime-only record (2d) gets no section, so most items never have one.
- **Built before `Load`.** `Load` runs inside `Objects.Create` (`IGame_ObjectPool::create`), before `net_Spawn`, so `CLevel::g_sv_Spawn` builds the section from the spawn entity, which already carries the record and the ID, just before `Objects.Create`. 2g can then hand it to `Load`.
- **Item side.** `CInventoryItem` keeps the name while the section exists (`m_instance_section`, read with `instance_section()`, which falls back to the base, and `has_instance_section()`). `set_instance_record` rebuilds the section when the overrides change; a 2d runtime sync leaves it alone. `net_Destroy` removes it, so going offline, being destroyed and a level unload all clean up.
- **Not read yet.** The weapon still loads and reads its base section; 2g switches it over.
- **Debug:** `instance_record_dump` prints the item's config section and reads each override back from it through `pSettings`.

### 5.3 Identity

- `cNameSect()` / `section()` keep returning the **base** section. Trade lists, ammo checks, repair kits, upgrade roots and most scripts key on it.
- `instance_section()` (new) returns the generated name. `CWeapon::Load` and `reload` receive it instead of the base.
- The upgrade manager keeps looking up roots by base section (`xrGame/inventory_upgrade_manager.cpp:304`, `get_root(item.m_section_id)`).
- `GetScopeName()` and the other functions that re-read the INI at runtime (WEAPON_OBJECTS.md §12.1, point 4) read the generated section.

**Implemented 2026-11-25 (2g). Tested in game 2026-11-25:** an `rpm` override changes the fire rate after save/load, and `section()` still returns the base.
- **`Load` reads the generated section.** `IGame_ObjectPool::create` and `CObjectList::Create` take an optional load section; `CLevel::g_sv_Spawn` passes the one 2f built. The pool still sets `cNameSect` to the base before `Load`.
- **Identity stays the base.** `CObject::Load` keeps the pool's `cNameSect` (and uses it as the debug name) when it is given a dynamic name. `CInventoryItem::Load` sets `m_section_id`, which trade and upgrades key on, to the base. Scripts' `section()` and the server entity's `s_name` are untouched.
- **`reload`.** `CGameObject::net_Spawn` reloads an inventory item from its generated section when one exists. The script binder still gets the base.
- **`CInventoryItem::config_section()`** returns the generated section, or the base, and is valid from `Load` on (`Load` records a dynamic name). The weapon's runtime config reads now use it:
  - `SDS_Radius`, `UpdateZoomParams`, `UpdateUIScope`, `SetZoomTypeAndParams` and the zoom and scope reads in `CWeapon::Load`;
  - `LoadLights` in `CWeaponMagazined::InitAddons`;
  - `motion_mark_reload` in both `OnMotionMark` functions.
  - Identity uses keep `cNameSect()`: ammo matching, addon matching, icons, anticheat names, logs.
- **Limits:**
  - an override takes effect when the item next spawns (save/load, level change, offline and back), not at once; that is `ReloadConfig` (phase 3);
  - a `visual` override doesn't apply, because the world visual comes from the server entity, which takes it from the base;
  - inventory display reads that still use `cNameSect()` at runtime (`inv_grid_*`, `upgr_icon_*`, `kill_msg_*`) read the base.

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

**Implemented 2026-11-24** in `CKinematicsAnimated::BuildBoneMatrix` (`Layers/xrRender/SkeletonAnimated.cpp`), the per-frame path only. The two root-motion helpers that call `LL_BoneMatrixBuild` directly with their own key tables (`animation_movement_controller.cpp`, `imotion_position.cpp`) are unchanged. Console toggle `rs_skeleton_bind_fallback` (default 1; 0 restores the old behaviour for comparison). **Tested in game 2026-11-24** on plain Anomaly with `tests\bind_pose`: with 0 the bolt and magazine collapse onto the gun's origin, with 1 they stay in place.

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

**Implemented 2026-11-24 (1b). Tested in game 2026-11-25** with the `tests/bind_pose` gun, `hud_shared_model 1` and `rs_skeleton_bind_fallback 1`:
- Reload and shooting motions play in first person and in third person (F1).
- A dropped gun keeps its parts at the bind pose and picks up normally.
- No crash and the correct model after: level change while holding the gun, save and load while holding it, selling it and buying it back, and giving it to an NPC (`npc_give`), who carries it.
- The test gun needs two-field aliases (`anm_reload = ak_74_reload1, reload`) because the vanilla hand motion names don't exist in its motion file. With one field the gun falls back to `idle` and looks static.
- A first third-person run looked static with either setting; a later run with a freshly spawned gun animated. The likely cause was a gun spawned while `hud_shared_model` was 0 (it persists in `user.ltx`), but this was not confirmed.
- **Opt-in rule.** `attachable_hud_item::load` (`xrGame/player_hud.cpp`) shares the owner's world visual when all of these hold:
  - the item is a `CWeapon`;
  - the world visual is animated and has an `idle` motion;
  - the HUD section's `item_visual` is missing or names the same file.
- **Fallback.** Anything else, including every vanilla gun, still loads `item_visual` as before.
- **Console toggle.** `hud_shared_model` (default 1) applies to HUD data created after the change.
- **Ownership.** The shared model is flagged `m_model_shared`, and the destructor never deletes it.
- **Error messages** use `m_visual_name` instead of reading `item_visual`.
- **Skip marker.** It is set on the model in `attachable_hud_item::render`.
- **Leaving the hands** (`release_shared_model`, called from `CHudItem::on_b_hud_detach` and the destructor):
  - clears the root override, unless a physics callback owns the root;
  - closes all cycles, so on the back, on the ground and after a drop the world view shows the bind pose (1a).
  - It only touches the model while it is still the owner's live visual, because `net_Destroy` deletes the visual before the item is destroyed.
- **One callback slot.** A kinematics has a single update-callback slot, and the world list (`CGameObject`) and HUD list (`CHudItem`) both use it, for example script attachments' visbox updates. On a shared model, both `SetKinematicsCallback` functions install `VisualCallbackShared` (`HudItem.cpp`), which runs both lists.
- **Not handled yet:**
  - the world visual changing while HUD data exists (only `net_Spawn` sets it today);
  - `remove_hud_model` dropping the HUD callbacks (as it already did with two models);
  - the two bone-visibility functions both writing the one model: done in 1d, below.

**Implemented 2026-11-25 (1d). Tested in game 2026-11-25 on a vanilla gun:** scope and grenade launcher show in first and third person. Not yet checked: silencer, a dropped gun, RPG-7 reload.
- **One rule.** `CWeapon::ApplyAddonsVisibility(K, hud_copy)` (`Weapon.cpp`) holds the scope, silencer and launcher bone rule. `UpdateAddonsVisibility` applies it to the world model, and to a separate first-person model through `UpdateHUDAddonsVisibility` (still called every frame from `UpdateCL`). A shared model is the world model, so it gets the rule once and the per-frame first-person pass skips it.
- **Kept per copy, on purpose:**
  - a separate first-person copy still asserts on a missing attachable bone, and the world model still ignores one;
  - with `modular_attachments`, a separate first-person copy hides `wpn_scope` and switches `wpn_scoped_hide` / `wpn_scoped_unhide`, because the scope is drawn as its own HUD item. These guns no longer share a model (`shareable_world_visual`), since the two views need different bones.
- **Small change for every gun:** the world model now also shows the addon bone of a permanent addon. Before, only the first-person copy did this; bones start visible, so this should change nothing visible.
- **RPG-7:** on a shared model the rocket bone uses the first-person rule (shown while reloading) in every view, since the reload is animated in every view. Two-model guns are unchanged.
- **Already one-model safe:** the bullet bones (`HUD_VisualBulletUpdate`) and the underbarrel grenade bone (`UpdateGrenadeVisibility`) write only while in first person. On a shared model their last state carries over to third person and to a dropped gun.
- **Legacy only (decided 2026-11-25).** New guns ship every part, built-in ones included, as a separate part model (W6), so their models have no `wpn_scope` / `wpn_silencer` / `wpn_launcher` bones, and the world-model rule skips missing bones silently. The rule stays unchanged so old guns keep working, and it gets no new features. When parts land, a part-based gun turns it off with the existing `use_engine_attachments = false`, or W7 skips it automatically for guns with a part record.
- **Test:** the bind-pose test gun has no addon bones, so this is a regression test on vanilla guns: attach and detach a scope, silencer and launcher, in first and third person and dropped; an RPG-7 reload in first and third person.

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

**Implemented 2026-11-25 (1c). Runs without problems on plain Anomaly; not yet checked with SSS.** Reading the code changed the scope:
- **The lens exclusion is dead code.** `hud_disabled` (`ResourceManager.cpp:263`) has two readers, the HUD mask insert in `r__dsgraph_build.cpp` and `hud_node` in `r__dsgraph_render.cpp`. Both are commented out: SSS 23 retired the HUD mask (`r__dsgraph_structure.h:64`). Nothing needs moving to draw time. The flag-setting code is left in place; it has no effect.
- **The shared gun already loads plain.** Since 1b it is the world visual, which never had the HUD flag. So with SSS it already has no HUD raindrops and uses `ssfx_glass` in first person (the costs in the table above). The glass still needs a visual check with SSS.
- **`MotionExists`** (`level_script.cpp`, Lua `motion_exists`) no longer sets `hud_loading`. A probe can no longer give a model's world copies HUD shaders through the pool.
- **The two-model fallback** (`attachable_hud_item::load`) loads `item_visual` with the HUD flag only when it is a separate file. When it names the owner's world model (a gun that can't share, for example one with no `idle` motion), it loads plain, so the pool stays consistent.
- **Kept with the HUD flag:** separate first-person files (every vanilla gun keeps its raindrops), the hands, and the script animation item model.

### 7.5 Changes

| # | Change | Where | Size |
|---|---|---|---|
| 1a | Bind-pose fallback for bones with no blend | `Layers/xrRender/SkeletonAnimated.cpp`, `LL_BoneMatrixBuild` | S |
| 1b | One shared instance; `item_visual` retired; skip marker; root override cleared on drop | `xrGame/player_hud.cpp` (`attachable_hud_item::load`, destructor, `anim_play`), `HudItem.cpp`, `Weapon.cpp` drop and pickup | M |
| 1c | No `hud_loading` for weapons and parts; lens mask exclusion at draw time | `player_hud.cpp`, `level_script.cpp:1775`, `ResourceManager.cpp:263`, the HUD mask pass | S |
| 1d | One bone-visibility function | `Weapon.cpp:2010`, `:2071` | S |
| 1e | Force LOD0 in the HUD pass; world draws use progressive LOD | `r__dsgraph_build.cpp`, `rimp_select_sh_dynamic` | S |
| 1f | **Skipped (decided 2026-11-25).** New gun models ship a motion file whose bone set matches the model exactly, `mnt_*` bones included, so no motion file leaves a bone without a track. Tracks match bones by name, and the bone-count check exists only in debug builds, so a mismatch crashes instead of reporting; the export pipeline should check it. LTX mount offsets stay supported for older content | — | — |
| 1g | Optional: a short world motion (bolt cycle) on NPC shots if the model has it | `CWeaponMagazined` shot path | S |

Notes:

- **Do 1a first.** It is small, it can ship on its own, and it is what makes a single animated file usable in the world at all.
- **Mount bones without 1f.** Mounts can be bones in the gun model, as long as the gun's motion file is exported with the same bones (1f, skipped). Mounts as LTX offsets stay supported.
- **Content requirement.** Each gun model is one animated file with bone shapes (for dropping), the motion reference (for first person), and ideally LODs. Part_bench's `wpn_ak74_n_one.ogf` from spike 8 is exactly this. `tools\ogf_hud_from_world.ps1` makes it from a world export.
- **What the HUD view still needs from config:** the `hud` section's hand and item placement, motions and fire bones. Those describe placement in the hands, not a separate model.

**Done when:** `wpn_ak74_n_one.ogf` from Part_bench spike 8 is the only model of its gun, and it looks right in first person, in third person (bolt and magazine in place), dropped, and in an NPC's hands. Test with SSS on and off. In first person with actor shadows on, the shadow and the held gun show the same pose.

**Implemented 2026-11-25 (1e), built, not yet run in game.** What reading the code found:
- **Mesh detail** comes from `calcLOD` (1 = full detail), which only progressive meshes use (`CSkeletonX_PM::Render`, `FProgressive`). Plain skinned meshes (`CSkeletonX_ST`) ignore it.
- **The HUD buckets** draw through `sorted_L1`, which passes the node key, the squared *distance*, into `calcLOD` instead of the SSA. A closer object therefore gets less detail. At normal settings the held gun still lands on full detail, but at low resolution with a low `r__geometry_lod` it could drop.
- **Change:** `sorted_L1_hud` (`Layers/xrRender/r__dsgraph_render.cpp`) always draws at full detail. It is used for every HUD-only bucket: `mapHUD`, `mapCustomFOV`, `mapCamAttached`, `mapScopeHUD`, `mapScopeCamAttached`, `mapHUDSorted`, `mapCamAttachedSorted`, `mapScopeHUDSorted`, `mapScopeCamAttachedSorted`, `mapHUDEmissive`.
- **World draws need no change.** Opaque world draws already pick detail from the SSA (`mapNormal_Render`, `mapMatrix_Render`), and the shader quality from distance (`rimp_select_sh_dynamic`: HQ within `r_dtex_range`, so always HQ in first person).
- **Left alone:** world sorted, emissive, wallmark and distort draws still feed the distance into `calcLOD`, which in practice means full detail. Switching them to the SSA is not safe, because the static strict-sort insert in `r__dsgraph_build.cpp` never sets `ssa`. Non-HUD custom-FOV maps also stay on `sorted_L1`.
- **Content:** the test model `wpn_ak74_n_one.ogf` has 13 plain skinned meshes (type 5, `MT_SKELETON_GEOMDEF_ST`) and no LODs, so it can't show any of this. A gun only gets distance LODs once its export has progressive meshes (type 4).
- **No LOD authoring (decided 2026-11-25).** Weapon mods such as Boomsticks and Sharpsticks ship no lower-detail meshes, so the plan doesn't count on them. NPC guns draw at full detail; if that proves too slow (section 17), the fallback is a distance cutoff for cosmetic parts, not LOD swaps.

**Implemented 2026-11-25 (1g). Tested in game 2026-11-25:** an NPC firing the test gun plays `shoot` on every shot, and `mw_bp.watch()` shows the bolt moving (0.185 m at rest, down to 0.093 m mid-burst) and returning between bursts. The actor's own gun is skipped (first-person view), and so is a vanilla PKM (rigid world model). The visibility test uses the holder's spatial sphere: a held item never updates its own (`spatial.sphere`), so the first build rejected every shot as off screen. Console `wpn_world_shot_motion_debug 1` logs why each shot plays or not.
- **When it plays.** `CWeaponMagazined::OnShot` calls `CWeapon::PlayWorldShotMotion` (`Weapon.cpp`). It plays only when all of these hold:
  - the gun is held and not attached to the first-person view (an NPC's gun; the actor's shared model already plays the first-person motion in third person, 1b);
  - the world model is animated and has the motion named by `world_shot_motion` in the weapon section (default `shoot`, the name Boomsticks and Sharpsticks uses; an empty value turns it off for that gun);
  - the gun is within `wpn_world_shot_motion_dist` metres of the camera (console, default 50; 0 turns it off everywhere) and inside the view frustum.
- **How.** It plays the motion on every partition without mixing, so each shot restarts it, and pins the root bone to identity like `attachable_hud_item::anim_play`, so recoil in a first-person motion doesn't pull the gun out of the hands. `CWeapon::UpdateCL` advances the tracks while it plays. When the motion ends, or the gun is dropped (`OnH_B_Independent`) or taken into first person, `StopWorldMotion` closes the cycles, which returns the model to the bind pose (1a), and releases the root. On a shared model that the first-person view has taken over, it leaves the motions alone.
- **Cost.** Bone matrices of a visible animated model are rebuilt every frame anyway. A playing motion turns each bone's bind-pose copy into a keyframe sample, for about 0.1 s per shot. Offscreen and distant guns skip it.
- **Test:** `mw_bp.npc_give()` the test gun, get the NPC into a fight, aim at them and run `mw_bp.watch()`. The bolt offset should change with each shot and return to 0.185 between bursts.

---

## 8. W5 + W6: Mounts and part rendering in the engine

### 8.1 W5: Resolver

Port `mw_resolve.script` to C++ as `CWeaponPartResolver`. Input: base section plus record. Output: an ordered node list (path, part, parent, transform for HUD and world, role) and a list of errors.

Keep Part_bench's rules exactly, and port `mw_resolve_test.script` as a test suite run from a console command:

- `accepts` by `mount_type`; `positions` with `*`; `blocks` on mounts and parts; the earlier mount wins a clash
- defaults from `mw_defaults`, explicit empties
- `long` / `far_*`, `pair` / `pair_only`, `canted` and `canted_on_canted`
- `align_bone` and `fix`

Added on top of Part_bench: rails, positions, span overlap, stops and placement-only changes (section 8.3).

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
- **Performance:** NPC guns now draw 1 + N models. Measure early (section 17). If needed, skip cosmetic parts beyond a distance, and later merge a gun's static parts into one draw.

**Done when:** Part_bench's spike 9 checklist passes with no `mw_render` script loaded: one tree of parts showing in first and third person, nested parts, dropped guns, NPC guns, offline and back, save/load.

### 8.3 Rails, placement and extension points for mods

**Goal:** the engine ships fixed mount points, but a mod can add rails, slide parts along them, put several parts on one rail, or add its own placement rules, without engine changes or a new save format. The engine doesn't need to ship rail editing, only to avoid blocking it. The parts that are expensive to change after release are the record format (4.1) and the resolver's contract. Everything else here can grow later.

**A point mount is a rail of length 0.** A rail is marked by two points in the model: its start (`bone` + `offset`, as today, the end nearer the model origin) and its end. The engine works out the length and direction from them, so nobody has to measure the rail. A mount section takes these optional keys:

| Key | Meaning | Default |
|---|---|---|
| `bone_end`, `offset_end` | Rail end, in the same forms as `bone` / `offset`. `offset_end` alone is relative to the start bone, for models without mount bones (such as the M4 world mesh) | the bone `<bone>_end` if the model has it, else no end |
| `bone_end_world`, `offset_end_world` | End for the world model, matching `bone_world` / `offset_world` | as `bone_end` / `offset_end` |
| `length` | Rail length in metres along the mount's +Z. Fallback when no end is given; an end wins | `0`, a point mount |
| `stops` | Named positions, `name:pos, ...`. `pos` is metres from the start or a bone name, which is projected onto the rail | none |
| `default_pos` | Where a part goes when nothing else says; metres, a stop name or a bone | first stop, else `0` |

**The rail frame from two points.** Two points fix the rail's axis but not its roll, which sets the way a sight faces. The engine builds the frame as follows:

- Origin: the start point.
- Z: the unit vector from start to end. Length: the distance between them.
- Y: the start bone's Y (with its `offset` rotation), made perpendicular to Z. X = Y × Z.
- If the start bone's Y is within a few degrees of Z, the model is wrong. Log the mount and fall back to the start bone's own frame.

The length and the end's position in the start bone's space are computed once, from bind poses (8.1), when the gun's mounts are loaded. Per frame, only the start bone's transform is followed, so a rail on an animated bone (a sliding handguard, a folding stock) moves with it. Rails are straight and rigid. Both points must be rigid relative to each other, normally children of the same bone. If the end bone is animated on its own, its animation is ignored.

**Content convention:** a rail is a pair of bones named `mnt_<name>` and `mnt_<name>_end`, with the start bone's Y pointing out of the rail face. With that naming, the mount section needs only `bone` and `accepts`. Extra bones are subject to the motion-file spike (W1 1f). Until it lands, the `offset_end` form covers every model.

A `[mount_type_<name>]` section holds data shared by one rail standard: `pitch`, the slot spacing that positions snap to. `0` means the part slides freely, as on dovetail. For example, picatinny `0.01001`, M-LOK `0.04`, dovetail `0`. A part's `mount_type` may list several standards (`picatinny, weaver`), and it fits a mount that accepts any of them.

A part takes two lengths, each `rear, front` relative to its origin along the rail:

- `span`: where its clamps sit. Clamping keeps the span on the rail. Without `span` the part clamps at a point.
- `body`: how far the part reaches along the rail, used for clashes. It may overhang the rail, as a scope's bell does. Default: `span`.

Every existing gun and part stays valid with no changes. Point mounts are rails of length 0 and parts without `span` are points, so the current rules give the same results.

**Placement.** A part's transform is `mount frame × translate(0, 0, pos) × part local`. `pos` comes from the record, is snapped to the mount type's `pitch`, and is clamped so the part's span stays on the rail. That one rule covers sliding along a rail and several parts on one rail. Nested mounts on a part move with it.

**Rails on parts.** Any number of rails, on the gun or on parts, use the same mount sections. A part lists its own rails in `mw_mounts`, in sections `[<part>_mount_<name>]`, with bones from the part's model or offsets. That covers:

- **Adapters**, such as dovetail to picatinny, or a picatinny section bolted to an M-LOK slot: a part with `mount_type = dovetail` and one picatinny rail of its own.
- **Canted rails**: a part whose rail start bone (or `offset` rotation) is rolled, for example 45°. The rail frame inherits the roll, and so does whatever sits on it.
- **Risers and stacks**: any depth. The record links each part to its parent by id (4.1), so a chain such as gun → adapter → riser → red dot is just four entries.

The roll of a part is read from its whole mount chain. A sight counts as canted when that roll is more than 10° from the gun's up axis. Part_bench's `canted` flag becomes an override for parts whose geometry doesn't show it. `canted_on_canted` reads the same derived value, so a canted adapter carrying a plain red dot counts as canted without a special part.

**Stops keep the old mount names working.** Part_bench guns model one rail as separate point mounts, such as the M4's `near_top` and `far_top`. A gun author can merge them into one `top` rail with `stops = near_top:mnt_near_top, far_top:mnt_far_top`. The existing bones become stops by projection, so their positions don't have to be measured again. A part's `positions` list then resolves each name either as a whole rail (`top`: anywhere on it) or as a stop (`near_top`: on `top`, at that position). Old records that name `near_top` load onto the rail at that stop. Merging is optional, and both forms can exist on one gun.

**Clashes:**

- On one rail, two parts clash when their bodies overlap.
- **Through parts:** a part's footprint on its rail is its own body joined with the bodies of everything fitted on it, projected onto that rail. A part counts only when its rail is parallel to the parent's (within 5°). So a red dot on a riser that sticks out past the riser clashes with a scope beside the riser on the gun's rail. Parts on side rails or canted rails don't project, and clash only through `blocks`.
- Between mounts, `blocks` keeps working with whole mount names, and also takes a range, `top:0.20-0.26`. This covers a long scope that reaches over the front of a handguard rail.
- `hides` applies while the mount holds any part. Rules that depend on position, such as hiding only the front sight post, go through the resolve callback below.
- Part_bench's `long` / `far_*` and `pair` rules stay as they are. Where a gun uses stops, they read the stop names.

**Placement changes vs. topology changes.** The resolver returns its nodes with a flag that says whether the set of parts and their mounts changed, or only `pos` and placement props changed. `ReloadConfig` takes the short path for a placement-only change: update each moved attachment's offset, recompute aim (W8) and write the new aim keys into the generated HUD section and the live `hud_item_measures`. It skips the generated section's stats, `CWeapon::Load` and the part tree rebuild. The short path must give the same result as a full rebuild. A debug check runs both and compares them (section 17). This keeps the "in-place refresh only" decision: the live gun still always matches what a reload would rebuild.

**Script contract.** A mod that edits placement needs only the calls in section 14: read the rails and what occupies them (`get_mounts`), test a fit (`can_fit` with a position), commit (`fit_part` with a position, `move_part`), and store its own state per part (`get_part_prop` / `set_part_prop`). The engine's fit by drag and drop and the bench use `default_pos` and never move a part. They are one user of the API, and a rail mod is another.

**Resolve callback.** `on_part_resolve(wpn, node, ctx)` runs for each node during a rebuild. It may refuse the node with a reason or add a local transform, such as rotating a part from a `rot` prop. To keep rebuilds reproducible, it may read only the record (props included) and config, not game state or time. The same record must always give the same gun. The callback receives nothing unless a script registers it, so it costs nothing in an unmodded game.

**What stays out of the engine:** the dragging UI, rules for how far an eye-relief scope may sit from the eye, and any per-mod placement data. These live in Lua, in props and in mod `.ltx` sections.

**Done when:** a test script fits two parts on one rail at different positions, moves one along it, sees `can_fit` refuse an overlapping position, and the configuration survives save/load. A gun with only point mounts and old records gives the same nodes as before this change.

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
- On a rail, the part's icon slides from the `mw_icon_<mount>` anchor (the rail start) towards a new `mw_icon_<mount>_end` anchor, in proportion to `pos` / length. This works for rails that look short or slanted in the icon. The same keys on a part place the parts fitted on it.
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

## 13. W14: Paintjobs

**Goal:** a gun and each of its parts can carry a paintjob, meaning a set of replacement textures. The paintjob is saved with the object, stays on a part when it moves between guns, and looks the same in first and third person.

### 13.1 What exists today

`obj:set_shader(id, shader, texture, hud)` / `reset_shader` (`xrGame/script_game_object.cpp:1658`) already rebuild the shader of one sub-mesh, or all of them, on one model instance. Instances don't share the change, and the model pool restores the default texture (`Layers/xrRender/ModelPool.cpp:327`). Mods can't use it for paintjobs because:

- the world and HUD models are separate, so every change needs two calls;
- the HUD model is recreated on every attach, which drops the change;
- nothing is saved, so a mod must track skins itself and reapply them on every spawn, load and drop.

W1 (one instance) fixes the first two. The record and `ReloadConfig` (W2, W4) fix the third.

### 13.2 Data

A paintjob is a section that maps **default texture → replacement texture**. Keying on the texture the model file names (`dbg_texture_def`), not on the sub-mesh index, means one paintjob can cover a gun and its parts, and keeps working if a mesh is re-exported with its sub-meshes in a different order.

```ini
[pj_ak74_tiger]
textures      = wpn\wpn_ak74_body:wpn\skins\ak74_tiger_body, wpn\wpn_ak74_wood:wpn\skins\ak74_tiger_wood, wpn\mw_kobra:wpn\skins\kobra_tiger
shader        = models\weapons     ; optional; default keeps each sub-mesh's own shader
applies_to    = wpn_ak74, mw_part_kobra   ; optional; bases and parts it may be put on, for the bench and validation
inv_item      = pj_ak74_tiger_kit  ; optional; an item consumed when applying it (content's choice)
```

Each replacement texture needs its own `.thm` and `_bump` / `_bump#` files, like any model texture. Otherwise the default bump map is used with the new albedo.

### 13.3 Where it is stored

| Object | Storage |
|---|---|
| Gun | Override `paintjob = <section>` in the record. It ends up in the generated section like any override, so a base section can also set a default paintjob. |
| Fitted part | The part's `paintjob` field in the record (4.1). |
| Loose part item | Override `paintjob` in the item's own record. The record lives on `CSE_ALifeInventoryItem`, so part items hold one too (section 1). |

Fit (`FitPart`) moves the item's `paintjob` into the part's field. Remove (`RemovePart`) writes it back to the spawned item. A painted part keeps its paint on any gun.

### 13.4 Which texture wins

For each sub-mesh of a part, in order:

1. the part's own paintjob, if it lists this sub-mesh's default texture;
2. the gun's paintjob, if it lists it;
3. the default texture.

The gun's own sub-meshes use rule 2, then rule 3. A whole-gun paintjob therefore also paints unpainted parts whose textures it lists, and a part with its own paint keeps it.

### 13.5 Applying it

- One function, `CWeapon::ApplyPaintjobs()`, walks the gun's model instance and each part attachment. For each sub-mesh it picks the texture by 13.4 and calls `SetShaderTexture` only if the result differs from the current one. A sub-mesh no paintjob covers gets `ResetShaderTexture`, so removing a paintjob restores the defaults.
- Called at the end of `ReloadConfig` (after step 9, section 6) and after the part tree is built in `net_Spawn` and on HUD attach (W6).
- Loose part items: the same lookup (rules 1 and 3) in the item's `net_Spawn`, so a painted part on the ground or in a stash shows its paint.
- **Shared model (W1):** one call covers both views. The visual's `hud` flag is false, so `SetShaderTexture` builds world shaders, as 7.4 requires.
- **Fallback guns** (two instances, see 7.3): also apply to `HudItemData()->m_model` on attach, until the gun is converted.
- Changing a paintjob is a record change like any other: update the field, then rebuild. A bench preview uses the preview record (6.3).

### 13.6 Limits

- A paintjob texture is painted for one model's UV layout. Reusing it on another model needs that model to share the default texture and the UVs.
- Full texture replacement only. Layered decals, wear masks or tint colours need a shader with extra inputs, which belongs to the shader packs and is out of scope here.
- Script `set_shader` calls on a gun or part are overwritten by the next rebuild. Mods that use it for skins should move to `set_paintjob`.
- Icons don't show paint until the runtime 3D icon option in W10 exists. Until then a paintjob can provide an optional `icon` override for the base only.
- The first use of a skin loads its textures and can hitch. Bench previews and traders that show many skins should preload the textures.

**Done when:** a painted gun with a painted part keeps both through save/load, drop and pickup, an NPC carrying it going offline and back, and a level change. Moving the part to another gun keeps the part's paint. Removing a paintjob restores the default textures. First and third person look the same.

---

## 14. W13: Script API

On the weapon's game object unless noted:

| Function | Purpose |
|---|---|
| `instance_section()` | Generated section name |
| `get_parts()` | Table of path → `{id, parent, mount, pos, props, section, condition, upgrades, data}` |
| `get_mounts([path])` | Mounts and what each accepts, for the bench; for rails also `length`, `pitch`, `stops` and the occupied spans `{id, from, to}` |
| `can_fit(path, section [, pos])` | `nil` / `"clash", reason` / `true`, the same contract as `mw_resolve.check`; `pos` defaults to `default_pos` |
| `fit_part(path, item_or_section [, state [, pos]])`, `remove_part(path_or_id [, spawn])` | Commit changes; `fit_part` returns the new part's id |
| `move_part(path_or_id, pos)` | Move a part along its rail; a placement-only change (8.3) |
| `get_part_prop(path_or_id, key)`, `set_part_prop(path_or_id, key, value)` | Placement and mod state saved in the record and visible to resolve callbacks (8.3) |
| `register_part_resolve(func)` | Add an `on_part_resolve` callback (8.3) |
| `set_preview(record)`, `clear_preview()` | Bench preview (6.3) |
| `get_part_data(path)`, `set_part_data(path, string)` | Per-part blob (battery, reticle mark) |
| `get_override(key)`, `set_override(key, value)`, `iterate_overrides()` | Persistent per-gun keys, and persistent versions of `SetRPM` and the other setters |
| `get_paintjob([path])`, `set_paintjob([path,] section_or_nil)` | Gun paintjob, or a fitted part's paintjob by path; `nil` clears it. Also available on loose part items (section 13) |
| `part_attachment(path, hud)` | Read-only handle to a part's attachment (bone queries, lights) |
| `combo_section()` | Compatibility name (12.2) |
| `alife():weapon_record(se_obj)` | Read and write the record of an offline weapon |

Callbacks: `on_weapon_parts_changed(wpn)`, `on_weapon_rebuilt(wpn)`.

Console commands (with `-dbg`): `mw_dump <id>` writes the generated section to an `.ltx` in `appdata` for inspection; `mw_resolve_test`; `mw_fit`; `mw_remove`.

**First slice implemented 2026-11-25 (2h). Tested in game 2026-11-25 (`mw_ir.api()`, 38 checks pass).** `xrGame/instance_record_script.*`, registered on `game_object` (any inventory item) and on `cse_alife_inventory_item`:

| Function | Notes |
|---|---|
| `instance_section()` | `game_object` only. The generated section, or the base when the item has none |
| `get_override(key)` | The value as a string, or `nil` |
| `set_override(key, value)` | Strings, numbers and booleans are stored as text (`120`, `0.25`, `true`). `nil`, or no value, clears the key. A table is refused. Returns `false` when refused |
| `clear_override(key)` | `false` when the key wasn't set |
| `get_overrides()` | A table key → value. Replaces `iterate_overrides()` from the table above |
| `get_parts()` | The final form: path → `{path, id, parent, mount, pos, section, condition, paintjob, upgrades, props, data_size}`. A part sharing its parent and mount with another is named `<mount>#<id>`. The blob itself waits for `get_part_data` |
| `record_add_part(parent_id, mount, section [, pos])`, `record_remove_part(id)` | **Raw record edits, for tests.** Nothing is checked against the gun and nothing is drawn. `fit_part` and `remove_part` replace them in phase 5. `record_remove_part` removes children too and returns the count |
| `record_size()` | Bytes the record adds to the save |

- **Which copy is edited (4.3).** On `game_object`, edits go through the game-side copy and the event. On the server object, an edit goes through the game object while one exists, and otherwise changes the server copy, which the next spawn reads. Reads on the server object also return the game-side copy while the item is online, so both sides always report the same record.
- **No empty `luabind::object` returns.** Copying one on the way out reads a missing reference and crashes, so `get_override` returns a string pointer, and a null pointer reaches Lua as `nil`.
- **Lua state.** The functions that return a table (`get_overrides`, `get_parts`) take the caller's Lua state through luabind's `raw<1>()` policy. A value built on the main state is pushed onto the main state's stack, so a call from a coroutine (`run_string`, many callbacks) would get nothing back. The engine's older table getters (`list_bones`, `get_shaders`) have that problem.
- **Effect.** An override changes the generated section at once, but the weapon reads it on its next spawn (5.3 limits).

**Tests (2i), run 2026-11-25, all pass:** save/load (`verify()` 32 checks), a save → load round trip (`compare`, no record changed), an NPC carrying the gun offline after a level change (server copy, 15 checks), an edit made while offline that reached the game side when the gun came back online (33 checks), and `cleanup()`. Set up with `tests/instance_record` (`install.ps1`) adds `mw_ir.script`:
- `api()` exercises every function above on both objects of the gun in hand, then removes what it added;
- `prepare()` marks the gun in hand (rpm 120, a mark key, a scope with a cover, two parts on one rail), and `verify()` finds it wherever it is, online or offline, and checks the whole record, the generated section and the base identity;
- `npc_give()` and `offline_edit()` cover an NPC carrying it offline and an edit made while it is offline;
- `snapshot(name)` writes every non-empty record in the game to `appdata`, and `compare(a, b)` reports any record that changed between two snapshots, for the save → load → save round trip.

---

## 15. Where Part_bench's code goes

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

## 16. Order and estimates

Rough figures for one developer who knows the codebase, including testing.

| Phase | Workstreams | Result | Estimate |
|---|---|---|---|
| 1 | W1 (1a–1e), spike for 1f | One model instance per gun, same pose and shaders in both views | 2–3 weeks |
| 2 | W2, W3 | Per-gun record saved; generated sections; overrides persist | 2–3 weeks |
| 3 | W4 | Same-object rebuild; mechanic upgrades without detaching | 2–4 weeks |
| 4 | W5, W6 | Parts drawn by the engine from the record; rails and placement (8.3) | 4–6 weeks |
| 5 | W7, W8, W9 | Parts drive gameplay and aim; fit and remove from inventory and bench | 4–6 weeks |
| 6 | W10, W11, W14 | Icons, NPC loadouts, trade; paintjobs on guns and parts | 3–4 weeks |
| 7 | W12 | Combo map, old-save migration, mod shims | 3–6 weeks, plus content |

Engine total: about **4–7 months**. Content conversion runs in parallel from phase 4 and is the open-ended part.

Each phase ships on its own. After phase 3, Part_bench can stop cloning. After phase 4, it can drop `mw_render`.

---

## 17. Testing

- **Resolver unit tests:** port `mw_resolve_test.script` to C++, run from a console command, plus a case for every error code.
- **Placement short path:** after every `move_part`, a debug build also runs a full rebuild and compares transforms, aim keys and stats with the short path's result.
- **Save round trip:** for guns with deep part trees, save, load and compare records byte for byte. Load a save made by the current exe and check that every combo gun becomes base + parts.
- **Regression checklist:** Part_bench spikes 1, 2, 3a–c, 4, 8, 9, 11, 12 and 13, run without the scripts they replace.
- **Performance:** a scene with 30 armed NPCs; frame time before and after, with 0, 3 and 6 parts per gun.
- **Mod compatibility:** a GAMMA profile run with 3DSS, WPO, Mark Switch, Borksy's lasers and Mags Redux enabled. Log every script error that mentions a weapon section.

---

## 18. Risks and open questions

- **Old exes can't read new saves.** Bumping `SPAWN_VERSION` is one-way. Warn users and keep a save backup on first load.
- **Mod breakage.** "Replace everywhere" means any script that parses combo section names breaks until shimmed. The combo map and `combo_section()` cover the common readers. The long tail is unknown until a full GAMMA run.
- **NPC render cost.** HUD-detail models plus parts on every NPC gun may cost noticeable frame time without LODs. Measure in phases 1 and 4.
- **HUD rebind while held** (W4 step 8) is the riskiest code path. Gate it so it only runs while the gun is hidden.
- **Threads and the dynamic section store.** Check whether any worker thread reads `pSettings` while sections are added or removed. Use a lock.
- **Shared instance edge cases** (W1, 7.3): the root-bone override on drop and pickup, script animations that swap the HUD model (`script_anim_item_model`), and mods that call `HudItemData().m_model` expecting a separate model. Test drop, pickup, holster and script animations early.
- **Lost SSS HUD effects** (W1, 7.4): no raindrops on the held gun until the SSS shaders take a per-draw flag, and the held gun's glass needs a visual check.
- **Motion files and extra bones** (W1 1f) are unproven. Without them, gun mounts stay LTX offsets, and rails use `offset_end` instead of an end bone (8.3).
- **Nested bone lookup** (W6) behaves differently between Part_bench's tested build (MT-TEST 2026.10.02) and the current source. Re-test before relying on part bones.
- **Resolve callbacks that aren't reproducible** (8.3): a mod callback that reads game state can make the same record give different guns after a load. The rule is documented, but the engine can't enforce it. The debug build can run the resolver twice and log nodes that differ.
- **Magazine mods** (Mags Redux) keep their own magazine state. A `magazine` part may need an adapter for them.
- **Paintjobs keyed by texture name** (W14) rely on the gun and its parts using distinct default textures. If two sub-meshes share one texture but need different paint, the paintjob can't tell them apart. Add a `sub-mesh index → texture` form only if content needs it.
