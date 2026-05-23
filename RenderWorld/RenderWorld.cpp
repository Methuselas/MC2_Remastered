// RenderWorld/RenderWorld.cpp
//
// Slice M1: thin forwarder. RenderWorld::upsertStaticProp routes into
// GpuStaticPropRegistry::registerRecipe; no new GPU behavior.
//
// Sentinel translation happens HERE on the engine side too: registry
// returns int32_t with -1 sentinel; we translate to RenderObjectHandle.
// The mirror translation (game-side -1 -> invalid()) happens in the
// adapter. Both endpoints translate so int32_t -1 cannot leak upward
// AND RenderObjectHandle::invalid() cannot leak downward. (Spec
// Section 10 amendment 2026-05-22: two seams in M1.)

#include "RenderWorld.h"

// C1 fix: this TU MUST NOT include gos_static_prop_batcher.h (it
// transitively pulls GL + Stuff). All translation between
// StaticPropInstanceDesc <-> GpuStaticPropInstance and all calls into
// GpuStaticPropRegistry::registerRecipe live in
// RenderWorld/legacy/static_prop_backend.cpp.
#include "legacy/static_prop_backend.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

#include "../GameOS/gameos/gos_postprocess.h"

namespace {

// Anonymous-namespace state per Decision D3.A. Adapter does NOT see
// these; the only public surface is RenderWorld:: free functions.
std::atomic<uint64_t> s_upsertOk{0};
std::atomic<uint64_t> s_upsertFail{0};
std::atomic<uint64_t> s_destroyCalls{0};
std::atomic<uint64_t> s_markVisibleCalls{0};
std::atomic<uint64_t> s_frameCounter{0};

bool envFlag(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] && v[0] != '0';
}

// M1.5: cached env-flag accessor. First call reads MC2_OBJECT_ID_BUFFER
// from getenv(); subsequent calls return the cached bool. Designed so
// the hot static-prop draw loop sees ONE branch-not-taken per draw on
// the env-OFF default path.
bool readObjectIdBufferEnv() {
    return envFlag("MC2_OBJECT_ID_BUFFER");
}

uint32_t recipeIndexToHandleIndex(int32_t r) {
    // M1: generation is always 1 (no slot recycle yet). Index is the
    // raw recipe slot. -1 -> invalid (index=0, generation=0).
    if (r < 0) return 0;
    // 20-bit clamp; assert if registry ever overflows (it cannot under
    // current configuration; this is a future-proofing guard).
    return static_cast<uint32_t>(r) & 0x000FFFFFu;
}

int32_t handleToRecipeIndex(RenderCore::RenderObjectHandle h) {
    if (!h.isValid()) return -1;
    return static_cast<int32_t>(h.index());
}

// M1.5: per-slot inspection table. Always populated (M1 decision);
// ~85 KB peak at tier1 mc2_24 = 2641 props. Indexed by
// handle.index(); resized lazily on upsert. mutex guards resize +
// write; reads in lookupAtPixel acquire the same lock (cheap,
// click-rate).
std::mutex                                  s_objectRecordsMutex;
std::vector<RenderWorld::RenderObjectRecord> s_objectRecords;

void populateRecord(uint32_t handleIndex,
                    uint16_t generation,
                    uint32_t gameObjectId)
{
    std::lock_guard<std::mutex> lk(s_objectRecordsMutex);
    if (handleIndex >= s_objectRecords.size()) {
        // Grow with headroom; doubling beyond demand to amortize.
        const size_t want = static_cast<size_t>(handleIndex) + 1;
        const size_t cap  = (want * 3) / 2 + 16;
        s_objectRecords.resize(cap);
    }
    auto& rec = s_objectRecords[handleIndex];
    rec.generation         = generation;
    rec.flags              = RenderWorld::kRenderObjectFlagAlive;
    rec.meshHandleBits     = 0;           // M1.5: unknown (no MeshHandle producer)
    rec.materialHandleBits = 0;           // M1.5: unknown
    rec.lodLevel           = 0xFFu;       // M1.5: unknown
    rec.pipelineId         = 0;           // M1.5 sentinel
    rec.drawPacketIndex    = 0xFFFFFFFFu; // M1.5 sentinel
    rec.pathReasonCode     = 0;           // M1.5 sentinel
    rec.gameObjectId       = gameObjectId;
}

void retireRecord(uint32_t handleIndex)
{
    std::lock_guard<std::mutex> lk(s_objectRecordsMutex);
    if (handleIndex >= s_objectRecords.size()) return;
    auto& rec = s_objectRecords[handleIndex];
    rec.flags &= static_cast<uint16_t>(~RenderWorld::kRenderObjectFlagAlive);
    // Bump generation so the next upsert at this index produces a
    // distinct handle and stale pixels read back as invalid via the
    // generation check in lookupAtPixel.
    rec.generation = static_cast<uint16_t>(rec.generation + 1u);
}

} // namespace

