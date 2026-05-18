#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <unordered_map>
#include <GL/glew.h>
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

// Substrate-coalesce per-draw entry (plan v3.8 Step 2.5).
// Layout mirror of the GLSL std430 struct in the coalesce static_prop variant.
// One PerDrawEntry per registered type, ordered by sortedTypeOrder; consumed
// by the coalesce fragment shader via slot 4 (PerDrawData block, §5.3).
// CHANGING THIS STRUCT REQUIRES CHANGING THE SHADER IN LOCKSTEP
// (cpp_glsl_ubo_struct_lockstep.md — extending one side without the other
// corrupts per-element stride for arr[i>0]).
struct PerDrawEntry {
    int32_t packetID;          //  0 — index into s_packets[]
    int32_t materialFlags;     //  4 — 0 or STATIC_PROP_FLAG_ALPHA_TEST
    int32_t maxLocalVertexID;  //  8 — type.vertexCount - 1
    int32_t texArrayLayer;     // 12 — group-relative layer in s_texArrayOff/On
    float   uvScaleX;          // 16 — 1.0f for Stage A
    float   uvScaleY;          // 20 — 1.0f for Stage A
    int32_t _pad0;             // 24 — std430 alignment + size = 32
    int32_t _pad1;             // 28
};
static_assert(sizeof(PerDrawEntry) == 32, "PerDrawEntry std430 size");
static_assert(offsetof(PerDrawEntry, packetID)         ==  0, "packetID offset");
static_assert(offsetof(PerDrawEntry, materialFlags)    ==  4, "materialFlags offset");
static_assert(offsetof(PerDrawEntry, maxLocalVertexID) ==  8, "maxLocalVertexID offset");
static_assert(offsetof(PerDrawEntry, texArrayLayer)    == 12, "texArrayLayer offset");
static_assert(offsetof(PerDrawEntry, uvScaleX)         == 16, "uvScaleX offset");
static_assert(offsetof(PerDrawEntry, uvScaleY)         == 20, "uvScaleY offset");
static_assert(offsetof(PerDrawEntry, _pad0)            == 24, "_pad0 offset");
static_assert(offsetof(PerDrawEntry, _pad1)            == 28, "_pad1 offset");

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

// Stage 2.D.1.1: exported so gos_object_parity.cpp can de-duplicate its
// local kRingFrames constant (Option A — include rather than static_assert).
// Both the parity SSBO ring and the batcher's s_fence ring share this depth;
// changing one without the other corrupts the async readback handshake.
// The .cpp defines its private `RING_FRAMES` separately to keep the hot-path
// anonymous-namespace TU clean, but MUST match this value.
constexpr uint32_t STATIC_PROP_RING_FRAMES = 3u;

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
//
// Substrate-coalesce fields (instanceCap, coalesceByteOffsetWithinGroup,
// lastSeenGosHandle, alphaClass) are populated by finalizeGeometry() and
// are UNDEFINED before then. Step group 1.1 of plan v3.8.
struct GpuStaticPropType {
    uint32_t firstPacket;
    uint32_t packetCount;
    uint32_t vertexCount;    // number of vertices in the owning TG_TypeShape
    const TG_TypeShape* source;
    uint32_t instanceCap;                    // §5.1 per-type capacity (2× avg)
    uint32_t coalesceByteOffsetWithinGroup;  // §5.1b group-relative byte offset
    uint32_t lastSeenGosHandle;              // §5.4 eviction-detect snapshot
    uint8_t  alphaClass;                     // §CRITICAL-C 0=alpha-OFF, 1=alpha-ON
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
    // multiShape is the parent type-multi-shape, used to look up texture
    // names for register-time alpha-test material classification (Path 4).
    void registerType(TG_TypeShape* typeShape, TG_TypeMultiShape* multiShape);

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

    // Slice 2 (object-offload) — Stage 2.C+ instrumentation: optional
    // callerName (e.g., appearType->name) is included in the
    // [OBJBATCHER v1] event=late_register log line when a leaf type fails
    // registration. Pass nullptr if no caller-side name is available
    // (legacy path); the log will fall back to the bare TG_TypeShape
    // pointer. Helps identify which actor classes own the unregistered
    // type so the allowlist / registration walk can be repaired.
    [[nodiscard]] bool submitMultiShape(TG_MultiShape* multi,
                                        GpuStaticPropPopulation pop,
                                        const char* callerName = nullptr);

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

