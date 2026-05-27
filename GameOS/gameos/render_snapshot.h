// GameOS/gameos/render_snapshot.h
//
// Per-frame RenderSnapshot: immutable frame-lifetime view of render
// state populated by ExtractRenderSnapshot() from engine layers.
//
// Extraction v1 (2026-05-24): static-prop snapshot populated alongside
// v0 mech and light records. Arena 1 MiB per slot (2 MiB ping-pong total).
// v1.1 (2026-05-25): texArrayLayer/materialIdx wired; material cache counters added.
// Ping-pong: two module-static RenderFrameArena instances alternate each frame;
// no per-frame heap allocation. Snapshot is valid only for the current frame.

#pragma once

#include "../RenderCore/Handle.h"
#include <cstdint>
#include <cstdlib>
#include <memory>

// Frame arena management: persistent allocator with per-frame reset.
// Two module-static instances live in render_snapshot.cpp; ExtractRenderSnapshot()
// flips between them each call. After the flip the idle arena is reset and
// used for the new frame. Span pointers in RenderSnapshot are valid only for
// the frame in which they were produced — do not hold them past the call site.
class RenderFrameArena {
public:
    RenderFrameArena() = default;
    ~RenderFrameArena() = default;

    // Allocate n elements of type T from the current arena.
    // Returns nullptr on overflow (snapshot->arenaOverflow set by caller).
    template <typename T>
    T* alloc(size_t count) {
        size_t bytes = count * sizeof(T);
        if (m_used + bytes > kArenaBytes) {
            return nullptr;  // overflow
        }
        T* ptr = reinterpret_cast<T*>(m_buffer.get() + m_used);
        m_used += bytes;
        return ptr;
    }

    // Reset for next frame; caller alternates between two instances.
    void reset() {
        m_used = 0;
    }

    size_t bytesUsed() const { return m_used; }

private:
    static constexpr size_t kArenaBytes = 1024u * 1024u; // 1 MiB per slot; 2 MiB total ping-pong
    std::unique_ptr<uint8_t[]> m_buffer{new uint8_t[kArenaBytes]};
    size_t m_used = 0;
};

// --- v0: mech extraction record ---
struct ExtractedMech {
    // Placeholder for v0 fields; implementation deferred.
    // Expected fields: handle, typeId, transform, etc.
};

// --- v0: light record ---
struct LightRecord {
    // Placeholder for v0 fields; implementation deferred.
    // Expected fields: position, color, falloff, etc.
};

// --- v1: static-prop snapshot record ---
// Extraction v1: static-prop snapshot record.
// Fields marked "sentinel v1" are typed for v1.1/v2 but not authoritative here.
// Sentinel values mean "not yet extracted" — callers must not treat them as valid data.
struct ExtractedStaticProp {
    // --- identity (authoritative v1) ---
    RenderCore::RenderObjectHandle rwHandle;      // validated alive+generation by fillStaticPropSlots
    int32_t            recipeIndex;   // == rwHandle.index() by construction
    uint32_t           typeId;        // from GpuStaticPropInstance.typeID

    // --- transform (authoritative v1) ---
    // Row-major matrix from GpuStaticPropInstance.modelMatrix.
    // Translation is at [3],[7],[11] with Stuff-space axis swap:
    //   worldCenterX = -worldMatrix[3]   (MC2 east)
    //   worldCenterY =  worldMatrix[11]  (MC2 north)
    //   worldCenterZ =  worldMatrix[7]   (MC2 elevation)
    float worldMatrix[16];
    float worldCenterX;
    float worldCenterY;
    float worldCenterZ;
    // RecipeRange::extentRadius from previous frame's markVisible(); 0.0f if never visible.
    float boundingRadius;

    // --- material / lighting (sentinel v1) ---
    uint32_t materialIdx;    // sentinel 0xFFFFFFFFu — MaterialGpu wiring is v1.1
    int32_t  texArrayLayer;  // sentinel -1
    // RecipeRange::lightDataIndex; 0xFFFFFFFFu if getter failed.
    uint32_t lightDataIndex;

    // --- cull / AABB ---
    // v2: true iff substrate_appendStaticPropRecord was called for this recipe
    // in the most recent PREVIOUS render frame's flush() pass.
    // ExtractRenderSnapshot() runs BEFORE flush() — hasCullRecord reflects prior frame.
    // Sphere AABB is caller-derived (no new struct fields needed):
    //   hasBounds = (boundingRadius > 0.0f)
    //   aabbMin   = {worldCenterX, worldCenterY, worldCenterZ} - boundingRadius
    //   aabbMax   = {worldCenterX, worldCenterY, worldCenterZ} + boundingRadius
    bool hasCullRecord;

