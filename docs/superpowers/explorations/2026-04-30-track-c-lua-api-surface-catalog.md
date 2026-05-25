# Track C — Lua API Surface Catalog

**Date:** 2026-04-30
**Mode:** Design only. No code changes. Catalog of the ~290 ABL extensions enumerated at `code/ablmc2.cpp:7791-8126`, classified into Lua exposure tiers for `mc2_api_version=1`.
**Predecessors:**
- [`2026-04-29-track-c-lua-scripting-status.md`](2026-04-29-track-c-lua-scripting-status.md)
- [`2026-04-30-track-c-lua-implementation-shape.md`](2026-04-30-track-c-lua-implementation-shape.md)
- Spec: [`specs/2026-04-29-modders-paradise-roadmap-design.md`](../specs/2026-04-29-modders-paradise-roadmap-design.md) §5.4, §5.5

The status doc confirmed **290 active `ABLi_addFunction` calls**. Reading the registration block in full produces the actual count of **289** active calls (1 commented out: `setobjectivetimer` / `checkobjectivetimer`, plus 3 commented `getstrikes`/`setstrikes`/`addstrikes`, plus `selectunit`). Within rounding the "290" figure is correct.

This catalog assigns every active registration to one of four tiers and proposes a coherent Lua-side namespace. It is a design artifact, not a generator input — when bindings land, the per-name decisions can be revisited, but the namespacing and tier policy should not.

> **⚠️ Two updates from [`2026-04-30-track-c-blocking-questions-resolution.md`](2026-04-30-track-c-blocking-questions-resolution.md):**
> 1. **Namespace tree is now LOCKED.** The blocking-questions doc §Q2 produced the final `mc2.<sub>.<verb>` tree with 5 close-call rationales documented. Treat the blocking-questions doc as the authoritative source for namespace placement; the namespace-tree section below is the *proposal* it ratified, with these per-binding adjustments: `mc2.object.apply_damage` (renamed from `damage` to disambiguate from `damage_state`/`damage_pts`), `mc2.object.is_in_area` (kept, not `mc2.area.contains_object`), `mc2.object.is_dead_or_fled` (kept, not `mc2.pilot.*`), `mc2.object.teleport`, `mc2.video.*` (not `mc2.movie.*`).
> 2. **`magicpatrol` / `magicguard` / `magicescort` are RECLASSIFIED from EXPERIMENTAL to INTERNAL.** Per blocking-questions §Q3, these primitives are stub no-ops in the stock profile and only meaningful for unsupported Omnitech content. The `replaces_corebrain` opt-in concept in `mod.json` is rejected — the gate that matters is the profile-launcher's `.abx` audit at `ABLi_addFunction` registration time, not a per-mod manifest flag. **Do not expose these three in `mc2_api_version=1`**; revisit per-binding when stock content actually emits the events they would gate.

---

## 1. Tier definitions

| Tier | Meaning | Lua-exposed? | API-version contract |
|------|---------|--------------|----------------------|
| **STABLE** | Part of `mc2_api_version=1` modder API. Signature locked. | Yes | Removal/rename bumps `mc2_api_version`. |
| **EXPERIMENTAL** | Exposed but tagged unstable; available under `mc2.experimental.*` only. | Yes (gated) | May change without bumping `mc2_api_version`. |
| **INTERNAL** | Used by stock missions / corebrain library. Wraps engine state better-modeled by upcoming Lua subsystems (e.g. memory slots, AI alarms). Not exposed. | No | Engine-side only; no contract. |
| **DEPRECATE** | Kept registered for ABL compatibility but is dead-code, redundant alias, or a misfeature. Should not propagate. | No | No contract. |

---

## 2. Namespace tree

The Lua API uses a flat-but-scoped tree under the global `mc2` table. Subnamespaces group by *engine subsystem*, not by ABL prefix (ABL prefixes are inconsistent — `objectStatus` vs `getObjectActive` vs `objectExists`).

```
mc2.log                     -- single function: debug-print channel
mc2.time                    -- mission clock, frame timing
mc2.object.*                -- per-object queries / mutations / spawning
mc2.objective.*             -- mission objective slots
mc2.timer.*                 -- named/numeric timers
mc2.area.*                  -- trigger zones, in-area tests
mc2.global.*                -- global ABL value table (cross-script state)
mc2.team.*                  -- side / team / commander queries
mc2.mission.*               -- mission status, win/lose, abort, end
mc2.audio.*                 -- music, sfx, betty, speech, wave
mc2.video.*                 -- play_video, fade, movie mode
mc2.camera.*                -- camera state read/write
mc2.ui.*                    -- HUD, debug window, tutorial callouts, large-msg
mc2.weapon.*                -- weapons-ready/locked/in-range, ranges
mc2.order.*                 -- tactical order issuance / inspection
mc2.pilot.*                 -- pilot state, wounds, hire, add-new
mc2.economy.*               -- resource points, money, salvage
mc2.strike.*                -- airstrike, sensor, artillery, repair, salvage support
mc2.gate.*                  -- gate lock/open/closed
mc2.ai.*                    -- magic*, request*, brain swaps, FSM primitives
mc2.tutorial.*              -- callouts, freezegui, invulnerable, voiceover
mc2.experimental.*          -- mirror tree of EXPERIMENTAL bindings only
mc2.data                    -- prototype tables (data-stage only)
mc2.on_event(name, cb)      -- event registration (control-stage only)
mc2.register_action(k, fn)  -- ActionRegistry hook (Track B interop)
```

