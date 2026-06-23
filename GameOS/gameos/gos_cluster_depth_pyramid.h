#pragma once
// CLUSTER-DEPTH-PYRAMID-NATIVE-1
//
// A gated, MC2-native GPU compute pass that reads the existing sampleable scene
// depth texture and writes a per-tile (min,max) depth image (RG32F). This is a
// SUBSTRATE slice only: there is NO lighting / decal / material consumer of the
// output yet. Its purpose is to build (and CPU-verify) the depth-tile pyramid
// that a future clustered-light cull pass needs, while shipping ZERO visual
// change.
//
// Gates (resolved once from env at first use):
//   MC2_CLUSTER_DEPTH_PYRAMID         default OFF — master gate. OFF => the pass
//                                     allocates nothing and dispatches nothing
//                                     (true no-op, byte-identical behavior).
//   MC2_CLUSTER_DEPTH_PYRAMID_VERIFY  default OFF — one-shot CPU-vs-GPU parity
//                                     check on a single fixed frame (requires
//                                     the master gate). Reads the scene depth
//                                     back to CPU, recomputes per-tile min/max,
//                                     compares against the GPU image readback,
//                                     logs PASS/FAIL with counts.
//   MC2_CLUSTER_DEPTH_PYRAMID_PLANT   default OFF — planted-error self-test of
//                                     the verifier (requires VERIFY). Corrupts
//                                     one CPU reference tile so the comparison
//                                     SHOULD report a mismatch — proves the
//                                     checker can actually fail.
//
// REVERSED-Z: output R = numeric MIN depth, G = numeric MAX depth. MC2 is
// reversed-Z (near ~= 1, far ~= 0); the pass stores raw extents and leaves the
// near/far interpretation to the consumer (nearest = MAX, farthest = MIN).

namespace cluster_depth_pyramid {

// True when MC2_CLUSTER_DEPTH_PYRAMID is set (and not "0"). Cached.
bool IsEnabled();

// Run the pass for the current frame. sceneDepthTex is the sampleable scene
// depth texture (gosPostProcess::getSceneDepthTexture()); width/height are the
// scene render-target dimensions. No-op when the gate is OFF, when the texture
// is 0, or when dimensions are non-positive. Safe to call every frame.
void Run(unsigned int sceneDepthTex, int width, int height);

// Release GL resources (program + output texture). Safe to call when nothing
// was ever allocated.
void Shutdown();

// --- Accessors for a downstream consumer (MC2-LIGHTGRID-BUILD-NATIVE-1) ------
// The per-tile (min,max) RG32F image written by the last Run(), and its tile
// grid dimensions. Returns 0 / 0x0 when nothing has been built yet (gate OFF,
// or before the first dispatch). Read-only — the lightgrid builder samples this
// texture; it never mutates the pyramid's state.
unsigned int TileTexture();
int TileGridW();
int TileGridH();
int TileSize();

}  // namespace cluster_depth_pyramid