    // --- metadata (v2) ---
    uint8_t  alphaClass;    // 0=alpha-off, 1=alpha-on; from s_types[typeID].alphaClass
    uint32_t instanceFlags; // GpuStaticPropInstance.flags: bit0=lightsOut, bit1=isWindow, bit2=isSpotlight
    uint32_t packetCount;   // s_types[typeID].packetCount; 0 if not found
    uint32_t firstPacket;   // s_types[typeID].firstPacket; use with batcher_getPacketTexArrayLayer
    char     shapeName[64]; // AppearanceType name; '\0' for bulk-registered props
};

static_assert(sizeof(ExtractedStaticProp) <= 256,
    "ExtractedStaticProp exceeded 256-byte budget; adjust arena sizing");

// --- v2.2: per-draw-slot packet snapshot record ---
// One entry per draw slot in s_sortedPacketOrder (geometry-stable after finalizeGeometry).
// sortedSlot: index i in the batcher's sorted draw command array.
// globalPacketIdx: s_sortedPacketOrder[sortedSlot] — index into s_packets[].
// pipelineId: RenderCore::PipelineId cast to uint32_t (StaticPropOpaque or StaticPropAlphaTest).
// materialIdx: s_packetMaterialIdx[sortedSlot]; 0xFFFFFFFFu if MC2_MATERIAL_GPU sidecar invalid.
// instanceCount: previous-frame per-type instance count from flush(); 0 on frame 1.
// texArrayLayer: packet's albedo tex-array layer (v2.2); -1 sentinel if not set.
struct ExtractedStaticPropPacket {
    uint32_t sortedSlot;       // draw-slot index in s_sortedPacketOrder
    uint32_t globalPacketIdx;  // s_sortedPacketOrder[sortedSlot]
    uint32_t typeId;           // s_packets[globalPacketIdx].owningTypeID
    uint32_t pipelineId;       // RenderCore::PipelineId cast to uint32_t (StaticPropOpaque or StaticPropAlphaTest)
    uint32_t materialIdx;      // s_packetMaterialIdx[slot]; 0xFFFFFFFFu sentinel if sidecar invalid
    uint32_t instanceCount;    // previous-frame; 0 on frame 1 or no visible instances
    int32_t  texArrayLayer;    // packet's albedo tex-array layer (v2.2); -1 sentinel if not set
};

static_assert(sizeof(ExtractedStaticPropPacket) == 28,
    "ExtractedStaticPropPacket layout changed — update v2.2 extraction consumers");

// Simple span wrapper for array views.
template <typename T>
struct Span {
    T* data = nullptr;
    size_t count = 0;

    Span() = default;
    Span(T* d, size_t c) : data(d), count(c) {}

    size_t size() const { return count; }
    bool empty() const { return count == 0; }
    T* begin() { return data; }
    T* end() { return data + count; }
    const T* begin() const { return data; }
    const T* end() const { return data + count; }
};

// Per-frame render snapshot: immutable view of extracted engine state.
struct RenderSnapshot {
    // Frame identity
    uint64_t frameIndex = 0;

    // --- v0: mech + light records ---
    Span<ExtractedMech> mechs;
    Span<LightRecord> lights;

    // --- v1: static-prop snapshot ---
    Span<ExtractedStaticProp> staticProps;           // populated by ExtractRenderSnapshot v1
    uint32_t staticPropValidationFail = 0;           // hard gate: must be 0
    uint32_t staticPropSentinelMat    = 0;           // v1: expected == staticProps.size(); v1.1: < size when MC2_MATERIAL_GPU=1
    uint32_t staticPropSentinelCull   = 0;           // expected == staticProps.size() in v1

    // v1.1 material/texture wiring counters
    uint32_t staticPropTexWired       = 0;  // texArrayLayer != -1
    uint32_t staticPropTexSentinel    = 0;  // texArrayLayer == -1 (no valid primary)
    uint32_t staticPropMatWired       = 0;  // materialIdx != 0xFFFFFFFF (MC2_MATERIAL_GPU=1 only)
    uint32_t staticPropMatSentinel    = 0;  // materialIdx == 0xFFFFFFFF
    uint32_t staticPropPrimaryAlphaOn = 0;  // primary came from alpha-on fallback
    uint32_t staticPropMultiPacket    = 0;  // type has > 1 packet

