// GameOS/gameos/gos_particle_bridge.h
//
// Bridge entry point for the GPU particle batcher. The CPU producer-side
// API lives in mclib/particles/ (no GL dependency); this bridge owns the
// SSBO + VAO + shader program + draw call.
//
// Plan v5 §5.4 B1 Stage 1' Commit 1 — stub registered, no GL work yet.
// Stage 1' Commit 4 wires the actual flush.
// FX-GPU-1 Phase 2: per-group UV sub-rect support (GroupInfo array).

#pragma once

namespace mc2 { namespace particles { struct GpuParticle; } }
namespace mc2 { namespace particles { struct GroupInfo; } }

// C linkage so mclib/particles/batcher.cpp can forward-declare without
// pulling C++ name-mangling expectations from GameOS into mclib.
//
// groups / numGroups: flat array of GroupInfo records (one per BeginGroup
//   call this frame). Each entry describes a contiguous sub-range of
//   `records[]` (via .start / .count), the gos texture handle (.handle),
//   and the UV sub-rect (.u0 .v0 .us .vs) to use for that draw call.
//   numGroups==0 is treated as a single implicit full-rect group for
//   back-compat (should not happen in practice after Phase 2).
extern "C" void gos_particle_bridge_flush(const mc2::particles::GpuParticle* records,
                                          unsigned int                       count,
                                          const mc2::particles::GroupInfo*   groups,
                                          unsigned int                       numGroups);

/* B2: active camera bridge — temporary stop-gap until RenderFrameContext lands.
 * Set by GameCamera::render() immediately before particle flush; cleared after.
 * If never set this frame, accessors return last-known basis (identity at boot).
 */
extern "C" void gos_SetActiveCamera(const float right_xyz[3], const float up_xyz[3]);
extern "C" void gos_GetCameraRight(float out_xyz[3]);
extern "C" void gos_GetCameraUp(float out_xyz[3]);
extern "C" void gos_ClearActiveCamera(void);

/* VFX-DEBUG-VIEWS-1 / VFX-TUNING-UI-1: read-only debug-mode + runtime
 * intensity scales for the Graphics Options "VFX Tuning" section. All scales
 * default 1.0 (byte-identical no-op), clamped 0..8; debug mode 0..4. Look-only:
 * no emission/lifetime/sorting/timing effect. */
extern "C" int   gos_vfx_getDebugMode(void);
extern "C" void  gos_vfx_setDebugMode(int mode);
extern "C" float gos_vfx_getBrightness(void);
extern "C" float gos_vfx_getAdditiveBrightness(void);
extern "C" float gos_vfx_getAlphaScale(void);
extern "C" void  gos_vfx_setBrightness(float v);
extern "C" void  gos_vfx_setAdditiveBrightness(float v);
extern "C" void  gos_vfx_setAlphaScale(float v);
