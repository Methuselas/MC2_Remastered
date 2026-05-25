//==========================================================================//
// File:    spawn_cardcloud.cpp                                              //
// Contents: mc2::particles::SpawnCardCloud implementation.                  //
//           Plan v6 §5.4 B1 Stage 2' C11.                                   //
//                                                                           //
// Spawn-event semantics derived from mclib/gosfx/cardcloud.cpp +            //
// mclib/gosfx/spinningcloud.cpp + mclib/gosfx/particlecloud.cpp:            //
//                                                                           //
//   - CardCloud is a multi-particle Effect. It derives from SpinningCloud   //
//     -> ParticleCloud, so starting population at Effect::Start is sampled  //
//     from spec->m_startingPopulation at age=0 / parentSeed (particle-      //
//     cloud.cpp:351). Birth-rate-driven ongoing spawns                      //
//     (m_particlesPerSecond) are RUNTIME spawns folded into the GPU         //
//     animation pass per the v6 plan (B2 polish debt).                      //
//   - Per-particle attributes (seed / lifetime / emitter offset / velocity ///
//     color) follow the same ParticleCloud chain as PointCloud/ShardCloud.  //
//   - CardCloud-specific size: halfY = m_halfHeight at (age=0, child_seed); //
//     halfX = halfY * m_aspectRatio at (age=0, child_seed); bounding radius //
//     = sqrt(halfX^2 + halfY^2) (cardcloud.cpp:375-383). We bake the radius //
//     into GpuParticle.size — the GPU billboard pass emits a camera-facing  //
//     quad whose footprint matches the legacy bounding extent.              //
//   - Legacy Draw() fires DrawEffect once per cloud (effect.cpp:697); we    //
//     match by firing FX_TRACE_DRAW once per SpawnCardCloud call.           //
//                                                                           //
// Schema-fidelity note (B2 polish debt):                                    //
//   - m_pIndex / m_animated / m_U/VOffset / m_U/VSize / m_width describe   //
//     per-particle UV-atlas animation. GpuParticle has only atlasIndex      //
//     (uint), no UV sub-rect. The billboard pass renders the full atlas     //
//     page; animated cards will visually degrade to a single frame.         //
//   - m_localRotation (per-particle spin from SpinningCloud) is not in the  //
//     schema. Quads are camera-facing axis-aligned.                         //
//   Both deferrals match the C5 ShardCloud precedent.                       //
//                                                                           //
// CPU projection invariant: this file MUST NOT include or reference any    //
// of the forbidden projection wrappers — the authoritative list lives      //
// in scripts/check-particles-no-cpu-projection.sh.                          //
//===========================================================================//

#include "gosfx/gosfxheaders.hpp"
#include "gosfx/cardcloud.hpp"
#include "spawn_cardcloud.h"
#include "batcher.h"
#include "fx_trace/fx_trace.h"

#include <stuff/linearmatrix.hpp>
#include <stuff/point3d.hpp>
#include <stuff/random.hpp>

