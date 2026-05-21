//==========================================================================//
// File:    gos_particle_bridge.cpp                                          //
// Contents: GameOS-side bridge for the GPU particle batcher. Owns the      //
//           SSBO (binding=14), the billboard VAO, and the GL state save/   //
//           restore around the draw.                                        //
//           Plan v5 §5.4 B1 Stage 1' Commit 1: stub (no GL work).          //
//           Plan v5 §5.4 B1 Stage 1' Commit 4: wired flush.                //
//===========================================================================//

#include "gos_particle_bridge.h"

#include "particles/spec.h"

extern "C" void gos_particle_bridge_flush(const mc2::particles::GpuParticle* /*records*/,
                                          unsigned int                       /*count*/) {
    // Stage 1' Commit 1: stub. Commit 4 replaces this with the SSBO upload
    // + billboard draw per gpu_direct_renderer_bringup_checklist.md.
}
