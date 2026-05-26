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

// ---------------------------------------------------------------------------
// v0 candidate: one entry per (typeId, globalPacketIdx).
// NOT a RenderCore::DrawPacket — different field set, different level of abstraction.
// A future dispatch slice converts candidates to DrawPacket.
// ---------------------------------------------------------------------------
struct StaticPropDrawPacketCandidate {
    uint32_t                   typeId;           // owning type (group key)
    uint32_t                   globalPacketIdx;  // source packet index in batcher global array
    uint32_t                   instanceCount;    // visible instances this frame
    uint32_t                   firstIndex;       // IBO byte offset
    uint32_t                   indexCount;       // index count for glDrawElements (triangle count x 3)
    RenderCore::MaterialHandle material;         // generation=0 debug wrapper; not a registry handle
                                                 // invalid() when MC2_MATERIAL_GPU=0
    RenderCore::MeshHandle     mesh;             // invalid() — v0 stub; no mesh registry yet
    uint8_t                    alphaPass;        // 0=opaque, 8=alpha-test (sortKey pass bits)
    uint64_t                   sortKey;          // packed per DrawPacket.h spec section 6
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
    uint32_t emitted;          // candidates written to out[]
    uint32_t distinctTypes;    // unique typeIds found in snapshot
    uint32_t expectedPackets;  // sum of prop.packetCount over distinct typeIds with valid ranges
    uint32_t skippedRanges;    // distinct types skipped due to sentinel/overflow packet ranges
    uint32_t invalidRanges;    // batcher_getPacketDrawInfo returned false for a valid-range packet
    bool     overflow;         // emitted hit maxPackets cap before completing
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
