#pragma once

#include <cstdint>
#include <vector>
#include <unordered_map>
#include "Stuff/Stuff.hpp"
#include "tgl.h"
#include "msl.h"

// Per-instance shader-visible struct.
// Layout mirror of the GLSL std430 struct in shaders/static_prop.vert.
// CHANGING THIS STRUCT REQUIRES CHANGING THE SHADER IN LOCKSTEP.
struct alignas(16) GpuStaticPropInstance {
    float    modelMatrix[16];   // shape-to-world, row-major (uploaded GL_FALSE)
    uint32_t typeID;
    uint32_t firstColorOffset;  // into per-frame color SSBO
    uint32_t flags;             // bit 0: lightsOut, bit 1: isWindow, bit 2: isSpotlight
    uint32_t lightDataIndex;    // Slice 2: index into the per-frame light-data
                                // UBO populated by mcTextureManager->addLightDataStructure().
                                // Stage 2.A repurposes the prior _pad0 slot at offset 76;
                                // Stage 2.C wires the writer in submitMultiShape.
    float    aRGBHighlight[4];
    float    fogRGB[4];
};

// Layout: 64 (mat4) + 16 (4 x uint32) + 16 (vec4) + 16 (vec4) = 112 bytes.
static_assert(sizeof(GpuStaticPropInstance) == 112,
              "GpuStaticPropInstance size must match std430 GLSL struct");
static_assert(offsetof(GpuStaticPropInstance, modelMatrix)      ==  0, "modelMatrix offset");
static_assert(offsetof(GpuStaticPropInstance, typeID)           == 64, "typeID offset");
static_assert(offsetof(GpuStaticPropInstance, firstColorOffset) == 68, "firstColorOffset offset");
static_assert(offsetof(GpuStaticPropInstance, flags)            == 72, "flags offset");
static_assert(offsetof(GpuStaticPropInstance, lightDataIndex)   == 76, "lightDataIndex offset");
static_assert(offsetof(GpuStaticPropInstance, aRGBHighlight)    == 80, "aRGBHighlight offset");
static_assert(offsetof(GpuStaticPropInstance, fogRGB)           == 96, "fogRGB offset");

// Packet descriptor (CPU-side only -- not uploaded as an SSBO).
struct GpuStaticPropPacket {
    uint32_t firstIndex;     // into shared IBO
    uint32_t indexCount;
    int32_t  baseVertex;     // into shared VBO
    uint32_t textureSlot;    // index into owning TG_TypeShape::listOfTextures.
                             // Resolved at draw time because MC2 mutates the
                             // handle each frame via SetTextureHandle (see
                             // msl.cpp:1321 TransformMultiShape).
    uint32_t materialFlags;  // bit 0: ALPHA_TEST_BIT
    uint32_t owningTypeID;
};

constexpr uint32_t STATIC_PROP_FLAG_ALPHA_TEST = 1u << 0;

// Slice 2 (object-offload): free-function form of the eligibility check used
// by the addRenderShape gate in tgl.cpp:2522 (the bShadersDrawPathEnabled
// block). Cheap, side-effect-free. Returns true iff `shape` belongs to a
// registered TG_TypeShape AND g_useGpuObjects is enabled. tgl.cpp gets this
// via a forward declaration (see tgl.cpp near the gate site) to avoid
// pulling the rest of batcher.h into the legacy translation unit.
//
// Stage 2.A: declared and defined. Under default `MC2_GPU_OBJECTS=0` (i.e.
// g_useGpuObjects==false) this returns false, so the addRenderShape gate
// becomes false-equivalent and the legacy path is byte-for-byte unchanged.
bool eligibleForGpuObjects(class TG_Shape* shape);

// Population tag — passed by caller so the batcher can split per-population
// counts in the [OBJBATCHER v1] summary. Not stored; consumed inside submit
// only.
//
// Legacy is the slice-1 fallback branch (g_useGpuStaticProps && !g_useGpuObjects)
// in *Appearance::render. Counted separately so Gate F's fallback-rate
// computation uses only slice-1 populations (Building/Tree/Generic).
enum class GpuStaticPropPopulation : uint8_t {
    Building = 0,
    Tree     = 1,
    Generic  = 2,
    Legacy   = 3,
};

// Per-type descriptor: range of packets + vertex count (for color block sizing).
struct GpuStaticPropType {
    uint32_t firstPacket;
    uint32_t packetCount;
    uint32_t vertexCount;    // number of vertices in the owning TG_TypeShape
    const TG_TypeShape* source;
};

class GpuStaticPropBatcher {
public:
    static GpuStaticPropBatcher& instance();

