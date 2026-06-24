#pragma once
// POSTPROCESS-COMPUTE-BLUR-1
//
// A gated, MC2-native GPU compute downsample + separable-Gaussian-blur SUBSTRATE.
// Greenfield (the old bloom/HDR path was DELETED — this is not a revive). It
// exercises the typed-sync + ping-pong compute pattern as Vulkan-prep, and is
// CPU-verifiable, while shipping ZERO visual change: there is NO bloom / glow /
// DOF consumer of the blurred output.
//
// PIPELINE (per Run):
//   1. DOWNSAMPLE  full-res input -> half-res RGBA16F (ping image A) via 2x2 box.
//   2. BLUR_H      ping A -> ping B (horizontal 5-tap Gaussian).
//   3. BLUR_V      ping B -> ping A (vertical   5-tap Gaussian).
// Output (blurred half-res) lives in ping A. No one reads it (substrate only).
// Typed gpuSyncBarrier edges order each stage's imageStore before the next
// stage samples it (ComputeImageWrite -> TextureSample). NO raw glMemoryBarrier.
//
// INPUT: to stay self-contained and give a clean CPU-vs-GPU parity, the pass owns
// a DETERMINISTIC input texture (a fixed test pattern it uploads). When the
// master gate is ON and a feedback-safe scene-color copy exists
// (MC2_VFX_SCENECOLOR_GRAB on => getSceneColorCopyTexture() != 0), the live ON
// path blurs that copy instead — but the PROVABLE acceptance (VERIFY) always runs
// on the controlled test pattern so parity is independent of any scene RNG.
//
// Gates (resolved once from env at first use):
//   MC2_POSTPROCESS_COMPUTE_BLUR         default OFF — master gate. OFF => the
//                                        pass allocates nothing and dispatches
//                                        nothing (true no-op, byte-identical).
//   MC2_POSTPROCESS_COMPUTE_BLUR_VERIFY  default OFF — one-shot CPU-vs-GPU parity
//                                        check on the controlled test pattern
//                                        (requires the master gate). Runs the same
//                                        separable Gaussian on the CPU, reads the
//                                        GPU output back, compares within a stated
//                                        float tolerance, logs PASS/FAIL.
//   MC2_POSTPROCESS_COMPUTE_BLUR_PLANT   default OFF — planted-error self-test of
//                                        the verifier (requires VERIFY). Corrupts
//                                        one CPU reference texel so the comparison
//                                        SHOULD report a mismatch.

namespace postprocess_blur {

// True when MC2_POSTPROCESS_COMPUTE_BLUR is set (and not "0"). Cached.
bool IsEnabled();

// Run the pass for the current frame. `srcTex` is an OPTIONAL live input (e.g.
// gosPostProcess::getSceneColorCopyTexture()); pass 0 if none. width/height are
// that texture's full-res dimensions (ignored when srcTex == 0). No-op when the
// gate is OFF. The VERIFY path always uses the internal test pattern regardless.
void Run(unsigned int srcTex, int width, int height);

// Release GL resources (programs + owned textures). Safe to call when nothing
// was ever allocated.
void Shutdown();

}  // namespace postprocess_blur
