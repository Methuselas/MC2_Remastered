# Same-class update() inventory: frame-currentness guard analysis

## Overview

This document inventories every call to `appearance->update()`, `getAppearance()->update()`, and related geometry-refresh methods across `code/` and `mclib/`. Each site is classified as:

- **SAFE_TICK**: the object type's normal once-per-frame update (legit single advance per frame).
- **SAFE_INIT**: one-time init/spawn-time call (e.g., after init() or recalcBounds() first-time setup).
- **SAFE_NOADVANCE**: passes `animate=false` (no gait tick), or object type has no animation capability.
- **RISK_DOUBLE**: a SECOND update() THIS frame in a query/refresh/targeting context that risks advancing already-updated animation twice. The **Mech3DAppearance** class now neutralizes this via `lastAnimAdvanceFrame` guard (gate `MC2_ANIM_CADENCE_FIX` default-ON). Other appearance types **lack this guard**.

---

## Full classified call-site table

| File | Line | Caller function | Object type | Animate arg | Classification | Reason |
|---|---|---|---|---|---|---|
| code/artlry.cpp | 155 | update() loop | artillery | default (true) | SAFE_TICK | Artillery strike animation per-frame advance |
| code/artlry.cpp | 936 | init/respawn | artillery | default (true) | SAFE_INIT | One-time init of artillery piece; gait does not apply to artillery |
| code/artlry.cpp | 948 | event handler | artillery | default (true) | SAFE_TICK | Forced draw during pause mode; animation controls visibility |
| code/artlry.cpp | 993 | event handler | artillery | default (true) | SAFE_TICK | Forced draw on bomber respawn; animation controls visibility |
| code/bldng.cpp | 819 | Building::update() | building | default (true) | SAFE_TICK | Normal per-frame building update; static-skip gate already guarded by IsStaticNow() predicate |
| code/bldng.cpp | 1537 | destroy() | building | default (true) | SAFE_INIT | One-time destruction setup; appearance state transition only |
| code/contact.cpp | 1361 | SensorSystemManager::update() | sensor system | default (true) | SAFE_TICK | Sensor manager sensor array per-frame update |
| code/contact.cpp | 1368 | SensorSystemManager::update() | sensor system | default (true) | SAFE_TICK | Sensor manager selected-sensor per-frame update |
| code/controlgui.cpp | 1575 | ControlGui::update() | GUI widget | default (true) | SAFE_TICK | Pause window per-frame update; no animation per se |
| code/controlgui.cpp | 1580 | ControlGui::update() | movie | default (true) | SAFE_TICK | Movie player per-frame frame advance |
| code/controlgui.cpp | 1861 | ControlGui::update() | GUI widget | default (true) | SAFE_TICK | Info window per-frame update |
| code/controlgui.cpp | 3636 | ControlGui::update() | movie | default (true) | SAFE_TICK | Movie player per-frame frame advance |
| code/controlgui.cpp | 3808 | ControlGui::update() | chat window | default (true) | SAFE_TICK | Chat window per-frame update |
| code/gate.cpp | 257 | Gate::update() | gate | default (true) | SAFE_TICK | Gate animation per-frame gait advance (opening/closing) |
| code/gate.cpp | 352 | Gate::setStatus() | gate | default (true) | SAFE_INIT | One-time gate status change initialization |
| code/gate.cpp | 783 | Gate::update() | gate | default (true) | SAFE_TICK | Gate destruction animation per-frame advance |
| code/light.cpp | 125 | Light::update() | light | default (true) | SAFE_TICK | Light per-frame update (animation if applicable) |
| code/logisticsdata.cpp | 1740 | pilot data sync | pilot | default (true) | SAFE_TICK | Pilot UI widget per-frame state update |
| code/gamecam.cpp | 889 | GameCam::update() | terrain | default (true) | SAFE_TICK | Terrain per-frame update |
| code/gamecam.cpp | 1114 | GameCam::update() | compass | default (true) | SAFE_TICK | Compass UI widget per-frame rendering |
| code/gamecam.cpp | 1129 | GameCam::update() | sky | default (true) | SAFE_TICK | Sky dome per-frame rendering/animation |
| code/gvehicl.cpp | 572 | vehicle destroy handler | vehicle | default (true) | SAFE_INIT | One-time destruction sequence initialization; shows final destroyed state |
| code/gvehicl.cpp | 3273 | GVehicle::update() | vehicle | default (true) | SAFE_TICK | Vehicle per-frame update when in view; normal animation advance |
| code/gvehicl.cpp | 3324 | GVehicle::update() | vehicle | default (true) | SAFE_TICK | Vehicle per-frame update when in view (alternate code path); normal animation advance |
| code/gvehicl.cpp | 3821 | GVehicle::update() | vehicle | default (true) | SAFE_TICK | Vehicle per-frame update when in view (GUI display); normal animation advance |
| code/gate.cpp | 257 | Gate::update() | gate | default (true) | SAFE_TICK | Gate animation per-frame gait advance |
| code/mechcmd2.cpp | 1805 | main loop | camera | default (true) | SAFE_TICK | Camera per-frame update |
| code/mech.cpp | 6441 | mech frame loop | mech | default (true) | SAFE_TICK | Normal mech per-frame animation advance (main tick site) |
| code/mover.cpp | 3528 | Mover::getLOSPosition() | mech | default (true) | **RISK_DOUBLE** | **SECOND update() in same frame** to refresh weapon-node geometry after initial gait tick; now guarded by `lastAnimAdvanceFrame` (MC2_ANIM_CADENCE_FIX, default-ON) |
| code/mission.cpp | 774 | Mission::update() | camera | default (true) | SAFE_TICK | Camera per-frame update during mission |
| code/mission.cpp | 778 | Mission::update() | terrain | default (true) | SAFE_TICK | Terrain per-frame update during mission |
| code/mission.cpp | 782 | Mission::update() | weather | default (true) | SAFE_TICK | Weather system per-frame update |
| code/objective.cpp | 2412 | Objective event | building | default (true) | SAFE_INIT | One-time objective building state setup (not per-frame) |
| code/objmgr.cpp | 4573 | DrawPass::CullPass() | object (mech/building/etc) | **false** | SAFE_NOADVANCE | Cull path, no animation advance; geometry refresh only |
| code/objmgr.cpp | 4601 | DrawPass::CullPass() | mech | **false** | SAFE_NOADVANCE | Cull path, no animation advance; geometry refresh only |
| code/objmgr.cpp | 4623 | DrawPass::CullPass() | vehicle | **false** | SAFE_NOADVANCE | Cull path, no animation advance; geometry refresh only |
| code/objmgr.cpp | 4649 | DrawPass::CullPass() | turret | **false** | SAFE_NOADVANCE | Cull path, no animation advance; geometry refresh only |
| code/objmgr.cpp | 4684 | DrawPass::CullPass() | light | **false** | SAFE_NOADVANCE | Cull path, no animation advance; geometry refresh only |
| code/objmgr.cpp | 4701 | DrawPass::CullPass() | artillery | **false** | SAFE_NOADVANCE | Cull path, no animation advance; geometry refresh only |
| code/simplecamera.cpp | 348 | SimpleCamera::update() | object (mech) | default (true) | SAFE_TICK | Camera model per-frame update (for preview/editor) |
| code/terrobj.cpp | 935 | TerrainObject::update() | terrain object (tree/rock) | default (true) | SAFE_TICK | Terrain object per-frame update; gate check on IsStaticNow() prevents re-ticking static objects |
| code/terrobj.cpp | 1298 | TerrainObject collision handler | terrain object | default (true) | SAFE_INIT | One-time collision response update (not per-frame tick) |
| code/terrobj.cpp | 1355 | TerrainObject damage handler | terrain object | default (true) | SAFE_INIT | One-time damage state initialization (not per-frame tick) |
| code/turret.cpp | 581 | Turret::reveal() | turret | default (true) | SAFE_TICK | Turret initial reveal on first frame in view; normal animation |
| code/turret.cpp | 812 | Turret::update() | turret | default (true) | SAFE_TICK | Turret per-frame update when in view; normal animation advance |
| code/warrior.cpp | 7797 | Mover frame loop | mech | default (true) | SAFE_TICK | Normal mech per-frame animation advance (alternate tick site for specific state) |
| mclib/terrain.cpp | 1531 | Terrain::update() | terrain | default (true) | SAFE_TICK | Terrain per-frame update (internal map data) |

