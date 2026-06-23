#pragma once
// MC2-LIGHTGRID-BUILD-NATIVE-1
//
// A gated, MC2-native GPU compute pass that builds a per-tile light-bin grid
// from a native sphere cull-geometry buffer, validated against a CPU reference.
// This is an INERT INFRASTRUCTURE slice: there is NO shading / lighting consumer
// of the grid, and it produces ZERO visual change. Its purpose is to build (and
// CPU-verify) the light-grid machinery a future clustered-light shading pass
// would need, mirroring the CLUSTER-DEPTH-PYRAMID-NATIVE-1 build+parity pattern
// one slice up.
//
// PIPELINE (two GPU stages, both from shaders/lightgrid_build.comp):
//   Stage 0 (sphere build): read the live ObjectLights SSBO (binding 20) and
//     derive a native MC2LightCullSphere[] buffer. POINT and SPOT are BOTH
//     binned as spheres (MC2 stores no cone half-angle for SPOT -> cone culling
//     is DEFERRED, no source data). center = light_to_world translation;
//     radius = light_falloff.y (far distance); type = light_dir.w.
//   Stage 1 (grid build): consume the sphere buffer + the depth pyramid's per-
//     tile (min,max) RG32F image. Per tile, reconstruct the world-space sub-
//     frustum (4 screen-tile corners x near/far depth) and test each sphere
//     against it. Survivors are LDS-staged, then ONE global atomicAdd reserves a
//     span in a compact global index pool, survivors are compact-written, and a
//     per-tile (offset,count) RG32UI header image is stored. (BT-shaped append:
//     stage-then-single-reserve, NOT a per-candidate global-atomic storm.)
//
// REVERSED-Z GATE: MC2 is reversed-Z (near ~= 1, far ~= 0). The depth pyramid
// stores R = numeric MIN, G = numeric MAX, so the NEAREST surface is the numeric
// MAX (G) and the FARTHEST is the numeric MIN (R). The grid builder reads
// near = MAX(G), far = MIN(R). (Asserted in the parity checker + documented.)
//
// Gates (resolved once from env at first use):
//   MC2_LIGHTGRID_BUILD    default OFF — master gate. OFF => the pass allocates
//                          nothing and dispatches nothing (true no-op, byte-
//                          identical behavior).
//   MC2_LIGHTGRID_VERIFY   default OFF — one-shot CPU-vs-GPU parity check on a
//                          single fixed frame (requires the master gate). Reads
//                          back the GPU sphere buffer + depth pyramid + grid
//                          header + index pool, recomputes the binning on CPU,
//                          compares, logs PASS/FAIL with counts.
//   MC2_LIGHTGRID_PLANT    default OFF — planted-error self-test of the verifier
//                          (requires VERIFY). Corrupts one CPU reference tile so
//                          the comparison SHOULD report a mismatch — proves the
//                          checker can actually fail.

namespace lightgrid_build {

// True when MC2_LIGHTGRID_BUILD is set (and not "0"). Cached.
bool IsEnabled();

// Run the pass for the current frame. Must be called AFTER the cluster depth
// pyramid has run this frame (it samples the pyramid's tile texture) and AFTER
// the ObjectLights SSBO is bound at binding 20 (true in gosPostProcess::endScene).
//   invViewProj16 : the engine's row-major inverseViewProj (gosPostProcess::
//                   getInverseViewProj()), uploaded GL_FALSE to match every other
//                   post pass's clip->world convention.
//   sceneW/sceneH : the scene render-target dimensions (for NDC reconstruction).
// No-op when the gate is OFF, when the depth pyramid produced no tile texture,
// or when dimensions are non-positive. Safe to call every frame.
void Run(const float* invViewProj16, int sceneW, int sceneH);

// Release GL resources. Safe to call when nothing was ever allocated.
void Shutdown();

}  // namespace lightgrid_build