`mc2.experimental.*` mirrors the regular tree shape so `mc2.experimental.object.foo` reads naturally. Promotion to STABLE is a copy of the binding to `mc2.object.foo` and a deprecation note on `mc2.experimental.object.foo` (kept for one release).

---

## 3. STABLE tier — `mc2_api_version=1`

The full STABLE list. Each row: Lua name, ABL counterpart, one-line description. The 10 starter bindings from the implementation-shape doc are marked `(M0)`; the remaining ~50 land progressively in the M1 fill-out commit.

### `mc2.log` and `mc2.time`

| Lua | ABL exec | Description |
|-----|----------|-------------|
| `mc2.log(msg)` (M0) | (debug-print channel) | Console / log line. |
| `mc2.time.now()` (M0) | `execGetTime` | Mission seconds since start. |
| `mc2.time.remaining()` | `execGetTimeLeft` | Seconds until mission timer expires. |

### `mc2.object.*`

| Lua | ABL exec | Description |
|-----|----------|-------------|
| `mc2.object.exists(id)` (M0) | `execObjectExists` | Boolean. |
| `mc2.object.status(id)` (M0) | `execObjectStatus` | Status enum (alive, destroyed, etc.). |
| `mc2.object.position(id) -> vec3` | `execGetObjectPosition` | Returns `{x,y,z}`. |
| `mc2.object.team(id)` | `execObjectTeam` (alias `objectside`) | Side index. |
| `mc2.object.commander(id)` | `execObjectCommander` | Commander index. |
| `mc2.object.class(id)` | `execObjectClass` | Object class enum. |
| `mc2.object.type_id(id)` | `execObjectTypeID` | CSV / object-type id. |
| `mc2.object.visible(viewer, target)` | `execObjectVisible` | Visibility. |
| `mc2.object.distance_to(a, b)` | `execDistanceToObject` | Real. |
| `mc2.object.distance_to_position(id, pos)` | `execDistanceToPosition` | Real. |
| `mc2.object.is_dead_or_fled(id)` | `execIsDeadOrFled` | Boolean (Omnitech FSM primitive promoted). |
| `mc2.object.armor_pts(id)` | `execGetArmorPts` | Current armor. |
| `mc2.object.max_armor(id)` | `execGetMaxArmor` | Max armor. |
| `mc2.object.damage(id)` | `execGetObjectDamage` | Damage state enum. |
| `mc2.object.damage_pts(id)` | `execGetObjectDmgPts` | Current dmg pts. |
| `mc2.object.max_damage(id)` | `execGetObjectMaxDmg` | Max dmg pts. |
| `mc2.object.set_damage(id, dmg)` | `execSetObjectDamage` | Mutator. |
| `mc2.object.spawn(typeId, pos, side) -> id` (M0) | `execObjectCreate` | Spawn at world pos. |
| `mc2.object.damage(id, dmg, src, hitloc, dtype, dirX, dirY)` (M0) | `execDamageObject` | Apply damage. |
| `mc2.object.suicide(id)` | `execObjectSuicide` | Self-destruct. |
| `mc2.object.remove(id)` | `execObjectRemove` | Remove from world. |
| `mc2.object.change_sides(id, side)` | `execObjectChangeSides` | Defection. |
| `mc2.object.set_active(id, active)` | `execSetObjectActive` | Active flag. |
| `mc2.object.is_active(id)` | `execGetObjectActive` | Active flag query. |
| `mc2.object.teleport(id, pos)` | `execTeleportToPoint` | Hard reposition. |
| `mc2.object.is_in_area(id, pos, radius, areaId)` (M0) | `execInArea` | Trigger-zone test. |
| `mc2.object.is_off_map(pos)` | `execIsOffMap` | Boundary check. |

### `mc2.objective.*`

| Lua | ABL exec | Description |
|-----|----------|-------------|
| `mc2.objective.set_status(id, status)` (M0) | `execSetObjectiveStatus` | Set state. |
| `mc2.objective.status(id)` (M0) | `execCheckObjectiveStatus` | Query state. |
| `mc2.objective.set_type(id, type)` | `execSetObjectiveType` | Type enum. |
| `mc2.objective.type(id)` | `execCheckObjectiveType` | Query type. |
| `mc2.objective.set_position(id, pos)` | `execSetObjectivePos` | Move marker. |

### `mc2.timer.*`

| Lua | ABL exec | Description |
|-----|----------|-------------|
| `mc2.timer.set(id, secs)` (M0) | `execSetTimer` | Start a numeric timer. |
| `mc2.timer.check(id) -> secs` (M0) | `execCheckTimer` | Seconds remaining. |
| `mc2.timer.end(id)` | `execEndTimer` | Cancel. |

