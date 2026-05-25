#pragma once
#include <cstdint>

namespace DebugRenderer {

struct Vec3 { float x, y, z; };

// Enqueue a world-space line segment. No-op when MC2_DEBUG_RENDERER is unset.
void drawLineWorld(Vec3 a, Vec3 b, uint32_t rgba);

// Enqueue a world-space AABB as 12 edges (24 verts).
// No-op when MC2_DEBUG_RENDERER is unset.
void drawAabbWorld(Vec3 mn, Vec3 mx, uint32_t rgba);

// Enqueue a world-space horizontal ring (XZ plane at center.y).
// segments clamped to [3, 256]. No-op when MC2_DEBUG_RENDERER is unset.
void drawRingWorld(Vec3 center, float radius, int segments, uint32_t rgba);

// Oriented ring in the plane perpendicular to 'normal'.
// Falls back to XZ-plane ring when normal has near-zero length (< 1e-6).
// segments clamped to [3, 256]. No-op when MC2_DEBUG_RENDERER is unset.
void drawRingWorld(Vec3 center, Vec3 normal, float radius, int segments, uint32_t rgba);

// World-space 3-axis cross: lines along X, Y, Z through center, halfSize each side.
// No-op when MC2_DEBUG_RENDERER is unset.
void drawCrossWorld(Vec3 center, float halfSize, uint32_t rgba);

// Approximate sphere: 3 great-circle rings (XY, YZ, XZ planes) plus (rings-1)
// latitude rings. rings clamped to [1, 16]; segments passed through ([3, 256]).
// No-op when MC2_DEBUG_RENDERER is unset.
void drawSphereApproxWorld(Vec3 center, float radius, int rings, int segments, uint32_t rgba);

// Reserved: world-space text label. No-op in M1/M2.
// Future spec: billboard projected from 'pos' to screen, rendered in flushScreenPrims.
void drawTextWorld(Vec3 pos, const char* text, uint32_t rgba);

// Flush collected world-space primitives to the GPU and draw.
// Must be called while the scene FBO is still bound
// (inside Environment.UpdateRenderers, after weather->render()).
// No-op when MC2_DEBUG_RENDERER is unset.
void flushWorldPrims();

// Reserved: future crisp HUD/debug-text pass (post post-process). No-op in M1.
void flushScreenPrims();

} // namespace DebugRenderer
