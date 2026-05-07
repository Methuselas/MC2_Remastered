#pragma once
// gpu_cull_compute.h — Track C: GPU cull compute pipeline.
//
// C1a (shadow mode): compute shader runs every frame alongside CPU path.
//   CPU still renders normally — zero change to rendering output.
//   Parity summary: [GPU_CULL v1] event=parity_summary every 600 frames.
//   Killswitch: MC2_GPU_CULL=0 (default) — nothing happens.
//   Enable:     MC2_GPU_CULL=1 + MC2_GPU_CULL_SUBSTRATE=1
//
// C1b (GPU render authority): extends C1a to drive static prop indirect draw.
//   The cull dispatch writes visibleIds[] + perBucketCount[], the patch dispatch
//   copies counts into DrawElementsIndirectCommand.instanceCount, and the block
//   rollup ORs per-actor visibility into per-block flags.
//   GpuStaticPropBatcher::flush() replaces its per-type loop with
//   glMultiDrawElementsIndirect when compute_isEnabled().
//   Killswitch: MC2_GPU_CULL=0 (default until C3).

#include <cstdint>
#include <GL/glew.h>

namespace gpu_cull {

// Initialise the compute pipeline: GL version probe, shader compile, SSBO alloc.
// Call once at mission load, after substrate_init() and after GL context is up.
// Hard-fails via STOP() if shader compile or SSBO allocation fails.
bool   compute_init();

// Release all GL resources. Call at mission teardown.
void   compute_shutdown();

// Returns true if C1 compute is enabled (MC2_GPU_CULL env var set, default 0).
// Lazy-init env probe — safe to call at any time including before init().
bool   compute_isEnabled();

// Per-frame: read the current substrate SSBO, run the cull compute shader,
// (C1b) run patch and rollup dispatches, (C1b) ensure indirect buffer is ready.
// Must be called AFTER substrate_flushUpload() (i.e. after ObjectManager->update()).
// Skips the frame silently if gos_GetTerrainMVPMat4() returns nullptr
// (terrain not yet rendered this frame).
void   compute_dispatch();

// Per-frame (C1a only): read back debug SSBO counts, compare with CPU prevVisibilityBit,
// emit [GPU_CULL v1] event=parity_summary every 600 frames.
// Call immediately after compute_dispatch().
// In C1b mode this is a no-op (parity replaced by overflow check).
void   compute_emitParitySummary();

// C1b: Build the DrawElementsIndirectCommand buffer at mission load.
// Called from compute_init() after static props are registered.
// typeCount: number of registered types in GpuStaticPropBatcher.
// Must be called AFTER GpuStaticPropBatcher::finalizeGeometry().
// typeCount = 0 is valid (no static props in mission — skips alloc).
bool   compute_buildIndirectBuffer(uint32_t typeCount);

// C1b: Returns the GL buffer name for the DrawElementsIndirectCommand buffer.
// Valid only after compute_buildIndirectBuffer() succeeds. Returns 0 otherwise.
GLuint compute_getIndirectCmdBuf();

// C1b: Returns the number of indirect draw buckets (= typeCount at build time).
uint32_t compute_getBucketCount();

// C1b: Per-type draw info accessor used by compute_buildIndirectBuffer().
// Reads the per-type geometry fields from the static-prop batcher's type table.
// typeID:       type index (0..typeCount-1)
// outIndexCount: number of indices in this type's first packet (summed across packets)
// outFirstIndex: byte offset / sizeof(index) into shared IBO (first packet only)
// outBaseVertex: base vertex offset into shared VBO (first packet only)
// outInstanceCap: per-frame instance capacity for this bucket
// Returns false if typeID is out of range or batcher is not finalized.
bool   compute_getTypeBucketInfo(uint32_t typeID,
                                  uint32_t* outIndexCount,
                                  uint32_t* outFirstIndex,
                                  int32_t*  outBaseVertex,
                                  uint32_t* outInstanceCap);

} // namespace gpu_cull
