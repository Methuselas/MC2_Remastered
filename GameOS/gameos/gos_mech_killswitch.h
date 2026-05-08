// GameOS/gameos/gos_mech_killswitch.h
#pragma once
#include <cstdint>

// Runtime toggle for the GPU mech renderer.
// Default: enabled by env-var MC2_GPU_MECHS=1 at process start.
// Can also be toggled at runtime via RAlt+M (wire in gameosmain.cpp hotkey handler).
extern bool g_useGpuMechs;

// Resolve a gosTextureHandle to the underlying raw GL texture name.
// Returns 0 if handle is INVALID_TEXTURE_ID or gosTexture is gone.
// Implemented in gameos_graphics.cpp (same as gos_static_prop_killswitch.h).
uint32_t gos_GetGLTextureId(uint32_t gosHandle);

// Terrain projection chain uniforms — same as gos_static_prop_killswitch.h.
const float* gos_GetTerrainViewportVec4();   // (vmx, vmy, vax, vay)
const float* gos_GetProj2ScreenMat4();       // screen-pixel -> NDC (upload GL_TRUE)
const float* gos_GetTerrainMVPMat4();        // axisSwap * worldToClip (upload GL_FALSE)