    // Slice 2 (Stage 2.D.3) — late-registration event counters.
    //
    // Consumed by gos_object_parity::ParityFrameTick() to surface the two
    // late-reg event aggregates in the [OBJECT_PARITY v1] event=summary
    // line. Both are monotonic since process start (the parity window in
    // 2.D.3's reduced contract is "since program start" — see
    // gos_object_parity.h "late-reg event counters" block for rationale).
    //
    //   getAllowedLateRegEventCount():   count of allowed=1 events
    //                                    (allowlisted nodeId, e.g. skybox)
    //   getDisallowedLateRegEventCount(): count of allowed=0 events
    //                                    (unallowlisted; non-zero is a STOP)
    //
    // Both increment per occurrence inside submitMultiShape's late-reg
    // branch (NOT once-per-type — the dedup-print logic stays the same,
    // but every event ticks the counter so the parity summary surfaces the
    // true frequency).
    static uint64_t getAllowedLateRegEventCount();
    static uint64_t getDisallowedLateRegEventCount();

    // Stage 3.C: snapshot of all leaf instances built by the most recent
    // successful submitMultiShape() call. Valid only until the next
    // submitMultiShape() call. Used by GpuStaticPropRegistry::registerRecipe()
    // to snapshot the registration batch immediately after submission.
    const std::vector<GpuStaticPropInstance>& getLastBuiltBatch() const;

    // Stage 3.C: inject a pre-built instance into the per-type bucket
    // without running the compute path. Called by GpuStaticPropRegistry::flush()
    // before batcher flush(). Updates firstColorOffset for this frame's bucket
    // position; pushes zero-fill colors (debug only; normal render ignores
    // the Colors SSBO binding 1).
    void submitCachedInstance(const GpuStaticPropInstance& inst);

    // Track B: pure recipe-construction path, side-effect-free.
    // Builds a GpuStaticPropInstance from static per-shape inputs.
    // Does NOT touch per-frame state (no bucket/SSBO writes).
    // Returns false if shape's TG_TypeShape is not registered yet.
    // firstColorOffset and lightDataIndex are placeholders (0 / 0xFFFFFFFF);
    // submitCachedInstance patches them per-frame at draw time.
    [[nodiscard]] bool buildRecipeFromShape(
            TG_Shape* shape,
            const Stuff::Matrix4D& shapeToWorld,
            uint32_t highlightARGB,
            uint32_t fogARGB,
            uint32_t flags,
            GpuStaticPropInstance* outRecipe) const;

private:
    GpuStaticPropBatcher() = default;

    // Declared here so the whole batcher state is visible for review;
    // implementation details live in .cpp.
    struct Impl;
    // State is file-static in .cpp to keep this header light; singleton
    // method bodies there forward to those statics.
    int debugAddrMode_ = 0;
};

// ---------------------------------------------------------------------------
// C1b free-function accessors (Track C slice C1b).
// These expose the immutable per-type geometry table so gpu_cull_compute.cpp
// can build the DrawElementsIndirectCommand buffer at mission load without
// pulling the batcher's internal anonymous namespace into another TU.
// All functions are valid only after GpuStaticPropBatcher::finalizeGeometry().
// ---------------------------------------------------------------------------

// Returns the number of registered types (= bucket count for indirect draw).
// Returns 0 if geometry is not yet finalized.
uint32_t batcher_getTypeCount();

// Per-type geometry info for indirect draw command construction.
// typeID:          index in [0, batcher_getTypeCount())
// outIndexCount:   total index count across all packets for this type
// outFirstIndex:   firstIndex of the first packet (IBO offset in indices)
// outBaseVertex:   baseVertex of the first packet (VBO offset in vertices)
// outInstanceCap:  per-frame instance capacity (safe upper bound for visibleIds[])
// Returns false if typeID is out of range or geometry not finalized.
bool batcher_getTypeDrawInfo(uint32_t  typeID,
                              uint32_t* outIndexCount,
                              uint32_t* outFirstIndex,
                              int32_t*  outBaseVertex,
                              uint32_t* outInstanceCap);