### `mc2.area.*`

| Lua | ABL exec | Description |
|-----|----------|-------------|
| `mc2.area.add(x, y, w, h, ...)` | `execAddTriggerArea` | Define trigger rect. |
| `mc2.area.is_hit(id)` | `execIsTriggerAreaHit` | Boolean. |
| `mc2.area.reset(id)` | `execResetTriggerArea` | Clear hit flag. |
| `mc2.area.remove(id)` | `execRemoveTriggerArea` | Delete. |

### `mc2.global.*`

| Lua | ABL exec | Description |
|-----|----------|-------------|
| `mc2.global.set(key, val)` (M0) | `execSetGlobalValue` | Cross-script state. |
| `mc2.global.get(key)` (M0) | `execGetGlobalValue` | Query. |
| `mc2.global.set_campaign(key, val)` | `execSetCampaignGlobalVar` | Persists across missions. |
| `mc2.global.get_campaign(key)` | `execGetCampaignGlobalVar` | Cross-mission state. |

### `mc2.mission.*`

| Lua | ABL exec | Description |
|-----|----------|-------------|
| `mc2.mission.status()` | `execGetMissionStatus` | Status enum. |
| `mc2.mission.is_won()` | `execGetMissionWon` | Bool. |
| `mc2.mission.is_lost()` | `execGetMissionLost` | Bool. |
| `mc2.mission.objective_success()` | `execGetObjectiveSuccess` | Bool. |
| `mc2.mission.objective_failed()` | `execGetObjectiveFailed` | Bool. |
| `mc2.mission.player_in_combat()` | `execPlayerInCombat` | Bool. |
| `mc2.mission.enemy_destroyed()` | `execGetEnemyDestroyed` | Bool. |
| `mc2.mission.friendly_destroyed()` | `execGetFriendlyDestroyed` | Bool. |
| `mc2.mission.is_server()` | `execIsServer` | MP host flag. |
| `mc2.mission.home_team()` | `execGetHomeTeam` | Player side. |

### `mc2.audio.*` and `mc2.video.*`

| Lua | ABL exec | Description |
|-----|----------|-------------|
| `mc2.audio.play_sound(id)` (M0) | `execPlaySoundEffect` | SFX. |
| `mc2.audio.play_music(id)` | `execPlayDigitalMusic` | Music. |
| `mc2.audio.stop_music()` | `execStopMusic` | Stop. |
| `mc2.audio.play_speech(id, who)` | `execPlaySpeech` | Voice line. |
| `mc2.audio.play_betty(id)` | `execPlayBetty` | Bitching Betty. |
| `mc2.audio.play_wave(name, id)` | `execPlayWave` | Loose wav. |
| `mc2.audio.set_radio(id, on)` | `execSetRadio` | Radio toggle. |
| `mc2.audio.current_music()` | `execGetCurrentMusicId` | Query. |
| `mc2.video.play(name)` (M0) | `execPlayVideo` | Cinematic. |
| `mc2.video.fade_to_color(c, secs)` | `execFadeToColor` | Fade. |
| `mc2.video.set_movie_mode()` | `execSetMovieMode` | Letterbox. |
| `mc2.video.end_movie_mode()` | `execEndMovieMode` | Exit. |
| `mc2.video.force_end()` | `execForceMovieEnd` | Hard cut. |

### `mc2.camera.*`

All twenty `cam*` getters/setters: `position`, `goal_position`, `rotation`, `goal_rotation`, `zoom`, `goal_zoom`, `velocity`, `goal_velocity`, `look_object`, `frame_length`. Identifiers map 1:1 from `execGetCameraXxx`/`execSetCameraXxx`. Total: 20 STABLE bindings.

### `mc2.team.*`

| Lua | ABL exec | Description |
|-----|----------|-------------|
| `mc2.team.is_targeting(team, target, who)` | `execIsTeamTargeting` | Bool. |
| `mc2.team.is_capturing(team, target, who)` | `execIsTeamCapturing` | Bool. |
| `mc2.team.print_status(team)` | `execPrintTeamStatus` | Debug. |
| `mc2.team.add_mover(side, id, ...)` | `execAddMoverToPlayer` | Force structure. |
| `mc2.team.remove_mover(side, id, ...)` | `execRemoveMoverFromPlayer` | Defection. |
| `mc2.team.set_force_group(id, group)` | `execSetUnitForceGroup` | Group assign. |

### `mc2.economy.*` and `mc2.strike.*`

| Lua | ABL exec | Description |
|-----|----------|-------------|
| `mc2.economy.add_resource(n)` | `execAddResourcePoints` | RP delta. |
| `mc2.economy.add_money(n)` | `execAddMoney` | Cash delta. |
| `mc2.strike.toggle_air()` | `execToggleAirStrike` | Enable/disable. |
| `mc2.strike.toggle_sensor()` | `execToggleSensorStrike` | Enable/disable. |
| `mc2.strike.toggle_artillery()` | `execToggleArtilleryPiece` | Enable/disable. |
| `mc2.strike.toggle_repair_truck()` | `execToggleRepairTruck` | Enable/disable. |
| `mc2.strike.toggle_salvage()` | `execToggleSalvageCraft` | Enable/disable. |
| `mc2.strike.call(id, side, x, y, z, doFx)` | `execCallStrike` | Spawn strike. |

