# Stage 2.E — Phase 1 + Phase 2 plan: natural mission camera capture + tolerance gate

**Date:** 2026-05-04 (round-6 — post-radical-simplification)
**Worktree HEAD:** see `git log` (Steps 1.1–1.5 + cleanup landed)
**Branch:** `claude/nifty-mendeleev`
**Recon:** [docs/superpowers/explorations/2026-05-04-stage2e-recon.md](../explorations/2026-05-04-stage2e-recon.md)

---

## Architecture summary

The engine captures its **natural mission camera** at a chosen frame.
Each mission's intro pan + command-position settle produces a repeatable
vantage on its own; the harness counts frames since
`SmokeMode::missionHasStarted()` and writes the pre-HUD framebuffer to
TGA at `MC2_VISUAL_DIFF_FRAME_N`. Per-mission frameN lives in the
Python harness, not the engine.

**What the engine does NOT do** (pruned 2026-05-04 after Step 1.5
revealed intro-pan / mission-script camera-driver fights):
- Camera teleport / API calls / goal-clearing
- Pose schema / JSON parsing
- Record-pose hotkey
- Layer-crossing bridge into `code/`

This is a deliberate scope reduction. The Stage 2.E spec wanted
"deliberately object-dense vantage" via camera-pin; the simplification
trades that for "engine-defined repeatable vantage" which experiment
will validate or invalidate at Step 1.7 variance measurement.

---

## Mission set (6 stock missions, 2026-05-04 swap)

| Mission | Intro | Notes |
|---|---|---|
| mc2_01 | ~10s estimate | typical opener |
| mc2_03 | ~20s | per user observation |
| mc2_05 | short / none | replaces mc2_17 (90s intro infeasible) |
| mc2_06 | sans intro | replaces mc2_10 (30s intro too long) |
| mc2_18 | unknown | object-density stress mission |
| mc2_24 | none | per user observation |

Per-mission `frameN` initial values (Step 1.6 Python harness; frame
count = `intro_seconds * 142 fps + ~200-frame settle buffer`):

```python
CAPTURE_FRAMES = {
    "mc2_01": 1700,   # ~10s intro
    "mc2_03": 3000,   # 20s intro
    "mc2_05": 400,    # ~no intro
    "mc2_06": 400,    # sans intro
    "mc2_18": 1700,   # unknown intro length, conservative default
    "mc2_24": 400,    # no intro
}
```

Tunable empirically after Step 1.7 variance measurement.

---

## Engine-side responsibility (already shipped Steps 1.1–1.5+cleanup)

| File | Role |
|---|---|
| `GameOS/gameos/gos_screenshot.{h,cpp}` | Shared `gos::screenshot::writeTGA()` helper (extracted from `gos_validate.cpp`) |
| `GameOS/gameos/gos_visual_diff.{h,cpp}` | Capture state machine: env reads, frame counter, `onFrameTick(viewportW, viewportH)` |
| `GameOS/gameos/gameosmain.cpp:524` | Call site — fires `VisualDiff::onFrameTick(...)` immediately after `pp->endScene()`, before projectZ overlay (`:530`) and HUD replay (`:533`) |

State machine (default-off):
```
on every frame tick:
  ++localFrame
  if first tick: read MC2_VISUAL_DIFF_FRAME_N + MAX_FRAMES env, log capture_armed
  if !missionStartObserved: snapshot if SmokeMode::missionHasStarted()
  if framesSinceStart > maxFrames: log capture_timeout, exit(4)
  if framesSinceStart >= frameN: writeTGA, log capture_done, phase=Done
```

Env vars:

| Var | Purpose | Default |
|---|---|---|
| `MC2_VISUAL_DIFF_CAPTURE` | Enables capture path. | unset / off |
| `MC2_VISUAL_DIFF_OUT` | TGA output path (required when CAPTURE=1). | unset |
| `MC2_VISUAL_DIFF_FRAME_N` | Target frame count post-mission-ready. | 90 |
| `MC2_VISUAL_DIFF_MAX_FRAMES` | Engine timeout before exit(4). | `frameN + 60` |
| `MC2_VISUAL_DIFF_MISSION` | Mission key (logging only — no pose lookup). | unset |

Exit codes:
- 0: clean exit (capture done or no-op when env unset)
- 4: `event=capture_timeout` — mission never reached frameN

---

## Phase 1 remaining steps

### Step 1.6 — Python harness skeleton + per-mission frameN dict

`tests/smoke/object_visual_diff.py` with `--measure-variance` and `--gate` modes
in one file. The variance mode runs each mission twice at
`MC2_GPU_OBJECTS=0`, computes per-pixel stats, and reports without
gating. Gate mode applies the spec's ≤2 LSB / ≤0.5% tolerance.

Per `feedback_smoke_serial_only.md`: launches sequential, never concurrent.
Per `feedback_smoke_no_canary.md`: no menu canary.

```python
TIER1   = ["mc2_01", "mc2_03", "mc2_05", "mc2_06", "mc2_24"]
STRESS  = ["mc2_18"]
MISSIONS = TIER1 + STRESS
CAPTURE_FRAMES = {...as above...}

def run_capture(mission, gpu_objects, out, harness_timeout_s=120):
    """Single mc2.exe run with MC2_VISUAL_DIFF_* env set, serial."""
    ...

def compute_stats(path_a, path_b):
    """Per-pixel: a pixel 'differs' if any channel abs(delta) > 2.
    Returns max_channel_delta, pixels_exceeding_2_lsb, ratio_exceeding_2_lsb,
    hot_regions (8x8 grid count)."""
    ...
```

