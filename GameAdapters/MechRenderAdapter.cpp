// GameAdapters/MechRenderAdapter.cpp
//
// Slice M2 (route-only): the ONLY TU that may include both
// mclib/mech3d.h and RenderWorld/RenderWorld.h.
//
// Spec: docs/superpowers/specs/2026-05-23-renderworld-slice-m2-mech-adapter-spec.md
// Firewall: scripts/check-include-firewall.allowlist lists this file as an
// explicit allowlist exception for the Mech3DAppearance forbidden-symbol check.

#include "MechRenderAdapter.h"

// Engine side.
#include "../RenderWorld/RenderWorld.h"

// Game side. This is the ONLY TU outside mclib/ that may include mech3d.h.
#include "../mclib/mech3d.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstdint>

namespace {

// Adapter state quarantined in anonymous namespace. Separate from
// static-prop counters per spec Section 9 counter discipline.
uint64_t s_mechs_registered   = 0;
uint64_t s_mechs_alive        = 0;
uint64_t s_mechs_destroyed    = 0;
uint64_t s_mech_register_fail = 0;

bool envFlag(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] && v[0] != '0';
}

} // namespace

namespace GameAdapters {
namespace Mech {

void beginMission() {
    s_mechs_registered   = 0;
    s_mechs_alive        = 0;
    s_mechs_destroyed    = 0;
    s_mech_register_fail = 0;
    if (envFlag("MC2_RENDER_WORLD_TRACE")) {
        std::fprintf(stderr, "[RENDER_WORLD v1] event=mech_begin_mission\n");
    }
}

void endMission() {
    // Always-on per-mission summary.
    std::fprintf(stderr,
        "[RENDER_WORLD v1] event=mech_end_mission registered=%llu destroyed=%llu "
        "alive=%llu fail=%llu\n",
        (unsigned long long)s_mechs_registered,
        (unsigned long long)s_mechs_destroyed,
        (unsigned long long)s_mechs_alive,
        (unsigned long long)s_mech_register_fail);

    // Always-on warning if any handles were not retired normally.
    if (s_mechs_alive > 0) {
        std::fprintf(stderr,
            "[RENDER_WORLD v1] WARN: event=mech_leaked_handles count=%llu\n",
            (unsigned long long)s_mechs_alive);
    }

    // Force-clear any remaining mech records in the engine table after
    // logging the warning, so stale handles cannot carry into the next mission.
    // s_nextMechSlot resets inside clearAllMechRecords().
    RenderWorld::clearAllMechRecords();

    s_mechs_registered   = 0;
    s_mechs_alive        = 0;
    s_mechs_destroyed    = 0;
    s_mech_register_fail = 0;
}

RenderCore::RenderObjectHandle syncSpawn(Mech3DAppearance& mech,
                                         uint32_t          gameObjectId) {
    // Double-spawn guard: a valid handle before spawn means a prior
    // destroyMech was missed (or the re-init destroyMech path was skipped).
    assert(!mech.getRenderWorldHandle().isValid() &&
           "MechRenderAdapter::syncSpawn: handle already valid -- "
           "prior destroyMech was missed");

    RenderWorld::RenderMechDesc desc;
    desc.mechTypeId   = 0u;  // M2: type identity deferred to M2.5
    desc.gameObjectId = gameObjectId;
    desc.debugCookie  = reinterpret_cast<uintptr_t>(&mech);

    RenderCore::RenderObjectHandle h = RenderWorld::registerMech(desc);

    if (h.isValid()) {
        mech.setRenderWorldHandleForAdapter(h);
        ++s_mechs_registered;
        ++s_mechs_alive;
        if (envFlag("MC2_RENDER_WORLD_TRACE")) {
            std::fprintf(stderr,
                "[RENDER_WORLD v1] event=mech_register mech=%p handle.index=%u "
                "gameObjectId=%u\n",
                (void*)&mech, (unsigned)h.index(), (unsigned)gameObjectId);
        }
    } else {
        ++s_mech_register_fail;
        std::fprintf(stderr,
            "[RENDER_WORLD v1] event=mech_register_fail mech=%p\n",
            (void*)&mech);
    }

    return h;
}

void destroyMech(Mech3DAppearance& mech) {
    const RenderCore::RenderObjectHandle h = mech.getRenderWorldHandle();
    if (!h.isValid()) {
        // No-op: never registered, or already retired.
        return;
    }

    if (envFlag("MC2_RENDER_WORLD_TRACE")) {
        std::fprintf(stderr,
            "[RENDER_WORLD v1] event=mech_destroy handle.index=%u\n",
            (unsigned)h.index());
    }

    RenderWorld::destroyMech(h);
    mech.clearRenderWorldHandleForAdapter();

    ++s_mechs_destroyed;
    if (s_mechs_alive > 0) {
        --s_mechs_alive;
    }
}

} // namespace Mech
} // namespace GameAdapters
