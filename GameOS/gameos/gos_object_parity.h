// GameOS/gameos/gos_object_parity.h
//
// Slice 2 (object-offload) — Stage 2.D: parity readback harness.
//
// Purpose: a default-off env-gated path that allocates a writable SSBO,
// binds it as binding=3 during the static-prop draw, has the shader write
// per-vertex lit ARGB into it, and reads the bytes back one frame later
// via the slice 1 batcher's existing fence ring. Stage 2.D.1 DISCARDS the
// bytes. Stage 2.D.1.1 adds slot-overflow accounting + harness cleanup.
// Stage 2.D.2 adds P3 dual-emit at first eligible frame post-mission-start
// + bytewise compare of GPU per-vertex lit ARGB against CPU
// listOfTriangles[].aRGBLight[i].
//
// Sidecar separation rationale: matches the gos_terrain_indirect{,.cpp}
// precedent — keeps the parity surface area out of the slice 1 batcher
// hot path. The batcher calls:
//   IsParityCheckEnabled / IsParityTraceEnabled
//   EnsureParityOutputSSBO / SlotBytes / RingFrames / ReleaseParityOutputSSBO
//   Counters_AddVerticesWrittenThisFrame / Counters_AddReadbackBytesThisFrame
//   Counters_AddSlotOverflowThisFrame  (2.D.1.1)
//   IsDualEmitArmed / IsDualEmitReadyForSlot  (2.D.2)
//   RecordInstanceSnapshot / RecordParityTypeRange  (2.D.2)
//   AdvanceDualEmitToWaiting / AdvanceDualEmitToDone  (2.D.2)
//   CompareAndReport  (2.D.2)
//   ParityPrintMismatch / ParityFrameTick
//   OnMissionLoad  (2.D.2 — mission re-arm)
//
// Default-off contract (per advisor, load-bearing): when
// MC2_OBJECT_PARITY_CHECK is unset or "0" — NO SSBO allocation, NO
// bind, NO readback, NO counter ticks, NO printf, ZERO additional GL
// calls beyond what slice 1 already does.
//
// Schema rule (per advisor): the parity SSBO is an OUTPUT diagnostic
// buffer, NOT part of the lighting input schema. Stage 2.C-locked
// GpuStaticPropInstance, TG_HWLightsData, ObjectLights, the per-vertex
// VBO layout, and per-type SSBO are NOT modified by this stage.
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

// Thin GL-free header for mclib/ callers. Pull it in here so that
// gos_object_parity.h is a strict superset — TUs that include this header
// also get IsParityCheckEnabled / IsDualEmitArmed without having to include
// the query header separately.
#include "gos_object_parity_query.h"

