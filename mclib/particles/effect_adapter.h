//==========================================================================//
// File:    effect_adapter.h                                                 //
// Contents: mc2::particles::EffectAdapter - a gosFX::Effect shell that      //
//           routes Start() to the new mc2::particles::Spawn() dispatcher    //
//           instead of constructing a legacy gosFX primitive subclass.      //
//                                                                           //
//           Per integrated plan v6 §5.4 B1 Stage 2' C7-revised.             //
//                                                                           //
//           Why an adapter at the EffectLibrary::MakeEffect boundary:       //
//           the original C7 producer-flip ("rewrite every Find+MakeEffect+  //
//           lifecycle site") was reverted (link failure exposed that plan   //
//           v6 §2.7 underspecified the producer-side surface area; each     //
//           site is Check_Object + Find + MakeEffect + lifecycle, not just  //
//           Find). The adapter strategy keeps the gosFX::Effect public API  //
//           contract intact - producers see a base-class Effect pointer     //
//           that responds to Start / Execute / Draw / HasFinished - while   //
//           the actual work routes to particles::Spawn under the env gate.  //
//                                                                           //
//           Lifecycle (when MC2_GPU_PARTICLES=1):                           //
//             Start()        -> particles::Spawn(spec, parentToWorld, seed) //
//             Execute(info)  -> base impl runs (advances m_age so           //
//                               HasFinished can retire the adapter); no GPU //
//                               work, no MLR draw.                          //
//             Draw(info)     -> no-op; particles draw via the post-         //
//                               renderLists batcher hook (B1 Stage 1').     //
//             HasFinished()  -> base impl (age >= 1.0 retires).             //
//                                                                           //
//           When MC2_GPU_PARTICLES=0: EffectAdapter is never constructed -  //
//           EffectLibrary::MakeEffect takes the legacy branch that returns  //
//           a concrete gosFX primitive subclass.                            //
//===========================================================================//

#pragma once

#include "gosfx/effect.hpp"

namespace mc2 {
namespace particles {

class EffectAdapter : public gosFX::Effect
{
public:
    // Constructor matches the gosFX::Effect::Factory call shape so it can
    // be invoked from EffectLibrary::MakeEffect with the same (spec, flags)
    // arguments the legacy factories take. Forwards to the protected base
    // Effect ctor with Effect::DefaultData so the Node base sees a
    // registered ClassData (no per-adapter class registration needed -
    // we deliberately re-use the base Effect identity).
    EffectAdapter(gosFX::Effect::Specification* spec, unsigned flags);

    ~EffectAdapter();

    // Routes to particles::Spawn() after running the base Start() so that
    // m_seed / m_age / m_localToWorld are populated identically to the
    // legacy path (the seed we pass to Spawn is the one Effect::Start
    // resolved, matching legacy gosFX seed semantics).
    void Start(ExecuteInfo* info) override;

    // Draw is a no-op: particles render via the post-renderLists batcher
    // hook (B1 Stage 1' Commit 3), NOT through gosFX::Effect::Draw.
    void Draw(DrawInfo* info) override;

    // Execute() and HasFinished() are inherited unchanged. The base impl
    // advances m_age (Execute) and reports retirement at age >= 1.0
    // (HasFinished); both are needed so the gosFX-side owners (call sites
    // that loop until HasFinished() returns true) retire the adapter on
    // the same cadence as a legacy Effect.
};

}  // namespace particles
}  // namespace mc2
