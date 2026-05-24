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

// Flush collected world-space primitives to the GPU and draw.
// Must be called while the scene FBO is still bound
// (inside Environment.UpdateRenderers, after weather->render()).
// No-op when MC2_DEBUG_RENDERER is unset.
void flushWorldPrims();

// Reserved: future crisp HUD/debug-text pass (post post-process). No-op in M1.
void flushScreenPrims();

} // namespace DebugRenderer
