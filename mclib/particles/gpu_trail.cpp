//==========================================================================//
// File:    gpu_trail.cpp                                                   //
// Contents: GpuTrailEmitter implementation with MissileSmoke tuning.      //
//           FX-GPU-1 B2 Phase 2 (Tasks P2.2/P2.5).                        //
//                                                                          //
// Emit API:                                                                //
//   Batcher::BeginGroup(handle, u0, v0, us, vs, blendMode)                //
//   Batcher::Emit(const GpuParticle&)                                      //
//   (matched from spawn_card.cpp / spawn_cardcloud.cpp pattern)            //
//                                                                          //
// HANDLE SURVEY (from P2.1b):                                              //
//   MLR pool handle 41 is the smoke texture used by gosFX specs `smoke`,  //
//   `Smoke`, `Initial_Smoke`, `large_poof` (cited in ResolveTextures path).//
//   Resolves to a valid gos_TextureHandle (980+ range) at flush time.     //
//   Handle 0 is an invalid sentinel — see batcher.cpp:172 early-out.      //
//                                                                          //
// Env gate: MC2_GPU_TRAIL_DISABLE=1 disables all trail emission (P2.5).   //
//==========================================================================//

// stuff.hpp must be first: it pulls in gameos.hpp which defines the types
// that point3d.hpp and vector3d.hpp depend on. All other spawn*.cpp files
// achieve this via gosfxheaders.hpp; we include stuff.hpp directly since
// gpu_trail has no gosFX dependency.
#include <stuff/stuff.hpp>

#include "gpu_trail.h"
#include "batcher.h"
#include "spec.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

static constexpr uint16_t kMissileSmokeTexId = 41;

namespace mc2 { namespace particles {

namespace {

const GpuTrailTuning kTuningTable[] = {
    /* [None]         */
    {
        {0.f, 0.f, 0.f, 0.f},
        {0.f, 0.f, 0.f, 0.f},
        0.f, 0.f, 0.f, 0.f, 0, 0
    },
    /* [MissileSmoke] */
    {
        {0.f, 0.f, 0.f, 0.f},      // head_color: no head sprite
        {1.0f, 1.0f, 1.0f, 0.5f},  // trail_color: white, 50% alpha
        0.0f,                       // head_size: none (no head sprite for missile)
        1.5f,                       // trail_particle_size (world units)
        1.2f,                       // trail_lifetime_s
        8.0f,                       // trail_density_per_meter
        0,                          // blend_mode: standard alpha
        kMissileSmokeTexId,         // texture_id: MLR pool handle 41 (smoke)
    },
    // PpcBolt entry added in P3
};

constexpr float kEpsilonLength = 0.01f;

// MC2_GPU_TRAIL_DISABLE env gate (P2.5). Cached on first call.
// Set MC2_GPU_TRAIL_DISABLE=1 to suppress all trail emission.
bool trail_disabled() {
    static int gate = -1;
    if (gate < 0) {
        const char* v = std::getenv("MC2_GPU_TRAIL_DISABLE");
        gate = (v && v[0] == '1') ? 1 : 0;
    }
    return gate != 0;
}

} // anonymous namespace

const GpuTrailTuning& GpuTrailEmitter::tuning_for(GpuTrailKind k)
{
    auto idx = static_cast<size_t>(k);
    if (idx >= sizeof(kTuningTable) / sizeof(kTuningTable[0])) idx = 0;
    return kTuningTable[idx];
}

void GpuTrailEmitter::Spawn(GpuTrailKind kind,
                            const Stuff::Vector3D& prev_world,
                            const Stuff::Vector3D& cur_world,
                            float deltaT)
{
    if (kind == GpuTrailKind::None) return;
    if (trail_disabled()) return;
    if (!Batcher::is_enabled()) return;

    const GpuTrailTuning& t = tuning_for(kind);

    const float dx = cur_world.x - prev_world.x;
    const float dy = cur_world.y - prev_world.y;
    const float dz = cur_world.z - prev_world.z;
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);

    const bool trail_ok = (deltaT > 0.f && len >= kEpsilonLength);
    int N = 0;
    if (trail_ok) {
        N = static_cast<int>(std::ceil(len * t.trail_density_per_meter));
        N = std::clamp(N, 0, MAX_PARTICLES_PER_SEGMENT);
    }

    Batcher& b = Batcher::Instance();

    if (N > 0) {
        // One group per segment; full-page UV (u0=0,v0=0,us=1,vs=1).
        b.BeginGroup(static_cast<uint32_t>(t.texture_id),
                     0.f, 0.f, 1.f, 1.f,
                     static_cast<int>(t.blend_mode));

        for (int i = 0; i < N; ++i) {
            const float u = (N == 1) ? 0.5f : static_cast<float>(i) / static_cast<float>(N - 1);
            GpuParticle p = {};
            p.position[0] = prev_world.x + dx * u;
            p.position[1] = prev_world.y + dy * u;
            p.position[2] = prev_world.z + dz * u;
            p.color[0]    = t.trail_color[0];
            p.color[1]    = t.trail_color[1];
            p.color[2]    = t.trail_color[2];
            p.color[3]    = t.trail_color[3];
            p.lifetime    = t.trail_lifetime_s;
            p.age         = 0.0f;
            p.size        = t.trail_particle_size;
            p.atlasIndex  = static_cast<uint32_t>(t.texture_id);
            b.Emit(p);
            ++Batcher::s_trail_spawn_total;
        }
    }

    if (t.head_size > 0.f) {
        b.BeginGroup(static_cast<uint32_t>(t.texture_id),
                     0.f, 0.f, 1.f, 1.f,
                     static_cast<int>(t.blend_mode));
        GpuParticle p = {};
        p.position[0] = cur_world.x;
        p.position[1] = cur_world.y;
        p.position[2] = cur_world.z;
        p.color[0]    = t.head_color[0];
        p.color[1]    = t.head_color[1];
        p.color[2]    = t.head_color[2];
        p.color[3]    = t.head_color[3];
        p.lifetime    = 0.05f;
        p.age         = 0.0f;
        p.size        = t.head_size;
        p.atlasIndex  = static_cast<uint32_t>(t.texture_id);
        b.Emit(p);
        ++Batcher::s_trail_head_total;
    }
}

}} // namespace mc2::particles