### `mc2.ui.*` and `mc2.tutorial.*`

| Lua | ABL exec | Description |
|-----|----------|-------------|
| `mc2.ui.text_msg(id, val, dur)` | `execSetTextMsg` | Banner text. |
| `mc2.ui.large_msg(id, val, dur)` | `execSetLargeMsg` | Big banner. |
| `mc2.ui.tutorial_text(id)` | `execTutorialText` | Tutorial overlay. |
| `mc2.ui.in_callout()` | `execInCallout` | Bool. |
| `mc2.tutorial.animation_callout(...)` | `execAnimationCallout` | Tutorial anim. |
| `mc2.tutorial.set_invulnerable(b)` | `execSetInvulnerable` | God mode. |
| `mc2.tutorial.freeze_gui(b)` | `execFreezeGUI` | UI lock. |
| `mc2.tutorial.is_voiceover_playing()` | `execIsPlayingVoiceOver` | Bool. |
| `mc2.tutorial.stop_voiceover()` | `execStopVoiceOver` | Cancel. |

### `mc2.gate.*`

| Lua | ABL exec | Description |
|-----|----------|-------------|
| `mc2.gate.lock_open(id)` | `execLockGateOpen` | Force open. |
| `mc2.gate.lock_closed(id)` | `execLockGateClosed` | Force closed. |
| `mc2.gate.release(id)` | `execReleaseGateLock` | AI control. |
| `mc2.gate.is_open(id)` | `execIsGateOpen` | Bool. |

**STABLE total:** ~85 bindings across 11 subnamespaces. This exceeds the 40-80 estimate from the brief; the surplus comes from camera (20) and the symmetric getter/setter pairs in object/objective/global.

---

## 4. EXPERIMENTAL tier

Functions that are useful to expose but whose engine semantics may shift, or whose ABL signature is subtle (`*` = scalar numeric, `?` = ANYTHING wildcard — see `memory/carver5_mission_playable.md`). Lives under `mc2.experimental.*` and prints a one-time `[LUA v1] event=experimental_use name=<...>` warning per binding.

