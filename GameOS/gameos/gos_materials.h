// GameOS/gameos/gos_materials.h
//
// Material profile name registry.
// Each profile maps to one MaterialGpu entry in a dedicated profile SSBO.
//
// In mech.frag, the buffer block must use binding=7 and a distinct name (e.g. MechMaterialTable)
// to avoid collision with static-prop materialTable_ at binding=5.
//
// This system manages GLOBALLY-LOADED material profiles (Normal + ORM textures
// for named surface types like "metal061b"). It is DISTINCT from the per-actor
// mech material table in gos_mech_batcher.cpp (which stores per-actor albedo
// texHandle at binding 2). This table lives at binding 7 (binding 5 is owned by
// the static-prop batcher; see D-material-unify debt to consolidate later).
//
// Texture semantic for this table: RawGlId
//   normalTex             = raw GL texture object (GL_RGB8, linear, tangent-space NM)
//   metallicRoughnessTex  = raw GL texture object (GL_RGB8, linear, R=AO G=Rough B=Metal)
//   albedoTex             = kMaterialTexAbsent (no per-profile albedo; driven per-draw)
//   emissiveTex           = kMaterialTexAbsent (no emissive in initial profiles)
//
// ORM packing convention (matches MaterialFlags::kMetallicRoughness bit comment):
//   R = AO       (filled 255 when no AO source)
//   G = Roughness
//   B = Metallic
//
// Gate: MC2_MECH_SURFACE_MATERIAL env var selects active profile(s).
//   "metal061b" or null/unset -> Metal061B profile registered and available.
//   "0" or "none"             -> no profiles loaded; all queries return index 0.
//
// Kill-switch: MC2_MATERIAL_GPU=0 disables all table upload and bind (mirrors
//              gos_mech_batcher.cpp and gos_static_prop_batcher.cpp convention).
//
// Usage (Slice C -- gos_mech_batcher.cpp):
//   gos_materials::init()  -- once, from map load / startup, before flush
//   uint32_t idx = gos_materials::getProfileIndex("metal061b");
//   gos_materials::bindMaterialTable();  -- binding 7, before draw
//
// IMPORTANT: do not call init() more than once per process (idempotent guard inside).
//
#pragma once
#include <cstdint>

namespace gos_materials {

// Load all registered material textures and populate the MaterialGpu profile
// SSBO. Idempotent: no-op if already called. Requires an active GL context
// (unless MC2_MATERIAL_GPU=0, in which case it is a full no-op).
void init();

// Release all GL textures and SSBO created by init(). Resets to pre-init state
// so that init() may be called again if needed (e.g. context re-creation).
void shutdown();

// Returns the MaterialGpu profile-table index for a named profile
// (case-sensitive). Returns 0 (default/passthrough entry) if name is null or
// the profile was not registered (e.g. gate=0/none).
uint32_t getProfileIndex(const char* name);

// Returns the number of registered profiles (includes index-0 default entry).
uint32_t profileCount();

// Binds the profile SSBO to GL_SHADER_STORAGE_BUFFER binding 7.
// No-op when MC2_MATERIAL_GPU=0, init() has not been called, or the SSBO
// handle is 0 (empty table -- only the default entry exists but no GL buffer
// was created because no textured profiles were registered).
void bindMaterialTable();

} // namespace gos_materials