    // v2 metadata counters
    uint32_t staticPropCullSubmitted      = 0;  // hasCullRecord == true
    uint32_t staticPropCullMissing        = 0;  // hasCullRecord == false
    uint32_t staticPropHasBounds          = 0;  // boundingRadius > 0
    uint32_t staticPropAlphaOn            = 0;  // alphaClass == 1
    uint32_t staticPropHasShapeName       = 0;  // shapeName[0] != '\0'
    uint32_t staticPropPacketRangesOk     = 0;  // firstPacket + packetCount within sidecar
    uint32_t staticPropPacketRangesFail   = 0;  // firstPacket + packetCount out of range

    // --- v2.1: per-draw-slot packet snapshot ---
    Span<ExtractedStaticPropPacket> staticPropPackets; // one per draw slot
    uint32_t staticPropPacketCount   = 0;  // packets successfully captured
    uint32_t staticPropPacketInvalid = 0;  // batcher_getDrawSlotEntry failures

    // v2.3 hard gate: extends v2.2. Adds spSnapCullSlotMismatch==0.
    //   staticPropValidationFail==0, staticPropPacketRangesFail==0,
    //   staticPropPacketInvalid==0, !arenaOverflow,
    //   spCountMismatch==0, spSortedSlotMismatch==0, spGlobalPacketMismatch==0,
    //   spPipelineMismatch==0, spMaterialIdxMismatch==0, spTexLayerMismatch==0,
    //   spSnapCullSlotMismatch==0.
    //   (spInstanceCountMismatch, spSnapCullSkipped, spSnapCullActive are informational.)
    uint32_t ok = 0u;

    // --- v2.2: dispatch-fact compare results (filled by batcher_compareSnapshotPackets) ---
    // Called from ExtractRenderSnapshot() after v2.1 packet capture, before ok computation.
    // spInstanceCountMismatch is informational only — different-frame authority by design.
    // All other counters are included in the ok gate.
    uint32_t spCompareSnapshotCount  = 0u;  // snap->staticPropPackets.count at compare time
    uint32_t spCompareLiveCount      = 0u;  // batcher_getDrawSlotCount() at compare time
    uint32_t spCountMismatch         = 0u;  // 1 if snapshot_count != live_count
    uint32_t spSortedSlotMismatch    = 0u;  // rows where snap[i].sortedSlot != i
    uint32_t spGlobalPacketMismatch  = 0u;  // rows where globalPacketIdx != s_sortedPacketOrder[i]
    uint32_t spPipelineMismatch      = 0u;  // rows where pipelineId diverges from live alpha split
    uint32_t spMaterialIdxMismatch   = 0u;  // rows where materialIdx != s_packetMaterialIdx[i]
    uint32_t spInstanceCountMismatch = 0u;  // rows where prev-frame count != current-frame count
    uint32_t spTexLayerMismatch      = 0u;  // rows where MaterialGpu.albedoTex != texArrayLayer

    // --- v2.3: snap-cull counters (populated by flush() of the PREVIOUS frame) ---
    // spSnapCullSkipped:      draw slots skipped because prev-frame instanceCount==0.
    // spSnapCullActive:       draw slots drawn with snap-cull enabled (prev-frame count > 0).
    // spSnapCullSlotMismatch: sortedSlot identity failures (row.sortedSlot != i); included in ok gate.
    // spSnapCullSkipped and spSnapCullActive are informational — excluded from ok gate.
    uint32_t spSnapCullSkipped      = 0u;
    uint32_t spSnapCullActive       = 0u;
    uint32_t spSnapCullSlotMismatch = 0u;

    // Non-owning pointer to the current frame's ping-pong arena.
    // Owned by module statics in render_snapshot.cpp; valid for this frame only.
    // Do NOT hold this pointer past the frame — the arena is reset on the next call.
    RenderFrameArena* arena = nullptr;
    bool arenaOverflow = false;
};

// Extraction entry point — implemented in render_snapshot.cpp.
// Called once per frame between DoGameLogic() and draw_screen().
// Returns a snapshot backed by a frame-lifetime arena (snap.arena).
RenderSnapshot ExtractRenderSnapshot();

// v2.3: returns a non-owning pointer to the most recently produced snapshot.
// Valid until the next ExtractRenderSnapshot() call (same frame lifetime as the arena).
// Returns nullptr before the first extraction — callers must null-check before use.
const RenderSnapshot* getLastRenderSnapshot();