- `mc2.experimental.ai.set_brain(id, name)` ← `execSetBrainFixed` / `execSetBrainNew`
- `mc2.experimental.ai.set_will_help(b)` ← `execSetWillHelp`
- `mc2.experimental.ai.set_will_request_help(b)` ← `execSetWillRequestHelp`
- `mc2.experimental.ai.scan_area_capture(r)` ← `execScanAreaCapture`
- `mc2.experimental.ai.scan_area_repair(r)` ← `execScanAreaRepair`
- `mc2.experimental.ai.auto_repair_within_radius(id, r)` ← `execAutoRepairWithinRadius`
- `mc2.experimental.ai.num_friends_within_radius(id, r)` ← `execNumFriendsWithinRadius`
- `mc2.experimental.ai.num_enemies_within_radius(id, r)` ← `execNumEnemiesWithinRadius`
- `mc2.experimental.ai.request_help(id, pos, range, pos2, range2, prio)` ← `execRequestHelp`
- `mc2.experimental.ai.request_target(pos, r)` ← `execRequestTarget`
- `mc2.experimental.ai.request_shelter(...)` ← `execRequestShelter`
- `mc2.experimental.weapon.ranges(out)` ← `execGetWeaponRanges`
- `mc2.experimental.weapon.ready(id)` ← `execGetWeaponsReady`
- `mc2.experimental.weapon.locked(id)` ← `execGetWeaponsLocked`
- `mc2.experimental.weapon.in_range(id)` ← `execGetWeaponsInRange`
- `mc2.experimental.weapon.shots(slot)` ← `execGetWeaponShots`
- `mc2.experimental.weapon.list(out, id)` ← `execGetWeapons`
- `mc2.experimental.weapon.status(out)` ← `execGetWeaponsStatus`
- `mc2.experimental.weapon.sort(out, mode)` ← `execSortWeapons`
- `mc2.experimental.weapon.fire_ranges(out)` ← `execGetFireRanges`
- `mc2.experimental.pilot.id(id)` ← `execGetPilotID`
- `mc2.experimental.pilot.wounds(id)` ← `execGetPilotWounds`
- `mc2.experimental.pilot.set_wounds(id, n)` ← `execSetPilotWounds`
- `mc2.experimental.pilot.hire(name)` ← `execHirePilot`
- `mc2.experimental.pilot.add_new(side, name, brain)` ← `execAddNewPilot`
- `mc2.experimental.pilot.add_new_to_player(side, name, brain)` ← `execAddNewPilotToPlayer`
- `mc2.experimental.pilot.set_state(s)` ← `execSetPilotState`
- `mc2.experimental.pilot.state()` ← `execGetPilotState`
- `mc2.experimental.pilot.next_event(out)` ← `execGetNextPilotEvent`
- `mc2.experimental.mech.destroy_body_location(id, loc)` ← `execDestroyMechBodyLocation`
- `mc2.experimental.mech.damage_armor(id)` ← `execDamageMechArmor`
- `mc2.experimental.mech.set_gesture(id, n)` ← `execSetMechGesture`
- `mc2.experimental.mech.set_animation(id, a, b)` ← `execSetAnimation`
- `mc2.experimental.salvage.set(id, a, b)` ← `execSetSalvage`
- `mc2.experimental.salvage.set_status(id, b)` ← `execSetSalvageStatus`
- `mc2.experimental.salvage.get(id, ...)` ← `execGetSalvage`
- `mc2.experimental.capture.set_captured(id)` ← `execSetCaptured`
- `mc2.experimental.capture.set_capturable(id, b)` ← `execSetCapturable`
- `mc2.experimental.capture.is_captured(id)` ← `execIsCaptured`
- `mc2.experimental.capture.is_capturable(id, who)` ← `execIsCapturable`
- `mc2.experimental.capture.was_ever_capturable(id)` ← `execWasEverCapturable`
- `mc2.experimental.path_exists(...)` ← `execPathExists`
- `mc2.experimental.create_infantry(pos, type)` ← `execCreateInfantry`
- `mc2.experimental.set_revealed(...)` ← `execSetRevealed`
- `mc2.experimental.set_building_name(id, n)` ← `execSetBuildingName`
- `mc2.experimental.set_txt_building_name(id, str)` ← `execSetTxtBuildingName`
- `mc2.experimental.add_prisoner(id, who)` ← `execAddPrisoner`
- `mc2.experimental.set_explosion_damage(id, r)` ← `execSetExplosionDamage`
- `mc2.experimental.set_explosion_radius(id, r)` ← `execSetExplosionRadius`
- `mc2.experimental.set_sensor_range(id, r)` ← `execSetSensorRange`
- `mc2.experimental.set_tonnage(id, r)` ← `execSetTonnage`
- `mc2.experimental.repair(id, r)` ← `execRepair`
- `mc2.experimental.get_fixed(id, a, b)` ← `execGetFixed`
- `mc2.experimental.repair_state(id)` ← `execGetRepairState`
- `mc2.experimental.set_fixed_building_rp(id, n)` ← `execSetFixedBuildingRp`
- `mc2.experimental.set_target_priority(...)` ← `execSetTargetPriority`
- `mc2.experimental.set_attack_radius(r)` ← `execSetAttackRadius`
- `mc2.experimental.set_move_area(pos, r)` ← `execSetMoveArea`
- `mc2.experimental.set_eject(b)` ← `execSetEject`
- `mc2.experimental.set_keep_moving(b)` ← `execSetKeepMoving`
- `mc2.experimental.set_goal_planning(b)` ← `execSetGoalPlanning`
- `mc2.experimental.set_general_alarm(n)` / `get_general_alarm()` ← `execSetGeneralAlarm` / `execGetGeneralAlarm`
- `mc2.experimental.calc_part_id(...)` ← `execCalcPartID`
- `mc2.experimental.convert_coords(...)` ← `execConvertCoords`
- `mc2.experimental.terrain_object_part_id(a, b)` ← `execGetTerrainObjectPartID`

**EXPERIMENTAL total:** ~65 bindings.

---

## 5. INTERNAL classification

Functions used by stock `.abx` content (corebrain library + per-mission `mission*.abx`) but conceptually engine-internal — they reach into per-warrior brain state, alarms, contact lists, or memory slots that are an artifact of the ABL VM model, not a Lua-friendly verb. Exposing them would be redundant once `mc2.on_event` and proper Lua tables exist.

