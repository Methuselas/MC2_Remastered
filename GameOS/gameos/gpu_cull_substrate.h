#pragma once
#include <cstdint>
#include <GL/glew.h>
#include "gpu_cull_record.h"

namespace gpu_cull {

// Substrate init: call once at mission load, after GL context is up.
// maxActors: worst-case per-frame record count (size at MAX_OBJECTS + 25% headroom).
void substrate_init(uint32_t maxActors);

// Substrate shutdown: call at mission teardown. Frees GL buffers.
void substrate_shutdown();

// Per-frame begin: advance ring slot, wait on slot fence if needed.
void substrate_frameBegin();

// Per-frame submit: append one actor record to the current ring slot.
// If capacity is exceeded: clamps and emits [GPU_CULL v1] event=substrate_overflow.
void substrate_submitDynamicActor(const GpuActorRecord& rec);

// Per-frame flush: writes header.recordCount, places fence for current slot.
// Also emits [GPU_CULL v1] event=substrate_ready summary every 600 frames
// and immediately on first flush of a session.
void substrate_flushUpload();

// Returns the GL buffer name for the instance SSBO (for C1 binding).
GLuint substrate_getInstanceSsboName();

// Returns the SSBO binding point used for instance data.
uint32_t substrate_getInstanceSsboBindingPoint();

// Returns true if the substrate is enabled (MC2_GPU_CULL_SUBSTRATE=1 env var).
// Lazy env probe — safe to call at any time.
bool substrate_isEnabled();

} // namespace gpu_cull
