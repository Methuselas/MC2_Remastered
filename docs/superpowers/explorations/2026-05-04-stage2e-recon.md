# Stage 2.E Phase 0 — Recon

**Date:** 2026-05-04
**Worktree HEAD:** `f443194` (handoff-prompt commit)
**Branch:** `claude/nifty-mendeleev`

Recon answers the questions the handoff laid out plus one user-supplied
question about mc2_18 spawn-camera framing. Read-only. No code changed.

---

## Q1. Does Stage 1.E exist?

**No.** Stage 2.E will build the harness from scratch; slice 1's PR
will reuse it.

- `docs/superpowers/specs/2026-05-02-object-offload-slice1-design.md:361-383`
  specifies `tests/smoke/object_visual_diff.py` but the file is absent.
- Slice 2 design at `docs/superpowers/specs/2026-05-02-object-offload-slice2-design.md:366`
  is explicit: *"If slice 1's Stage 1.E hasn't landed yet, this stage builds
  it."*

**Implication:** the harness is shared infra, not slice-2-specific. Land it
under `tests/smoke/object_visual_diff.py` (per spec) so slice 1 can drop in.

---

## Q2. In-engine screenshot capture mechanism?

**Partial — `gos_validate.cpp` already has a working TGA capture; rewire,
don't reinvent.**

- `GameOS/gameos/gos_validate.cpp:73-82` — `writeScreenshotTGA()`:
  ```
  glReadPixels(0, 0, w, h, GL_BGR, GL_UNSIGNED_BYTE, pixels);
  ```
  Writes uncompressed TGA from the **default framebuffer** (back buffer).
- `GameOS/gameos/gos_validate.cpp:114-121` — `validateWriteResults()` is the
  entry point, gated by `ValidateConfig::screenshotPath[512]` (CLI/env).

**Gotcha (load-bearing):** `validateWriteResults` is called against the
final framebuffer — **post-HUD composite**. HUD is non-deterministic
(timer text, mouse cursor position, dynamic prompts) and must be excluded
from the diff region.

**Resolution options:**
- **(A) Capture at a different point in the frame** — call the capture
  before HUD composite (insert hook in `gos_postprocess` after scene
  resolve, before HUD draw). Cleanest; one rewire site.
- **(B) Capture as-is, mask HUD pixels in the Python diff.** Cheaper
  to implement but couples diff math to HUD geometry; if HUD layout
  changes, baseline regions become wrong.

Recommend (A): one engine-side change, no per-resolution mask
maintenance, no false-positive risk from HUD repositioning.

---

## Q3. Camera-pinning mechanism?

**None exists.** Camera is player-controlled or mission-script-driven.
The smoke runner only pins RNG seed (`MC2_SMOKE_SEED=0xC0FFEE`,
`gos_smoke.h:17-18`), not camera.

This is the load-bearing engineering problem of Stage 2.E.

**Three architectures considered:**

### (i) Inspect deployed mission files for spawn camera
Mission `.fit`/`.mis` files live in FST archives (deploy
`data/missions/` is empty — `find` confirms). Extracting and
parsing FST archives just to read a spawn-camera coordinate is
significant yak-shaving for a Phase-0 lookup.

### (ii) Recorded minimap-click input replay (user's fallback)
Replay a fixed input sequence (minimap click → camera moves to dense
area). **Problem:** input replay takes wall-clock time. AI ticks,
mission timers, and mech movement progress during replay → camera
arrives at the dense area on a different frame each run, with
different per-actor LOD selection and per-actor positions.
Non-deterministic by construction.

### (iii) New debug command for camera override at frame N (recommended)
Wire a env-gated camera teleport into the smoke harness:
- Pre-configure per-mission `(eyeX, eyeY, eyeZ, lookAtX, lookAtY, lookAtZ)`
  as a Python dict OR a sidecar JSON.
- Engine reads `MC2_VISUAL_DIFF_CAPTURE=1` + `MC2_VISUAL_DIFF_MISSION=mc2_18`
  + the per-mission camera tuple.
- At frame N (e.g., 60), force the camera to the pinned pose, then
  fire `validateWriteResults` one frame later (frame 61) so the
  next render uses the pinned camera.

**Frame-counter reuse:** `SmokeMode::g_frameCount`
(`gos_smoke.cpp:29`) and the parity ring's `s_frameSlot %
RING_FRAMES` cycle (`gos_static_prop_batcher.h:STATIC_PROP_RING_FRAMES`,
`gos_object_parity.h:185-215`) prove the determinism pattern. Stage
2.E reuses the same monotonic frame index.

**Why (iii) wins:**
- No FST extraction (i avoided)
- No wall-clock real-time (ii avoided)
- Reuses the proven Stage-2.D frame-N timing discipline
- One env-gated code path, default-off (handoff acceptance criterion 7)

---

## Q4. `tests/smoke/baselines/` contents?

```
tests/smoke/baselines/
  pre-trackd-mvp.json   (1441 B)
