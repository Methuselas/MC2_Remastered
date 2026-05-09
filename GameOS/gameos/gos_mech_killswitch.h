// GameOS/gameos/gos_mech_killswitch.h
#pragma once
#include <cstdint>

// Runtime toggle for the GPU mech renderer.
// Default: enabled by env-var MC2_GPU_MECHS=1 at process start.
// Can also be toggled at runtime via RAlt+M (wire in gameosmain.cpp hotkey handler).
extern bool g_useGpuMechs;

// Slice B1: gates VS-side calc_light() in mech.vert. Independent of
// g_useGpuMechs so an operator can keep GPU mech rendering on while
// flipping lighting off if a B1 regression surfaces. No effect when
// g_useGpuMechs is off (the entire batcher path skips).
extern bool g_useGpuMechLighting;

// Slice C1: render-side mech cull. Skips submitActor for mechs the
// GPU frustum/visibility shader marks invisible. RENDER-ONLY — does
// NOT cascade into update/AI/lifecycle. No effect when g_useGpuMechs
// is off. Independent of MC2_GPU_CULL_LIFECYCLE.
extern bool g_useGpuMechCull;

// Slice C2: weighted multi-bone skinning in mech.vert. Off = rigid
// per-bone (single boneIndices.x lookup, Slice A behavior). On =
// weighted blend across all 4 bone slots. Stock data is byte-
// identical between the two modes (boneWeights = 1,0,0,0 collapses);
// the difference is only meaningful for imported meshes.
extern bool g_useGpuMechSkin;

// Slice C3-revised (2026-05-09): wire TransformMultiShape_PositionsOnly
// for the GPU mech body. Skips the per-vertex CPU lighting kernel that
// consumes ~65µs/mech and whose output (listOfVertices[j].argb) is only
// consumed by the legacy CPU Render(true) path that Slice A bypasses.
// Independent of g_useGpuMechs / g_useGpuMechLighting for bisect
// granularity. Requires g_useGpuMechs=true to take effect (when GPU
// mech path is off, the legacy path NEEDS the lighting bake's output).
// BODY-ONLY: arms (mech3d.cpp:4459, :4543) and shadow (mech3d.cpp:3377)
// stay full TransformMultiShape; their Render(true) callers still need
// listOfVertices[*].argb populated.
extern bool g_useGpuMechFastTransform;

// Resolve a gosTextureHandle to the underlying raw GL texture name.
// Returns 0 if handle is INVALID_TEXTURE_ID or gosTexture is gone.
// Implemented in gameos_graphics.cpp (same as gos_static_prop_killswitch.h).
uint32_t gos_GetGLTextureId(uint32_t gosHandle);

// Terrain projection chain uniforms — same as gos_static_prop_killswitch.h.
const float* gos_GetTerrainViewportVec4();   // (vmx, vmy, vax, vay)
const float* gos_GetProj2ScreenMat4();       // screen-pixel -> NDC (upload GL_TRUE)
const float* gos_GetTerrainMVPMat4();        // axisSwap * worldToClip (upload GL_FALSE)
