// GameOS/gameos/draw_packet_emitter.cpp
//
// DrawPacket v0 — static-prop packet emitter.
// Spec: docs/superpowers/specs/2026-05-25-draw-packet-v0-spec.md
//
// NOTE on index space: batcher_getSortedPacketCount() returns
// s_sortedPacketOrder.size(), which is the number of packets in sorted
// draw order. This CAN differ from s_packets.size() during the window
// between batcher_registerType() and batcher_prepareDraw() (before the
// sorted order is rebuilt). emitStaticPropDrawPackets() iterates by
// globalPacketIdx (prop.firstPacket..firstPacket+packetCount), accessing
// s_packets[] directly via batcher_getPacketDrawInfo(), so it is correct
// regardless of whether s_sortedPacketOrder is stale or incomplete.

#include "draw_packet_emitter.h"
#include "gos_static_prop_batcher.h"   // batcher_getPacketDrawInfo

#include <vector>
#include <cstdint>
#include <cstdio>
#include <climits>  // UINT32_MAX

DrawPacketEmitStats emitStaticPropDrawPackets(const RenderSnapshot&          snap,
                                              StaticPropDrawPacketCandidate* out,
                                              uint32_t                       maxPackets) {
    DrawPacketEmitStats stats{};

    if (snap.arenaOverflow || snap.staticProps.empty() || !out || maxPackets == 0)
        return stats;

    const size_t propCount = snap.staticProps.size();

    // -----------------------------------------------------------------------
    // Phase 1: find maxTypeId, then tally instances per typeId.
    // -----------------------------------------------------------------------
    uint32_t maxTypeId = 0u;
    for (size_t i = 0; i < propCount; ++i) {
        const uint32_t tid = snap.staticProps.data[i].typeId;
        if (tid > maxTypeId) maxTypeId = tid;
    }

    // Guard against sparse or corrupt typeId values.
    static constexpr uint32_t kMaxAllowedTypeId = 65535u;
    if (maxTypeId > kMaxAllowedTypeId) {
        std::fprintf(stderr,
            "[DRAW_PACKET v0] ERROR: maxTypeId=%u exceeds cap %u; aborting\n",
            maxTypeId, kMaxAllowedTypeId);
        return stats;
    }

    std::vector<uint32_t> instanceCounts(maxTypeId + 1u, 0u);
    for (size_t i = 0; i < propCount; ++i)
        ++instanceCounts[snap.staticProps.data[i].typeId];

    // -----------------------------------------------------------------------
    // Phase 2: emit one candidate per unique (typeId, globalPacketIdx).
    // vector<uint8_t>, not vector<bool> — avoids bit-proxy specialization.
    // -----------------------------------------------------------------------
    std::vector<uint8_t> seen(maxTypeId + 1u, 0u);

    for (size_t i = 0; i < propCount; ++i) {
        const ExtractedStaticProp& prop = snap.staticProps.data[i];
        const uint32_t tid = prop.typeId;

        if (seen[tid]) continue;
        seen[tid] = 1u;
        ++stats.distinctTypes;

        // Skip types with sentinel or overflow packet ranges.
        // Warn per type so the log identifies which typeId is missing data.
        if (prop.packetCount == 0u || prop.firstPacket == 0xFFFFFFFFu) {
            ++stats.skippedRanges;
            std::fprintf(stderr,
                "[DRAW_PACKET v0] WARNING: typeId=%u skipped (firstPacket=0x%X packetCount=%u)\n",
                tid, prop.firstPacket, prop.packetCount);
            continue;
        }
        if (prop.firstPacket > UINT32_MAX - prop.packetCount) {
            ++stats.skippedRanges;
            std::fprintf(stderr,
                "[DRAW_PACKET v0] WARNING: typeId=%u packet range overflow (firstPacket=%u packetCount=%u)\n",
                tid, prop.firstPacket, prop.packetCount);
            continue;
        }

        const uint32_t pEnd = prop.firstPacket + prop.packetCount;
        stats.expectedPackets += prop.packetCount;

        // Material: generation=0 is an observational index wrapper, NOT a validated
        // registry handle. Valid only for inspector/log use in v0.
        RenderCore::MaterialHandle mat = RenderCore::MaterialHandle::invalid();
        if (prop.materialIdx != 0xFFFFFFFFu)
            mat = RenderCore::MaterialHandle::make(prop.materialIdx, 0u);

        // Mesh: always invalid in v0 — no mesh registry wired yet.
        const RenderCore::MeshHandle mesh = RenderCore::MeshHandle::invalid();

        const uint8_t  alphaPass = (prop.alphaClass == 1u) ? 8u : 0u;
        const uint16_t matHash   = mat.isValid()
                                     ? static_cast<uint16_t>(mat.bits & 0xFFFFu)
                                     : 0u;
        const uint64_t sortKey   = buildSortKey(alphaPass, 0u, 0u, matHash, 0u);
        const uint32_t ic        = instanceCounts[tid];

        for (uint32_t pi = prop.firstPacket; pi < pEnd; ++pi) {
            if (stats.emitted >= maxPackets) {
                stats.overflow = true;
                return stats;
            }

            uint32_t idxCount = 0u;
            uint32_t firstIdx = 0u;
            int32_t  baseVtx  = 0;
            uint32_t ownType  = 0u;
            if (!batcher_getPacketDrawInfo(pi, &idxCount, &firstIdx, &baseVtx, &ownType)) {
                ++stats.invalidRanges;
                continue;
            }
            (void)baseVtx;  // consumed by batcher internally; not needed in candidate
            (void)ownType;  // redundant with tid

            out[stats.emitted++] = StaticPropDrawPacketCandidate{
                tid,       // typeId
                pi,        // globalPacketIdx
                ic,        // instanceCount
                firstIdx,  // firstIndex
                idxCount,  // indexCount
                mat,       // material (generation=0 debug wrapper)
                mesh,      // mesh (invalid v0 stub)
                alphaPass, // alphaPass
                sortKey    // sortKey
            };
        }
    }

    return stats;
}