namespace mc2 {
namespace particles {

void SpawnCardCloud(const gosFX::CardCloud__Specification* spec,
                    const Stuff::LinearMatrix4D*           parentToWorld,
                    float                                   spawnSeed)
{
    if (!spec) {
        return;
    }

    // FX_TRACE_DRAW key schema matches the legacy gosFX::Effect::Draw entry
    // (effect.cpp:697) — ONE event per spawn. Legacy CardCloud::Draw fires
    // DrawEffect once per cloud regardless of particle count.
    FX_TRACE_DRAW(spec->m_name);

    // Cheap early-out before sampling curves: when MC2_GPU_PARTICLES=0
    // the new path is dormant and we still want the FX_TRACE_DRAW count
    // (it is the per-spec invocation oracle, not the GPU activation flag).
    if (!Batcher::is_enabled()) {
        return;
    }

    // Const_cast because the legacy curve ComputeValue() methods are not
    // const-correct (FCurve internals mutate cache state). SpecLibrary
    // owns the spec lifetime, so the cast is safe for read-only sampling.
    gosFX::CardCloud__Specification* mut_spec =
        const_cast<gosFX::CardCloud__Specification*>(spec);

    // Sample curves at parent_age=0.5 (mid-life / peak-visibility). See
    // spawn_card.cpp for the rationale: the GPU shader does not advance
    // age, so age=0 bakes fade-in invisibility into every particle. 0.5
    // picks the typical peak of normalized 0->1 envelopes. Stage 2' polish
    // (shader-side age advancement) retires this constant.
    const Stuff::Scalar parent_age  = 0.5f;
    const Stuff::Scalar parent_seed = spawnSeed;

    Stuff::Scalar population_f =
        mut_spec->m_startingPopulation.ComputeValue(parent_age, parent_seed);
    if (population_f < 0.0f) {
        population_f = 0.0f;
    }
    int population = static_cast<int>(population_f);
    if (population > mut_spec->m_maxParticleCount) {
        population = mut_spec->m_maxParticleCount;
    }
    if (population <= 0) {
        return;
    }

    // Child-seed range — sampled per-Effect at (parent_age=0, parent_seed).
    const Stuff::Scalar min_seed =
        mut_spec->m_minimumChildSeed.ComputeValue(parent_age, parent_seed);
    Stuff::Scalar seed_range =
        mut_spec->m_maximumChildSeed.ComputeValue(parent_age, parent_seed) - min_seed;
    if (seed_range < 0.0f) {
        seed_range = 0.0f;
    }

    Stuff::Point3D parent_origin(0.0f, 0.0f, 0.0f);
    if (parentToWorld) {
        parent_origin = Stuff::Point3D(*parentToWorld);
    }

    // P1-2: resolve the GOS texture handle from the effect spec so we can
    // hand it to the GPU bridge for per-texture batching.  The spec carries
    // an MLRState whose texture field is an index into MLRTexturePool; pool
    // index 0 means "no texture".  We walk pool[mlrHandle]->GetImage()->
    // GetHandle() to reach the DWORD gos handle and cast it to uint32_t for
    // storage in atlasIndex.  Any step that returns 0/null produces
    // gosHandle=0, which the bridge maps to a missing-texture log + skip.
    uint32_t gosTexHandle = 0u;
    {
        const unsigned mlrHandle = mut_spec->m_state.GetTextureHandle();
        if (mlrHandle > 0 && MidLevelRenderer::MLRTexturePool::Instance) {
            MidLevelRenderer::MLRTexture* mlrTex =
                (*MidLevelRenderer::MLRTexturePool::Instance)[static_cast<int>(mlrHandle)];
            if (mlrTex) {
                MidLevelRenderer::GOSImage* img = mlrTex->GetImage();
                if (img) {
                    DWORD h = img->GetHandle();
                    gosTexHandle = static_cast<uint32_t>(h);
                }
            }
        }
    }

    Batcher& batcher = Batcher::Instance();

    for (int i = 0; i < population; ++i) {
        // Per-particle seed in [min_seed, min_seed + seed_range], clamped
        // to [0,1].
        Stuff::Scalar child_seed =
            Stuff::Random::GetFraction() * seed_range + min_seed;
        if (child_seed < 0.0f) child_seed = 0.0f;
        else if (child_seed > 1.0f) child_seed = 1.0f;

        // Per-particle lifetime; 33ms floor matches particlecloud.cpp:550.
        Stuff::Scalar lifetime =
            mut_spec->m_pLifeSpan.ComputeValue(parent_age, child_seed);
        if (lifetime < 0.0333333f) {
            lifetime = 0.0333333f;
        }

        // Per-particle emitter-volume offset (legacy unit-sphere-ish
        // YawPitchRange sample, particlecloud.cpp:558-563).
        Stuff::YawPitchRange initial_p(
            Stuff::Random::GetFraction() * Stuff::Two_Pi,
            Stuff::Random::GetFraction() * Stuff::Pi - Stuff::Pi_Over_2,
            Stuff::Random::GetFraction());
        Stuff::Vector3D unit_pos(initial_p);
        Stuff::Point3D local_offset(
            unit_pos.x * mut_spec->m_emitterSizeX.ComputeValue(parent_age, child_seed),
            unit_pos.y * mut_spec->m_emitterSizeY.ComputeValue(parent_age, child_seed),
            unit_pos.z * mut_spec->m_emitterSizeZ.ComputeValue(parent_age, child_seed));

        Stuff::Point3D world_pos;
        if (parentToWorld) {
            world_pos.Multiply(local_offset, *parentToWorld);
        } else {
            world_pos = local_offset;
        }

        // Initial velocity (matches particlecloud.cpp:577-591).
        Stuff::Scalar pitch_min =
            mut_spec->m_minimumDeviation.ComputeValue(parent_age, child_seed);
        Stuff::Scalar pitch_range =
            mut_spec->m_maximumDeviation.ComputeValue(parent_age, child_seed) - pitch_min;
        if (pitch_range < 0.0f) {
            pitch_range = 0.0f;
        }
        pitch_min +=
            pitch_range * Stuff::Random::GetFraction() - Stuff::Pi_Over_2;
        Stuff::YawPitchRange initial_v(
            Stuff::Random::GetFraction() * Stuff::Two_Pi,
            pitch_min,
            mut_spec->m_startingSpeed.ComputeValue(parent_age, child_seed));
        Stuff::Vector3D local_velocity(initial_v);
        Stuff::Vector3D world_velocity;
        if (parentToWorld) {
            world_velocity.Multiply(local_velocity, *parentToWorld);
        } else {
            world_velocity = local_velocity;
        }

        // Color at age=0 / child_seed.
        const Stuff::Scalar r = mut_spec->m_pRed  .ComputeValue(parent_age, child_seed);
        const Stuff::Scalar g = mut_spec->m_pGreen.ComputeValue(parent_age, child_seed);
        const Stuff::Scalar b = mut_spec->m_pBlue .ComputeValue(parent_age, child_seed);
        const Stuff::Scalar a = mut_spec->m_pAlpha.ComputeValue(parent_age, child_seed);

        // CardCloud-specific size: collapse legacy per-axis halfX/halfY to
        // a single bounding-radius scalar (cardcloud.cpp:375-383).
        Stuff::Scalar halfY =
            mut_spec->m_halfHeight.ComputeValue(parent_age, child_seed);
        if (halfY < 0.0f) halfY = 0.0f;
        const Stuff::Scalar aspect =
            mut_spec->m_aspectRatio.ComputeValue(parent_age, child_seed);
        Stuff::Scalar halfX = halfY * aspect;
        if (halfX < 0.0f) halfX = 0.0f;
        const Stuff::Scalar size =
            Stuff::Sqrt(halfX * halfX + halfY * halfY);

        GpuParticle p = {};
        p.position[0] = world_pos.x;
        p.position[1] = world_pos.y;
        p.position[2] = world_pos.z;
        p.color[0]    = r;
        p.color[1]    = g;
        p.color[2]    = b;
        p.color[3]    = a;
        p.velocity[0] = world_velocity.x;
        p.velocity[1] = world_velocity.y;
        p.velocity[2] = world_velocity.z;
        p.lifetime    = (float)lifetime;
        p.age         = 0.0f;
        // B2 polish debt: per-particle UV-atlas animation
        // (m_pIndex / m_animated / m_U/VOffset / m_U/VSize) and
        // per-particle rotation (m_localRotation) are not representable in
        // the current 64-byte GpuParticle schema. The billboard pass
        // renders camera-facing axis-aligned quads on the full texture page;
        // animated cards visually degrade to a single frame.
        p.size        = (float)size;
        // P1-2: atlasIndex carries the gos_TextureHandle cast to uint32.
        // Resolved to a raw GLuint at flush time by gos_GetGLTextureName().
        p.atlasIndex  = gosTexHandle;

        batcher.Emit(p);
    }
}

}  // namespace particles
}  // namespace mc2
