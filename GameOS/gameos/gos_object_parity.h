// GameOS/gameos/gos_object_parity.h
//
// Slice 2 (object-offload) — Stage 2.D: parity readback harness.
//
// Purpose: a default-off env-gated path that allocates a writable SSBO,
// binds it as binding=3 during the static-prop draw, has the shader write
// per-vertex lit ARGB into it, and reads the bytes back one frame later
// via the slice 1 batcher's existing fence ring. Stage 2.D.1 DISCARDS the
// bytes — there is no compare logic yet, no CPU recompute, no actor
// selection. Stage 2.D.1.1 adds slot-overflow accounting + harness cleanup.
// The acceptance gate is "the pipeline runs without crash or stall under
// MC2_OBJECT_PARITY_CHECK=1; +0 destroys delta vs unset."
//
// 2.D.2 (P3 dual-emit) and 2.D.3 (P1 sampled steady state) will plug in
// the comparison logic and per-actor accounting on top of this skeleton.
//
// Sidecar separation rationale: matches the gos_terrain_indirect{,.cpp}
// precedent — keeps the parity surface area out of the slice 1 batcher
// hot path. The batcher only calls:
//   IsParityCheckEnabled / IsParityTraceEnabled
//   EnsureParityOutputSSBO / SlotBytes / RingFrames / ReleaseParityOutputSSBO
//   Counters_AddVerticesWrittenThisFrame / Counters_AddReadbackBytesThisFrame
//   Counters_AddSlotOverflowThisFrame  (2.D.1.1)
//   ParityPrintMismatch (skeleton; called in 2.D.2+)
//   ParityFrameTick
//
// Default-off contract (per advisor, load-bearing): when
// MC2_OBJECT_PARITY_CHECK is unset or "0" — NO SSBO allocation, NO
// bind, NO readback, NO counter ticks, NO printf, ZERO additional GL
// calls beyond what slice 1 already does. Verified empirically: tier1
// 5/5 with the env unset must show +0 destroys delta and no Tracy
// regression vs the pre-2.D baseline.
//
// Schema rule (per advisor): the parity SSBO is an OUTPUT diagnostic
// buffer, NOT part of the lighting input schema. The Stage 2.C-locked
// per-vertex VBO layout, GpuStaticPropInstance, TG_HWLightsData,
// ObjectLights, and per-type SSBO are NOT modified by this stage.
//
// Hand-off prompt: docs/superpowers/specs/2026-05-02-object-offload-slice2-stage2d-handoff-prompt.md
// Design spec:    docs/superpowers/specs/2026-05-02-object-offload-slice2-design.md
//                 (search "Stage 2.D — Parity instrumentation" + "Compare-target caveat")

#pragma once

#include <cstddef>
#include <cstdint>

// GL/glew.h must come before any other GL header. Including it here is safe
// because every TU that includes this sidecar already links against GLEW.
// (The prior typedef was insufficient once callers needed GL_DYNAMIC_STORAGE_BIT
// and other GL constants; a bare typedef exposes only the name, not the consts.)
#include <GL/glew.h>

