// GameOS/gameos/draw_packet_emitter.cpp
//
// DrawPacket v1 — static-prop packet emitter (type-table walk).
// Spec: docs/superpowers/plans/2026-05-25-draw-packet-v1-type-table.md
//
// NOTE on index space: Phase 2 (v1) iterates by desc.firstPacket..firstPacket+packetCount
// from the type desc table, accessing s_packets[] via batcher_getPacketDrawInfo().
// instanceCounts[] is indexed by typeId (dense, 0..maxTypeId).

#include "draw_packet_emitter.h"
#include "gos_static_prop_batcher.h"   // batcher_getPacketDrawInfo
#include "../../RenderCore/StaticPropTypeDesc.h"  // v1: type table walk

#include <vector>
#include <cstdint>
#include <cstdio>
// <climits> not needed: UINT32_MAX comes from <cstdint> already included via draw_packet_emitter.h

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
            "[DRAW_PACKET v1] ERROR: maxTypeId=%u exceeds cap %u; aborting\n",
            maxTypeId, kMaxAllowedTypeId);
        return stats;
    }

    std::vector<uint32_t> instanceCounts(maxTypeId + 1u, 0u);
    for (size_t i = 0; i < propCount; ++i)
        ++instanceCounts[snap.staticProps.data[i].typeId];

    // -----------------------------------------------------------------------
    // Phase 1b: collect per-type materialIdx from first-seen snapshot instance.
    // alphaClass/firstPacket/packetCount now come from the type table (v1).
    // materialIdx is still snapshot-sourced (not yet in StaticPropTypeDesc).
    // materialMismatches warns when two instances of the same type disagree —
    // expected 0 while materialIdx is effectively per-type.
    // -----------------------------------------------------------------------
    std::vector<uint32_t> typeToMatIdx(maxTypeId + 1u, 0xFFFFFFFFu);
    for (size_t i = 0; i < propCount; ++i) {
        const ExtractedStaticProp& prop = snap.staticProps.data[i];
        const uint32_t tid = prop.typeId;
        if (tid > maxTypeId) continue;
        if (prop.materialIdx == 0xFFFFFFFFu) continue;
        if (typeToMatIdx[tid] == 0xFFFFFFFFu) {
            typeToMatIdx[tid] = prop.materialIdx;
        } else if (typeToMatIdx[tid] != prop.materialIdx) {
            ++stats.materialMismatches;
            std::fprintf(stderr,
                "[DRAW_PACKET v1] WARNING: typeId=%u materialIdx mismatch "
                "(%u vs %u)\n",
                tid, typeToMatIdx[tid], prop.materialIdx);
        }
    }

    // -----------------------------------------------------------------------
    // Cross-check: compute oldExpected from the representative snapshot scan.
    // Equivalence: oldExpected == expectedPackets if and only if, for every
    // typeId visible this frame, prop.packetCount matches desc.packetCount.
    // Both paths apply the same sentinel + overflow guards so that a skipped
    // type is excluded from both sums.
    // -----------------------------------------------------------------------
    {
        std::vector<uint8_t> seenCheck(maxTypeId + 1u, 0u);
        for (size_t i = 0; i < propCount; ++i) {
            const ExtractedStaticProp& prop = snap.staticProps.data[i];
            const uint32_t tid = prop.typeId;
            if (tid > maxTypeId || seenCheck[tid]) continue;
            seenCheck[tid] = 1u;
            // Mirror the same validity guards as the new path.
            if (prop.packetCount == 0u || prop.firstPacket == 0xFFFFFFFFu)
                continue;
            if (prop.firstPacket > UINT32_MAX - prop.packetCount)
                continue;
            stats.oldExpected += prop.packetCount;
        }
    }

    // -----------------------------------------------------------------------
    // Phase 2 (v1): type-table walk.
    // firstPacket/packetCount/alphaClass from StaticPropTypeDesc[].
    // instanceCount from Phase 1 instanceCounts[].
    // materialIdx from Phase 1b typeToMatIdx[].
    // -----------------------------------------------------------------------
    uint32_t typeCount = 0u;
    const RenderCore::StaticPropTypeDesc* descs =
        batcher_getStaticPropTypeDescTable(&typeCount);
    if (!descs || typeCount == 0u) {
        std::fprintf(stderr,
            "[DRAW_PACKET v1] ERROR: type table empty; emit aborted\n");
        return stats;
    }

    for (uint32_t i = 0u; i < typeCount; ++i) {
        const RenderCore::StaticPropTypeDesc& desc = descs[i];

        // Validate dense indexing: the type table is populated as desc.typeId == i.
        // A mismatch indicates a populate bug and makes the candidate's typeId wrong.
        if (desc.typeId != i) {
            ++stats.invalidRanges;
            std::fprintf(stderr,
                "[DRAW_PACKET v1] WARNING: type table row mismatch row=%u typeId=%u\n",
                i, desc.typeId);
            continue;
        }

        const uint32_t tid = desc.typeId;

        // Skip types with no instances this frame.
        // tid may exceed maxTypeId if a registered type has zero snapshot entries.
        if (tid > maxTypeId || instanceCounts[tid] == 0u) continue;
        ++stats.distinctTypes;

        // packetCount==0 means a malformed slot; log and skip.
        if (desc.packetCount == 0u) {
            ++stats.skippedRanges;
            std::fprintf(stderr,
                "[DRAW_PACKET v1] WARNING: typeId=%u skipped (packetCount=0)\n", tid);
            continue;
        }
        // Overflow guard on firstPacket + packetCount arithmetic.
        if (desc.firstPacket > UINT32_MAX - desc.packetCount) {
            ++stats.skippedRanges;
            std::fprintf(stderr,
                "[DRAW_PACKET v1] WARNING: typeId=%u packet range overflow "
                "(firstPacket=%u packetCount=%u)\n",
                tid, desc.firstPacket, desc.packetCount);
            continue;
        }

        stats.expectedPackets += desc.packetCount;

        // materialIdx from Phase 1b (still per-type, not yet in StaticPropTypeDesc).
        // generation=0 debug wrapper only; not a validated MaterialRegistry handle.
        RenderCore::MaterialHandle mat = RenderCore::MaterialHandle::invalid();
        if (tid <= maxTypeId && typeToMatIdx[tid] != 0xFFFFFFFFu)
            mat = RenderCore::MaterialHandle::make(typeToMatIdx[tid], 0u);

        const RenderCore::MeshHandle mesh = RenderCore::MeshHandle::invalid();

        const uint8_t  alphaPass = (desc.alphaClass == 1u) ? 8u : 0u;
        const uint16_t matHash   = mat.isValid()
                                     ? static_cast<uint16_t>(mat.bits & 0xFFFFu)
                                     : 0u;
        const uint64_t sortKey   = buildSortKey(alphaPass, 0u, 0u, matHash, 0u);
        const uint32_t ic        = instanceCounts[tid];

        const uint32_t pEnd = desc.firstPacket + desc.packetCount;
        for (uint32_t pi = desc.firstPacket; pi < pEnd; ++pi) {
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
            (void)baseVtx;
            (void)ownType;

            // v2: per-packet pipeline ID from material flags.
            // No assert — diagnostic slice must not abort release smoke.
            // OOB pi is a bug in the emitter; pipelineId=Invalid lets compare count it.
            uint32_t packetMatFlags = 0u;
            RenderCore::PipelineId packetPipelineId = RenderCore::PipelineId::Invalid;
            if (!batcher_getPacketMaterialFlags(pi, &packetMatFlags)) {
                std::fprintf(stderr,
                    "[DRAW_PACKET v2] batcher_getPacketMaterialFlags OOB pi=%u\n", pi);
                packetMatFlags = 0u;
            } else {
                const bool alpha = (packetMatFlags & STATIC_PROP_FLAG_ALPHA_TEST) != 0;
                packetPipelineId = alpha
                    ? RenderCore::PipelineId::StaticPropAlphaTest
                    : RenderCore::PipelineId::StaticPropOpaque;
            }

            // stats.emitted is both the write index and the running count.
            // The overflow guard above ensures emitted < maxPackets here.
            out[stats.emitted++] = StaticPropDrawPacketCandidate{
                tid,              // typeId
                pi,               // globalPacketIdx
                ic,               // instanceCount
                firstIdx,         // firstIndex
                idxCount,         // indexCount
                mat,              // material (generation=0 debug wrapper)
                mesh,             // mesh (invalid v0 stub)
                alphaPass,        // alphaPass (transitional — from desc.alphaClass)
                sortKey,          // sortKey
                packetPipelineId, // pipelineId (v2 — per-packet, from materialFlags)
                packetMatFlags    // cachedMaterialFlags (v2 — cached for compare round-trip)
            };
        }
    }

    return stats;
}
