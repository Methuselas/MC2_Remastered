// GameOS/gameos/draw_packet_emitter.h
//
// DrawPacket v0 — static-prop packet emitter.
// Spec: docs/superpowers/specs/2026-05-25-draw-packet-v0-spec.md
//
// Uses StaticPropDrawPacketCandidate (NOT RenderCore::DrawPacket) to avoid
// field-name misuse and prevent accidental dispatch. v0 is type-packet-level;
// DrawPacket::objectIndex and ::lightIndex have no meaning here.

#pragma once

#include "../RenderCore/Handle.h"
#include "render_snapshot.h"
#include <cstdint>
#include "../../RenderCore/PipelineRegistry.h"  // PipelineId enum, getPipelineDesc
#include "../../RenderCore/DrawPacket.h"

// ---------------------------------------------------------------------------
// v0 candidate: one entry per (typeId, globalPacketIdx).
// NOT a RenderCore::DrawPacket — different field set, different level of abstraction.
// A future dispatch slice converts candidates to DrawPacket.
// ---------------------------------------------------------------------------
struct StaticPropDrawPacketCandidate {
    uint32_t                   typeId;           // owning type (group key)
    uint32_t                   globalPacketIdx;  // source packet index in batcher global array
    uint32_t                   instanceCount;    // visible instances this frame
    uint32_t                   firstIndex;       // IBO element index (NOT byte offset)
    uint32_t                   indexCount;       // index count for glDrawElements (triangle count x 3)
    RenderCore::MaterialHandle material;         // generation=0 debug wrapper; not a registry handle
                                                 // invalid() when MC2_MATERIAL_GPU=0
    RenderCore::MeshHandle     mesh;             // invalid() — v0 stub; no mesh registry yet
    uint8_t                    alphaPass;        // TRANSITIONAL/debug-only; NOT authoritative v2+
    uint64_t                   sortKey;          // packed per DrawPacket.h spec section 6
    // v2 additions:
    RenderCore::PipelineId     pipelineId;       // per-packet pipeline; authoritative v2+
    uint32_t                   cachedMaterialFlags; // TRANSITIONAL: raw flags from batcher at emit;
                                                    // used by comparePacketsToLegacy to verify
                                                    // pipelineId assignment; remove when dispatch-ready
};

static_assert(sizeof(StaticPropDrawPacketCandidate) <= 64,
    "StaticPropDrawPacketCandidate grew past 64 bytes; check padding");

// ---------------------------------------------------------------------------
// Emit stats. Gate: all three required for PASS.
//   emitted == expectedPackets
//   invalidRanges == 0
//   overflow == false
// ---------------------------------------------------------------------------
struct DrawPacketEmitStats {
    uint32_t emitted;              // candidates written to out[]
    uint32_t distinctTypes;        // unique typeIds with instanceCount > 0 this frame
    uint32_t expectedPackets;      // type-table derived: sum(desc.packetCount) for visible types
    uint32_t oldExpected;          // cross-check: sum(prop.packetCount) from representative snapshot scan;
                                   // must equal expectedPackets each frame
    uint32_t skippedRanges;        // types skipped: packetCount==0, sentinel firstPacket, or
                                   // arithmetic overflow in both old and new paths
    uint32_t invalidRanges;        // batcher_getPacketDrawInfo returned false, OR desc.typeId != row index
    uint32_t materialMismatches;   // types where two snapshot instances disagreed on materialIdx;
                                   // expected 0 while materialIdx is per-type (v1); WARN-only
    bool     overflow;             // emitted hit maxPackets cap before all visible types processed
};

// ---------------------------------------------------------------------------
// Per-frame debug snapshot — written by gameosmain after each emit call,
// read by GuiRuntime/GraphicsOptionsWindow (DrawPackets panel).
//
// Thread-safety: writer (render loop) and reader (ImGui) are both on the
// render thread — no mutex needed; serial execution guarantees coherence.
// Zero-init = "no data yet" (before first emit / before mission load).
// ---------------------------------------------------------------------------
struct DrawPacketsDebugSnapshot {
    uint64_t frame              = 0;
    uint32_t emitted            = 0;
    uint32_t expected           = 0;   // = stats.expectedPackets
    uint32_t distinctTypes      = 0;
    uint32_t invalidRanges      = 0;
    uint32_t skippedRanges      = 0;
    uint32_t materialMismatches = 0;
    bool     overflow           = false;
    uint32_t typeDescCount      = 0;   // batcher_getStaticPropTypeDescCount() at emit time
};

extern DrawPacketsDebugSnapshot g_dpSnapshot;

// ---------------------------------------------------------------------------
// Per-selected-prop packet inspector snapshot.
// Written by gameosmain each frame when a static prop is selected via
// EditorInspector (Ctrl+Shift+Click). Read by GraphicsOptionsWindow.
//
// Selection bridge: g_dpSelectedRecipeIndex is written by EditorInspector
// (on setPickResult / setStaticPropData / clear) and read by gameosmain.
// Both are on the render thread — no mutex needed.
// ---------------------------------------------------------------------------
struct DrawPacketPropRow {
    uint32_t globalPacketIdx = 0;
    uint32_t firstIndex      = 0;
    uint32_t indexCount      = 0;
    int32_t  baseVertex      = 0;
    uint32_t pipelineId      = 0;   // 0=invalid, 1=opaque, 2=alpha_test
    uint32_t materialFlags   = 0;   // raw bit field for diagnostics
    // v1.1 render-spine additions:
    int32_t  texArrayLayer         = -1;           // legacy layer from ExtractedStaticProp; -1=sentinel
    uint32_t albedoTex             = 0xFFFFFFFFu;  // MaterialGpu.albedoTex; 0xFFFFFFFF=absent/MC2_MATERIAL_GPU=0
    bool     materialMatchesLegacy = false;        // albedoTex == (uint32_t)texArrayLayer when both valid
};