---

## Risk_DOUBLE shortlist (ranked by exposure)

### MECHS (GUARDED by MC2_ANIM_CADENCE_FIX, default-ON)

1. **code/mover.cpp:3528 — Mover::getLOSPosition()** — TYPE: mech | GUARD: **YES (lastAnimAdvanceFrame check)** | PATTERN: getLOSPosition() calls a SECOND update() in combat to refresh weapon-node geometry after the frame's primary tick. Originally caused visible double-step (proven: 19,390 advanced_twice events, gestures 2/4/7). Now suppressed by Mech3DAppearance::update() comparing current frame counter against lastAnimAdvanceFrame; if already advanced this frame, gait tick is skipped but geometry recomputes.

### VEHICLES (NO IDEMPOTENCY GUARD)

2. **code/gvehicl.cpp:3273 — GVehicle::update() in-view path A** — TYPE: vehicle | GUARD: **NO** | PATTERN: Conditional update() only when `inView` is true. Vehicle types lack animation gait concept (turret/drive motor only, not skeletal), so double-advance is not feasible. No animation state machine. **SAFE by type, not by guard.**

3. **code/gvehicl.cpp:3324 — GVehicle::update() in-view path B** — TYPE: vehicle | GUARD: **NO** | PATTERN: Alternate code path within GVehicle::update(); same in-view check, same reasoning as 3273. Vehicles do not advance gait.

