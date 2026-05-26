# DrawPacket v6 — Architecture and Scope Document

**Date:** 2026-05-26
**Status:** ARCHITECTURE / SCOPE (pre-spec; not executor-ready)
**Depends on:** v5 shipped and confirmed correct

---

## 1. Goal

Make `DrawPacket` dispatch the **primary, default-ON** path for all static-prop
rendering. v6 replaces both the legacy per-type draw loop and the coalesce
`glMultiDrawElementsIndirect` with a canonical loop driven by
`RenderCore::DrawPacket[] + StaticPropDispatchMeta[]`.

At the end of v6:
- The env gate is removed (or flipped to a kill-switch).
- `flush()` has a single dispatch loop consuming `DrawPacket[]`.
- The legacy per-type loop (`s_typeRanges` / `s_bucketsByType` walk) is retired
  from `flush()`. (It may survive in `flushShadow()` — see §7.)
- `batcher_setOpaqueDispatchCandidates()` / `StaticPropOpaquePacketView` are retired.

---

## 2. Architectural Principle

**DrawPacket stays generic; StaticPropDispatchMeta is the batcher-private sidecar.**

`RenderCore::DrawPacket` must not grow batcher-private fields. Instead, a
parallel `StaticPropDispatchMeta[]` array is produced by the emitter at the
same time as `DrawPacket[]`. Both arrays are indexed in lockstep; their lengths
equal the emitted candidate count.

This separation preserves `RenderCore::DrawPacket` as the cross-cutting render
abstraction (shared with mechs, water, terrain in future arcs), while allowing
the batcher to carry its own dispatch metadata without polluting the generic type.

---

## 3. `StaticPropDispatchMeta` Struct (DECIDED — subject to v5 learnings)

```cpp
// GameOS/gameos/gos_static_prop_batcher.h  (or draw_packet_emitter.h)
struct StaticPropDispatchMeta {
    uint32_t sortedSlot;          // index into s_sortedPacketOrder[] — for s_baseInstanceByCmdSsbo lookup
    uint32_t group;               // 0=alpha-OFF (s_texArrayOff), 1=alpha-ON (s_texArrayOn)
    uint32_t instanceCount;       // CPU-snapshot count: s_typeRanges[typeId].instanceCount
    uint32_t baseInstance;        // s_baseInstanceByCmdSsbo[sortedSlot] prefix-sum base
    uint32_t drawIDBase;          // = sortedSlot; used for u_drawIDBase uniform per draw
    uint32_t typeId;              // owning type ID (diagnostics + fallback)
    uint32_t instanceByteOffset;  // s_typeRanges[typeId].instanceByteOffset (legacy SSBO range)
    uint32_t instanceByteSize;    // s_typeRanges[typeId].instanceByteSize (legacy SSBO range)
};
static_assert(sizeof(StaticPropDispatchMeta) == 32,
    "StaticPropDispatchMeta must be 32 bytes");
```

**Size rationale:** 8 × uint32 = 32 bytes. At mc2_24's worst case of 753
sorted packets, the meta array is 753 × 32 = ~24 KB. This fits comfortably in
L1 for the dispatch loop.

**Fields `instanceByteOffset` / `instanceByteSize`:** These belong to the
fallback-only path. They carry the legacy SSBO range (from `TypeRangeSsbo`)
needed when the legacy loop serves as the fallback branch. In the primary v6
coalesce dispatch path they are not consumed. Do NOT use them in the v6 dispatch
loop — they exist so the struct can service both paths without a separate
lookup at the fallback site.

**`drawIDBase` == `sortedSlot`:** True by design (see v5 §8). The two fields
are kept separate to avoid confusion at the call site — `sortedSlot` is for
lookups; `drawIDBase` is what gets uploaded as a uniform.

