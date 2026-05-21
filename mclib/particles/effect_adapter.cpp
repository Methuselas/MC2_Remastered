//==========================================================================//
// File:    effect_adapter.cpp                                               //
// Contents: mc2::particles::EffectAdapter implementation.                   //
//           Plan v6 §5.4 B1 Stage 2' C7-revised.                            //
//===========================================================================//

#include "gosfx/gosfxheaders.hpp"
#include "effect_adapter.h"
#include "spawn.h"

namespace mc2 {
namespace particles {

EffectAdapter::EffectAdapter(gosFX::Effect::Specification* spec, unsigned flags)
    : gosFX::Effect(gosFX::Effect::DefaultData, spec, flags)
{
    // gosFX::Effect base ctor handles the heap-check / spec wiring /
    // m_localToParent identity init. No adapter-specific construction.
}

EffectAdapter::~EffectAdapter()
{
    // Base ~Effect deletes any adopted children. Adapter holds no extra
    // state.
}

void EffectAdapter::Start(ExecuteInfo* info)
{
    // Run the base Start first so m_seed (random fraction or override),
    // m_age (0 or info->m_age), m_ageRate (derived from m_lifeSpan), and
    // m_localToWorld (m_localToParent * info->m_parentToWorld) all settle
    // to identically-legacy values. Spawn then consumes the resolved
    // m_seed and m_localToWorld.
    gosFX::Effect::Start(info);

    // Dispatch to the per-primitive Spawn entry point. Unknown / deferred
    // ClassIDs (Pert / Shape / Debris / EffectCloud) return false here;
    // visual gap is acknowledged per Stage 0' recon (tier1 stock content
    // never spawns these types) and filed as B2 polish debt.
    (void)Spawn(m_specification, &m_localToWorld, (float)m_seed);
}

void EffectAdapter::Draw(DrawInfo* /*info*/)
{
    // Intentional no-op. Particles draw via the post-renderLists batcher
    // hook (gos_particle_bridge_flush), NOT through gosFX::Effect::Draw.
    // Deliberately NOT calling FX_TRACE_DRAW here: the spawn-event trace
    // (emitted from SpawnCard / SpawnPoint / SpawnShard / SpawnTube) is
    // the per-spec invocation oracle for the new path; mirroring the
    // legacy per-frame draw trace would double-count under any future
    // diff-self comparison.
}

}  // namespace particles
}  // namespace mc2