namespace RenderWorld {

void init() {
    s_upsertOk.store(0);
    s_upsertFail.store(0);
    s_destroyCalls.store(0);
    s_markVisibleCalls.store(0);
    s_frameCounter.store(0);
    const bool oid = IsObjectIdBufferEnabled();
    std::fprintf(stderr, "[RENDER_WORLD v1] event=init objectid_buffer=%s\n",
                 oid ? "on" : "off");
    if (oid) {
        // Once-per-process; helps log readers correlate the banner with
        // the integer-MRT attachment lifecycle in gos_postprocess.cpp.
        std::fprintf(stderr,
            "[OBJECT_ID v1] event=enabled format=R32UI attachment=GL_COLOR_ATTACHMENT2\n");
    }
}

void destroy() {
    std::fprintf(stderr,
        "[RENDER_WORLD v1] event=destroy upsert_ok=%llu upsert_fail=%llu "
        "destroy_calls=%llu mark_visible=%llu\n",
        (unsigned long long)s_upsertOk.load(),
        (unsigned long long)s_upsertFail.load(),
        (unsigned long long)s_destroyCalls.load(),
        (unsigned long long)s_markVisibleCalls.load());
}

RenderCore::RenderObjectHandle upsertStaticProp(RenderCore::StaticPropDesc desc) {
    // M1.5: capture POD fields before std::move; vector is moved out
    // but scalars survive on the moved-from object regardless, this
    // is just defensive.
    const uint32_t gameObjectId = desc.gameObjectId;
    const int32_t r = legacy::registerStaticPropRecipe(std::move(desc));
    if (r < 0) {
        s_upsertFail.fetch_add(1, std::memory_order_relaxed);
        if (envFlag("MC2_RENDER_WORLD_TRACE")) {
            std::fprintf(stderr,
                "[RENDER_WORLD v1] event=upsert_fail recipe=-1\n");
        }
        return RenderCore::RenderObjectHandle::invalid();
    }
    s_upsertOk.fetch_add(1, std::memory_order_relaxed);
    RenderCore::RenderObjectHandle h = RenderCore::RenderObjectHandle::make(
        recipeIndexToHandleIndex(r), 1u);
    // M1.5: populate the always-on record table (mission/upsert-time
    // metadata, ~85 KB peak; M1 decision).
    populateRecord(h.index(),
                   static_cast<uint16_t>(h.generation()),
                   gameObjectId);
    if (envFlag("MC2_RENDER_WORLD_TRACE")) {
        std::fprintf(stderr,
            "[RENDER_WORLD v1] event=upsert_ok recipe=%d handle.index=%u\n",
            r, (unsigned)h.index());
    }
    return h;
}

RenderCore::RenderObjectHandle adoptStaticPropRecipe(int32_t recipeIndex) {
    // m5 fix: wrap an existing registry slot in a Handle without
    // creating a new recipe entry. Counter is incremented so
    // [RENDER_WORLD v1] objects stays honest for the late-spawn path.
    if (recipeIndex < 0) {
        return RenderCore::RenderObjectHandle::invalid();
    }
    s_upsertOk.fetch_add(1, std::memory_order_relaxed);
    RenderCore::RenderObjectHandle h = RenderCore::RenderObjectHandle::make(
        recipeIndexToHandleIndex(recipeIndex), 1u);
    // M1.5: late-spawn populates the record table too. gameObjectId
    // is unknown at this seam (the adapter does not pass one through
    // syncStaticPropLateSpawn); 0 is the canonical "no cookie".
    populateRecord(h.index(),
                   static_cast<uint16_t>(h.generation()),
                   0u);
    return h;
}

void destroy(RenderCore::RenderObjectHandle h) {
    if (!h.isValid()) return;
    legacy::invalidateStaticProp(handleToRecipeIndex(h));
    s_destroyCalls.fetch_add(1, std::memory_order_relaxed);
    // M1.5: retire the record (clears alive flag, bumps generation).
    retireRecord(h.index());
    if (envFlag("MC2_RENDER_WORLD_TRACE")) {
        std::fprintf(stderr,
            "[RENDER_WORLD v1] event=destroy handle.index=%u\n",
            (unsigned)h.index());
    }
}

void markVisible(RenderCore::RenderObjectHandle h,
                 uint32_t lightDataIndex, float extentRadius) {
    if (!h.isValid()) return;
    legacy::markVisibleStaticProp(handleToRecipeIndex(h),
                                  lightDataIndex, extentRadius);
    s_markVisibleCalls.fetch_add(1, std::memory_order_relaxed);
}

bool isReady(RenderCore::RenderObjectHandle h) {
    if (!h.isValid()) return false;
    return legacy::isReadyStaticProp(handleToRecipeIndex(h));
}

bool IsObjectIdBufferEnabled() {
    // Function-local static: thread-safe initialization (C++11 magic
    // statics); init runs exactly once at first call.
    static const bool s_enabled = readObjectIdBufferEnv();
    return s_enabled;
}

uint32_t objectIdRawForStaticPropRecipe(int32_t recipeIndex) {
    // M1.5 C1: centralized Handle bit encoding for the batcher
    // producer. Mirrors the recipeIndex -> Handle convention used by
    // upsertStaticProp/adoptStaticPropRecipe (generation=1, 20-bit
    // index). Returns 0 (invalid raw) for negative recipeIndex.
    if (recipeIndex < 0) return 0u;
    return RenderCore::RenderObjectHandle::make(
        static_cast<uint32_t>(recipeIndex) & 0x000FFFFFu,
        1u
    ).raw();
}

void frameBannerTick() {
    const uint64_t f = s_frameCounter.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool perFrame = envFlag("MC2_RENDER_WORLD_TRACE");
    const bool summary  = (f % 600u) == 0u;
    if (!perFrame && !summary) return;
    // m4 fix: source the active prop count from the registry's own
    // active-recipe accessor, NOT from the adapter-side delta. Adapter
    // delta drifts if the registry tombstones via paths the adapter
    // never sees; registry count is canonical.
    const uint64_t active = legacy::getStaticPropActiveCount();
    const char* oidTok = IsObjectIdBufferEnabled() ? "on" : "off";
    std::fprintf(stderr,
        "[RENDER_WORLD v1] frame=%llu objects=%llu visible=0 packets=0 views=1 objectid_buffer=%s\n",
        (unsigned long long)f, (unsigned long long)active, oidTok);
}

LookupResult lookupAtPixel(int screenX, int screenY) {
    LookupResult out;
    if (!IsObjectIdBufferEnabled()) {
        static bool warned = false;
        if (!warned) {
            std::fprintf(stderr,
                "[RENDER_WORLD v1] WARN: lookupAtPixel called with MC2_OBJECT_ID_BUFFER=0\n");
            warned = true;
        }
        return out;
    }
    gosPostProcess* pp = getGosPostProcess();
    if (!pp) {
        static bool warned = false;
        if (!warned) {
            std::fprintf(stderr,
                "[RENDER_WORLD v1] WARN: lookupAtPixel called before postprocess init\n");
            warned = true;
        }
        return out;
    }
    const GLuint fbo = pp->getSceneFBO();
    const GLuint tex = pp->getSceneObjectIdTex();
    if (!fbo || !tex) {
        return out;
    }

    // Synchronous single-pixel readback. Per spec section 7: stalls
    // the GPU until prior-frame attachment-2 writes are visible.
    // GL_RED_INTEGER + GL_UNSIGNED_INT is the integer-format pair
    // (using GL_RED + GL_FLOAT would silently reinterpret bits).
    uint32_t raw = 0u;
    GLint prevReadFbo = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT2);
    glReadPixels(screenX, screenY, 1, 1, GL_RED_INTEGER, GL_UNSIGNED_INT, &raw);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prevReadFbo));

    if (raw == 0u) {
        // Background / cleared pixel.
        return out;
    }

    RenderCore::RenderObjectHandle h;
    h.bits = raw;

    // Look up the record under the mutex; copy out under the lock.
    RenderObjectRecord rec;
    {
        std::lock_guard<std::mutex> lk(s_objectRecordsMutex);
        if (h.index() >= s_objectRecords.size()) {
            return out;  // out-of-range index: treat as invalid
        }
        rec = s_objectRecords[h.index()];
    }

    // Generation check: stale pixel (rendered before slot recycle)
    // returns invalid even though the raw value parses to a Handle.
    if (rec.generation != static_cast<uint16_t>(h.generation())) {
        return out;
    }
    if ((rec.flags & kRenderObjectFlagAlive) == 0u) {
        return out;
    }

    out.isValid            = true;
    out.handle             = h;
    out.meshHandleBits     = rec.meshHandleBits;
    out.materialHandleBits = rec.materialHandleBits;
    out.lodLevel           = rec.lodLevel;
    out.pipelineId         = rec.pipelineId;
    out.drawPacketIndex    = rec.drawPacketIndex;
    out.pathReasonCode     = rec.pathReasonCode;
    out.gameObjectId       = rec.gameObjectId;
    return out;
}

} // namespace RenderWorld
