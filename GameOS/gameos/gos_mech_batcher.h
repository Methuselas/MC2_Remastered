// GameOS/gameos/gos_mech_batcher.h
#pragma once

#include <cstdint>
#include <cstddef>    // offsetof
#include <vector>
#include <unordered_map>
#include "Stuff/Stuff.hpp"
#include "tgl.h"
#include "msl.h"
#include "mech3d.h"

// CPU-side vertex layout — packed 48 bytes for VBO storage.
// CHANGING THIS STRUCT REQUIRES CHANGING mech.vert ATTRIBUTE LAYOUT IN LOCKSTEP.
struct GpuMechVertex {
    float    position[3];    // 12B — a_position  (loc 0, GL_FLOAT)
    float    normal[3];      // 12B — a_normal     (loc 1, GL_FLOAT)
    float    uv[2];          //  8B — a_uv         (loc 2, GL_FLOAT)
    uint8_t  boneIndices[4]; //  4B — a_boneIndices (loc 3, GL_UNSIGNED_BYTE, IPointer -> uvec4)
    uint8_t  boneWeights[4]; //  4B — a_boneWeights (loc 4, GL_UNSIGNED_BYTE, GL_TRUE -> vec4 [0,1])
    int16_t  tangentOct[2];  //  4B — a_tangentOct  (loc 5, GL_SHORT, GL_TRUE -> vec2 [-1,1])
    uint32_t aRGBLight;      //  4B — a_aRGBLight   (loc 6, GL_UNSIGNED_INT, IPointer -> uint)
};                           // 48B total
static_assert(sizeof(GpuMechVertex) == 48, "GpuMechVertex must be 48 bytes");
static_assert(offsetof(GpuMechVertex, position)    ==  0, "position offset");
static_assert(offsetof(GpuMechVertex, normal)      == 12, "normal offset");
static_assert(offsetof(GpuMechVertex, uv)          == 24, "uv offset");
static_assert(offsetof(GpuMechVertex, boneIndices) == 32, "boneIndices offset");
static_assert(offsetof(GpuMechVertex, boneWeights) == 36, "boneWeights offset");
static_assert(offsetof(GpuMechVertex, tangentOct)  == 40, "tangentOct offset");
static_assert(offsetof(GpuMechVertex, aRGBLight)   == 44, "aRGBLight offset");

// Per-instance GPU record — std430, 48 bytes.
// CHANGING THIS STRUCT REQUIRES CHANGING mech.vert IN LOCKSTEP.
struct alignas(16) GpuMechInstance {
    uint32_t typeLodRecordIndex;  // resolved Mech3DAppearanceType × LOD record index
    uint32_t baseBoneOffset;      // index into per-frame bone SSBO for this actor's nodes
    uint32_t lightDataIndex;      // into LightsData UBO (Slice B1; 0 in Slice A)
    uint32_t renderFlags;         // bit 0: ALPHA_TEST, bit 1: lightsOut, bit 2: isHighlighted
    float    aRGBHighlight[4];    // forwarded to FS as v_highlightColor
    float    fogRGB[4];           // forwarded to FS as v_fogRGB
};
// Layout: 16 (4 × uint32) + 16 (vec4) + 16 (vec4) = 48 bytes.
static_assert(sizeof(GpuMechInstance) == 48,
              "GpuMechInstance size must match std430 GLSL struct");
static_assert(offsetof(GpuMechInstance, typeLodRecordIndex) ==  0);
static_assert(offsetof(GpuMechInstance, baseBoneOffset)     ==  4);
static_assert(offsetof(GpuMechInstance, lightDataIndex)     ==  8);
static_assert(offsetof(GpuMechInstance, renderFlags)        == 12);
static_assert(offsetof(GpuMechInstance, aRGBHighlight)      == 16);
static_assert(offsetof(GpuMechInstance, fogRGB)             == 32);

// Bone matrix: 4 explicit rows to avoid GLSL column-major confusion.
// Upload rows as row0..row3; GLSL mat4(row0,row1,row2,row3) fills COLUMNS from
// these, making boneT the transpose of the Stuff LinearMatrix4D —
// boneT * vec4(pos,1) == row-vector math. Named boneT in shader; do NOT remove.
struct GpuMechBone {
    float row0[4], row1[4], row2[4], row3[4];
};
static_assert(sizeof(GpuMechBone) == 64, "GpuMechBone must be 64 bytes");

