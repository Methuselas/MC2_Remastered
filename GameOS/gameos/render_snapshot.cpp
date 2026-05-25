// GameOS/gameos/render_snapshot.cpp
//
// Per-frame RenderSnapshot implementation.
// ExtractRenderSnapshot() populates the snapshot from RenderWorld and
// GpuStaticPropRegistry each frame (after sim, before render).
//
// v0: mech + light blocks (placeholder sentinel records; structs are empty in v0)
// v1: static-prop block (identity + transform, observational)
//
// Arena: a fresh RenderFrameArena is allocated per call (1 MiB slab; allocated
// once in RenderFrameArena ctor, reset to 0 bytes-used each frame).

#include "render_snapshot.h"
#include "../../RenderWorld/RenderWorld.h"
#include "gos_static_prop_registry.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

// ---------------------------------------------------------------------------
// ExtractRenderSnapshot
//
// Called once per frame between DoGameLogic() and draw_screen().
// Returns a snapshot backed by a frame-lifetime arena in snap.arena.
// The unique_ptr keeps the arena alive for the frame; caller discards
// the snapshot before the next call.
// ---------------------------------------------------------------------------
RenderSnapshot ExtractRenderSnapshot()
{
    static uint64_t s_frameIndex = 0;

    RenderSnapshot snap;
    snap.frameIndex    = ++s_frameIndex;
    snap.arenaOverflow = false;
    // TODO(extraction-v2): This allocates a fresh 1 MiB arena every frame.
    // The spec (v0) called for a module-static ping-pong design (two persistent arenas
    // alternating via reset()) to avoid 60 MiB/s of allocator churn at render frequency.
    // Acceptable in v1 (observational, not on hot render path) but must be fixed before
    // this path drives rendering. Track as extraction-v2 follow-up.
    snap.arena         = std::make_unique<RenderFrameArena>();
    // Arena is fresh (used=0) from make_unique.

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

                ExtractedStaticProp& p = propBuf[writeIdx++];
                p.rwHandle    = v.handle;
                p.recipeIndex = v.recipeIndex;
                p.typeId      = typeId;
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

                // Sentinel v1 fields (wired in v1.1+).
                p.materialIdx   = 0xFFFFFFFFu;
                p.texArrayLayer = -1;
                p.hasCullRecord = false;

                ++sentinelMat;
                ++sentinelCull;
            }

            snap.staticProps              = Span<ExtractedStaticProp>(propBuf, writeIdx);
            snap.staticPropValidationFail = validationFail;
            snap.staticPropSentinelMat    = sentinelMat;
            snap.staticPropSentinelCull   = sentinelCull;
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
        visibilityStaticPropsCount,
        static_cast<int32_t>(snap.staticProps.size()) -
            static_cast<int32_t>(visibilityStaticPropsCount));

    return snap;
}