    // Called from gameosmain at map load / unload.
    void onMapLoad();
    void onMapUnload();

    // Register one TG_TypeShape (idempotent). Builds packet table entries
    // and appends geometry to the in-progress VBO/IBO staging.
    // Called during onMapLoad for every static-prop type + its damage variants.
    void registerType(TG_TypeShape* typeShape);

    // Convenience wrapper: iterate a multishape's listOfTypeShapes and call
    // registerType on each SHAPE_NODE leaf. Safe to call with NULL (no-op).
    void registerMultiShape(TG_TypeMultiShape* multiShape);

    // Called at end of registration to upload the immutable VBO/IBO.
    void finalizeGeometry();

    // Per-frame submission. shape->listOfColors must be fresh (set by
    // TransformMultiShape earlier in the frame).
    // Returns true if the instance was accepted. Returns false (Layer B
    // safety net) when the type was never registered; in that case the
    // caller MUST render the shape via the old CPU path this frame.
    //
    // Slice 2 (object-offload) — Stage 2.C: lightDataIndex parameter is
    // the dedup-cache index returned by TG_Shape::GatherGpuObjectLightDataOnly()
    // for the OWNING multishape (per-actor, not per-leaf — Recon Section 9
    // Item 5 confirmed all leaves see identical lightData_). Caller hoists
    // the gather call between submitMultiShape's two for-loops and
    // broadcasts the index into every leaf's submit() in the second loop.
    [[nodiscard]] bool submit(TG_Shape* shape,
                              const Stuff::Matrix4D& shapeToWorld,
                              uint32_t highlightARGB,
                              uint32_t fogARGB,
                              uint32_t flags,
                              uint32_t lightDataIndex);

    // Iterate a multishape's children and submit each SHAPE_NODE leaf using
    // the child's own listOfShapes[i].shapeToWorld. Per-child highlight/fog/
    // flags are pulled from the TG_Shape node itself. Returns false if ANY
    // child fails registration — caller MUST CPU-fallback the whole
    // multishape for this frame to keep the visual self-consistent.
    // Caller-side accounting. recordEligibleActor() is called by
    // *Appearance::render BEFORE submit so caller-side bypasses
    // (e.g., null shape) still count toward eligible_actors.
    // recordCpuFallback() is called when no submit succeeded.
    void recordEligibleActor(GpuStaticPropPopulation pop);
    void recordCpuFallback(GpuStaticPropPopulation pop);

    [[nodiscard]] bool submitMultiShape(TG_MultiShape* multi,
                                        GpuStaticPropPopulation pop);

    // Slice 2 (object-offload): cheap, side-effect-free eligibility query.
    // Returns true iff every leaf SHAPE_NODE under `multi` is currently
    // registered in the batcher AND g_useGpuObjects is enabled. Mirrors slice
    // 1's render-time per-child gates EXCEPT the late-registration case
    // (Recon Section 9 Item 4). Stage 2.A: declared and defined; Stage 2.B
    // wires it into BldgAppearance/TreeAppearance/GenericAppearance::update
    // inside their existing cull gates. MUST NOT mutate any batcher state,
    // emit any logs, or increment any counters — this is a read-only check
    // used by the addRenderShape gate at tgl.cpp:2522 in the legacy path.
    bool isMultiShapeEligibleForGpuObjects(const TG_MultiShape* multi) const;

    // Per-frame dispatch.
    void flush();         // main color pass
    void flushShadow();   // depth-only into dynamic shadow FBO

    // Debug: color-address validation mode. 0=off, 1=gradient, 2=hash.
    void setDebugAddrMode(int mode);
    int  getDebugAddrMode() const { return debugAddrMode_; }

    // Slice 2 (Stage 2.A) — late-registration signal for the caller.
    // Returns true iff the most recent submitMultiShape() call returned false
    // specifically because a SHAPE_NODE child type was not registered at
    // submission time (the late-registration branch, not some other failure
    // such as a fatal GPU error or program-load failure).
    //
    // Stage 2.B wires the caller: *Appearance::render queries this after a
    // false return from submitMultiShape and sets needsFullBakeNextFrame=true
    // on the owning actor so the map-load registration walk picks it up.
    //
    // The flag is cleared (reset to false) at the TOP of every
    // submitMultiShape() call so a stale "true" from a prior call never
    // masquerades as a signal for the current call.
    bool wasLastFailureLateRegistration() const;

private:
    GpuStaticPropBatcher() = default;

    // Declared here so the whole batcher state is visible for review;
    // implementation details live in .cpp.
    struct Impl;
    // State is file-static in .cpp to keep this header light; singleton
    // method bodies there forward to those statics.
    int debugAddrMode_ = 0;
};
