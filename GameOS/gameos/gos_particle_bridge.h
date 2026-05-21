// GameOS/gameos/gos_particle_bridge.h
//
// Bridge entry point for the GPU particle batcher. The CPU producer-side
// API lives in mclib/particles/ (no GL dependency); this bridge owns the
// SSBO + VAO + shader program + draw call.
//
// Plan v5 §5.4 B1 Stage 1' Commit 1 — stub registered, no GL work yet.
// Stage 1' Commit 4 wires the actual flush.

#pragma once

namespace mc2 { namespace particles { struct GpuParticle; } }

// C linkage so mclib/particles/batcher.cpp can forward-declare without
// pulling C++ name-mangling expectations from GameOS into mclib.
extern "C" void gos_particle_bridge_flush(const mc2::particles::GpuParticle* records,
                                          unsigned int                       count);
