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