namespace gos_object_parity {

// ---------------------------------------------------------------------------
// Env-gate readers (boot-time once; cached in function-scope statics so
// subsequent calls are a single branch-predicted bool load).
//
// All Stage 2.D gates default OFF — only literal "1" turns them on.
// IsParityCheckEnabled() and IsDualEmitArmed() are declared in
// gos_object_parity_query.h (the thin GL-free header included above).
// ---------------------------------------------------------------------------
// IsParityCheckEnabled() — in gos_object_parity_query.h
// IsDualEmitArmed()      — in gos_object_parity_query.h
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
// Stage 2.D.2 additions:
//   - compared: total (type,instance,vertex) triples compared across all
//     dual-emit + future P1 compares.
//   - passed: triples where ALL channels were within ±2 LSB.
//   - mismatched: triples where ANY channel exceeded ±2 LSB.
// ---------------------------------------------------------------------------
void Counters_AddVerticesWrittenThisFrame(uint64_t n);
void Counters_AddReadbackBytesThisFrame(uint64_t n);
void Counters_AddSlotOverflowThisFrame(uint32_t typeId,
                                        size_t   needBytes,
                                        size_t   budgetBytes);

uint64_t Counters_GetVerticesWrittenTotal();
uint64_t Counters_GetReadbackBytesTotal();
uint64_t Counters_GetSlotOverflowsTotal();

// Stage 2.D.2 counters (total across all dual-emit + P1 compares).
uint64_t Counters_GetComparedTotal();
uint64_t Counters_GetPassedTotal();
uint64_t Counters_GetMismatchedTotal();
void     Counters_AddCompared(uint64_t n);
void     Counters_AddPassed(uint64_t n);
void     Counters_AddMismatched(uint64_t n);

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
// Stage 2.D.2 — Dual-emit state machine.
//
// The dual-emit latch fires once per mission at the first eligible frame
// post-mission-start:
//
//   Armed          — initial state after OnMissionLoad(); dual-emit hooks
//                    and snapshot collection are active this frame.
//   WaitingForReadback — frame N's GPU draw is committed (fence inserted);
//                    readback handshake will compare frame N's parity bytes
//                    against the snapshot on frame N+1.
//   Done           — compare complete; dual-emit and snapshot are dormant
//                    for the rest of this mission.
//
// Transitions (all called from GpuStaticPropBatcher):
//   OnMissionLoad()              → state = Armed
//   AdvanceDualEmitToWaiting()   → state = WaitingForReadback  (after fence)
//   AdvanceDualEmitToDone()      → state = Done  (after compare on frame N+1)
//
// Default-off: all these calls are no-ops when IsParityCheckEnabled()==false.
// ---------------------------------------------------------------------------
void OnMissionLoad();
bool IsDualEmitArmed();
// Returns true iff the latch is WaitingForReadback AND slotNow matches the
// ring slot that the Armed frame wrote to. This ensures the compare only runs
// when the ring has come back to the exact slot whose fence was waited (the
// GPU-complete guarantee). slotNow = current s_frameSlot in the batcher.
bool IsDualEmitReadyForSlot(uint32_t slotNow);
// slotUsed: the s_frameSlot value on which the Armed frame's parity writes
// landed. Stored so IsDualEmitReadyForSlot can match against it.
void AdvanceDualEmitToWaiting(uint32_t slotUsed);
void AdvanceDualEmitToDone();

// ---------------------------------------------------------------------------
// Stage 2.D.2 — Per-instance CPU snapshot collection and type-range record.
//
// RecordInstanceSnapshot:
//   Called from GpuStaticPropBatcher::submit() on the dual-emit frame (state
//   == Armed) for each SHAPE_NODE leaf that was accepted. Appends one
//   instance worth of per-vertex CPU aRGBLight to the per-type snapshot
//   buffer. Instance order matches the order of GpuStaticPropInstance entries
//   in the per-type PerTypeBucket — which is the same order as the parity
//   SSBO's gl_InstanceID mapping, so no secondary indexing is needed.
//
//   perVertexCpuARGB[0..vertexCount-1]: each entry is the aRGBLight value
//   for vertex v, built by caller from listOfTriangles[j].aRGBLight[i] via
//   the triangle-to-vertex mapping (typeShape->listOfTypeTriangles[j].Vertices[i]).
//   Format: (A<<24)|(R<<16)|(G<<8)|B, matching the GPU parity SSBO packing.
//
// RecordParityTypeRange:
//   Called from the per-type draw loop in GpuStaticPropBatcher::flush() on
//   the dual-emit frame, after the parity bind succeeds. Records which byte
//   range in the per-slot parity SSBO corresponds to this type's draw.
//   cursorOffset is the byte offset within the slot (same cursor value that
//   was passed to glBindBufferRange's offset parameter).
// ---------------------------------------------------------------------------
void RecordInstanceSnapshot(uint32_t typeId,
                             const uint32_t* perVertexCpuARGB,
                             uint32_t vertexCount);

void RecordParityTypeRange(uint32_t typeId,
                           size_t   cursorOffset,
                           uint32_t instanceCount,
                           uint32_t vertexCount);

// ---------------------------------------------------------------------------
// Stage 2.D.2 — Compare readback bytes against CPU snapshot.
//
// Called from GpuStaticPropBatcher::flush() at the top of the readback
// handshake when IsDualEmitReadyForSlot(s_frameSlot)==true. Iterates every
// (type, instance, vertex) entry in the snapshot, decodes the matching GPU
// uint32 from readbackBytes (indexed via the recorded cursor ranges), and
// applies the ±2 LSB per-channel ULP tolerance from the spec.
//
// On mismatch, calls ParityPrintMismatch (throttled 16/frame).
// On completion, does NOT advance state — caller must call
// AdvanceDualEmitToDone() after this returns.
//
// readbackBytes / readbackSize: the contiguous byte block read from the
// parity SSBO slot via glGetBufferSubData in the batcher's readback site.
// frameNum: the global frame counter, passed to ParityPrintMismatch.
// ---------------------------------------------------------------------------
void CompareAndReport(const uint8_t* readbackBytes,
                      size_t         readbackSize,
                      unsigned       frameNum);

// ---------------------------------------------------------------------------
// Mismatch printer + 600-frame summary.
//
// Stage 2.D.1 lands the printer SKELETON. Stage 2.D.2 wires it to
// CompareAndReport and extends the summary with compared/passed/mismatched/
// skipped_allowed_late_reg fields.
//
// Schema (grep-friendly; matches the spec on line 327 of the design doc):
//   [OBJECT_PARITY v1] event=lighting_mismatch frame=N typeId=T inst=I vert=V
//                      cpu=0xAARRGGBB gpu=0xAARRGGBB
//   [OBJECT_PARITY v1] event=summary frames=N vertices_written=N readback_bytes=N
//                      compared=N passed=N mismatched=N slot_overflows=N
//
// Throttle: 16 mismatch lines per frame, reset by ParityFrameTick(). Same
// budget as terrain-indirect's printer.
//
// Note: the spec lists the mismatch fields as "actor=X tri=Y corner=Z" but
// since the compare maps at (type, instance, vertex) granularity — not at
// actor-name / tri / corner — we use typeId/inst/vert. This is a minor
// format deviation that was not anticipated by the spec; documented in the
// commit body.
// ---------------------------------------------------------------------------
void ParityPrintMismatch(int frame, const char* actor, int tri, int corner,
                         uint32_t cpuARGB, uint32_t gpuARGB);

// Variant used by CompareAndReport (no actor name available at compare time).
void ParityPrintMismatchByIndex(unsigned frame,
                                uint32_t typeId,
                                uint32_t instIdx,
                                uint32_t vertIdx,
                                uint32_t cpuARGB,
                                uint32_t gpuARGB);

// Call once per frame at the close of GpuStaticPropBatcher::flush(). Resets
// the per-frame mismatch throttle and emits the summary line every 600
// frames. Internally a no-op when the env-gate is off — cheap to call
// unconditionally (a single branch-predicted bool load).
void ParityFrameTick();

}  // namespace gos_object_parity