- `getId`, `selectObject`, `selectWarrior`, `getWarriorStatus` — per-warrior selection is an ABL idiom; Lua callbacks receive `id` directly.
- `getContacts`, `getEnemyCount`, `selectContact`, `getContactId`, `isContact`, `getContactStatus`, `getContactRelativePosition` — sensor contact list is a brain construct.
- `setTarget`, `getTarget`, `getChallenger`, `setChallenger` — per-warrior target slot.
- `getIntegerMemory`, `getRealMemory`, `setIntegerMemory`, `setRealMemory` — ABL static-memory slots; Lua has tables.
- `getAlarmTriggers`, `getNextPilotEvent`, `getTimeWithoutOrders`, `getLastTacOrder`, `getTacOrder`, `getCurTacOrderTime`, `getTimeOfLastStep`, `isCurTacOrderMoveOrder`, `clearTacOrder`, `clearMoveOrdersOmni` — order/alarm queue introspection; will be replaced by `mc2.on_event("order_complete", ...)`.
- `hasMoveGoal`, `hasMovePath`, `getUnitMates`, `getObjects`, `getUnitStatus`, `getSensorsWorking`, `getSensorsActive`, `getCurrentBRValue`, `setCurrentBRValue`, `getVisualRange`, `getLastScan`, `getMapInfo`, `getRelativePositionToPoint`, `getRelativePositionToObject`, `getTargetRelativePosition`, `getRelativePositionToTarget`, `newDistanceToPosition`, `getMissionTune`, `setMissionTune` — engine-internal queries; some surface in `mc2.experimental` only when concrete modder demand exists.
- `coreMoveTo`, `coreMoveToObject`, `corePower`, `coreAttack`, `coreCapture`, `coreScan`, `coreControl`, `coreEject`, `newMoveTo`, `newMoveToObject`, `newPower`, `newAttack`, `newCapture`, `newScan`, `newControl` — `isOrder=true` order-issuance verbs that yield the ABL VM. Lua coroutines + `mc2.order.*` will replace these (M2+).
- `orderWait`, `orderMoveTo`, `orderMoveToObject`, `orderMoveToContact`, `orderPowerDown`, `orderPowerUp`, `orderAttackObject`, `orderAttackContact`, `orderWithdraw`, `objectInWithdrawal`, `orderRefit`, `orderCapture`, `orderLoadElementals`, `orderDeployElementals`, `hasOrderFromPlayer`, `isSelected`, `isRefit`, `needsRefit` — non-yielding order shims; Lua exposes a single `mc2.order.issue(id, kind, params)` once the order dispatcher is uniform (M2).
- `magicPatrol`, `magicGuard`, `magicEscort` — Omnitech FSM primitives that *intentionally do not shadow* the corebrain.abx routines (see comment at registration site, `memory/omnitech_abl_stubs_session.md`). They exist only because Omnitech/Carver5O content does not implement them in ABL. Exposing these to Lua would re-export the same shadow hazard.
- `setDebugString`, `setDebugWindow`, `tDebugString`, `aBlPrint`, `mcPrint`, `break`, `forceMovieEnd` — ABL debugging hooks; replaced by `mc2.log` and Lua `error()`.
- `sendMessage`, `getMessage` — inter-warrior messaging; Lua uses tables / `mc2.global.*`.
- `addNewPilot`, `addNewPilotToPlayer`, `hirePilot` — these classify as EXPERIMENTAL above for mission-script use; the underlying logistics-layer bindings stay INTERNAL.
- `getAttackers`, `getAttackerInfo` — combat-state introspection; promote later if modders ask.

**INTERNAL total:** ~110 bindings.

---

## 6. DEPRECATE classification

Registered but should not propagate.

- `selectUnit` (commented out), `setObjectiveTimer`, `checkObjectiveTimer`, `getStrikes`, `setStrikes`, `addStrikes` — already commented in the source. Keep commented; do not bind.
- `objectside` — alias for `objectteam` (`execObjectTeam` registered twice). Bind one Lua name (`mc2.object.team`).
- `newmoveto` / `coremoveto`, `newmovetoobject` / `coremovetoobject`, `newpower` / `corepower`, `newattack` / `coreattack`, `newcapture` / `corecapture`, `newscan` / `corescan`, `newcontrol` / `corecontrol` — duplicated `new*` and `core*` aliases pointing at the same `execCoreXxx` functions. Pick `core*` as the canonical Lua names *if* this whole family is ever promoted; today, all are INTERNAL anyway, so this is a forward-looking rule.
- `forcemovieend`, `setdebugwindow`, `setdebugstring`, `playspeech` (engine-internal; modders use `mc2.audio.play_sound`) — kept registered for stock compatibility; don't propagate.
- `mcprint` (`?` ANYTHING) — replaced by `mc2.log` with proper Lua-side formatting.

**DEPRECATE total:** ~15 bindings.

---

## 7. Naming convention

**Convention:** `mc2.<subsystem>.<verb_or_noun_in_snake_case>(args)`, with these rules:

1. **Subsystem first.** ABL prefixes are inconsistent (`object*`, `get*`, `set*`, `is*`); Lua groups by the *thing being operated on*, not the verb. `objectStatus` → `mc2.object.status(id)`, not `mc2.get_object_status(id)`.
2. **Drop redundant `get_`.** Read-only queries don't need `get_`; their noun is the function name. `mc2.object.team(id)` not `mc2.object.get_team(id)`. Mutators retain `set_` for symmetry: `mc2.object.set_active(id, b)`.
3. **Booleans use `is_*` or `has_*`.** `mc2.object.is_in_area(...)`, `mc2.gate.is_open(id)`, `mc2.team.is_targeting(...)`.
4. **`snake_case` everywhere.** ABL is mixed-case (`getRelativePositionToObject`); Lua is snake. The ugly `getrelativepositiontoobject` ABL string becomes `mc2.experimental.relative_position_to_object(...)`.
5. **No abbreviations except domain canon.** `BR` (Battle Rating), `RP` (Resource Points), `dmg` are abbreviations stock content already uses; keep them. `pos`, `vel`, `rot` are universal. Everything else spells out (`captured` not `capt`).
6. **Vec3 is a 3-element table.** `{x=10, y=20, z=0}` reads/writes; ABL's by-ref `R` output convention becomes a return value.