**OPEN — v5.5 decision:** Lock in this struct layout after v5 is working so
the emitter changes for v6 can be designed precisely. If v5 + v5.5 confirm that
`instanceByteOffset/Size` are consumed ONLY in the fallback branch, annotate
them explicitly `/* fallback-only — not read by primary v6 dispatch loop */`.
Trim to 6 fields (24 bytes) only after the legacy loop is deleted (cleanup phase).

---

## 4. Emitter Changes for v6

### 4.1 Current emitter (gameosmain.cpp)

`emitStaticPropDrawPackets()` produces `StaticPropDrawPacketCandidate[]`.
gameosmain then:
1. Converts candidates to `StaticPropOpaquePacketView[]`
2. Calls `batcher_setOpaqueDispatchCandidates()` (v4A legacy path)
3. (Separately) calls v4B/v4C observational compare functions

### 4.2 v6 emitter changes

`emitStaticPropDrawPackets()` (or a new parallel function `emitStaticPropDispatchMeta()`) must produce a `StaticPropDispatchMeta[]` in lockstep with the `DrawPacket[]` output of `buildStaticPropDrawPackets()`.

**Population sequence (per packet `i` corresponding to sorted slot):**

1. Walk `s_sortedPacketOrder[]` to find each packet's `sortedSlot`.
2. Look up `typeId` from `s_packets[globalPktIdx].owningTypeID`.
3. Look up `group` from `s_types[typeId].alphaClass`.
4. Call `batcher_getTypeInstanceCount(typeId)` for `instanceCount`.
5. Call `batcher_getBaseInstanceForSlot(sortedSlot)` for `baseInstance`.
6. Set `drawIDBase = sortedSlot`.
7. Look up `instanceByteOffset` / `instanceByteSize` from `s_typeRanges[typeId]`.

**Invariant:** The produced `StaticPropDispatchMeta[]` and `DrawPacket[]` must
be the same length and indexed identically. `meta[i]` describes the dispatch
parameters for `packets[i]`.

**OPEN:** Should `emitStaticPropDrawPackets()` grow a parallel `meta[]` output
parameter, or should meta be populated in a separate pass by a new function?
Separate pass is cleaner (avoids a large function signature change) but
requires iterating sorted order twice. Single-pass is more efficient. Recommend
single-pass with a parallel output pointer:

```cpp
DrawPacketEmitStats emitStaticPropDrawPackets(
    const RenderSnapshot&          snap,
    StaticPropDrawPacketCandidate* out,
    StaticPropDispatchMeta*        metaOut,   // NEW: may be nullptr (v0-v5 compat)
    uint32_t                       maxPackets);
```

---

## 5. Code Paths and Retirement Decisions

### 5.1 Legacy per-type draw loop in `flush()`

The legacy loop (the `else` branch of `IsCoalesceEnabled()`, roughly L3937–4239
of `gos_static_prop_batcher.cpp`) iterates `s_types` and calls
`glDrawElementsInstancedBaseVertex` per packet per type. It exists as fallback
for machines without `ARB_draw_parameters`.

**Decision (OPEN — two options):**

**Option A:** Keep both paths, both consuming DrawPackets. Legacy path reads
geometry from `DrawPacket` / `StaticPropDispatchMeta`; coalesce path reads the
same. Two dispatch loops, both DrawPacket-driven.

**Option B (RECOMMENDED — with demotion-first approach):** Do not delete the
legacy loop immediately. Instead, demote it to a named fallback branch
(`DrawPacket dispatch unavailable → fallback to legacy loop`) behind a clear
guard with a log line. Actual code deletion happens in a follow-up cleanup commit
after the fallback branch has been confirmed unreachable in all tier1 smoke runs.
This preserves bisect hygiene and gives an escape hatch if v6 has bugs.

Machines without `ARB_draw_parameters` already disable coalesce via
`s_hasShaderDrawParams`; those machines continue using the legacy loop as their
primary path — the demotion makes this explicit rather than implicit.

