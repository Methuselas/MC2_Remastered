//==========================================================================//
// File:    spawn_card.cpp                                                   //
// Contents: mc2::particles::SpawnCard implementation.                       //
//           Plan v6 §5.4 B1 Stage 2' C3.                                    //
//                                                                           //
// Spawn-event semantics derived from mclib/gosfx/card.cpp:                  //
//                                                                           //
//   - Card is a Singleton effect: one Card instance == exactly one          //
//     particle for its entire lifetime (m_cardCount = 1, see card.cpp:295). //
//   - Legacy per-frame chain: Execute() samples spec->m_halfHeight /        //
//     m_aspectRatio / m_UOffset etc at the instance's m_age (card.cpp:380-  //
//     456), Draw() emits one quad through the MLR clipper (card.cpp:474).   //
//   - Lifetime sourced from the Effect base spec->m_lifeSpan (sampled at    //
//     m_seed per Effect::Start; see effect.cpp:461).                        //
//   - Color sourced from Singleton spec curves m_red/m_green/m_blue/m_alpha //
//     (see singleton.cpp:241-244 ComputeValue(m_age,m_seed)).               //
//   - Size sourced from Card spec m_halfHeight (Card::Start at card.cpp:349 //
//     uses this as the world-space half-extent; m_radius = sqrt(hx^2+hy^2)  //
//     at card.cpp:351).                                                     //
//                                                                           //
// In the new path the GPU shader owns animation (age advance, billboard     //
// alignment, projection). The CPU emits exactly ONE record at spawn time    //
// containing the initial state; the shader extrapolates age from the spawn //
// timestamp baked into the record (Stage 2' lifecycle, see plan §5.4).     //
//                                                                           //
// CPU projection invariant: this file MUST NOT include or reference         //
// cameraToClip / projectZ / worldToClipMatrix / projectForObjectAdmission / //
// projectForEffectAdmission. CI gate enforced by                            //
// scripts/check-particles-no-cpu-projection.sh.                             //
//===========================================================================//

#include "gosfx/gosfxheaders.hpp"
#include "gosfx/card.hpp"
#include "spawn_card.h"
#include "batcher.h"
#include "fx_trace/fx_trace.h"

#include <stuff/linearmatrix.hpp>
#include <stuff/point3d.hpp>

namespace mc2 {
namespace particles {

void SpawnCard(const gosFX::Card__Specification* spec,
               const Stuff::LinearMatrix4D*      parentToWorld,
               float                              spawnSeed)
{
    if (!spec) {
        return;
    }

    // FX_TRACE_DRAW key schema matches the legacy gosFX::Effect::Draw entry
    // (effect.cpp:697) — ONE event per spawn (Card is one-particle so the
    // per-Effect-Draw and per-particle keys coincide).
    FX_TRACE_DRAW(spec->m_name);

    // Cheap early-out before sampling curves: when MC2_GPU_PARTICLES=0
    // the new path is dormant and we still want the FX_TRACE_DRAW count
    // (it is the per-spec invocation oracle, not the GPU activation flag).
    if (!Batcher::is_enabled()) {
        return;
    }

    // Sample curves at age=0 / spawnSeed. Card legacy samples at instance
    // m_age during Execute(); at SPAWN time m_age is 0 (Effect::Start
    // initializes m_age=0 then Singleton::Start runs Execute, see Card::
    // Start at card.cpp:341). The GPU shader advances age per-frame from
    // the baked spawn timestamp, so the CPU records the t=0 state.
    const Stuff::Scalar age = 0.0f;
    const Stuff::Scalar seed = spawnSeed;

    // Const_cast because the legacy curve ComputeValue() methods are not
    // const-correct (FCurve internals mutate cache state). SpecLibrary
    // owns the spec lifetime, so the cast is safe for read-only sampling.
    gosFX::Card__Specification* mut_spec =
        const_cast<gosFX::Card__Specification*>(spec);

    const Stuff::Scalar halfY = mut_spec->m_halfHeight.ComputeValue(age, seed);
    const Stuff::Scalar halfX =
        halfY * mut_spec->m_aspectRatio.ComputeValue(age, seed);
    // Match Card::Start radius idiom (card.cpp:351). For a square-ish card
    // m_radius is the diagonal half-extent; the GPU billboard pass uses the
    // single 'size' field, so we collapse the X/Y pair to the bounding
    // radius (worst-case footprint preserves visibility parity).
    const Stuff::Scalar radius = Stuff::Sqrt(halfX * halfX + halfY * halfY);

    const Stuff::Scalar r = mut_spec->m_red  .ComputeValue(age, seed);
    const Stuff::Scalar g = mut_spec->m_green.ComputeValue(age, seed);
    const Stuff::Scalar b = mut_spec->m_blue .ComputeValue(age, seed);
    const Stuff::Scalar a = mut_spec->m_alpha.ComputeValue(age, seed);

    // Effect base spec lifetime (effect.cpp:461 uses ComputeValue(m_seed,
    // 0.0f) — the lifeSpan curve takes seed as its primary axis).
    const Stuff::Scalar lifetime =
        mut_spec->m_lifeSpan.ComputeValue(seed, 0.0f);

    // World-space spawn position is parent transform * local origin. Card
    // local origin is (0,0,0) in effect space (card.cpp:442-456 builds
    // vertices around the origin). Same idiom as card.cpp:503 (Point3D
    // card_in_world(local_to_world)) — extracts the translation from the
    // affine matrix.
    Stuff::Point3D worldPos(0.0f, 0.0f, 0.0f);
    if (parentToWorld) {
        worldPos = Stuff::Point3D(*parentToWorld);
    }

    GpuParticle p = {};
    p.position[0] = worldPos.x;
    p.position[1] = worldPos.y;
    p.position[2] = worldPos.z;
    p.color[0]    = r;
    p.color[1]    = g;
    p.color[2]    = b;
    p.color[3]    = a;
    // Card has no per-particle velocity (it's a billboard locked to the
    // parent transform in the legacy path). Leave velocity[] at zero.
    p.lifetime    = (lifetime > 0.0f) ? (float)lifetime : 1.0f;
    p.age         = 0.0f;
    p.size        = (float)radius;
    // Stage 2' C3: atlasIndex stays 0 (single-atlas path). Atlas paging
    // arrives with the animated-UV slice (Card m_animated / m_index curve)
    // — filed as B2 polish debt; current GPU shader samples atlas page 0.
    p.atlasIndex  = 0u;

    Batcher::Instance().Emit(p);
}

}  // namespace particles
}  // namespace mc2
