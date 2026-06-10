# Tactical Overview Camera — Design Spec

**Date:** 2026-06-10
**Branch:** `claude/terrain-gen-pcg` (worktree `nifty-mendeleev`)
**Status:** Approved (brainstorming) — ready for writing-plans

## Goal

SupCom/Warno-style "tactical overview": zoom-out continuum **and** hotkey snap into a
high-altitude, steep-tilt view where the battlefield reads as strategic icons + info
overlays instead of (or on top of) full 3D models. Hybrid cross-fade model.

## Hard constraints (DO NOT CHANGE)

- No orthographic projection — steep-tilt **perspective** pull-back only.
- No new render pipeline.
- No new pick path.
- No threat / enemy-zone aggregation.
- No model-mesh replacement.
- No duplication of minimap gameplay queries.

Rationale: the engine has documented D3D↔GL projection split-brain history (cull/shadow/
pick/admission all assume one `worldToClipGL`). An ortho swap or new draw pass reopens that
class. This design is **additive 2D overlay + camera-API driving** only.

## Architecture

One new controller: `code/tacticaloverview.{h,cpp}` (`TacticalOverview`).
Owns overview state; pure logic + a 2D overlay draw. No new pipeline.

Three taps into existing code (grep to confirm exact lines before editing):

- **Camera** (`mclib/camera.*`) — raise `setMaximumCameraAltitude` ceiling; drive
  altitude/tilt via existing `zoomValue` / `tiltValue` **public API only**.
- **Input** (`code/gamecam.cpp` frame loop, wheel + hotkey read) — feeds the controller's
  blend factor. UI-exclusion contract below.
- **2D draw** (`code/missiongui.cpp` / `code/gametacmap.cpp` HUD pass) — icon + tint overlay
  draws after world render, where blips already draw.

Refactor: extract gametacmap's contact/mover/objective walk into one shared
`enumerateTacticalBlips()`. Both minimap and overview consume it. (No new minimap gameplay
queries — pure extraction.)

## State — single blend factor `t`

`t ∈ [0,1]`: 0 = gameplay, 1 = full overview. Camera altitude/tilt, icon alpha, and tint
strength are all functions of `t`.

- **Wheel:** zoom past old max-altitude raises `t` continuously.
- **Hotkey:** lerps `t` → 1 (and back) at a fixed rate. Hotkey is an animated setpoint over
  the same `t`; no separate two-state machine.

Camera mapping: `altitude = lerp(maxGameplayAlt, overviewAlt, t)`,
`tilt = lerp(currentTilt, steepTilt, t)`. Clamp; restore on exit per camera-return contract.

### Camera-return contract

On entry to **hotkey** overview:
- capture return altitude/tilt/rotation/position/zoom **once**;
- do **not** keep updating the return snapshot while overview is active;
- on exit, lerp back to that snapshot **unless** the user manually panned/rotated in overview.

Wheel-driven overview does **not** overwrite the hotkey return snapshot.

### Input ownership / UI exclusion

Only feed overview zoom when mission **world input owns the wheel**.
Do not trigger overview while hovering/scrolling UI panels, tacmap, dialogs, or chat.

## Altitude bands (cross-fade)

Over `t`:
- `t < 0.4` — normal 3D, no icons.
- `0.4–0.7` — cross-fade band: icon alpha `0 → 1`.
- `t > 0.7` — full overview: icons full alpha, tint on.

**Model fade is optional v2.** Tactical Overview v1 only fades icons/tint in.
**No mech3d / material / TG_Shape alpha changes in v1.** The icon layer alone gets the
readability win; model fade is the one part that can unexpectedly touch the renderer.

## Icon layer — 2D billboard overlay

For each blip from `enumerateTacticalBlips()`: project world pos → screen via the existing
camera project (NOT a new matrix), draw a screen-space sprite (reuse `blip.tga` + tac-map
symbol textures) at that pixel, team-colored (friendly green / neutral blue / enemy red /
selected brighter / shutdown variants as gametacmap already does), alpha = icon-band(`t`).
Honors sensor **contact status** (fog) — unscouted contacts are not enumerated, identical to
the minimap. Objectives/nav reuse the existing objective-marker + navMarker draw at overview
scale.

### `enumerateTacticalBlips()` contract

- Read-only.
- No sensor updates.
- No visibility recompute.
- No per-blip heap allocation in the hot path if avoidable.
- No gameplay-side effects.
- Consumes already-current mission visibility/contact state.

## Friendly-coverage tint

Reduced "force/zone": additive friendly-colored soft circles, one per **friendly sensor**
(radius = sensor range, already walked in the loop), composited as a screen-space overlay
quad layer at low alpha, strength = tint-band(`t`). No aggregation / threat map — pure union
of friendly sensor radii.

Escape hatch: `MC2_TACTICAL_OVERVIEW_TINT` — default ON under the overview flag, but
**independently killable** (clutter / cost control).

## Interaction in overview

Picking/selection/orders keep working — projection unchanged, so existing
`findTerrainObjectByMouse` / click-to-ground stay valid (cursor uses
`screenToGroundPlaneApprox`, fine at altitude). Icon = same world-pos pick (click near icon =
click near unit). No new pick path.

## Performance budget

- Overview overlay target: **<0.3 ms CPU, <0.3 ms GPU** on tier1-scale maps.
- No per-frame texture loads.
- No per-blip heap allocation.

## Testing & oracles

- tier1 5/5 smoke (canonical `run_smoke.py --tier tier1 --duration 30 --keep-logs`) —
  overview is idle-flythrough-inert, must not regress.
- Env gate `MC2_TACTICAL_OVERVIEW=1`, **default-OFF** first; flip on after visual confirm
  (headless can't see pixels → user visual gate, as with terrain LOD).
- Screenshot oracle: `MC2_TACTICAL_OVERVIEW=1` + `MC2_SCREENSHOT_AT_FRAME=N`.
  Acceptance captures: `t=0` normal, `t≈0.5` crossfade, `t=1` full overview.
- Manual: wheel continuum smooth; hotkey snap + return; icons track units; fog honored; tint
  follows friendly sensors; picking works at altitude; UI hover does not trigger overview.

## Env flags

| Flag | Default | Effect |
|---|---|---|
| `MC2_TACTICAL_OVERVIEW` | OFF (v1) | master gate for the feature |
| `MC2_TACTICAL_OVERVIEW_TINT` | ON under master | friendly-coverage tint, independently killable |
| `MC2_SCREENSHOT_AT_FRAME` | (existing) | acceptance captures |

## Implementation sequence

| Step | Scope |
|---|---|
| T0 | Spec only (this doc) |
| T1 | Controller + env flag + hotkey/wheel `t` state — **no camera movement, debug log only** |
| T2 | Camera blend via wheel/hotkey (altitude/tilt lerp + return contract) |
| T3 | Shared `enumerateTacticalBlips()` refactor (minimap + overview share) |
| T4 | Icon overlay |
| T5 | Friendly-coverage tint |
| T6 | Pick/selection polish |
| T7 | Default-on decision after visual gate |

Keep v1 boring and reversible. First patch = controller + flag + `t` state, no camera
movement except a debug log. Second patch = camera blend only. Third patch = icons.

## Out of scope (YAGNI)

Ortho projection, threat/enemy zone aggregation, model-mesh replacement, per-object model
fade (v2), minimap changes beyond the shared-enumerate refactor.