```

The directory exists with ONE JSON manifest from earlier work. No image
baselines. Stage 2.E will populate it (per advisor decision in handoff:
sidecar / `.gitignore`-d initially, decide commit policy later).

---

## Q5. mc2_18 spawn-camera framing (load-bearing user input)

**Cannot answer from static files alone.** Mission FIT files are inside
FST archives, not on disk in source or deploy.

**Mitigation by architecture (iii):** spawn-camera framing becomes
irrelevant — the harness teleports the camera to a known pose at frame
N, regardless of where the mission spawned it. The user's mc2_18
preference is honored by *choosing the camera teleport target* to
frame an object-dense region. This requires one human-in-the-loop step
per mission: user records the (eye, lookAt) tuple at a chosen vantage
point and adds it to the per-mission camera dict. After that, every
future capture run is deterministic.

**Recording the per-mission camera pose** can be automated via:
- One-shot debug command (`MC2_VISUAL_DIFF_RECORD=1`) that prints
  the current camera pose to stdout when a hotkey fires; user
  positions camera, hits hotkey, copy-pastes into the dict.
- Or simply add a `--record-camera mc2_18` flag to the harness CLI
  that drives that flow.

This is the place where the user's "minimap click" intuition is correct:
human positions the camera ONCE at a good vantage point. Then determinism
takes over.

---

## Q6. Parity harness frame-pinning — reusable?

**Yes, the pattern is directly reusable.**

- `GameOS/gameos/gos_object_parity.h:185-215` — dual-emit state machine
  uses `RING_FRAMES = 3` and tracks `s_frameSlot % RING_FRAMES` to know
  when an Armed frame's GPU readback is ready.
- `GameOS/gameos/gos_static_prop_batcher.cpp:64,1169,1216` — increments
  `s_frameSlot` once per `flush()` (per rendered frame).

For visual diff:
- We don't need the readback ring — we just need a stable monotonic
  "frames since mission-start" counter.
- `SmokeMode::g_frameCount` (`gos_smoke.cpp:29`) plus
  `SmokeMode::missionHasStarted()` give us that for free.

---

## Bottom line

**BUILD FROM SCRATCH** — but reuse three pieces:
1. `gos_validate.cpp` TGA capture (rewire from post-HUD to pre-HUD).
2. `SmokeMode::g_frameCount` for "frame N post-mission-start" timing.
3. The Stage-2.D frame-N + env-gate pattern as the determinism contract.

**Three open questions blocking Phase 1 → user input requested:**

1. **Camera-pin architecture.** Recon recommends architecture (iii) —
   per-mission camera-teleport-at-frame-N override, with a
   record-camera flow for one-time human-in-the-loop setup. User's
   "minimap click" intuition is preserved (human picks a good
   vantage point ONCE per mission), but the harness then runs
   deterministic. **Confirm this approach before Phase 1.**

2. **Capture point in frame.** Pre-HUD (architecture A) vs post-HUD
   with mask (architecture B). Recon recommends A. **Confirm.**

3. **Tier1 mission set + per-mission camera vantage points.**
   The handoff lists tier1 as `mc2_01, mc2_03, mc2_10, mc2_17, mc2_24`
   (5 missions). User suggested mc2_18 specifically as the
   most object-heavy intro mission. Options:
   - (a) Replace one tier1 mission with mc2_18 for the visual-diff gate.
   - (b) Capture all 5 tier1 + mc2_18 as a 6th gate-only mission.
   - (c) Keep tier1 as-is and rely on the user's vantage-point
        selection to make each mission object-heavy at the pinned camera.
   Recommend (c) — keeps tier1 stable (already validated by 2.D parity)
   and uses the camera-pin mechanism to ensure each mission's capture
   contains enough objects to detect divergence. mc2_18 can still be
   added as a 6th if the user wants extra coverage.

---

## Suggested Phase 1 scope (pending Q1-Q3 answers above)

If the user confirms architecture (iii) + (A) + (c):

1. **Engine side:**
   - Add `MC2_VISUAL_DIFF_CAPTURE=1` env gate.
   - Add per-mission camera-pose lookup (driven by `MC2_VISUAL_DIFF_MISSION`
     or read from a sidecar JSON).
   - Add capture hook in `gos_postprocess` after scene composite,
     before HUD draw.
   - Wire `SmokeMode::g_frameCount` to fire teleport at frame N-1
     and capture at frame N.
   - Add `MC2_VISUAL_DIFF_RECORD=1` flow for human vantage-point
     authoring (hotkey prints camera pose).

2. **Python side (`tests/smoke/object_visual_diff.py`):**
   - Per-mission camera-pose dict (or sidecar JSON).
   - Run baseline + test passes serially per `feedback_smoke_serial_only`.
   - Diff: ≤2 LSB per channel, ≤0.5% pixels differing → PASS.
   - Output diff image (red overlay on test) for failures.

3. **Phase 1 gate:** same-config repeat captures match within tolerance
   (determinism check). Per handoff, this is the prerequisite for
   trusting the gate.

---

## Hand-off

The substrate is locked. Phase 0 is done. Awaiting user decisions on:
- Camera-pin architecture (recommend iii)
- Capture point (recommend pre-HUD / A)
- Mission set (recommend tier1-as-is + camera-pose authoring per mission)

Then Phase 1 is bounded and ready.