### Step 1.7 — Same-config variance measurement

`python tests/smoke/object_visual_diff.py --measure-variance` runs each
mission TWICE at `MC2_GPU_OBJECTS=0` and reports stats. **NOT a gate** —
measurement only. Phase 2 entry contingent on the report.

If every mission's `ratio_exceeding_2_lsb ≤ 0.005`: proceed to Phase 2 gate.
If any exceeds: STOP and surface to advisor with hot_regions for cause
diagnosis. Decide between:
- (a) accept wider documented tolerance with rationale
- (b) frame-time shim for `SDL_GetTicks()` post-process uniforms (escalates scope)
- (c) disable specific time-driven effects during capture
- (d) different `frameN` for the failing mission (may catch transient issues)

### Step 1.8 — README

`tests/smoke/README.md` `## Visual diff (Stage 2.E)` section:
- Architecture: natural mission camera at frameN
- Env vars + defaults
- Variance measurement command
- Per-mission frameN dict in Python (where to tune)
- Out-of-scope: in-process mission restart (relaunch mc2.exe per capture)

---

## Phase 2 — same-config determinism gate (contingent)

### Step 2.0 — Variance review checkpoint
Read Step 1.7 report. Gate or escalate per options above.

### Step 2.1 — Python harness gate mode
`python tests/smoke/object_visual_diff.py --gate` runs the 6 missions twice
each at `MC2_GPU_OBJECTS=0`, applies ≤2 LSB / ≤0.5% tolerance, returns
exit 0 on PASS / 2 on FAIL. Diff-image output (red overlay) on failure.

### Step 2.2 — README gate command
Add gate command and exit-code mapping to `tests/smoke/README.md`.

---

## Phase 1 acceptance gate

1. With all `MC2_VISUAL_DIFF_*` env vars unset: zero behavior change.
   Verify via tier1 smoke 5/5 PASS, +0 destroys. **Already passing**
   (commits `5f00142`, `3417238`, `f1d0f28`).
2. With CAPTURE=1: TGA appears at MC2_VISUAL_DIFF_OUT path, scene+post-process
   minus projectZ minus HUD. **Already passing** (Step 1.5 / cleanup test runs).
3. Step 1.7 measurement produces a report for all 6 missions; report is
   the gate input for Phase 2.

---

## Phase 2 acceptance gate

`python tests/smoke/object_visual_diff.py --gate` exits 0 on all 6 missions.
If gate fails: STOP, do not widen tolerance, investigate via diff images +
hot_regions. Surface options (a)–(d).

---

## Out-of-scope reminders

- No edits to `gos_static_prop_batcher.{h,cpp}`, `gos_object_parity.{h,cpp}`, `mclib/{tgl,msl,bdactor,genactor}.cpp`, shaders.
- No new SSBO bindings.
- No default-on flip of any env var.
- Stock missions only.
- Build via `cmake --build build64 --config RelWithDebInfo --target mc2` ONLY.
- Deploy per-file `cp -f` + `diff -q`. NEVER `cp -r`.
- No frame-time shim unless Step 2.0 forces escalation.
- No in-process mission-restart support.
- No camera APIs in engine path (deferred to a future arc if natural-camera
  approach proves inadequate).

---

## Round history (educational)

The plan went through 5 adversarial-review rounds before reaching this
simplification. Key lessons preserved as memory notes:

- Round 1: pre-HUD seam was in `gameosmain.cpp`, not `gos_postprocess.cpp`. Wall-clock-driven shader uniforms make byte-exact gate infeasible — use spec tolerance from start.
- Round 2: stale negative-grep claim — `gos_State_IsHUD` IS alive; `markMissionReady()` is one-shot per process; pose schema must be MC2-native (RTS orbit camera).
- Round 3: `setFOV` was fictional (real API: `setFieldOfView`); `position.z` is overwritten by terrain elevation; goal-tracking interpolation defeats teleport.
- Round 4: Discipline win (17/17 spot-checks). Rotation goal-clearing was a strict no-op (`setGoalRotation(getGoalRotation())`); needed live rotation + zero timer.
- Round 5: Plan came back clean; code execution began.
- **Round 6 (this revision)**: Step 1.5's bridge worked correctly per telemetry, but visual evidence + user observation revealed the intro pan / mission-script camera-driver overwrites our teleport between teleport (frame N-3) and capture (frame N). Goal-clearing only stops the goal-tracking interpolator, not other camera drivers. Architectural simplification: don't fight the engine's camera director; capture its output. Drops 672 lines net (parser, bridge, teleport, hotkey, schema). Mission set swapped (mc2_17→mc2_05, mc2_10→mc2_06) per intro-duration data.

Memory notes saved during this arc:
- `feedback_inherited_citations_must_regrep.md` — round-2/3 lesson on negative-grep regression.

---

## Hand-off

Engine side is shipped (Steps 1.1–1.5+cleanup, commits
`290f8c1`, `686e29d`, `a7eff01`, `5f00142`, `3417238`, `f1d0f28`).

Python harness (Step 1.6) is the next deliverable. Variance measurement
(Step 1.7) answers the architectural question:

> Is the engine's natural mission camera at frame N reproducible enough
> for visual diff to gate at ≤2 LSB / ≤0.5% tolerance?

If yes: Phase 2 packages the gate. If no: surface hot_regions, decide
among options (a)–(d) including "revisit camera-pin if natural-camera
proves inadequate."