struct DrawPacketSelectedPropSnapshot {
    bool     valid           = false;
    int32_t  recipeIndex     = -1;
    uint32_t typeId          = 0;
    uint32_t firstPacket     = 0;
    uint32_t packetCount     = 0;
    uint32_t instanceCount   = 0;   // visible instances this frame (snapshot count)
    uint32_t materialIdx     = 0xFFFFFFFFu;
    uint8_t  alphaClass      = 0;
    char     shapeName[64]   = {};

    static constexpr uint32_t kMaxRows = 8;
    DrawPacketPropRow rows[kMaxRows]    = {};
    uint32_t          rowCount         = 0;
};

// Selection bridge: written by EditorInspector, read by gameosmain.
// -1 = no selection; >= 0 = recipeIndex of selected static prop.
extern int32_t g_dpSelectedRecipeIndex;

extern DrawPacketSelectedPropSnapshot g_dpSelProp;

// ---------------------------------------------------------------------------
// Compare result. Returned by comparePacketsToLegacy().
// All mismatch counters must be zero for the dispatch flip gate to open.
// ---------------------------------------------------------------------------
struct DrawPacketCompareResult {
    uint32_t packets;            // total candidates compared; must be > 0 (finalize ran)
    uint32_t pipelineInvalid;    // pipelineId == Invalid (batcher_getPacketMaterialFlags OOB at emit)
    uint32_t pipelineOob;        // pipelineId outside [1, Count_) — enum out of range
    uint32_t geomMismatch;       // firstIndex or indexCount differs from batcher ground truth
    uint32_t typeMismatch;       // owningTypeID from batcher != candidate.typeId
    uint32_t alphaMismatch;      // pipelineId alpha class disagrees with cachedMaterialFlags
};

// ---------------------------------------------------------------------------
// Sort-key packer (matches DrawPacket.h spec section 6):
//   [63:60] pass     opaque=0, alpha=8, overlay=12
//   [59:56] view     main=0, shadow=4, minimap=8
//   [55:32] pipeline 24 bits; 0 in v0
//   [31:16] material hash 16 bits
//   [15:0]  depth bucket; 0 in v0
// ---------------------------------------------------------------------------
inline uint64_t buildSortKey(uint8_t  pass,
                              uint8_t  view,
                              uint32_t pipeline,
                              uint16_t matHash,
                              uint16_t depth) noexcept {
    return (static_cast<uint64_t>(pass)                   << 60)
         | (static_cast<uint64_t>(view)                   << 56)
         | (static_cast<uint64_t>(pipeline & 0x00FFFFFFu) << 32)
         | (static_cast<uint64_t>(matHash)                << 16)
         | (static_cast<uint64_t>(depth));
}

// ---------------------------------------------------------------------------
// Emit static-prop draw-packet candidates from the snapshot.
//
// Two-phase linear scan. Phase 1: count instances per typeId.
// Phase 2: one candidate per (typeId, globalPacketIdx).
//
// out[] must hold at least batcher_getSortedPacketCount() entries to avoid overflow.
// Returns stats; inspect stats.overflow / stats.invalidRanges / stats.skippedRanges
// before treating emitted candidates as complete.
// ---------------------------------------------------------------------------
DrawPacketEmitStats emitStaticPropDrawPackets(const RenderSnapshot&          snap,
                                              StaticPropDrawPacketCandidate* out,
                                              uint32_t                       maxPackets);

// ---------------------------------------------------------------------------
// Compare emitted candidates against batcher ground truth.
// Env-gated: call only when MC2_DRAW_PACKET_COMPARE=1 (use s_compareEnabled).
// Logs one [DRAW_PACKET_COMPARE v1] summary line to stderr.
// Per-candidate details gated on MC2_DRAW_PACKET_COMPARE_VERBOSE=1.
// ---------------------------------------------------------------------------
DrawPacketCompareResult comparePacketsToLegacy(
    const StaticPropDrawPacketCandidate* candidates,
    uint32_t                             count,
    uint32_t                             frameIndex);

// ---------------------------------------------------------------------------
// DrawPacket v3 — candidate → RenderCore::DrawPacket ABI promotion.
// Call buildStaticPropDrawPackets() after comparePacketsToLegacy() to convert
// emitted candidates into canonical DrawPackets. No GL state mutation.
// ---------------------------------------------------------------------------
struct DrawPacketBuildStats {
    uint32_t inputCandidates;
    uint32_t emitted;
    uint32_t invalidPipeline;          // pipelineId == Invalid
    uint32_t pipelineOutOfRange;       // pipelineId outside [1, Count_) — enum value out of range
    uint32_t invalidIndexCount;        // indexCount == 0
    uint32_t invalidInstanceCount;     // instanceCount == 0
    uint32_t overflow;                 // candidates exceeded maxPackets (should be 0; see call site)
    uint32_t objectIndexSentinelCount; // audit: always == emitted
    uint32_t lightIndexSentinelCount;  // audit: always == emitted
};

DrawPacketBuildStats buildStaticPropDrawPackets(
    const StaticPropDrawPacketCandidate* candidates,
    uint32_t                             candidateCount,
    RenderCore::DrawPacket*              out,
    uint32_t                             maxPackets);