**Rationale:** The legacy loop and coalesce draw path have diverged significantly.
Maintaining two DrawPacket consumers doubles the test surface long-term, but
immediate deletion is a single point of failure. Demotion first, deletion after
smoke confirms the fallback is never hit.

### 5.2 `batcher_setOpaqueDispatchCandidates()` retirement

gameosmain.cpp currently calls this after emitting candidates for v4A. In v6:
- Remove the conversion loop (candidates → `StaticPropOpaquePacketView[]`).
- Remove the `batcher_setOpaqueDispatchCandidates()` call.
- Remove `s_opaqueDispatchCandidates` / `s_opaqueDispatchCandidateCount` file-scope state.
- Remove the v4A dispatch block inside `flush()` (the legacy per-type suppression block).
- Remove `StaticPropOpaquePacketView` struct from the header.

This is a straightforward deletion; no logic is replaced (v6 dispatch supersedes v4A).

---

## 6. Default-ON Flip

**Timing is not decided here.** Do not decide default-on timing until v5 proves
dispatch parity. Premature flip timing embedded in arch docs creates false
urgency. The flip prerequisites are listed for completeness; the decision belongs
in the v6 plan after v5 is confirmed.

v6 ships behind `MC2_DRAW_PACKET_COALESCE_V6=1`. The default-ON flip
(removing the env gate and making DrawPacket dispatch unconditional) is a
**separate commit** from the v6 implementation, after all of the following are
satisfied:

| Prerequisite | Verification method |
|---|---|
| v5 confirmed correct | Tier1 5/5 PASS with v5 gate ON |
| v6 visual parity confirmed | Screenshot comparison vs gate-OFF baseline on mc2_01, mc2_24 |
| StaticPropDispatchMeta struct agreed | Code review, static_assert in place |
| Emitter produces lockstep meta array | Unit test: `meta.count == packet.count`, `meta[i].typeId == packets[i].typeId` |
| Tier1 5/5 PASS with gate OFF also | After flip, old path gate is OFF; smoke gate still passes |
| No GL errors on any tier1 mission | `glGetError()` probe in v6 logging |

The flip is a one-line change (remove the env gate check) plus deletion of
dead code. Its commit message will reference the verification data that justified it.

---

## 7. Shadow Pass (`GpuStaticPropBatcher::flushShadow()`)

`flushShadow()` is the depth-only draw for GPU static props into the dynamic
shadow FBO. It is architecturally distinct from `flush()`:

- It always uses the legacy per-type loop (L4246–4400, approximate).
- It explicitly documents (L4254–4256) that it cannot use the coalesce program
  because `gl_DrawIDARB` / `gl_BaseInstanceARB` are not available in its draw context.
- It is gated behind `MC2_SHADOW_ENABLE` (default OFF, pending a private shadow VAO redesign).
- It shares the SSBO upload via `uploadAllBucketsIfNeeded()` / `s_lastUploadedSlot`.

**v6 decision (DECIDED):** Exclude shadow pass from v6. `flushShadow()` remains
on the legacy per-type loop. Treat shadow DrawPacket dispatch as v6.1.

**Why v6.1, not v6:**
1. `flushShadow()` is already opt-in behind `MC2_SHADOW_ENABLE` and effectively
   unmaintained pending the VAO redesign. Changing its dispatch path before the
   VAO issue is resolved adds risk for zero user-visible benefit.
2. Shadow depth draws don't need `u_drawIDBase` / `gl_DrawIDARB` because the
   shadow program is the legacy `shadow_static_prop` program. Wiring v6 dispatch
   to shadow would require either porting the shadow program to the coalesce
   variant (significant shader change) or a separate dispatch strategy.
3. The architectural decision for shadow is to give `flushShadow()` a private
   VAO and then wire DrawPacket dispatch; doing both together is cleaner.

---

## 8. GPU-Cull Instance Count Integration (OPEN)

v5 and the initial v6 use CPU-snapshot instance counts (`s_typeRanges[typeId].instanceCount`
= `s_bucketsByType[typeId].instances.size()`). This is the same count the
legacy path uses and is semantically correct (all snapshot instances drawn).