// ---------------------------------------------------------------------------
// §5.6b accessors — substrate-coalesce path (plan v3.8 Step 1.2).
// All return safe sentinels (0 / nullptr / false) before
// GpuStaticPropBatcher::finalizeGeometry() runs.
// ---------------------------------------------------------------------------

// Sorted type-order array (alpha-OFF group first, alpha-ON second).
// Length = batcher_getTypeCount(); valid until onMapUnload().
const uint32_t* batcher_getSortedTypeOrder();

// Counts per alpha group. Sum = batcher_getTypeCount().
uint32_t batcher_getAlphaOffCount();
uint32_t batcher_getAlphaOnCount();

// 2026-05-11 per-packet rework. Sorted-packet order: global packet indices
// in [alpha-OFF | alpha-ON] order. Length = batcher_getSortedPacketCount().
const uint32_t* batcher_getSortedPacketOrder();
uint32_t batcher_getSortedPacketCount();
uint32_t batcher_getAlphaOffCmdCount();   // packet count in alpha-OFF group
uint32_t batcher_getAlphaOnCmdCount();    // packet count in alpha-ON group
GLuint   batcher_getCmdToBucketSsbo();    // uint typeID per cmd (binding 7)
// Per-packet draw info (for the per-packet indirect cmd build).
bool batcher_getPacketDrawInfo(uint32_t globalPacketIdx,
                                uint32_t* outIndexCount,
                                uint32_t* outFirstIndex,
                                int32_t*  outBaseVertex,
                                uint32_t* outOwningTypeID);

// Per-type instance capacity (§5.1 formula). 0 if typeID out of range.
uint32_t batcher_getInstanceCap(uint32_t typeID);

// GL handles for the coalesce-path SSBOs and texture arrays. 0 before
// finalize, or if coalesce is disabled/disarmed.
GLuint batcher_getCoalesceInstanceSsbo();   // ring-buffered, persistent-mapped
GLuint batcher_getPerDrawSsbo();            // one PerDrawEntry per type, sorted
GLuint batcher_getTexArrayOff();            // alpha-OFF group GL_TEXTURE_2D_ARRAY
GLuint batcher_getTexArrayOn();             // alpha-ON  group GL_TEXTURE_2D_ARRAY
GLuint batcher_getPermutationSsbo();        // sortedSlot[typeID] mapping (slot 15)

// Total instance bytes for ONE ring frame (off-group + on-group). size_t so
// sub-4GB ring totals carry without truncation.
size_t batcher_getCoalescePerFrameInstanceBytes();

// State-machine flags (§7).
bool batcher_isCoalesceLayoutReady();
bool batcher_isCoalesceArmed();

// Returns the highest admitted instance count ever seen for typeID since
// mission load. Slice-1 NOT consumed (foundation for slice 2 GPU-emit).
uint32_t batcher_getPerTypePeakCount(uint32_t typeID);

// true when MC2_STATIC_PROP_GLOBAL_POOL_LEGACY=1 — keeps legacy per-type-cap layout.
bool batcher_isGlobalPoolLegacy();
// Per-ring-frame instance capacity for the global pool (slice 1).
uint32_t batcher_getGlobalInstanceCap();
// Current coalesce ring frame slot (diagnostic only; NOT the legacy s_frameSlot).
uint32_t batcher_getCoalesceFrameSlot();

// Bind/unbind the base-instance SSBO at slot BASE_INSTANCE_SSBO_BINDING (16) for the
// patch shader. Both are no-ops under legacy mode.
void batcher_bindBaseInstanceByCmdSsboForPatch();
void batcher_unbindBaseInstanceByCmdSsboForPatch();

// Called from txmmgr.cpp between GpuStaticPropRegistry::flush() and compute_dispatch().
// Non-legacy mode: advances s_coalesceFrameSlot, waits prior-frame fence, computes
// per-cmd baseInstance prefix-sum, writes to s_baseInstanceByCmdSsbo.
// Legacy mode: returns immediately (s_coalesceFrameSlot mirrors s_frameSlot at :2644).
void batcher_prepareBaseInstanceTable();

extern int g_lightProbeSetupPath;  // [GPUPROPS v1]