// Per mechType×LOD registration record (CPU-side only).
struct GpuMechTypeLodRecord {
    uint32_t firstBoneIndex;   // vertex-local bone index namespace base (normally 0)
    uint32_t numBones;         // count of SHAPE_NODE children; must be <= 255
    uint32_t firstPacket;      // into s_packets table
    uint32_t packetCount;
    uint32_t vertexCount;      // total triangle-soup vertices across all nodes
    const TG_TypeShape* sourceNode0; // for late-reg logging
};

// Per-node per-texture-group draw record (CPU-side only, not uploaded).
struct GpuMechPacket {
    uint32_t firstIndex;         // index into shared IBO (in indices, not bytes)
    uint32_t indexCount;
    int32_t  baseVertex;         // signed; passed to glDrawElementsInstancedBaseVertex
    uint32_t textureSlot;        // index into owning TG_TypeShape::listOfTextures.
                                 // Slot 0 is per-actor (paint scheme); resolved from
                                 // GpuMechSubmitDesc::slot0TexHandle at submitActor time.
                                 // Slots 1+ are type-stable; resolved from owningTypeShape.
    uint32_t materialFlags;      // bit 0: ALPHA_TEST_BIT
    uint32_t owningTypeLodRecord;
    uint32_t nodeLocalIndex;     // which bone index within the type (index into listOfShapes)
    const TG_TypeShape* owningTypeShape; // type-level shape for slots 1+ texture resolution
};

// All per-actor context needed at submit / flush time.
struct GpuMechSubmitDesc {
    TG_MultiShape*              mechShape;      // live per-instance shape (shapeToWorld only)
    const Mech3DAppearanceType* mechType;       // stable type pointer
    int                         currentLOD;
    uint32_t                    slot0TexHandle; // mcTextureManager SLOT INDEX (NOT a gos handle)
                                               // for texture slot 0 (per-actor paint scheme;
                                               // localTextureHandle from Mech3DAppearance, set
                                               // by mcTextureManager->loadTexture). Resolved to
                                               // a live gos handle at flush time via
                                               // mcTextureManager->get_gosTextureHandle(slot)
                                               // per memory/mc2_texture_handle_is_live.md.
                                               // TG_TypeShape::listOfTextures is a shared type-
                                               // level cache mutated by TransformMultiShape across
                                               // all actors — do NOT read it for slot 0.
    uint32_t                    lightDataIndex; // Slice B1; 0 in Slice A
    uint32_t                    renderFlags;
    uint32_t                    highlightARGB;
    uint32_t                    fogARGB;
};

// Fallback accounting reasons (used in MC2_MECH_BATCHER_STATS output).
enum class GpuMechFallbackReason : uint8_t {
    UnregisteredType  = 0,
    U8BoneOverflow    = 1,
    RingOverflow      = 2,
    TglGpuUnsupported = 3,
    ShaderInitFailure = 4,
};

// Ring depth — must match STATIC_PROP_RING_FRAMES to share fence semantics
// with gos_object_parity.cpp. Cross-checked by static_assert in .cpp.
constexpr uint32_t MECH_RING_FRAMES = 3u;

class GpuMechBatcher {
public:
    static GpuMechBatcher& instance();

    void onMapLoad();
    void onMapUnload();

    // Register one Mech3DAppearanceType × LOD. Idempotent.
    // Reads mechType->mechShape[lod] (TG_TypeMultiShape*, stable type-level pointer).
    void registerTypeLod(const Mech3DAppearanceType* mechType, int lod);

    // Upload immutable VBO/IBO after all registerTypeLod() calls.
    void finalizeGeometry();

    // Caller-side accounting (called BEFORE any registration check).
    void recordEligibleActor();
    void recordCpuFallback(GpuMechFallbackReason reason);

    // Per-frame submission. Returns false on any failure; caller MUST
    // CPU-fallback (mechShape->Render(true)) this frame when false.
    [[nodiscard]] bool submitActor(const GpuMechSubmitDesc& desc);

    // Post-renderLists() draw flush.
    void flush();
    void flushShadow();  // no-op in Slice A/B1/B2; reserved for future shadow-offload slice

    bool wasLastFailureLateRegistration() const;

    static uint64_t getAllowedLateRegEventCount();
    static uint64_t getDisallowedLateRegEventCount();
};
