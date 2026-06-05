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

// Returns the number of records with prevVisibilityBit==1 accumulated write-side
// across both producers (dynamic actors + static props) for the current frame.
// Updated incrementally by substrate_writeRecord at each record submission; the
// final all-records value is valid after the last substrate_appendStaticPropRecord
// call for the frame (pre compute_dispatch). Resets in substrate_frameBegin.
uint32_t substrate_getCpuVisibleCount();

#if defined(MC2_SUBSTRATE_COUNT_PARITY)
void substrate_countParityCheck();   // proof-only; removed post-proof
#endif

// Returns the byte offset of the most recently flushed ring slot within the
// substrate SSBO. Use with glBindBufferRange() so the compute shader reads the
// correct slot (C1a). Returns 0 if not initialized.
GLintptr substrate_getCurrentSlotOffset();

// Returns the byte size of one ring slot (header + records).
// Use with glBindBufferRange() to bound the binding range. Returns 0 if not init.
GLsizeiptr substrate_getSlotBytes();

// C1b GPU authority flip: append additional records AFTER substrate_flushUpload().
//
// Called from GpuStaticPropRegistry::flush() (render phase, after objmgr::update's
// substrate_flushUpload()) to inject static prop records into the already-flushed slot
// before compute_dispatch() runs. Appends records to the persistent-mapped buffer and
// updates hdr->recordCount in-place (coherent write, visible to GPU immediately).
//
// Contract: call ONLY after substrate_flushUpload() and BEFORE compute_dispatch().
// compute_dispatch() re-reads hdr->recordCount from the mapped buffer at copy time
// (via glCopyBufferSubData then glGetBufferSubData on the staging SSBO), so it picks
// up the updated count. Safe because the fence placed by flushUpload() only gates
// GPU reads from the PREVIOUS ring-slot visit; CPU writes to the persistent-mapped
// buffer are always immediately coherent regardless of fence state.
void substrate_appendStaticPropRecord(const GpuActorRecord& rec);

// M1 FROZEN-STATIC-CULL-RECORDS: install the frozen static-prop cull-record
// prefix [0,S) (pool-ordered, so record-index == instance-pool slot). Call on a
// registry-generation dirty event. Copies into a CPU golden copy and marks all
// ring slots dirty; each slot is refilled from the golden copy lazily at its
// next substrate_frameBegin(), placing the static prefix before the per-frame
// dynamic records. Consumed only when MC2_GPU_CULL_STATIC_FROZEN_RECORDS is set.
void substrate_rebuildStaticPrefix(const GpuActorRecord* recs, uint32_t count);

// Returns the current record count for the active slot (after any appends).
// Used by compute_dispatch() to get the post-append count before dispatching.
uint32_t substrate_getCurrentRecordCount();

// C3: return the GpuActorRecord array for a given ring slot.
// slot: ring slot index (0..RING_FRAMES-1)
// outCount: populated with the record count for this slot (may be 0)
// Returns a pointer into the persistent-mapped SSBO (read-only on CPU).
// Safe to call after substrate_flushUpload() and while the ring is live.
// Returns nullptr if substrate is not initialized.
const GpuActorRecord* substrate_getSlotRecords(uint32_t slot, uint32_t* outCount);

// PERF-GPU-CULL-READBACK-ID-CACHE-1: CPU-side actor ID cache.
// Returns a pointer to the CPU-side actorId array for the given ring slot.
// Parallel to substrate_getSlotRecords — same index ordering, same record count.
// Reading from this array avoids PCIe/BAR reads from the persistent-mapped SSBO.
// outCount is populated with the record count for the slot (may be 0).
// Returns nullptr if substrate is not initialized.
const uint32_t* substrate_getCpuActorIds(uint32_t slot, uint32_t* outCount);

} // namespace gpu_cull
