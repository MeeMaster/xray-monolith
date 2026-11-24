# Weapon Objects: initialization, state, addons, upgrades

How a weapon goes from an `.ltx` section to a live object in xray-monolith, what gets stored, and how addons and upgrades change it. It also explains why a weapon instance is so hard to change after it spawns. §12 proposes a per-weapon configuration design to fix that.

Paths are relative to `xray-monolith/`. Line numbers are as of 2026-11-24.

---

## 1. Summary

- **A weapon instance is mostly its section name plus a few bytes of state.** The saved server entity holds the section name, world visual name, condition, ammo count/type, a 3-bit addon mask, weapon state and a list of upgrade IDs. Every other property (stats, recoil, sounds, HUD model, animations, fire points, scope list, zoom, addon compatibility) is read again from `pSettings[section]` each time the object is created.
- **The section is fixed for the object's whole life.** `cNameSect()` is set once, when the client object is created ([IGame_ObjectPool.cpp:56](../src/xrEngine/IGame_ObjectPool.cpp#L56)), and nothing changes it later. Anything that depends on the section (the HUD model most of all) can only change by creating a **new object with a different section**.
- **Upgrades replay on every spawn.** The base values always come from `Load(section)`. The engine then re-runs every upgrade on the stored list, in order, adding or overwriting fields ([inventory_item_upgrade.cpp:135](../src/xrGame/inventory_item_upgrade.cpp#L135)). Upgrades can change numbers and some addon settings. They **cannot** change the HUD section, HUD model, world model, animations, sounds or fire bones.
- **Addons are one bit each and switch bone visibility.** Scope, silencer and grenade launcher are each a bit in `m_flagsAddOnState`. Attaching one shows a bone (`wpn_scope`, `wpn_silencer`, `wpn_launcher`) that already exists in the same model. A scope that needs different geometry (a different reticle, mount or lens) therefore needs a different model, which means a different `hud` section, which means a different weapon section.
- **That is why "attach scope" swaps the weapon.** `item_weapon.attach_scope` builds the section name `<parent>_<scope>` and calls `alife_clone_weapon`. That spawns a new server entity of that section, copies a few fields across, and releases the old one ([item_weapon.script:67](../gamedata/scripts/item_weapon.script#L67), [_g_patches.script:731](../gamedata/scripts/_g_patches.script#L731), [alife_simulator_script.cpp:464](../src/xrGame/alife_simulator_script.cpp#L464)).
- **Modular attachments are a partial way out.** With `modular_attachments = true` the weapon keeps its section and draws the scope as a separate HUD model (`CAnonHudItem` in the third HUD slot). But the active scope index is not stored in the server entity, and the world model only has the one generic scope bone (see §6.3 and §10).

So your reading is right. One refinement: the instance is not literally "read from the .ltx" at runtime. It is rebuilt from the `.ltx` on each spawn, and the only state that can differ per instance is the small saved record above.

---

## 2. The four layers

| Layer | Where | What it holds | Lifetime |
|---|---|---|---|
| **Config** | `pSettings` (system.ltx and includes) | Everything: stats, `visual`, `hud`, scopes, sounds, upgrade tree | Static, global |
| **Server entity** | `CSE_ALifeItemWeapon*` in [xrServer_Objects_ALife_Items.h](../src/xrServerEntities/xrServer_Objects_ALife_Items.h) | Section name, visual name, condition, ammo, addon flags, state, upgrade IDs (+ opaque `client_data`) | Persists in the save file; exists while the weapon is offline |
| **Client object** | `CWeapon` hierarchy in `src/xrGame` | Live gameplay state, all section-derived params, HUD binding | Only while the weapon is online (on a loaded level) |
| **HUD item** | `attachable_hud_item` in [player_hud.cpp](../src/xrGame/player_hud.cpp) | First-person model (`item_visual` of the `hud` section), measures, motions | Created lazily when the weapon is first drawn |

Scripts (`item_weapon.script`, `_g_patches.script`, `aaaa_script_fixes_mp.script`) sit on top. They switch between sections by cloning server entities.

---

## 3. Class hierarchy

### Client

```
CObject / CGameObject
 └─ CPhysicItem / CInventoryItemObject (CInventoryItem)
     └─ CHudItemObject (CHudItem)            hud_sect, animations, sounds
         └─ CWeapon  (+ CShootingObject)      Weapon.h / Weapon.cpp
             ├─ CWeaponMagazined              addons attach/detach, fire modes, scope koeffs
             │   ├─ CWeaponMagazinedWGrenade  2nd ammo set, GL mode
             │   ├─ CWeaponShotgun → CWeaponAutomaticShotgun
             │   ├─ CWeaponPistol, CWeaponCustomPistol, CWeaponBinoculars, CWeaponRG6/RPG7, …
             │   └─ (legacy per-model classes: AK74, SVD, Val, …; mostly thin)
             └─ CWeaponKnife, CMissile-based, …
```

### Server

```
CSE_ALifeInventoryItem    m_fCondition, m_upgrades
CSE_ALifeDynamicObjectVisual  visual_name (+flags)
 └─ CSE_ALifeItem
     └─ CSE_ALifeItemWeapon             addon status ×3, a_current, a_elapsed, wpn_state, ammo_type,
         │                               m_addon_flags, a_elapsed_grenades, wpn_flags, m_bZoom
         └─ CSE_ALifeItemWeaponMagazined      m_u8CurFireMode (UPDATE only)
             ├─ CSE_ALifeItemWeaponMagazinedWGL   m_bGrenadeMode (UPDATE only)
             └─ CSE_ALifeItemWeaponShotGun        m_AmmoIDs (UPDATE only)
                 └─ CSE_ALifeItemWeaponAutoShotGun
```

The `class` key in the section picks which client and server classes are used.

---

## 4. Lifecycle: from section to live object

### 4.1 Server-side creation

`alife():create(section, …)` or `spawn_item` → `F_entity_Create(section)` → `CSE_ALifeItemWeapon(caSection)` ([xrServer_Objects_ALife_Items.cpp:493](../src/xrServerEntities/xrServer_Objects_ALife_Items.cpp#L493)):

- `hit_power`, `hit_type`, `ammo_class` are read from the section (the server only uses them for ALife).
- `visual_name` is set from the section's `visual`.
- `scope_status`, `silencer_status`, `grenade_launcher_status` are read from the section.
- `m_addon_flags = 0`, `a_elapsed = 0`, `ammo_type = 0`, `wpn_state = 0`.

### 4.2 Client object creation

When the entity comes online, `CLevel::g_cl_Spawn` → `Objects.Create(s_name)` → `IGame_ObjectPool::create` ([IGame_ObjectPool.cpp:52](../src/xrEngine/IGame_ObjectPool.cpp#L52)):

```cpp
CLASS_ID CLS = pSettings->r_clsid(name, "class");
CObject* O   = NEW_INSTANCE(CLS);
O->cNameSect_set(name);   // section is now fixed for the object's lifetime
O->Load(name);
```

`Load` runs **before** any server state is known, so every field is filled from the section:

| Function | Reads |
|---|---|
| `CInventoryItem::Load` ([inventory_item.cpp:95](../src/xrGame/inventory_item.cpp#L95)) | `inv_name`, `inv_weight`, `cost`, `slot`, `description`, immunities, flags |
| `CHudItem::Load` ([HudItem.cpp:84](../src/xrGame/HudItem.cpp#L84)) | **`hud_sect = r_string(section, "hud")`**, `animation_slot`, HUD FOV/nearwall |
| `CShootingObject::Load` | `rpm`, `hit_power`, `bullet_speed`, fire particles, lights |
| `CWeapon::Load` ([Weapon.cpp:609](../src/xrGame/Weapon.cpp#L609)) | `ammo_class`, `ammo_mag_size`, recoil (`cam_*`, `zoom_cam_*`), PDM, misfire, `fire_point*`, addon statuses, **scope list** (§6), silencer/GL names, zoom params, `scope_texture` for permanent scopes, safemode anims (from `hud_sect`) |
| `CWeaponMagazined::Load` ([WeaponMagazined.cpp:100](../src/xrGame/WeaponMagazined.cpp#L100)) | all sounds, silencer particles/sounds, `fire_modes`, `bullet_bones`, silencer/scope koeffs |
| `CWeaponMagazinedWGrenade::Load` | `grenade_class`, GL sounds and so on |

### 4.3 `net_Spawn`: applying the server state

`CWeapon::net_Spawn` ([Weapon.cpp:1086](../src/xrGame/Weapon.cpp#L1086)) runs in this order:

1. Computes the run-time zoom factor from what `Load` produced.
2. `inherited::net_Spawn(DC)` goes down the chain:
   - `CGameObject::net_Spawn` ([GameObject.cpp:274](../src/xrGame/GameObject.cpp#L274)):
     - sets the **world visual from `CSE_Visual::visual_name`** (falls back to the section `visual` if the model is missing)
     - `cNameSect_set(E->s_name)`
     - **`reload(section)`** → `CWeapon::reload` ([Weapon.cpp:2371](../src/xrGame/Weapon.cpp#L2371)): strap bones, world `position`/`orientation`, `holder_*_modifier` from the current scope
     - `reinit()`
     - **loads `client_data`** via `net_Load` → `CWeapon::load` (§5.2), if present
   - `CInventoryItem::net_Spawn` ([inventory_item.cpp:341](../src/xrGame/inventory_item.cpp#L341)): takes condition, then **`net_Spawn_install_upgrades(E->m_upgrades)`** (§7)
3. Copies `a_elapsed` → `iAmmoElapsed` and `m_addon_flags` → `m_flagsAddOnState`.
4. Modular attachments only: if a scope flag is set but `m_cur_scope == 0`, it **picks a random scope** (`Random.randI(1, m_scopes.size())`).
5. `m_ammoType = E->ammo_type`, state = `E->wpn_state`.
6. Rebuilds the magazine as `iAmmoElapsed` copies of the **same** cartridge (`m_DefaultCartridge` of `m_ammoType`).
7. `UpdateAddonsVisibility()` + `InitAddons()` (§6).

`CWeaponMagazinedWGrenade` installs upgrades itself **before** calling `inherited::net_Spawn`, and overrides `net_Spawn_install_upgrades` to do nothing so they don't install twice ([WeaponMagazinedWGrenade.cpp:73](../src/xrGame/WeaponMagazinedWGrenade.cpp#L73), [:1178](../src/xrGame/WeaponMagazinedWGrenade.cpp#L1178)).

### 4.4 HUD binding (lazy)

`CHudItem::HudItemData()` ([HudItem.cpp:953](../src/xrGame/HudItem.cpp#L953)) creates `attachable_hud_item` the first time it's needed and calls `attachable_hud_item::load(hud_sect)` ([player_hud.cpp:619](../src/xrGame/player_hud.cpp#L619)):

- `item_visual` → `Render->model_Create(...)`: **the first-person model**
- `attach_place_idx`, `hud_item_measures::load` (hands offsets, aim/GL/alt aim positions, fire bones, inertion)
- hand motions for this `hud_sect`

The HUD section comes only from the weapon section's `hud` key. No per-instance value exists in the server entity or client data.

### 4.5 Teardown

- Going offline: `CSE_ALifeDynamicObject::switch_offline` → **`clear_client_data()`** ([alife_dynamic_object.cpp:63](../src/xrGame/alife_dynamic_object.cpp#L63)). Weapons don't override `keep_saved_data_anyway`, so all client-only state is dropped (§5.2).
- Release: the entity is removed and its ID is freed.

---

## 5. What is stored, and where

### 5.1 Server entity (saved in the `.scop` save; travels while the weapon is offline)

STATE (save file) and UPDATE (periodic net sync from client to server) packets:

| Field | STATE | UPDATE | Notes |
|---|:-:|:-:|---|
| `s_name` (section) | ✔ | | Fixed at creation |
| `visual_name` + flags | ✔ | | **Per-instance**, via `CSE_Visual::visual_write`. A different world model per instance is already possible |
| `m_fCondition` | ✔ | ✔ | |
| `m_upgrades` (vector of IDs) | ✔ | | Also updated by the `GE_INSTALL_UPGRADE` event |
| `a_current` | ✔ | | Mostly unused for magazined weapons |
| `a_elapsed` | ✔ | ✔ | Count only; per-round ammo types are not stored |
| `wpn_state` | ✔ | ✔ | |
| `m_addon_flags` (u8: scope=1, GL=2, silencer=4) | ✔ | ✔ | Client → server through `net_Export`/`UPDATE_Read` |
| `ammo_type` | ✔ | ✔ | |
| `a_elapsed_grenades` (6-bit count + 2-bit type) | ✔ | | GL rounds |
| `m_u8CurFireMode` | | ✔ | **Not in STATE** |
| `m_bGrenadeMode` | | ✔ | **Not in STATE** |
| `m_AmmoIDs` (shotgun) | | ✔ | **Not in STATE** |
| `m_bZoom`, `wpn_flags` | | ✔ | |

### 5.2 Client data (`CGameObject::net_Save` → `save()`; stored as raw bytes in `CSE_Abstract::client_data`)

`CWeapon::save` ([Weapon.cpp:1241](../src/xrGame/Weapon.cpp#L1241)) and `CWeaponMagazined::save`:

- `iAmmoElapsed`, **`m_cur_scope`**, `m_flagsAddOnState`, `m_ammoType`, zoom-mode flag, NV-remember flag
- `m_iQueueSize`, `m_iShotNum`, `m_iCurFireMode`
- `CInventoryItem::save`: item place, condition, physics state

This data only exists while the object stays online. **It is cleared when the weapon goes offline** (for example when an NPC carrying it leaves the online radius). From that point the weapon gets back only what the server entity has.

### 5.3 Not stored anywhere (rebuilt from the section on every spawn)

All numeric stats, recoil, dispersion, RPM, `m_ammoTypes`, `iMagazineSize`, `m_scopes`, silencer/GL names, sounds, particles, `hud_sect`, HUD model, measures, animations, fire points. The base values come from `Load(section)`, then upgrades are applied on top (§7).

The script setters exposed on `CWeapon` (`SetHitPower`, `SetRPM`, `Set_PDM_*`, `SetCam*`, `SetZoomFactor`, `SetAmmoMagSize`, …) only change the live client object. **The changes are lost** on save/load, on going offline, and on any clone. Scripts that use them have to store the values themselves and apply them again (usually from `CGameObject_NetSpawn` or `actor_on_item_*`).

---

## 6. Addons

### 6.1 Model

- Status per addon type, from the section: `scope_status`, `silencer_status`, `grenade_launcher_status`. Values are `0 = disabled`, `1 = permanent`, `2 = attachable` (`ALife::EWeaponAddonStatus`).
- State: `m_flagsAddOnState` (`eWeaponAddonScope=1`, `eWeaponAddonGrenadeLauncher=2`, `eWeaponAddonSilencer=4`).
- `IsXAttached()` = (attachable **and** flag set) **or** permanent ([Weapon.cpp:1968](../src/xrGame/Weapon.cpp#L1968)).
- `use_engine_attachments = false` turns off all engine addon handling (visibility, `InitAddons`).

### 6.2 Visual effect: bone toggling only

`UpdateAddonsVisibility` (world) and `UpdateHUDAddonsVisibility` (HUD) ([Weapon.cpp:2010](../src/xrGame/Weapon.cpp#L2010), [:2071](../src/xrGame/Weapon.cpp#L2071)) show or hide fixed bone names on the **existing** model:

| Bone | Meaning |
|---|---|
| `wpn_scope` | scope mesh (legacy) |
| `wpn_silencer` | silencer mesh |
| `wpn_launcher` | GL mesh |
| `wpn_scoped_hide` / `wpn_scoped_unhide` | modular only: iron sights to hide or mount to show when a scope is on |

So the weapon's model must already contain every addon mesh. One `wpn_scope` bone means one scope shape per model.

### 6.3 Scope selection: three mechanisms

**A. Legacy `scopes_sect` (engine)**: `scopes_sect = sect_a, sect_b` in the weapon section ([Weapon.cpp:815](../src/xrGame/Weapon.cpp#L815)). Each entry is a section with `scope_name` (the addon item section), `scope_x/y` (inventory icon offset), `scope_zoom_factor`, `scope_texture`, and so on. `m_cur_scope` indexes the list. Without `scopes_sect`, the weapon section itself is the single scope entry.

**B. Modular attachments (engine)**: `modular_attachments = true` + `modular_scope_group = grp`, where `[grp] scopes = …` lists the **addon item sections** directly ([Weapon.cpp:803](../src/xrGame/Weapon.cpp#L803)).
- On attach, `m_scopeItem = new CAnonHudItem; m_scopeItem->Load(scope_section)`, and the scope's own `hud` section is attached in `SCOPE_ATTACH_IDX` of `player_hud` ([WeaponMagazined.cpp:1387](../src/xrGame/WeaponMagazined.cpp#L1387), [:633](../src/xrGame/WeaponMagazined.cpp#L633)). This gives a separate first-person scope model with no change to the weapon section.
- Zoom, texture and koeff data come from the scope item section (`GetScopeName()` returns the section directly).
- The world model gets nothing scope-specific: `wpn_scope` is shown or hidden as in legacy mode.

**C. Section swap (scripts, the default Anomaly path)**: `[wpn_x] scopes = scope_a, scope_b` and `parent_section = wpn_x`, plus a hand-written child section `[wpn_x_scope_a]:wpn_x` with its own `visual`, `hud` (→ different `item_visual`), zoom, `scope_texture`, and so on. Attaching runs `item_weapon.attach_scope`:

```lua
local child_section = parent_section .. "_" .. item:section()
local new_weapon = alife_clone_weapon(alife_object(weapon:id()), child_section)
if new_weapon then alife_release(item) end
```

Detaching clones back to `parent_section` and spawns the scope item, which is found by `string.find(weapon:section(), k)` over `addons_table`. In engine terms the child weapon has **no scope addon at all**: its scope is "permanent" or baked into the model. That is why the engine's `wpn_scope` flag and `scopes_sect` aren't used here.

### 6.4 Attach and detach flow (engine)

- Script `obj:weapon_addon_attach(item)` or the inventory UI → `CWeaponMagazined::CanAttach` / `Attach` ([WeaponMagazined.cpp:1287](../src/xrGame/WeaponMagazined.cpp#L1287)):
  - matches `item.cNameSect()` against `m_scopes[i]` (modular) or `m_scopes[i].scope_name` (legacy), or `m_sSilencerName` / `m_sGrenadeLauncherName`
  - sets the flag and `m_cur_scope`, then **destroys the addon item object** (`DestroyObject`). The addon stops existing as an entity; only its section name can be recovered later.
  - `UpdateAddonsVisibility()` + `InitAddons()`
- `Detach(section, spawn)` clears the flag. `CInventoryItem::Detach` **spawns a fresh item of that section** in the owner's inventory ([inventory_item.cpp:295](../src/xrGame/inventory_item.cpp#L295)). Condition and upgrades of the addon item are not kept.
- `InitAddons` ([WeaponMagazined.cpp:1516](../src/xrGame/WeaponMagazined.cpp#L1516)) reads the current scope's `scope_zoom_factor`, `scope_texture`, `scope_nightvision`, `scope_dynamic_zoom`, `scope_alive_detector` and `zoom_step_count`, then switches silencer particles, sounds, lights and koeffs.
- Koeffs: `LoadScopeKoeffs` / `LoadSilencerKoeffs` read multiplier fields (`*_k`, clamped 0.01–2.0) from the addon section. They're applied as `cur_*_koef` while attached.

---

## 7. Upgrades

### 7.1 Data model

- `inventory::upgrade::Manager` (one per ALife) is built from `[upgraded_inventory]`. Each listed item section becomes a `Root`, which owns `Group`s, which own `Upgrade`s. Each `Upgrade` points to a **property section** (`upgrade->section()`) that holds the field deltas.
- Roots are looked up by **exact item section** (`get_root(item.m_section_id)`). A cloned child section needs its own root in `upgraded_inventory`, or its upgrades are not verified (§10).
- Per-item installed list: `CInventoryItem::m_upgrades` on the client, `CSE_ALifeInventoryItem::m_upgrades` on the server. The client sends `GE_INSTALL_UPGRADE` to the server ([xrServer_process_event.cpp:273](../src/xrGame/xrServer_process_event.cpp#L273)).

### 7.2 Install pipeline

`Manager::upgrade_install(item, id, loading)` ([inventory_upgrade_manager.cpp:366](../src/xrGame/inventory_upgrade_manager.cpp#L366)):

1. `can_install` checks dependencies, group exclusivity and "known" status.
2. If not loading: `item.pre_install_upgrade()` ([inventory_item_upgrade.cpp:260](../src/xrGame/inventory_item_upgrade.cpp#L260)) **unloads the magazine(s) and detaches every addon** (each one spawns back as an item).
3. `item.install_upgrade(section)` → virtual `install_upgrade_impl` chain:
   - `CInventoryItem`: `cost`, `inv_weight`, inventory icon rect, `inv_name`/`description`, sprint/ruck flags, immunities
   - `CWeapon` ([WeaponUpgrade.cpp:23](../src/xrGame/WeaponUpgrade.cpp#L23)): ammo class/mag size, dispersion and recoil, hit/impulse/speed/RPM, **addon statuses and scope list**, HUD inertion
   - `CWeaponMagazined`: `fire_modes`, and so on
4. `run_effects`, `add_upgrade`.

### 7.3 Semantics

- `process_if_exists` **adds** the value (`value = value + ini`). `process_if_exists_set` **overwrites** it ([inventory_item_inline.h](../src/xrGame/inventory_item_inline.h)). Whether a field is a delta or an absolute value is decided per field in C++, not in the config.
- **Upgrades replay on every spawn.** `net_Spawn_install_upgrades`:
  1. clears `m_upgrades`
  2. `init_install`: installs the section's `installed_upgrades` (defaults)
  3. installs every saved ID again with `loading = true`

  The result depends on the **current** `.ltx` files and on install order. Changing an upgrade section changes every existing weapon that has it, retroactively.
- Upgrades work on **client-object fields** only. The fields that set a weapon's look (`hud_sect`, `visual`, sounds, animations, fire bones, `bullet_bones`) are read once in `Load`/`reload`, and no upgrade handler touches them. **An upgrade cannot change the weapon's model.** In Anomaly, visual "upgrades" are therefore done by scripts that clone into another section.
- `install_upgrade_addon` can switch an addon status and replace the scope list. Scope sections are taken from the upgrade section (`scopes_sect`, `modular_scope_group`, or the upgrade section itself). In the attachable branch with `scopes_sect`, it **appends** to `m_scopes` without clearing it first ([WeaponUpgrade.cpp:267](../src/xrGame/WeaponUpgrade.cpp#L267)).

---

## 8. The script layer: `alife_clone_weapon`

Engine side, `alife():clone_weapon(se, section, pos, lvid, gvid, parent, register)` ([alife_simulator_script.cpp:464](../src/xrGame/alife_simulator_script.cpp#L464)):

```cpp
clone = spawn_item(section, ...);          // new entity, new ID, constructed from the NEW section
clone->wpn_flags     = src->wpn_flags;
clone->m_addon_flags = src->m_addon_flags;
clone->m_fCondition  = src->m_fCondition;
clone->ammo_type     = src->ammo_type;
clone->m_upgrades    = src->m_upgrades;
clone->set_ammo_elapsed(src->get_ammo_elapsed());
```

Script side, `_G.alife_clone_weapon` ([_g_patches.script:731](../gamedata/scripts/_g_patches.script#L731)) also copies the `alife_storage_manager` per-object table (keyed by ID), copies `item_parts` condition data, registers the new entity, and **releases the old one**.

**What the clone loses or changes:**

| Lost / changed | Why |
|---|---|
| Object ID | New entity. Every external reference by ID breaks (scripts must remap; the patch handles only `se_object` storage and `item_parts`) |
| `visual_name` | Comes from the new section, so any per-instance visual override is reset |
| `a_elapsed_grenades` | Not copied, so loaded GL grenades disappear |
| Fire mode, GL mode, queue size, `m_cur_scope`, zoom state | Only in UPDATE or client data; not copied |
| Mixed-ammo magazine | Only `ammo_type` + count are kept |
| `name_replace`, custom `m_ini_string` | Not copied |
| Upgrades | IDs are copied, but they replay against the **new** section's root and base values |
| Inventory slot / position, active-item state | Respawned as a new item; the HUD re-draws |

Every attach or detach is a full release + spawn + inventory re-placement + HUD re-load.

---

## 9. Why weapons are hard to change

1. **The section is the object's identity, and it's immutable.** `cNameSect` is set at construction and every subsystem reads `pSettings[cNameSect()]` lazily and repeatedly (`GetScopeName()` even reads `scope_name` from the INI every call, [Weapon.h:359](../src/xrGame/Weapon.h#L359)).
2. **The HUD model is a property of the section, not the instance.** `hud_sect` comes from `Load`, `attachable_hud_item` loads `item_visual` from it, and no runtime path changes it. The world visual is the opposite: it is per-instance and saved.
3. **Addons are flags plus bone toggles**, so the model must contain all addon geometry. Only one scope shape fits per model.
4. **The upgrade system can't express visuals.** It only patches numeric and some addon fields on the live client object, and it replays on every spawn.
5. **Per-instance state is very small.** About 15 bytes of weapon state go into the save. Anything richer (scope index, mixed magazine, script-set stats) either lives in client data, which is lost when the weapon goes offline, or isn't persisted at all.
6. **Addon items aren't kept as objects.** Attaching destroys the addon entity and detaching spawns a new one from the section, so addon instances can't carry condition or upgrades either.

---

## 10. Bugs and quirks found while reading

Read from the code only; none of these were reproduced in-game.

1. **`hit_power_critical` upgrade writes zeros.** [WeaponUpgrade.cpp:180](../src/xrGame/WeaponUpgrade.cpp#L180) reads the value into `s_sHitPower` but parses `s_sHitPowerCritical`, which is still empty. Any upgrade with `hit_power_critical` sets all four difficulty values to `atof("") = 0`.
2. **The modular scope index doesn't survive going offline.** `m_cur_scope` is only in client data, which `switch_offline` clears. On the next spawn `net_Spawn` sees scope flag = 1 and `m_cur_scope == 0`, and **picks a random scope** ([Weapon.cpp:1109](../src/xrGame/Weapon.cpp#L1109)). On the actor this is mostly hidden, because the actor's inventory stays online and client data survives a save/load.
3. **Modular scope koeffs probably go stale after load.** When `m_cur_scope != 0` comes back from client data, `net_Spawn` skips `LoadScopeKoeffs()`. `CWeaponMagazined::Load` ran it earlier with the flag still 0, so for modular weapons nothing was loaded. `InitAddons` then applies `m_scope_koef` at its defaults. The scope HUD item is rebuilt lazily in `on_a_hud_attach`, so it looks right, but the stat multipliers are probably lost until the scope is reattached.
4. **`install_upgrade_addon` appends scopes in the `scopes_sect` branch** without `m_scopes.clear()` (the modular branch clears). Installing such an upgrade duplicates or extends the list, and `m_cur_scope` may now point to a different entry.
5. **`DetachScope` sets `m_cur_scope = NULL` (0) in a loop** without breaking. That's harmless, but for modular weapons index 0 also means "no scope / pick random", so 0 is overloaded.
6. **The silencer glow setup in `CWeapon::Load` has no effect.** `renderable.visual` isn't set yet at `Load` time (the visual comes from the server entity in `net_Spawn`), so the `MarkAsGlowing` branch in [Weapon.cpp:940](../src/xrGame/Weapon.cpp#L940) never finds the bone on first load.
7. **Mixed magazines collapse after reload.** `net_Spawn` refills the magazine with `iAmmoElapsed` copies of `m_ammoTypes[ammo_type]`, so the per-round types in `m_magazine` aren't kept.
8. **Shotgun `m_AmmoIDs`, `m_u8CurFireMode`, and `m_bGrenadeMode` are UPDATE-only** and never written to STATE, so they don't persist on their own.
9. **Upgrades on cloned sections.** `upgrade_verify` checks the root and its membership with `VERIFY2`, which is debug-only. In release, upgrades copied onto a child section that has no `upgraded_inventory` root aren't validated, and `init_install` skips that section's defaults.

---

## 11. Levers for a more mutable weapon (directions, not a design)

Starting points, ordered by how much existing code each one reuses:

- **Per-instance world visual already works.** `CSE_Visual::visual_name` is saved per object. A setter that writes it (and calls `cNameVisual_set` + `OnChangeVisual` on the client) is cheap.
- **Make `hud_sect` per-instance.** Store an optional HUD-section override in the server entity (a new STATE field, versioned like the existing `m_wVersion > N` checks). Apply it in `net_Spawn` before `HudItemData()` is created, and add a runtime "re-bind HUD" path: delete `m_attachable`, then `player_hud::attach_item` again. The `CAnonHudItem` path in §6.3 B shows HUD models can already be rebuilt at runtime.
- **Promote the modular-attachment model.** It already keeps the weapon section stable and draws the scope as its own HUD model. To be complete it needs:
  - `m_cur_scope` (or better, the attached scope **section**) in STATE
  - koeff loading in `net_Spawn` (bug 3)
  - a world-model counterpart (child visual or bone-attached model)
  - silencer and GL slots in the same style
- **Store addon identity, not one bit.** Replace `m_addon_flags` (or add alongside it) a small list of slot → addon section, and optionally slot → addon condition/upgrades. Attach and detach then become state changes on one object instead of destroying and spawning.
- **A "visual" channel for upgrades.** Add handlers in `install_upgrade_impl` for keys like `hud`, `visual`, `scope_hud`, `bone_visibility` that write to the per-instance overrides above. Because upgrades replay on spawn, the overrides wouldn't need their own persistence.
- **Persist script-set stats**, or route them through a per-instance "modifier" list that is replayed like upgrades.

Whatever the approach, `alife_clone_weapon` and the `parent_section`/child-section convention would become a compatibility path rather than the main mechanism.

---

## 12. Proposed design: per-instance weapon configuration

> Status: **proposal**. Nothing here is implemented. Sizes and effort are estimates from reading the code; nothing was measured or prototyped.

### 12.1 What the problem actually is

When a weapon fires, it doesn't look anything up in the `.ltx`. `Load(section)` copies section values into member fields (for example `fOneShotTime`, `fvHitPower`, `m_fStartBulletSpeed` in [ShootingObject.cpp:72-168](../src/xrGame/ShootingObject.cpp#L72-L168)), and gameplay reads those fields. Each weapon already has its own changeable copy of its stats, and the script setters (`SetHitPower`, `SetRPM`, …) already change it.

The problems are:

1. **That copy isn't saved.** Only the record in §5.1 is.
2. **The copy is rebuilt from the section on every spawn** (save load, going online, clone), and upgrades are replayed on top. Direct changes are lost.
3. **Some state is bound once:** HUD section and model, sounds, animations, fire bones. There's no path to rebind them.
4. **A few values are re-read from the `.ltx` at runtime:** `GetScopeName()`, `GetScopeX/Y()`, cartridge data on reload, `InitAddons()`.

So this is a problem of **storing the per-weapon configuration and re-applying it after a rebuild**. The firing code doesn't need to change.

### 12.2 Today's trade-off

| | Now (section per combination) | Proposed (configuration per weapon) |
|---|---|---|
| Content on disk | weapons × combinations: one child `.ltx` section, `hud` section and model per combination | weapons + attachments models; no child sections |
| NPC loadout | rolls a combination, which means a child section such as `wpn_ak74_pso1` (Anomaly loadout scripts aren't in this repo) | rolls attachments into the weapon's slot dictionary; same base section |
| Attach / detach | `alife_clone_weapon`: release + spawn, new ID, state lost (§8) | state change on the same object, same ID |
| RAM (models) | each distinct combination model is loaded once and shared ([ModelPool.cpp:235](../src/Layers/xrRender/ModelPool.cpp#L235)) | base + each attachment model loaded once and shared; usually less in total |
| Rendering cost | 1 draw per weapon | 1 + N attachments per weapon |

### 12.3 Representing dictionaries in `.ltx`

LTX has no nested structures. A section is a flat `key = value` list, values are strings (comma lists are split with `_GetItem`), and sections can inherit with `[child]:parent1,parent2`. Dictionaries can still be expressed:

- **A section is a dictionary.**
- **Nesting is by reference:** a value names another section.
- **A small map fits on one line:** `attachments = scope:scope_pso1, fwd_rail:grip_v`, parsed by hand.

```ini
[wpn_ak74_i1234]:wpn_ak74        ; generated per weapon, inherits everything from the base
base_obj    = wpn_ak74
attachments = wpn_ak74_i1234_att
upgrades    = up_a_ak74, up_b_ak74

[wpn_ak74_i1234_att]
scope        = scope_pso1
forward_rail = grip_vertical
```

Upgrade **trees** stay where they are (static definitions under `[upgraded_inventory]`). Each weapon only needs its list of installed upgrade IDs, which it already has (`m_upgrades`).

### 12.4 Data model: a per-weapon record on the server entity

The configuration lives in the **server entity**, not in the global config. That's where saving, versioning and cloning already work:

```
WeaponInstanceRecord   (on CSE_ALifeItemWeapon, or more generally CSE_ALifeInventoryItem)
  base section          = s_name (already saved)
  slots                 : map<slot_id, attachment_section>    e.g. scope → scope_pso1, fwd_rail → grip_v
  slot state (optional) : map<slot_id, {condition, upgrades}> keeps addons as real objects (§9.6)
  upgrades              : vector<upgrade_id>                  existing m_upgrades
  overrides             : map<key, value>                     any engine or script key (§12.6)
```

- **Unmodified weapons** store an empty record, so their cost is about zero.
- **The scope index goes away:** the attached scope **section** is stored instead, which fixes bugs 2 and 3 in §10.
- **The 3-bit addon flags** become derived from `slots`, and are kept for compatibility (UPDATE packets, scripts that call `weapon_is_scope`, …).
- **`clone_weapon` copies the record,** for as long as cloning still exists.

### 12.5 Saving

Each object is saved by `CSE_Abstract::Spawn_Write`, which writes the state block behind a u16 size prefix ([xrServer_Object_Base.cpp:248](../src/xrServerEntities/xrServer_Object_Base.cpp#L248)). `Spawn_Read` reads it back with checks on `m_wVersion` (`SPAWN_VERSION = 128` in [xrServer_Objects.h:166](../src/xrServerEntities/xrServer_Objects.h#L166)).

- **Write the record at the end of the state block:** after `STATE_Write`, inside the size frame. Read it after `STATE_Read` from the remaining bytes, gated on `m_wVersion > 128`, and bump `SPAWN_VERSION` to 129.
- **Why the end:** every existing field keeps its offset. Anomaly scripts parse item and weapon packets byte by byte, and putting the record mid-chain (for example in `CSE_ALifeInventoryItem::STATE_Write`) would shift their fields.
- **Old saves** load with an empty record. **New saves** don't load in older exes.
- **Size limit:** the whole object packet must fit in `NET_PacketSizeLimit` (16 KB). That's ample for slots and overrides, but not for blobs.
- **Do not use client data** (`CGameObject::save`) for any of this. It's wiped when the object goes offline (§4.5).

### 12.6 Applying it: the generated section

At spawn, the record is **turned into a generated section** (`[<base>_i<id>]:<base>` + overrides + slot data) that the client object loads instead of the base section.

- **Why:** every existing `pSettings->r_*(section, key)` read (hundreds across stats, HUD, sounds, scopes) works unchanged through inheritance. No per-field refactor.
- **Lifetime:** created on going online, freed on going offline. The record is the source of truth, and the generated section is a cache built from it. Because it's rebuilt from base + differences each time, changes to the base section from mod updates reach existing weapons.
- **Storage:** `pSettings` is a `const` global. Sections are kept **by value in one sorted vector** (`CInifile::DATA`), and `w_string` inserts into it ([Xr_ini.cpp:1833](../src/xrCore/Xr_ini.cpp#L1833)). Inserting at runtime can move every section and break code that still holds a reference to one. Options:
  - a separate instance `CInifile`, with section lookups checking it before `pSettings`
  - stable storage for sections (node-based, or a vector of pointers)
- **Identity:** a lot of script and engine code treats `section()` as "what this weapon is": trader lists, `parent_section`, ammo and repair compatibility, upgrade roots (`get_root(m_section_id)`), icons, names.
  - `section()` / `cNameSect()` keep returning the **base** section.
  - The generated name is exposed separately as `instance_section()`.
  - The upgrade manager looks up roots by base section.
- **Upgrades** stay replayed (§7.3), now against the generated section's values.
- **Script-set stats** (`SetHitPower` and the rest) can be made persistent by writing to `overrides` instead of only the live field.

An alternative to the generated section is to make `process_if_exists` take a value source (`CInifile*` or the override map) and treat `overrides` as a final upgrade that's always replayed. That's less invasive for numeric fields, but it doesn't cover anything that isn't handled as an upgrade (HUD, sounds, scope lists). The generated section covers everything.

### 12.7 Making `Load` safe to re-run (`ReloadConfig`)

This is needed so that changing slots or overrides updates the **same object** instead of cloning it.

Problems in the current `Load` chain:

| Problem | Where |
|---|---|
| Lists appended to but never cleared: `m_ammoTypes`, `m_scopes`, `m_aFireModes`, `bullets_bones` (+ `bullet_cnt++`), `m_ammoTypes2` | `CWeapon`, `CWeaponMagazined`, `CWeaponMagazinedWGrenade` |
| Sounds added again, so every alias would play twice | `HUD_SOUND_COLLECTION_LAYERED::LoadSound` ([HudSound.cpp:422](../src/xrGame/HudSound.cpp#L422)), called about 30 times in the chain |
| `m_UIScope` created again without freeing the old one | `CWeapon::Load` |
| Runtime state overwritten: `iAmmoElapsed = ammo_elapsed`, `m_iCurFireMode`, zoom params | `CWeapon`, `CWeaponMagazined` |
| Upgrade effects lost (base values reset) | every upgradeable field |
| HUD model bound once in `m_attachable` and never re-bound | `CHudItem` |

Steps for `ReloadConfig()`:

1. Only run while the weapon is idle or hidden, never during reload or fire. Otherwise queue the request.
2. Save the runtime state: ammo count, magazine contents, ammo type, fire mode, slots, condition, weapon state.
3. Clear the lists and sounds; free `m_UIScope`.
4. Rebuild the generated section from the record.
5. Run `Load(section)` and `reload(section)`.
6. Replay upgrades with `net_Spawn_install_upgrades`.
7. Restore the runtime state, clamped to the new config (the magazine may be smaller, the ammo index may no longer exist).
8. Run `InitAddons()` and `UpdateAddonsVisibility()`.
9. If the HUD section changed, rebuild `m_attachable`. If the weapon is currently shown, detach it from the player HUD and attach it again.
10. If the world model changed: `cNameVisual_set`, `OnChangeVisual`, and rebuild the physics shell.

**Effort:** 18 classes override `Load` (`Weapon*`, `Missile`, `Grenade`, `GrenadeLauncher`). Most are trivial. The real work is in `CWeapon`, `CWeaponMagazined`, `CWeaponMagazinedWGrenade`, `CWeaponShotgun`/`CWeaponAutomaticShotgun`, `CWeaponStatMgun` and `CHudItem`. Estimate: **300–500 lines**, mostly cleanup and save/restore. The riskiest part is step 9, rebinding the HUD while the weapon is held.

### 12.8 Attachment slots

The engine has three on/off addon flags and nothing else, so new slots (for example `forward_rail`) need C++ regardless of how they're stored:

- **Slot definitions** per weapon section, for example `slots = scope, muzzle, fwd_rail, launcher`, with a compatible-attachments list for each slot. This generalizes `scopes` / `silencer_name` / `grenade_launcher_name`.
- **Stat effects:** generalize the scope/silencer multipliers (`LoadScopeKoeffs`, `LoadSilencerKoeffs`, `cur_*_koef`) to one multiplier set per slot, combined on apply.
- **Behaviour hooks:** keep the scope (zoom, texture, night vision), muzzle (fire point, particles, sounds, lights) and launcher handling. Other slots are stats-only at first.
- **First-person rendering:** extend the modular-attachments path (`CAnonHudItem` in `SCOPE_ATTACH_IDX`, [WeaponMagazined.cpp:633](../src/xrGame/WeaponMagazined.cpp#L633)) from one scope slot to N slots, each attached to a named bone of the weapon's HUD model.
- **World rendering:** nothing like this exists yet for weapons on NPCs or on the ground. It needs bone-attached child models on the world visual. **This is probably the biggest single piece of the redesign.**
- **Addons as real objects (optional):** store the attachment's condition and upgrades in the slot state instead of destroying the item on attach and spawning a fresh one on detach (§6.4).
- **NPC loadouts:** roll slot contents into the record at spawn instead of picking a child section.

### 12.9 Memory and save size (estimates)

Every weapon already has its own server entity, and a client object while online. These are the **added** costs only.

- **One `.ltx` entry** is two `shared_str` pointers (≈16 bytes). The strings themselves are interned and shared.
- **A fully copied weapon section** (≈250–400 keys with inheritance) is ≈4–7 KB.

| Approach | RAM | Save |
|---|---|---|
| Full section copy for every weapon in ALife (several thousand) | ~20–40 MB | large; also not needed |
| **Record for every weapon + generated section only for online weapons** (a few hundred) | ~1–2 MB | ~0 for unmodified weapons; ~50–100 B for a weapon with ~3 slots, so a few hundred KB in total |

String section names in the record could be replaced with indices into a per-save string table to make saves smaller. Weapon counts and section sizes are based on typical Anomaly content, not measured.

### 12.10 Suggested order

1. **Record + save format** (§12.4–12.5), with script API: `set_property` / `get_property` / `iterate_properties`, slot get/set, and `clone_weapon` copying the record. Small: a few hundred lines.
2. **Generated section + base-section identity** (§12.6): instance ini storage, `instance_section()`, upgrade roots looked up by base.
3. **`ReloadConfig`** (§12.7): live changes without cloning, same ID.
4. **Slots replacing the addon bits** (§12.8): stats and first-person rendering first, then world rendering.
5. **Migrate content and scripts:** `item_weapon.attach_scope`/`detach_scope` and loadouts write slots instead of cloning, and child sections become optional for compatibility.

### 12.11 Open questions to check before implementing

- **Load order:** where save loading creates the server entities, and whether anything reads the generated section before the record is read. The entity constructor reads only the **base** section, which should be safe.
- **Rebuilding the client object with the same ID:** whether an online item's client object can be destroyed and re-created while its server entity and ID stay. This would be an alternative to `ReloadConfig`.
- **Anomaly scripts that parse packets** (for example `utils_stpk`): confirm they tolerate trailing bytes in the state block.
- **Threads:** whether any worker thread reads `pSettings` while sections could be inserted.
