// gos_rdoc_capture.h - Tier 5 in-process RenderDoc capture hook.
//
// Env-gated, default-off. Triggers a single-frame RenderDoc capture at
// MC2_RDC_CAPTURE_FRAME (counted from the first frame after the engine's
// own intro-pan-complete latch, gos_terrain_indirect::WasEverFrameSolidArmed),
// then optionally exits the process so the harness can post-process the
// resulting .rdc deterministically.
//
// The hook calls into the RenderDoc in-application API
// (3rdparty/renderdoc/renderdoc_app.h). renderdoc.dll must be loadable at
// runtime; the canonical way is to launch mc2.exe under
// `renderdoccmd capture` (RenderDoc injects itself). The Python harness
// (scripts/renderdoc_capture.py) drives that wrapper end-to-end.
//
// When MC2_RDC_CAPTURE_FRAME is unset or renderdoc.dll cannot be resolved,
// every hook becomes a cheap early-return; the default smoke / build is
// untouched. The hook intentionally shares the post-PP / pre-HUD seam with
// VisualDiff::onFrameTick so the captured frame matches the visual-diff
// frame semantics (intro-complete + N frames).
#pragma once

namespace RdocCapture {

// True iff MC2_RDC_CAPTURE_FRAME is set in the environment (lazy, cached).
bool isEnabled();

// Per-frame hook. Wired alongside VisualDiff::onFrameTick. No-op when
// isEnabled() is false. When enabled: waits for the WasEverFrameSolidArmed
// latch, counts frames, drives RENDERDOC_API_1_5_0::TriggerCapture() on the
// target frame, and exits the process on the following frame (after
// RenderDoc has finalized the .rdc) when MC2_RDC_EXIT_AFTER is set.
void onFrameTick();

}  // namespace RdocCapture