Rationale: modders coming from Factorio, Stellaris, or Bannerlord scripting expect `subsystem.action(args)` shape. This convention is purely additive — the underlying ABL registration stays untouched, so stock `.abx` content is unaffected. The renaming is one-time and contained in the Lua bindings file.

---

## 8. API stability and versioning policy

`mc2_api_version` is a single integer in mod manifests (§5.4). Policy for Lua-affecting changes:

| Change | Bumps `mc2_api_version`? |
|--------|--------------------------|
| Adding a new STABLE binding | **No** (additive) |
| Removing a STABLE binding | **Yes** |
| Renaming a STABLE binding (no alias) | **Yes** |
| Renaming a STABLE binding *with* one-version-deprecation alias | No (the alias keeps old code working); bump the version *after* the deprecation window closes |
| Changing a STABLE binding's parameter count or types | **Yes** |
| Changing a STABLE binding's return type | **Yes** |
| Tightening a STABLE binding's runtime contract (e.g. now errors on bad id where it previously returned -1) | **Yes** |
| Loosening a STABLE binding's contract (accepts new arg) with optional/defaulted params | No |
| Promoting `mc2.experimental.foo` to `mc2.foo` | No (additive; the experimental name remains as an alias for one version) |
| Removing an EXPERIMENTAL binding | No (experimental is by definition unstable) |
| Adding a new event name to `mc2.on_event` | No (additive) |
| Changing an event's payload shape | **Yes** |
| Adding a new ActionRegistry key | No |
| Changing the sandbox surface (whitelisting more stdlib, narrowing) | **Yes** |

Engine emits `[LUA v1] event=manifest_api_mismatch mod=<id> declared=<n> engine=<m>` warning at mod load when a mod declares a `mc2_api_version` lower than the engine's, and refuses to load when the mod declares higher. (Lower is recoverable: the engine knows what changed and may shim; higher means the mod uses verbs the engine doesn't know.)

---

## 9. Documentation auto-generation pattern

Each binding registers through a single helper that captures docstring + signature + tier + stage. The generator pass walks the binding table at startup and emits a Markdown file when `MC2_LUA_DOCGEN=1` is set.

Registration shape (illustrative — no implementation in this commit):

```cpp
// modding/lua_bindings_mc2.cpp — sketch of the registration helper.
struct BindingSpec {
    const char* name;          // "mc2.object.status"
    const char* signature;     // "(id: int) -> int"
    const char* doc;           // "Returns the status enum for object id (alive=0, destroyed=1, ...)."
    Tier tier;                 // STABLE / EXPERIMENTAL / INTERNAL / DEPRECATE
    Stage stage;               // Data / Control / Any
    std::function<sol::object(sol::variadic_args)> fn;
};

#define MC2_BIND(table, name, sig, doc, tier, stage, fn) \
    g_bindings.push_back({name, sig, doc, tier, stage, fn});  \
    table[name##_last_segment] = fn;

// Usage:
MC2_BIND(mc2_object, "mc2.object.status",
    "(id: int) -> int",
    "Returns status enum: 0=alive, 1=destroyed, 2=disabled.",
    Tier::STABLE, Stage::Control,
    [](int id) { /* ... */ });
```

`g_bindings` is a flat `std::vector<BindingSpec>` populated at `LuaVM::Init`. Doc-gen path:

```cpp
void LuaVM::DumpDocs(const char* outPath) {
    std::sort(g_bindings.begin(), g_bindings.end(), /* by name */);
    FILE* f = fopen(outPath, "w");
    fprintf(f, "# mc2.* Lua API (api_version=%d)\n\n", MC2_API_VERSION);
    for (const auto& b : g_bindings) {
        fprintf(f, "### `%s%s`\n\n**Tier:** %s · **Stage:** %s\n\n%s\n\n",
                b.name, b.signature, tierStr(b.tier), stageStr(b.stage), b.doc);
    }
    fclose(f);
}
```

Output: `docs/modding/lua-api-reference.md`, regenerated each release; never hand-edited. Tests can grep this file to verify all STABLE bindings have non-empty docstrings.

---

## 10. ActionRegistry interop entries

Per §5.5, FIT button declarations carry `Action="..."` strings. The registry admits both C++ and Lua handlers (status doc §8). The following STABLE bindings should ship as default ActionRegistry-callable entries so vanilla FIT files can dispatch to them without writing Lua:

