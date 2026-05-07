#pragma once
// gpu_cull_readback.h — Track C, Slice C2: async readback ring buffer.
//
// 3-slot persistent-mapped read-back ring buffer + per-frame glFenceSync.
// Three-tier non-blocking tryConsume: T1 (N-1), T2 (N-2), T3 (conservative).
// Tracy plots for GPU.VisibleCount.GPU and GPU.VisibleCount.CPU.
//
// Killswitch: MC2_GPU_CULL_READBACK env var; default 0.
// Force-not-ready test: MC2_GPU_CULL_FORCE_FENCE_NOT_READY env var.

#include <cstdint>
#include <GL/glew.h>

namespace gpu_cull {

// Init: call at mission load after compute_init() and after maxActors is known.
// maxActors: same value passed to substrate_init().
bool readback_init(uint32_t maxActors);

// Shutdown: call at mission teardown.
void readback_shutdown();

// Returns true if readback is enabled (MC2_GPU_CULL_READBACK env var, default 0).
bool readback_isEnabled();

// Frame end: barrier + GPU→staging copy + GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT + glFenceSync.
// Call AFTER all GPU commands for the frame are issued (post compute_dispatch).
// Handles GL_SHADER_STORAGE_BARRIER_BIT internally; caller does not need to place it.
void readback_frameEnd(uint32_t frameIndex);

// Frame begin: non-blocking tryConsume poll.
// Call at the start of compute_dispatch() before dispatching.
// Updates internal last-good-slot on success.
enum class ReadbackTier { Tier1_NMinusOne, Tier2_NMinusTwo, Tier3_Conservative };
ReadbackTier readback_tryConsume();

// Returns the GL buffer name for the GPU-side SSBO.
// Compute shader binds this at binding 14 for atomic writes.
// CPU never reads from this buffer directly (see readback_getStagingBuf()).
GLuint readback_getSsboBuf();

// Returns the GL buffer name for the CPU-readable staging buffer.
// compute_dispatch() copies the current GPU slot here after each dispatch.
// Used by compute_dispatch() for diagnostic reads (glGetNamedBufferSubData).
GLuint readback_getStagingBuf();

// Returns the byte offset of the current write slot within the readback SSBO.
// Binding range: [offset, offset + slotBytes).
GLintptr readback_getCurrentSlotOffset();
GLsizeiptr readback_getSlotBytes();

// Accessor: returns visible actor count from last good slot (Tier1 or Tier2).
// Returns UINT32_MAX if no valid readback (Tier3 / never consumed).
uint32_t readback_getLastGoodVisibleCount();

// Zero rb_visibleCount in the current GPU SSBO slot via glClearNamedBufferSubData.
// Call before each dispatch so the GPU's atomicAdd accumulates from 0.
void readback_zeroCurrentSlotVisibleCount();

// Startup selftest. Hard-fails via STOP() if any tier path is broken.
void readback_selftest();

} // namespace gpu_cull
