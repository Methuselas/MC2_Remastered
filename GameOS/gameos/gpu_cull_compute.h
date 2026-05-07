#pragma once
// gpu_cull_compute.h — Track C slice C1a: GPU visibility mirror (diagnostic mode).
//
// C1a is a shadow/diagnostic mode. The compute shader runs every frame alongside
// the CPU render path. CPU still renders normally — zero change to rendering output.
// The deliverable is a per-frame parity log comparing GPU visibility decisions
// vs CPU inView (prevVisibilityBit in the substrate records).
//
// C1a is NOT C1b: no indirect draw consumption, no DrawElementsIndirectCommand writes.
// Killswitch: MC2_GPU_CULL=0 (default) disables all C1a work.

#include <cstdint>

namespace gpu_cull {

// Initialise the compute pipeline: GL version probe, shader compile, SSBO alloc.
// Call once at mission load, after substrate_init() and after GL context is up.
// Hard-fails via STOP() if shader compile or SSBO allocation fails.
bool   compute_init();

// Release all C1a GL resources. Call at mission teardown.
void   compute_shutdown();

// Returns true if C1a compute is enabled (MC2_GPU_CULL env var set, default 0).
// Lazy-init env probe — safe to call at any time including before init().
bool   compute_isEnabled();

// Per-frame: read the current substrate SSBO, run the cull compute shader,
// write the debug SSBO (binding 9).
// Must be called AFTER substrate_flushUpload() (i.e. after ObjectManager->update()).
// Skips the frame silently if gos_GetTerrainMVPMat4() returns nullptr
// (terrain not yet rendered this frame).
void   compute_dispatch();

// Per-frame: read back debug SSBO counts, compare with CPU prevVisibilityBit,
// emit [GPU_CULL v1] event=parity_summary every 600 frames.
// Call immediately after compute_dispatch().
void   compute_emitParitySummary();

} // namespace gpu_cull