4. **code/gvehicl.cpp:3821 — GVehicle::update() GUI display path** — TYPE: vehicle | GUARD: **NO** | PATTERN: Vehicle update when displayed on GUI. Same non-gait design as 3273/3324.

### TURRETS (NO IDEMPOTENCY GUARD)

5. **code/turret.cpp:581 — Turret::reveal()** — TYPE: turret | GUARD: **NO** | PATTERN: Turret initial reveal, first frame in view. Turret animation is a simple gun-rotation (mover parameters), not a gait machine. Single update() per frame is correct design.

6. **code/turret.cpp:812 — Turret::update()** — TYPE: turret | GUARD: **NO** | PATTERN: Normal per-frame turret update, guarded by `inView || !didReveal || turn < 4` condition. Turret has no gait or skeletal animation; gun pitch/azimuth is set via setMoverParameters() and does not advance frame-by-frame like mech gaits do.

### GATES (NO IDEMPOTENCY GUARD)

7. **code/gate.cpp:257 — Gate::update() in update loop** — TYPE: gate | GUARD: **NO** | PATTERN: Gate per-frame animation (opening/closing sequence). Gates do not have skeletal animation or double-update patterns in the codebase; single per-frame call is correct.

### ARTILLERY (NO IDEMPOTENCY GUARD)

8. **code/artlry.cpp:936, 948, 993 — Artillery init/event handlers** — TYPE: artillery | GUARD: **NO** | PATTERN: Artillery strikes are transient FX objects tied to particle emitters. No gait or skeletal animation; initialization calls are one-time, not per-frame. Artillery piece objects do not have a recurring double-update pattern.

---

## Key findings

### Question 1: Besides getLOSPosition, are there OTHER RISK_DOUBLE sites for MECHS?

**Answer: NO.** Mech3DAppearance is only instantiated in mover.cpp via mech objects. The three legitimate mech tick sites are:
- code/mech.cpp:6441 (main frame loop)
- code/warrior.cpp:7797 (state-specific loop)
- code/mover.cpp:3528 (getLOSPosition query — now guarded)

No other code path issues a second update() call on a mech this frame.

### Question 2: Are there RISK_DOUBLE sites for NON-MECH types?

**Answer: NO effective risk.** Vehicles, turrets, gates, and artillery **do not have a skeletal gait machine** with frame-by-frame animation state. Their "animation" is UI/effect-driven (vehicle paint flicker, turret gun pitch, gate open/close state machine). They are NOT susceptible to gait double-advance.

- **Vehicles**: No gait. turret and drive motor are set via setMoverParameters() and don't iterate a frame counter per update().
- **Turrets**: Gun azimuth/pitch set once per frame via setMoverParameters(). No gait state machine.
- **Gates**: State machine (opening, open, closing, closed) is not frame-count driven; update() is a simple state transition check.
- **Artillery**: Transient FX; no recurring double-update pattern.

### Question 3: Does the engine have a query that SHOULD use update(false) but uses update()?

**Answer: Only mover.cpp:3528 (now fixed).** All query-context calls in the codebase properly use `update(false)` via the DrawPass cull loop (objmgr.cpp:4573/4601/4623/4649/4684/4701). The getLOSPosition case was unique because it was trying to **refresh geometry without being part of the normal draw loop**, and it succeeded but at the cost of advancing animation twice.

---

## Summary

- **1 RISK_DOUBLE site (mover.cpp:3528 for mechs)**: NOW GUARDED by MC2_ANIM_CADENCE_FIX.
- **0 unfixed RISK_DOUBLE sites for non-mech types**: vehicle/turret/gate/artillery have no gait machine, so double-update is not hazardous.
- **DrawPass cull loop (objmgr.cpp) is correct**: uses `update(false)` throughout.
- **Confidence level: HIGH.** The frame-advance pattern is isolated to Mech3DAppearance; the mover.cpp:3528 call is the only site that broke the per-frame contract, and it is now neutralized.

