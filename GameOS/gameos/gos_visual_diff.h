// gos_visual_diff.h - Stage 2.E natural-mission-camera capture harness.
//
// The engine waits for mission_ready, counts frames, captures the pre-HUD
// framebuffer at MC2_VISUAL_DIFF_FRAME_N, and exits. No camera teleport, no
// pose authoring, no goal-clearing, no camera APIs. The mission's own intro
// pan / command-position settle gives us a deterministic vantage; the
// harness just snapshots it at a chosen frame.
//
// Per-mission frameN values live in the Python harness (Phase 1 Step 1.7),
// not in the engine.
#pragma once

namespace VisualDiff {

// True iff MC2_VISUAL_DIFF_CAPTURE is set in the environment.
// Reads env once on first call; cached.
bool isCaptureEnabled();

// Per-frame hook. Wired into the gameosmain.cpp render loop at the pre-HUD
// seam (between pp->endScene() and projectz_overlay_render). When
// isCaptureEnabled() is false, returns immediately.
//
// When enabled:
//   - Owns its own localFrame counter (independent of SmokeMode::g_frameCount)
//   - Snapshots its own "mission start frame" on the first tick after
//     SmokeMode::missionHasStarted() returns true
//   - At framesSinceStart == MC2_VISUAL_DIFF_FRAME_N (default 90):
//     writes a TGA via gos::screenshot::writeTGA to MC2_VISUAL_DIFF_OUT
//   - At framesSinceStart > MC2_VISUAL_DIFF_MAX_FRAMES (default frameN+60):
//     logs capture_timeout and exits process with code 4
//
// viewportW/viewportH come from Environment.drawableWidth/Height at the
// caller.
void onFrameTick(int viewportW, int viewportH);

// Mission-load reset hook (currently uncalled from external code; reserved
// for future engine-side wiring if in-process restart support is needed).
void onMissionLoad();

}  // namespace VisualDiff