namespace gos_object_parity {

// ---------------------------------------------------------------------------
// Env-gate readers (boot-time once; cached in function-scope statics so
// subsequent calls are a single branch-predicted bool load).
//
// All Stage 2.D gates default OFF — only literal "1" turns them on.
// ---------------------------------------------------------------------------
bool IsParityCheckEnabled();   // MC2_OBJECT_PARITY_CHECK
bool IsParityTraceEnabled();   // MC2_OBJECT_PARITY_TRACE — gates
                                // [OBJECT_PARITY v1] event=... lifecycle prints

// ---------------------------------------------------------------------------
// Counter Add/Get (used by the static-prop batcher draw site to record what
// the harness exercised this frame). Counters are zero-initialized statics
// that never tick when the env-gate is off — callers should still gate the
// Add* calls themselves to avoid dead branches in the hot path.
//
// Stage 2.D.1 counter set:
//   - vertices_written: total uint32 entries the shader was authorized to
//     write into the parity SSBO this frame (sum of per-draw bound-range
//     element counts when u_parityWrite=1u).
//   - readback_bytes: total bytes glGetBufferSubData'd this frame.
//
// Stage 2.D.1.1 additions:
//   - slot_overflows: times a per-type draw was skipped because the parity
//     slot's byte budget was exhausted (the silent-skip branch). Counter
//     is per-frame; rolled into the 600-frame summary. Printf fires once
//     per (typeId) per process — see the "once-per-type" set in the .cpp.
//
// 2.D.2 / 2.D.3 will add compared/passed/mismatched/skipped_allowed_late_reg.
// ---------------------------------------------------------------------------
void Counters_AddVerticesWrittenThisFrame(uint64_t n);
void Counters_AddReadbackBytesThisFrame(uint64_t n);
void Counters_AddSlotOverflowThisFrame(uint32_t typeId,
                                        size_t   needBytes,
                                        size_t   budgetBytes);

uint64_t Counters_GetVerticesWrittenTotal();
uint64_t Counters_GetReadbackBytesTotal();
uint64_t Counters_GetSlotOverflowsTotal();

// Per-frame reset for slot_overflows (called from ParityFrameTick alongside
// the other per-frame resets). Not intended for external callers.
void Counters_ResetSlotOverflowsThisFrame();

// ---------------------------------------------------------------------------
// Parity-output SSBO lifecycle.
//
// EnsureParityOutputSSBO(bytesPerSlot):
//   - Returns 0 when IsParityCheckEnabled() is false (no allocation; the
//     caller should treat 0 as "skip the parity bind/uniform").
//   - On first call when parity is on, allocates a single GL buffer sized
//     RING_FRAMES * bytesPerSlot via glBufferStorage (DYNAMIC_STORAGE so
//     callers can glBindBufferRange + glGetBufferSubData against it).
//   - Subsequent calls: returns the existing handle. If a caller requests
//     a larger bytesPerSlot than what was allocated, the buffer is
//     re-created at the new size (rare; matches slice 1 ensureRingCapacity
//     pattern).
//
// SlotBytes() / RingFrames(): return the currently-allocated per-slot byte
// budget and the ring depth (matches slice 1's RING_FRAMES = 3). Callers
// (the static-prop batcher) compute per-slot offsets as
//   slotByteBase = currentFrameSlot * SlotBytes();
// and accumulate per-draw byte usage within the slot, capping at SlotBytes().
//
// ReleaseParityOutputSSBO(): free the buffer if allocated. Called nowhere
// in 2.D.1's draw path; provided for clean-shutdown wiring if the operator
// ever wants to disable mid-process. No-op when never allocated.
// ---------------------------------------------------------------------------
GLuint EnsureParityOutputSSBO(size_t bytesPerSlot);
size_t SlotBytes();
unsigned RingFrames();
void   ReleaseParityOutputSSBO();

// ---------------------------------------------------------------------------
// Mismatch printer + 600-frame summary.
//
// Stage 2.D.1 lands the printer SKELETON ONLY — no caller in 2.D.1's batcher
// edits invokes ParityPrintMismatch. It exists so the schema and throttling
// behavior are settled before 2.D.2 plugs in the actual byte comparison.
//
// Schema (grep-friendly; matches the spec on line 327 of the design doc):
//   [OBJECT_PARITY v1] event=lighting_mismatch frame=N actor=A tri=T corner=C
//                      cpu=0xAARRGGBB gpu=0xAARRGGBB
//   [OBJECT_PARITY v1] event=summary frames=N vertices_written=N readback_bytes=N
//                      [+ compared/passed/mismatched/skipped_allowed_late_reg
//                       fields added in 2.D.2/2.D.3]
//
// Throttle: 16 mismatch lines per frame, reset by ParityFrameTick(). Same
// budget as terrain-indirect's printer.
// ---------------------------------------------------------------------------
void ParityPrintMismatch(int frame, const char* actor, int tri, int corner,
                         uint32_t cpuARGB, uint32_t gpuARGB);

// Call once per frame at the close of GpuStaticPropBatcher::flush(). Resets
// the per-frame mismatch throttle and emits the summary line every 600
// frames. Internally a no-op when the env-gate is off — cheap to call
// unconditionally (a single branch-predicted bool load).
void ParityFrameTick();

}  // namespace gos_object_parity
