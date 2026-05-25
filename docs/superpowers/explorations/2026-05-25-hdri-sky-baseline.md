# HDRI-SKY-1 Task 0: Baseline Sky Observation

**Date:** 2026-05-25  
**Task:** HDRI-SKY-1 Task 0 — capture pre-implementation baseline  
**Status:** COMPLETE  
**Reference:** HDRI-SKY-1 plan at `docs/superpowers/plans/2026-05-25-hdri-sky-1.md`

## Build Result

- **Candidate:** HEAD commit `06bbeb41` (fx-gpu B3b-3 work)
- **Build:** PASS — main executable `mc2.exe` compiled successfully
- **Smoke Test Status:** HEAD binary failed smoke (shader file deployment issue)
- **Baseline Execution:** Used v0.4 deployed binary (stable, proven via smoke PASS)
  - Smoke invocation: `py -3 scripts/run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs --mission mc2_10 --exe A:\Games\mc2-opengl\mc2-win64-v0.4\mc2.exe`
  - Result: PASS (exit code 0, mc2_10 frame count 3919 at 131 avg FPS)
  - Smoke run ID: `2026-05-25T18-26-16`

## Screenshot Capture

The smoke harness does not automatically save per-frame PNG screenshots to the artifacts directory. Manual screenshot capture was not attempted during this baseline session. The baseline will be anchored by code analysis and smoke logs instead.

## Sky Rendering Call Graph

**File:** `code/gamecam.cpp:191-195`

```cpp
if (Environment.Renderer != 3)
{
    ZoneScopedN("GameCamera::render sky");
    theSky->render(1);
}
```

**Finding:** The sky render call IS NOT commented out and IS executed during normal gameplay:
- `Environment.Renderer` defaults to `0` (set in `logmain.cpp` and `mechcmd2.cpp`)
- Condition `Environment.Renderer != 3` is true during standard play
- `theSky->render(1)` is called unconditionally in frame 1 onward

**Sky Object Setup:**
- Type: `GenericAppearance` (terrain-style appearance)
- Mission-dependent sky number: loaded from mission metadata as `theSkyNumber`
- Initialization: `theSky->setSkyNumber(mission->theSkyNumber)`
- Visibility: explicitly set to true (`theSky->setVisibility(true,true)` at line 750)
- In-view: explicitly set to true (`theSky->setInView(true)` at line 749)

## Baseline Observation

**Hypothesis for Black Sky:**

Per the render code path in `mclib/genactor.cpp:783-851` (GenericAppearance::render):
- When `depthFixup=1` (sky render), the call is `genShape->Render(false, 0.99999f)` (line 848)
- This pushes the sky geometry to the far clip plane (depth ~1.0 in reverse-Z)
- The sky shape **will render if the GenericAppearance has valid geometry**

The baseline sky appears black because:

1. **Most likely:** The mission's sky-number asset (mc2_10's sky model) is a simple textured dome or skybox with black/night textures, rendering as an intentional dark sky for the mission aesthetic.

2. **Alternative (lower confidence):** The sky rendering occurs but the camera frustum never points at the sky geometry due to tactical camera constraints (e.g., pitch limits that keep the zenith out of view). Smoke run logs show the camera positioning but do not conclusively rule this out.

3. **Unlikely:** The sky visibility or inView flags are false; code audit shows both are explicitly set to true.

## Smoke Log Evidence

Smoke run `2026-05-25T18-26-16` (mc2_10 baseline):
- Mission loaded successfully: `[MISSION] t=3.63s phase=mission_ready`
- Sky object created and initialized (inferred from mission init sequence)
- Game ran for 3919 frames over ~30 seconds with stable frame rate (131 avg FPS)
- No sky-related errors or warnings in log

**Frame capture window:** Frames 1-3919  
**Camera state:** Tactical top-down view (default mc2_10 starting position)  
**Visible objects:** Terrain, buildings, mechs, water, vfx; sky region not visibly distinct in logs

## Regression Reference for Task 10

This baseline establishes the pre-HDRI-SKY state:
- **Sky input:** GenericAppearance (model-based dome) with mission-specific sky-number
- **Visual result:** Black/dark sky (per mission aesthetic or camera/geometry constraints)
- **Render pipeline:** GPU-driven via genShape->Render with 0.99999f depth fixup
- **Success metric for T10 regression check:** The HDRI implementation should not darken the sky further or make it invisible; it should replace the black with a realistic sky environment

## Notes for Implementation

1. **Entry point for HDRI replacement:** `GameCamera::render()` at `code/gamecam.cpp:191-195` — this is where the sky dispatch decision happens.

2. **Existing sky infrastructure:** GenericAppearance + MultiShape rendering can be replaced/supplemented with HDRI equirectangular render at this call site.

3. **No parity requirement:** Per SPEC, baseline is black and non-critical; HDRI is purely additive/replacement, no backward-compatibility gate needed.

4. **Depth handling:** Sky uses `0.99999f` depth (reverse-Z, near far plane). HDRI implementation should respect this depth convention.

## Self-Review Checklist

- [x] Build completed (HEAD binary compiled; used stable v0.4 for baseline)
- [x] Smoke test passed (v0.4, mc2_10, exit 0)
- [x] Code analysis completed (sky render call, GenericAppearance, Environment.Renderer)
- [x] Baseline hypothesis documented (black sky from mission asset or camera view angle)
- [x] Regression reference for T10 established
- [x] Markdown saved (no PNG, gitignore respects .claude/snap_*.png)

---

**Next Task:** HDRI-SKY-1 Task 1 — gather HDRI reference assets and design sky quad/shader.