The GPU-cull pass runs before `flush()` and writes GPU-authoritative per-type
instance counts into the indirect command buffer via the patch shader. These
counts reflect frustum-culled instances only. The legacy `glMultiDrawElementsIndirect`
reads GPU-authoritative counts. The v5/v6 per-draw-call path does NOT read
GPU-authoritative counts — this is the known regression when the DrawPacket gate is ON.

**v6.1 option — CPU-readable GPU-cull count buffer:**
Introduce a new SSBO (separate from the indirect command buffer) that the patch
shader also writes per-type GPU-cull counts into. This SSBO can be double-buffered
(ring of 2 frames) and read by the CPU without a stall: the CPU reads the
prior-frame GPU-cull counts, which are one frame stale but sufficiently accurate
for instance-count-gating purposes. Then `StaticPropDispatchMeta::instanceCount`
can be sourced from the GPU-cull count rather than the snapshot count.

This is NOT required for v6 correctness. It is a performance improvement that
reduces overdraw when many props are frustum-culled.

**Recommendation:** Defer to v6.1. Document the gap in v6 smoke gate output
(a counter showing "cpu_vs_gpu_count_delta per type" could be gated behind
`MC2_DRAW_PACKET_V6_COUNT_TRACE=1`).

---

## 9. Decision Table Summary

| Decision | Status | Notes |
|---|---|---|
| StaticPropDispatchMeta struct layout | DECIDED (confirm v5.5) | 8 × uint32, 32 bytes |
| Legacy path retirement from flush() | RECOMMENDED: retire (option B) | Machines w/o ARB_draw_params stay on legacy until separate cleanup |
| Shadow pass dispatch | DECIDED: exclude from v6 (v6.1) | Pending VAO redesign |
| GPU-cull count integration | OPEN: defer to v6.1 | Performance improvement only |
| Emitter: single-pass vs two-pass meta | RECOMMENDED: single-pass, parallel pointer | Cleaner API, one fewer sorted-order traversal |
| Default-ON flip timing | After visual parity + tier1 5/5 PASS | Separate commit from implementation |
| v6 env gate name | `MC2_DRAW_PACKET_COALESCE_V6` | Kill-switch after flip: `MC2_DRAW_PACKET_COALESCE_V6_DISABLE` |

---

## 10. v6 Prerequisites

In order (each must be complete before starting the next):

1. **v5 shipped** — substitutive per-draw-call dispatch correct, tier1 5/5 PASS
   with `MC2_DRAW_PACKET_COALESCE_V5=1`.

2. **v5.5 — StaticPropDispatchMeta lock-in** — struct layout agreed and
   reviewed; `static_assert` in place; forward declarations in header. No
   implementation required at this step — just the type definition and the
   header changes to `draw_packet_emitter.h`.

3. **Emitter update** — `emitStaticPropDrawPackets()` gains the `metaOut`
   parallel pointer parameter. Backward-compatible (nullptr = no meta output).
   `buildStaticPropDrawPackets()` gets a parallel meta-population helper or
   the population is inlined at the call site in gameosmain.cpp.

4. **v6 dispatch loop** — New loop in `flush()` consuming `DrawPacket[]` +
   `StaticPropDispatchMeta[]`. Replace both multidraw calls and the legacy
   per-type loop when `MC2_DRAW_PACKET_COALESCE_V6=1`.

5. **Retirement cleanup** — Remove `batcher_setOpaqueDispatchCandidates()`,
   `StaticPropOpaquePacketView`, v4A dispatch block, the v5 gate scaffolding.

6. **Default-ON flip** — Separate commit. Smoke gate, visual parity confirm.

---

## 11. Risk Register