| Action key | Resolves to | Notes |
|------------|-------------|-------|
| `Mission.AbortMission` | (engine-side abort handler) | Wrapped by `mc2.mission.abort()` (M1) |
| `Mission.GetStatus` | `mc2.mission.status()` | DataSource read, not Action; example only |
| `Audio.StopMusic` | `mc2.audio.stop_music()` | Pure passthrough |
| `Audio.PlaySound` | `mc2.audio.play_sound(arg)` | One-arg via `Action.Param` |
| `Video.PlayBriefing` | `mc2.video.play("briefing")` | Cinematic launcher button |
| `Camera.ResetZoom` | `mc2.camera.set_zoom(default)` | UI button |
| `Strike.ToggleAir` | `mc2.strike.toggle_air()` | Logistics-screen toggle |
| `Strike.ToggleArtillery` | `mc2.strike.toggle_artillery()` | Same |
| `Tutorial.FreezeGUI` | `mc2.tutorial.freeze_gui(true)` | Tutorial overlay |
| `Mod.Reload` | (Track C hot-reload entrypoint) | Dev-only key, EXPERIMENTAL |

ActionRegistry keys are **PascalCase.PascalCase** (not snake_case) to match the FIT-file convention from the original engine's `.fit` syntax. The Lua-side `mc2.register_action("MyMod.OpenMarket", function() ... end)` maps cleanly through this convention; the registry doesn't care what the handler ultimately calls.

Five-to-ten examples is the brief; the full default action map should land alongside Track B's FIT loader, not Track C.

---

## 11. Open questions

1. **`magicpatrol` / `magicguard` / `magicescort` exposure.** These are deliberately not shadowing corebrain.abx (see note in §5 INTERNAL). If a Lua mod registers a same-named action, does it shadow the ABL library implementation for *its own* mission set, or globally? Decision: per-mission, gated on `mod.json` declaring `replaces_corebrain: ["magicpatrol", ...]`. Track E concern.
2. **`requesthelp` / `requestshelter` signatures.** ABL uses `*` (PARAM_TYPE_INTEGER_REAL scalar) repeatedly. Translating to Lua requires inspecting each param at runtime — `sol::variadic_args` plus a type-check helper. Confirm before binding.
3. **`callstrike` vs `callstrikeex`.** Two registrations differing in one extra real arg (`callstrikeex` has trailing `r`). Bind one (`mc2.strike.call`) with the optional last param.
4. **`setobjectivepos` `i***`.** Three trailing `*` slots can be int or real. The sane Lua surface is `mc2.objective.set_position(id, vec3)`; the marshaller accepts either ints or reals from the Lua table. Verify `execSetObjectivePos` doesn't care about which.
5. **`addtriggerarea` six-int signature `iiiiii`.** Naming the six ints (id, x0, y0, x1, y1, kind?) requires reading `execAddTriggerArea`. Keep STABLE pending that read.
6. **`getmapinfo` output.** Reads as `I` (int output) but actually fills a multi-field record into the brain's static memory. Cannot expose 1:1; need a redesign that returns a Lua table. INTERNAL until then; bumped to STABLE in M2 with a new shape.
7. **`mcprint` `?` wildcard.** Single ANYTHING parameter. Folded into `mc2.log`; do not expose separately.
8. **`break`** — drops into the ABL VM debugger. No Lua analogue worth exposing; INTERNAL.
9. **`getfireranges`, `getrelativepositiontopoint`, `getrelativepositiontoobject`, `gettargetrelativeposition`** — all return-by-ref-into-`R` array. Lua surface is "return a table"; needs the marshaller helper template referenced in implementation-shape §11.
10. **`isserver`** — multiplayer-only. Mod content running in MP needs deterministic synchronization (Lua state must be authoritative on the server, replayed on clients). Not addressed in v1; classify STABLE-but-readonly and document `mc2.mission.is_server() == false` is the only safe-to-mutate state.

---

## 12. Tally

| Tier | Count |
|------|-------|
| STABLE | ~85 |
| EXPERIMENTAL | ~65 |
| INTERNAL | ~110 |
| DEPRECATE | ~15 |
| Commented-out / dormant | ~5 |
| **Total registered** | **~280-290** |

The tier ratio (STABLE ≈ 30%, EXPERIMENTAL ≈ 23%, INTERNAL ≈ 38%, DEPRECATE ≈ 5%) is the right shape for v1: roughly one in three ABL functions earns a stable Lua binding immediately; another in four is exposed under `experimental`; the rest stays engine-side. Subsequent versions promote experimental verbs to stable based on observed mod usage, keeping the v1 contract small enough to defend.

---

## 13. References

- `code/ablmc2.cpp:7791-8126` — the canonical 290-call registration block (read in full for this catalog).
- `code/ablmc2.cpp:8040-8063` — Omnitech tier-2 FSM-primitive stubs and the corebrain shadow rule comment.
- `code/ablmc2.cpp:8065-8119` — Omnitech mission/economy/AI extensions.
- `mclib/ablsymt.h:166` — `MAX_STANDARD_FUNCTIONS 512`.
- Status doc: `2026-04-29-track-c-lua-scripting-status.md`.
- Implementation shape: `2026-04-30-track-c-lua-implementation-shape.md`.
- Spec: `specs/2026-04-29-modders-paradise-roadmap-design.md` §5.4 stable mod ABI, §5.5 ActionRegistry/DataSourceRegistry.
- Memory: `omnitech_abl_stubs_session.md`, `omnitech_abl_missing_names.md`, `carver5_mission_playable.md` (the `?` ANYTHING wildcard rule).
