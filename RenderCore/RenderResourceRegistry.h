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
    SceneColorCopy       = 20,  // POSTPROCESS-SUBGRAPH-2: feedback-safe scene color copy (sceneColorCopyTex_, RGBA16F); producer not yet modeled; consumed by PostprocessComputeBlur (substrate only, default-OFF).
    Count
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

} // namespace RenderCore