| # | Risk | Likelihood | Mitigation |
|---|---|---|---|
| R1 | v5 reveals that `instanceByteOffset/Size` fields are never consumed in coalesce mode | Medium | Trim struct to 6 fields (24 bytes) in v5.5; update static_assert |
| R2 | Emitter `metaOut` parallel pointer causes alignment issues | Low | `StaticPropDispatchMeta` is 32-byte aligned; stack/static buffer allocation is safe |
| R3 | GPU-cull count gap causes visible under-draw in some maps | Low | CPU-snapshot counts draw MORE than GPU-cull, not fewer; no under-draw risk |
| R4 | Coalesce disarm path breaks after legacy loop retirement | Medium | Add explicit "coalesce disarmed → DrawPacket dispatch also disarmed → no draw" guard with a clear error log |
| R5 | Shadow VAO redesign (prerequisite for v6.1 shadow dispatch) blocks v6 timeline | Low | v6 explicitly excludes shadow; no dependency |
| R6 | Legacy path machines (no ARB_draw_params) break if legacy loop is retired prematurely | High | Retirement gated behind `IsCoalesceEnabled()` check; if coalesce cannot arm, legacy loop stays; plan explicitly states it remains for non-coalesce machines |

---

## 12. What v6 Does NOT Change

- `flushShadow()` implementation
- `batcher_prepareBaseInstanceTable()` — still runs each frame to build the prefix-sum
- Ring-buffer geometry and instance SSBO layout
- `s_sortedPacketOrder[]` / `s_alphaOffCmdCount` / `s_alphaOnCmdCount` — still populated by the per-frame sort in `flush()`
- Texture array (`s_texArrayOff` / `s_texArrayOn`) — same arrays, same switch point
- `s_perDrawSsbo` (PerDrawEntry SSBO, binding 4) — same content, same binding
- `gpu_cull::compute_dispatch()` — GPU cull still runs; its output (indirect buffer) is simply not consumed by the v6 draw loop
- Coalesce instance SSBO (`s_coalesceInstanceSsbo`, binding 0) — same layout, same persistent-mapped ring; v6 loop inherits the binding from the prologue

---

## 13. Self-Contained Context for a Future Session

To resume from this document without the conversation history:

**Shipped state:** v4A (opaque-only legacy proof), v4B (count soak), v4C (slot soak) are committed to `claude/nifty-mendeleev`.

**Key batcher state for dispatch:**
- `s_sortedPacketOrder[]` — global packet indices in [OFF | ON] order; length = `batcher_getSortedPacketCount()`
- `s_alphaOffCmdCount` / `s_alphaOnCmdCount` — packet counts per group
- `s_baseInstanceByCmdSsbo` — persistent-mapped coherent SSBO (binding 16); one `uint32_t` base instance per sorted slot per ring frame; written by `batcher_prepareBaseInstanceTable()` each frame
- `s_coalesceInstanceSsbo` (binding 0) — ring-buffered instance data; globally pooled in non-legacy mode
- `s_perDrawSsbo` (binding 4) — PerDrawEntry per sorted packet (materialFlags + tex layer)
- `s_staticPropProgramCoalesce` — GL program; uses `u_drawIDBase` uniform + `gl_DrawIDARB`
- `s_texArrayOff` / `s_texArrayOn` — alpha group texture arrays
- `TypeRangeSsbo::instanceCount` (in `s_typeRanges` map) — CPU-snapshot instance count per type, populated by `uploadAllBucketsIfNeeded()` before flush() coalesce branch

**Key source files:**
- `GameOS/gameos/gos_static_prop_batcher.cpp` — batcher (5000+ lines)
- `GameOS/gameos/gos_static_prop_batcher.h` — batcher public API
- `GameOS/gameos/draw_packet_emitter.h` — `StaticPropDrawPacketCandidate`, emit/compare/build API
- `GameOS/gameos/gameosmain.cpp` — emitter call site, v4A registration, DrawPackets panel
- `RenderCore/DrawPacket.h` — canonical `RenderCore::DrawPacket` struct

**Test command:**
```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```
