#pragma once
#include <cstddef>
#include <cstdint>

namespace RenderCore {

// Enumerated render resource slots. Values are stable indices into the
// fixed registry array — do not reorder or renumber.
// Registry is descriptive only; owners retain GL texture/buffer lifetimes.
enum class RenderResourceId : uint16_t {
    Unknown              = 0,
    MainColor            = 1,
    MainDepth            = 2,
    ShadowStaticMap      = 3,
    TerrainHeightTexture = 4,
    MaterialGpuBuffer    = 5,
    ShadowDynamicMap     = 6,
    WaterReflectionColor = 7,   // WATER-REFLECTION-RESOURCE-1: 1/4-res reflection RT (color)
    WaterReflectionDepth = 8,   // WATER-REFLECTION-RESOURCE-1: 1/4-res reflection RT (depth)
    Backbuffer           = 9,   // FRAME-GRAPH-FBO-LEDGER-1: default framebuffer (GL name 0)
    TerrainRecipeBuffer  = 10,  // TERRAIN-SUBPASS-MODEL-1: Indirect: recipe SSBO (slot1)
    TerrainThinBuffer    = 11,  // TERRAIN-SUBPASS-MODEL-1: Indirect/PatchStream: thin-record SSBO (slot2)
    CementAtlas          = 12,  // TERRAIN-SUBPASS-MODEL-1: Indirect: cement atlas (unit3)
    TransitionMaskArray  = 13,  // TERRAIN-SUBPASS-MODEL-1: Indirect: transition-mask 2D_ARRAY (unit4)
    TerrainHeightSsbo    = 14,  // TERRAIN-SUBPASS-MODEL-1: LOD-chunk: height SSBO (distinct from static TerrainHeightTexture)
    MainNormal           = 15,  // POSTPROCESS-SUBGRAPH-1: GBuffer1 (sceneNormalTex_, COLOR_ATTACHMENT1 of sceneFBO_); read by Shoreline, ScreenShadow, SSAO, BoxDecals
    SceneDepthCopy       = 16,  // POSTPROCESS-SCENEDEPTHCOPY-RESOURCE-1: depth-copy RT (sceneDepthCopyTex_); produced by copySceneDepthForParticles() in the VFX/particle path (cross-boundary); read by projected/box decals + particles soft-depth
    SceneObjectId        = 17,  // POSTPROCESS-SCENEOBJECTID-RESOURCE-1: GBuffer2 object-id RT (sceneObjectIdTex_, GL_R32UI, COLOR_ATTACHMENT2 of sceneFBO_); gated RenderWorld::IsObjectIdBufferEnabled(); written by MechOpaque+StaticPropOpaque (layout location=2 when MRT+objectId enabled); read by Composite (unit2, effectiveMode==1 / objectId debug mode)
    HzbPyramid           = 18,  // POSTPROCESS-SUBGRAPH-2: Hi-Z pyramid mip chain (hzbLevelTex_[0..N], R32F); produced by HzbReduce (draw pass via hzbFBO_); read by HzbProbe (CPU diagnostic). Frame-persistent (survives mission reload).
    SsaoOcclusion        = 19,  // POSTPROCESS-SUBGRAPH-2: half-res AO result (ssaoColorTex_, R8/RGBA8); produced by SSAO pass1 (ssaoFBO_); consumed by SSAO pass2/apply (sceneFBO_). Transient within endScene.
    SceneColorCopy       = 20,  // POSTPROCESS-SUBGRAPH-2: feedback-safe scene color copy (sceneColorCopyTex_, RGBA16F); producer = VFX pass (copySceneColorForVfx, REGISTRY-SCENECOLORCOPY-PRODUCER-1); consumed by PostprocessComputeBlur (substrate only, default-OFF).
    ClusterDepthPyramid  = 21,  // REGISTRY-COMPUTE-IDS-1: cluster_depth_pyramid tile min/max texture (s_tileTex, RG32F, tile-grid res); produced by cluster_depth_pyramid::Run (compute), default-OFF gated substrate. Consumed by Lightgrid build (tileMinMax).
    LightgridGrid        = 22,  // REGISTRY-COMPUTE-IDS-1: lightgrid sphere SSBO (s_sphereSsbo, kSphereStride*kMaxLights bytes); produced by lightgrid_build (compute), default-OFF gated substrate.
    LightgridIndex       = 23,  // REGISTRY-COMPUTE-IDS-1: lightgrid per-tile index pool SSBO (s_indexPool, nTiles*kMaxLights uints); produced by lightgrid_build (compute), default-OFF gated substrate.
    PostprocessComputeBlur = 24,// REGISTRY-COMPUTE-IDS-1: compute-blur ping-pong output substrate (postprocess_blur s_pingA, RGBA16F half-res); produced by postprocess_blur::Run (compute), default-OFF gated substrate. No consumer (substrate only).
    ViewUniformsUbo      = 25,  // GPU-BUFFER-OWNER-SKELETON-1: view-uniforms UBO (s_viewUniformsUbo, view_uniforms_gl.cpp); LIVE, raw-owned, UNregistered today. First owner target.
    TerrainTypeSsbo      = 26,  // GPU-BUFFER-OWNER-SKELETON-1: LOD-chunk per-quad type SSBO (s_typeSsbo, gos_terrain_lod_chunk.cpp); LIVE, raw-owned, UNregistered today.
    TerrainCementSsbo    = 27,  // GPU-BUFFER-OWNER-SKELETON-1: LOD-chunk cement-word SSBO (s_cementSsbo, gos_terrain_lod_chunk.cpp); LIVE, raw-owned, UNregistered today.
    StaticPropMaterialGpuBuffer = 28,  // STATICPROP-MATERIAL-SSBO-OWNER-1: static-prop MaterialGpu table SSBO (s_materialGpuSsbo, binding 5, gos_static_prop_batcher.cpp); LIVE default-ON, Mission lifetime. Distinct from mech-material MaterialGpuBuffer(id5, binding 2).
    MechProfileMaterialGpuBuffer = 29,  // MECH-PROFILE-SSBO-OWNER-1: mech material-profile table SSBO (s_ssbo, binding 7, gos_materials.cpp); Persistent lifetime (init→shutdown). Only created when MC2_MECH_SURFACE_MATERIAL loads a profile; default run leaves glName==0. Distinct from binding-5 static-prop and binding-2 mech-material tables.
    Count
};

// REGISTRY-LIFETIME-CLASS-1: how long a registered resource stays valid.
// Observe-only metadata — the bridge to a future scheduler/backend that needs to
// know aliasing/recreation lifetime. Vocabulary is deliberately small.
//   Unset      — sentinel; NOT a real lifetime. A valid resource left at Unset is
//                a registration bug (the validator fails on it). Note: this is
//                distinct from the existing valid=false / gated-absent behavior,
//                which means "registered but currently unavailable" and is NOT a
//                lifetime.
//   FrameLocal — transient, produced+consumed within a frame, aliasable.
//   Mission    — rebuilt on each mission load (terrain SSBOs, atlases).
//   Persistent — process/long-lived; survives mission reload (screen-sized FBO
//                targets are recreated only on resize, still Persistent).
//   External   — temporal / cross-frame N-1 / externally-owned (water reflection).
enum class RenderResourceLifetime : uint8_t {
    Unset      = 0,
    FrameLocal = 1,
    Mission    = 2,
    Persistent = 3,
    External   = 4,
};

enum class RenderResourceKind : uint8_t {
    Unknown        = 0,
    Texture2D      = 1,
    Texture2DArray = 2,
    TextureCube    = 3,
    Buffer         = 4,
};

enum class RenderResourceFormat : uint8_t {
    Unknown   = 0,
    R32F      = 1,
    RGBA16F   = 2,
    RGBA8     = 3,
    Depth16   = 4,
    Depth24   = 5,
    Depth32F  = 6,
    BufferRaw = 7,
};

// Per-resource descriptor. GL-free conceptually; glName is debug-only.
// valid=false means the slot is registered but the resource is currently
// unavailable (mission not loaded, subsystem not yet initialized, etc.).
// debugName must point to a string literal; never heap-allocated.
struct RenderResourceDesc {
    RenderResourceId     id             = RenderResourceId::Unknown;
    RenderResourceKind   kind           = RenderResourceKind::Unknown;
    RenderResourceFormat format         = RenderResourceFormat::Unknown;
    // REGISTRY-LIFETIME-CLASS-1: lifetime class. No safe default — a valid
    // resource MUST set this explicitly; the validator fails on Unset.
    RenderResourceLifetime lifetime     = RenderResourceLifetime::Unset;
    const char*          debugName      = nullptr;
    uint32_t             width          = 0;
    uint32_t             height         = 0;
    uint32_t             layers         = 1;
    uint32_t             samples        = 1;
    uint32_t             glName         = 0;    // debug only; 0 if unavailable
    uint64_t             sizeBytes      = 0;    // 0 if unknown
    uint32_t             producerPassId = 0;
    uint32_t             consumerMask   = 0;
    bool                 valid          = false;
};

// Register or update a resource descriptor indexed by desc.id.
// Overwrites any existing record for that id. Unknown id is a no-op.
// Calling with valid=false marks the slot unavailable without removing it.
void registerOrUpdateRenderResource(const RenderResourceDesc& desc);

// Returns nullptr if id is Unknown, out of range, or valid=false.
const RenderResourceDesc* getRenderResource(RenderResourceId id);

// Count of currently-valid registered resources.
size_t getRenderResourceCount();

// Enumerate valid resources by dense index [0, getRenderResourceCount()).
const RenderResourceDesc* getRenderResourceByIndex(size_t index);

const char* toString(RenderResourceId id);
const char* toString(RenderResourceKind kind);
const char* toString(RenderResourceFormat fmt);
const char* toString(RenderResourceLifetime lifetime);

// REGISTRY-LIFETIME-CLASS-1: validate that every currently-registered (valid)
// resource has a lifetime set (!= Unset). Returns true if all valid resources
// carry a lifetime; on failure, *offending (if non-null) receives the id of the
// first valid resource missing a lifetime. A clean registry returns true.
bool validateRenderResourceLifetimes(RenderResourceId* offending = nullptr);

} // namespace RenderCore
