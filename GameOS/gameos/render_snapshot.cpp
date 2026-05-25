// GameOS/gameos/render_snapshot.cpp
//
// Per-frame RenderSnapshot implementation.
// ExtractRenderSnapshot() populates the snapshot from RenderWorld and
// GpuStaticPropRegistry each frame (after sim, before render).
//
// v0: mech + light blocks (placeholder sentinel records; structs are empty in v0)
// v1: static-prop block (identity + transform, observational)
// v1.1: texArrayLayer/materialIdx wired from per-typeID primary material cache.
//
// Arena: module-static ping-pong — two 1 MiB RenderFrameArena instances
// alternate each frame via reset(). No per-frame heap allocation.

#include "render_snapshot.h"
#include "../../RenderWorld/RenderWorld.h"
#include "gos_static_prop_registry.h"
#include "gos_static_prop_batcher.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

// ---------------------------------------------------------------------------
// Module-static ping-pong arenas (2 × 1 MiB allocated at startup, never freed).
// s_arenaIndex flips each call: the incoming frame resets the idle arena and
// writes into it; the previous frame's data sits untouched in the other slot.
// ---------------------------------------------------------------------------
static RenderFrameArena s_arenas[2];
static uint32_t         s_arenaIndex = 0;

// ---------------------------------------------------------------------------
// ExtractRenderSnapshot
//
// Called once per frame between DoGameLogic() and draw_screen().
// Returns a snapshot whose Span pointers are backed by a module-static
// ping-pong arena (snap.arena is non-owning).  The snapshot is valid until
// the next call, at which point the same arena slot is reset and reused.
// ---------------------------------------------------------------------------
RenderSnapshot ExtractRenderSnapshot()
{
    static uint64_t s_frameIndex = 0;

    RenderSnapshot snap;
    snap.frameIndex    = ++s_frameIndex;
    snap.arenaOverflow = false;
    // Ping-pong: flip to the idle arena, reset it, and use it for this frame.
    s_arenaIndex ^= 1u;
    s_arenas[s_arenaIndex].reset();
    snap.arena = &s_arenas[s_arenaIndex];

    // -----------------------------------------------------------------------
    // v0: mech block
    // ExtractedMech is an empty struct placeholder in v0.
    // No arena allocation needed for zero-size structs.
    // -----------------------------------------------------------------------
    snap.mechs = Span<ExtractedMech>(nullptr, 0);

    // -----------------------------------------------------------------------
    // v0: light block
    // LightRecord is an empty struct placeholder in v0.
    // -----------------------------------------------------------------------
    snap.lights = Span<LightRecord>(nullptr, 0);

    // -----------------------------------------------------------------------
    // Extraction v1: static-prop snapshot
    // -----------------------------------------------------------------------
    {
        // Temp views on the heap — NOT the frame arena (extraction scratch only).
        const uint32_t slotCount = RenderWorld::getStaticPropSlotCount();
        std::vector<RenderWorld::StaticPropRecordView> views;
        views.resize(slotCount);

        const uint32_t total = RenderWorld::fillStaticPropSlots(views.data(), slotCount);

        // Handle growth between getStaticPropSlotCount() and fill (rare race).
        if (total > slotCount) {
            views.resize(total);
            const uint32_t total2 = RenderWorld::fillStaticPropSlots(views.data(), total);
            if (total2 > total) {
                // Slot count grew again between retries (extremely unlikely, not worth looping).
                // Process what we have; sp_fail will reflect any misses from the shortened view.
                // The mismatch will appear as sp_vis_delta in the log.
            }
        }

        // Count alive records for arena allocation.
        uint32_t aliveCount = 0;
        for (uint32_t i = 0; i < static_cast<uint32_t>(views.size()); ++i) {
            if (views[i].alive) ++aliveCount;
        }

        // Allocate from frame arena.
        ExtractedStaticProp* propBuf = snap.arena->alloc<ExtractedStaticProp>(aliveCount);
        if (!propBuf && aliveCount > 0) {
            snap.arenaOverflow = true;
            std::fprintf(stderr,
                "[RENDER_SNAPSHOT v1] WARNING: arena overflow allocating %u ExtractedStaticProp\n",
                aliveCount);
            // Leave staticProps empty; counters remain zero.
        } else {
            uint32_t writeIdx       = 0;
            uint32_t validationFail = 0;
            uint32_t sentinelMat    = 0;
            uint32_t sentinelCull   = 0;
            uint32_t texWired       = 0;
            uint32_t texSentinel    = 0;
            uint32_t matWired       = 0;
            uint32_t matSentinel    = 0;
            uint32_t primaryAlphaOn = 0;
            uint32_t multiPacket    = 0;

            uint32_t cullSubmitted    = 0;
            uint32_t cullMissing      = 0;
            uint32_t hasBounds        = 0;
            uint32_t alphaOn          = 0;
            uint32_t hasShapeName     = 0;
            uint32_t packetRangesOk   = 0;
            uint32_t packetRangeFail  = 0;

            for (uint32_t i = 0; i < static_cast<uint32_t>(views.size()); ++i) {
                const RenderWorld::StaticPropRecordView& v = views[i];
                if (!v.alive)           continue;
                if (!v.generationValid) { ++validationFail; continue; }  // belt-and-suspenders

                // Required field: model matrix.
                float mtx[16];
                if (!GpuStaticPropRegistry::staticPropGetModelMatrix(v.recipeIndex, mtx)) {
                    ++validationFail;
                    continue;
                }

                // Required field: typeId.
                uint32_t typeId = 0;
                if (!GpuStaticPropRegistry::staticPropGetTypeId(v.recipeIndex, &typeId)) {
                    ++validationFail;
                    continue;
                }

                // Required field: instanceFlags (v2).
                // staticPropGetInstanceFlags uses the same recipeValid() check as
                // staticPropGetModelMatrix above; failure here means the registry
                // entry was invalidated between the two calls (extremely unlikely).
                uint32_t instanceFlags = 0u;
                if (!GpuStaticPropRegistry::staticPropGetInstanceFlags(
                        v.recipeIndex, &instanceFlags)) {
                    ++validationFail;
                    continue;
                }

                ExtractedStaticProp& p = propBuf[writeIdx++];
                p.rwHandle    = v.handle;
                p.recipeIndex = v.recipeIndex;
                p.typeId      = typeId;
                p.instanceFlags = instanceFlags;
                std::memcpy(p.worldMatrix, mtx, sizeof(float) * 16);

                // Translation: row-major Stuff-space axis swap.
                // Must match gos_static_prop_registry.cpp:550-555.
                p.worldCenterX = -mtx[3];   // MC2 east
                p.worldCenterY =  mtx[11];  // MC2 north
                p.worldCenterZ =  mtx[7];   // MC2 elevation

                // Optional fields — failure keeps sentinel, no validation error.
                p.boundingRadius = 0.0f;
                GpuStaticPropRegistry::staticPropGetExtentRadius(v.recipeIndex, &p.boundingRadius);

                p.lightDataIndex = 0xFFFFFFFFu;
                GpuStaticPropRegistry::staticPropGetLightDataIndex(v.recipeIndex, &p.lightDataIndex);

                // v2: hasCullRecord from previous frame's flush().
                p.hasCullRecord = false;
                {
                    bool cullRec = false;
                    GpuStaticPropRegistry::staticPropGetHasCullRecord(v.recipeIndex, &cullRec);
                    p.hasCullRecord = cullRec;
                }

                // v1.1 + v2: wire material cache fields (single call fills all).
                p.texArrayLayer = -1;
                p.materialIdx   = 0xFFFFFFFFu;
                p.alphaClass    = 0u;
                p.packetCount   = 0u;
                p.firstPacket   = 0xFFFFFFFFu;

                GpuStaticPropRegistry::StaticPropTypeMaterialCache matInfo{};
                if (GpuStaticPropRegistry::staticPropGetMaterialCacheInfo(
                        v.recipeIndex, &matInfo)) {
                    p.texArrayLayer = matInfo.texArrayLayer;
                    p.materialIdx   = matInfo.materialIdx;
                    p.alphaClass    = matInfo.alphaClass;    // v2
                    p.packetCount   = matInfo.packetCount;   // v2
                    p.firstPacket   = matInfo.firstPacket;   // v2
                    if (matInfo.primaryWasAlphaOn) ++primaryAlphaOn;
                    if (matInfo.multiPacket)       ++multiPacket;
                }

                // v2: shapeName — safe truncating copy.
                p.shapeName[0] = '\0';
                {
                    const char* sn = GpuStaticPropRegistry::getRecipeShapeName(v.recipeIndex);
                    if (sn) {
                        std::snprintf(p.shapeName, sizeof(p.shapeName), "%s", sn);
                    }
                }

                // v1 counters (backward compat)
                if (p.materialIdx == 0xFFFFFFFFu) ++sentinelMat;
                ++sentinelCull;

                // v1.1 counters
                if (p.texArrayLayer != -1)        ++texWired;   else ++texSentinel;
                if (p.materialIdx != 0xFFFFFFFFu) ++matWired;   else ++matSentinel;

                // v2 counters
                if (p.hasCullRecord) ++cullSubmitted; else ++cullMissing;
                if (p.boundingRadius > 0.0f)   ++hasBounds;
                if (p.alphaClass == 1u)         ++alphaOn;
                if (p.shapeName[0] != '\0')     ++hasShapeName;

                // v2: packet range validity — confirm sidecar covers this type's range.
                // Checks bounds via the last packet index; does NOT verify per-packet layer quality.
                if (p.packetCount > 0u && p.firstPacket != 0xFFFFFFFFu) {
                    int32_t dummy = -1;
                    if (batcher_getPacketTexArrayLayer(
                            p.firstPacket + p.packetCount - 1u, &dummy)) {
                        ++packetRangesOk;
                    } else {
                        ++packetRangeFail;
                    }
                }
            }

            snap.staticProps              = Span<ExtractedStaticProp>(propBuf, writeIdx);
            snap.staticPropValidationFail = validationFail;
            snap.staticPropSentinelMat    = sentinelMat;
            snap.staticPropSentinelCull   = sentinelCull;
            snap.staticPropTexWired       = texWired;
            snap.staticPropTexSentinel    = texSentinel;
            snap.staticPropMatWired       = matWired;
            snap.staticPropMatSentinel    = matSentinel;
            snap.staticPropPrimaryAlphaOn = primaryAlphaOn;
            snap.staticPropMultiPacket    = multiPacket;
            snap.staticPropCullSubmitted    = cullSubmitted;
            snap.staticPropCullMissing      = cullMissing;
            snap.staticPropHasBounds        = hasBounds;
            snap.staticPropAlphaOn          = alphaOn;
            snap.staticPropHasShapeName     = hasShapeName;
            snap.staticPropPacketRangesOk   = packetRangesOk;
            snap.staticPropPacketRangesFail = packetRangeFail;
        }
    }

    // -----------------------------------------------------------------------
    // Visibility query for log line
    // -----------------------------------------------------------------------
    uint32_t visibilityStaticPropsCount = 0;
    {
        RenderWorld::VisibilityRequest req;
        req.kindMask = RenderWorld::VisibilityKindMask::StaticProp;
        RenderWorld::VisibilityResult vr = RenderWorld::queryVisibility(req);
        visibilityStaticPropsCount = static_cast<uint32_t>(vr.static_props);
    }

    // -----------------------------------------------------------------------
    // Per-frame log line (v1)
    // -----------------------------------------------------------------------
    std::fprintf(stderr,
        "[RENDER_SNAPSHOT v1] frame=%llu mechs=%u static_props=%u lights=%u "
        "bytes=%zu overflow=%d\n"
        "  sp_fail=%u sp_sentinel_mat=%u sp_sentinel_cull=%u sizeof_static_prop=%zu\n"
        "  sp_tex_wired=%u sp_tex_sentinel=%u sp_mat_wired=%u sp_mat_sentinel=%u\n"
        "  sp_primary_alpha_on=%u sp_multi_packet=%u\n"
        "  sp_cull_submitted=%u sp_cull_missing=%u sp_has_bounds=%u\n"
        "  sp_alpha_on=%u sp_has_shape_name=%u\n"
        "  sp_packet_ranges_ok=%u sp_packet_ranges_invalid=%u\n"
        "  visibility_static_props=%u sp_vis_delta=%d\n",
        static_cast<unsigned long long>(snap.frameIndex),
        static_cast<uint32_t>(snap.mechs.size()),
        static_cast<uint32_t>(snap.staticProps.size()),
        static_cast<uint32_t>(snap.lights.size()),
        snap.arena->bytesUsed(),
        snap.arenaOverflow ? 1 : 0,
        snap.staticPropValidationFail,
        snap.staticPropSentinelMat,
        snap.staticPropSentinelCull,
        sizeof(ExtractedStaticProp),
        snap.staticPropTexWired,
        snap.staticPropTexSentinel,
        snap.staticPropMatWired,
        snap.staticPropMatSentinel,
        snap.staticPropPrimaryAlphaOn,
        snap.staticPropMultiPacket,
        snap.staticPropCullSubmitted,
        snap.staticPropCullMissing,
        snap.staticPropHasBounds,
        snap.staticPropAlphaOn,
        snap.staticPropHasShapeName,
        snap.staticPropPacketRangesOk,
        snap.staticPropPacketRangesFail,
        visibilityStaticPropsCount,
        static_cast<int32_t>(snap.staticProps.size()) -
            static_cast<int32_t>(visibilityStaticPropsCount));

    return snap;
}
