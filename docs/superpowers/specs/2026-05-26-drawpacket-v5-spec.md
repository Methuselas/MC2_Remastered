# DrawPacket v5 — Substitutive Coalesce Dispatch Spec

**Date:** 2026-05-26
**Status:** SPEC (pre-implementation)
**Env gate:** `MC2_DRAW_PACKET_COALESCE_V5=1`
**Predecessor specs:**
- v4A: `2026-05-25-draw-packet-v0-spec.md` (opaque-only legacy path proof)
- v4B: `2026-05-26-drawpacket-v4b-spec.md` (count coverage soak)
- v4C: `2026-05-26-drawpacket-v4c-spec.md` (slot coverage soak)

---

## 1. Goal

Replace the two `glMultiDrawElementsIndirect` calls in `flush()` (alpha-OFF
group at L3888, alpha-ON at L3910 of `gos_static_prop_batcher.cpp`) with a
per-draw-call loop driven by `StaticPropDrawPacketCandidate[]` candidates and
CPU-side instance counts.

This is the first dispatch slice that **replaces** GL draw calls (not merely
observes). It proves the full dispatch path — program, uniform, SSBO bindings,
instance count source, base-instance source — before multidraw optimization in v6.

**Anti-goals for v5:**
- No multidraw (defer to v6 if perf demands).
- No GPU-cull instance count integration (CPU-snapshot counts only; same as legacy path).
- No new shader programs (reuse `s_staticPropProgramCoalesce`).
- No change to SSBO layout, texture arrays, or ring-buffer geometry.
- No shadow-pass changes (`GpuStaticPropBatcher::flushShadow()` is always legacy; exclude from v5).

---

## 2. Classification

**Dispatch-changing.** This slice:
- Issues GL draw calls from a new code path
- Suppresses the coalesce `glMultiDrawElementsIndirect` when the gate is ON
- May produce different per-draw instance counts (CPU-snapshot vs GPU-cull)
- Must save/restore any GL state it mutates beyond what the coalesce path already restores

---

## 3. Insertion Point

`flush()` inside `gos_static_prop_batcher.cpp`, within the coalesce draw branch
(the block entered when `IsCoalesceEnabled()` is true, roughly L3727–3936).

The two existing multidraw calls:

```
// alpha-OFF (L3888):
glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT,
    reinterpret_cast<const void*>(0u),
    static_cast<GLsizei>(s_alphaOffCmdCount), 0);

// alpha-ON (L3910):
glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT,
    reinterpret_cast<const void*>(alphaOnOffset),
    static_cast<GLsizei>(s_alphaOnCmdCount), 0);
```

When `MC2_DRAW_PACKET_COALESCE_V5=1`, these two calls are replaced by a
per-draw-call loop. All surrounding setup (program switch, uniform upload,
SSBO bindings, texture binding, indirect buffer) is inherited from the
existing coalesce prologue code (L3753–3886). The v5 loop runs in place of
the two `glMultiDrawElementsIndirect` calls only.

---

## 4. Dispatch Source

The caller (gameosmain.cpp) already produces `StaticPropDrawPacketCandidate[]`
via `emitStaticPropDrawPackets()` before calling into the batcher. Currently
gameosmain converts these to `StaticPropOpaquePacketView[]` for v4A. For v5,
the full candidate array (all alpha groups, all packets) must be passed to a
new batcher entry point, or the coalesce draw path must be able to iterate the
sorted packet order directly.

**Decision (DECIDED):** v5 dispatch loop iterates `s_sortedPacketOrder[]`
directly (file-scope batcher state, already populated by `finalizeGeometry()`
+ the per-frame packet-sort in `flush()`'s Step 5.7). The candidate array from
gameosmain provides the authoritative `instanceCount` per type; it is consumed
via the new accessor `batcher_getTypeInstanceCount(typeId)`. No new batcher
registration call is required for v5.

---

## 5. Per-Draw Dispatch Strategy

For each sorted slot `i` in `[0, s_alphaOffCmdCount + s_alphaOnCmdCount)`:

```
globalPktIdx  = s_sortedPacketOrder[i]
packet        = s_packets[globalPktIdx]          // GpuStaticPropPacket (geometry)
typeId        = packet.owningTypeID
instanceCount = batcher_getTypeInstanceCount(typeId)   // CPU-snapshot count
baseInstance  = batcher_getBaseInstanceForSlot(i)      // prefix-sum from persistent map
drawIDBase    = i                                       // for u_drawIDBase uniform

if instanceCount == 0: skip (no instances this frame for this type)

// Set drawIDBase for this packet's PerDrawEntry lookup:
glUniform1i(s_locsCoalesce.drawIDBase, (GLint)drawIDBase);

glDrawElementsInstancedBaseVertexBaseInstance(
    GL_TRIANGLES,
    (GLsizei)packet.indexCount,
    GL_UNSIGNED_INT,
    (const void*)(uintptr_t)(packet.firstIndex * sizeof(uint32_t)),  // byte offset into IBO
    (GLsizei)instanceCount,
    (GLint)packet.baseVertex,           // baseVertex from GpuStaticPropPacket
    (GLuint)baseInstance);
```

**Alpha group boundary:** The texture array switch happens at the group
boundary, not per packet. Before the loop begins, bind `s_texArrayOff`. When
`i == s_alphaOffCmdCount`, switch to `s_texArrayOn`. Set `u_drawIDBase`
per packet (not once per group), because `gl_DrawID` is always 0 in a
non-multidraw call — the draw ID must come entirely from `u_drawIDBase`.

---

## 6. Required New Batcher Accessors

These three functions are **new batcher-public API** to be added in v5.

### 6.1 `batcher_getTypeInstanceCount(uint32_t typeId)`

```cpp
// Returns s_typeRanges[typeId].instanceCount.
// TypeRangeSsbo::instanceCount is populated by uploadAllBucketsIfNeeded()
// (the per-frame SSBO upload) from s_bucketsByType[typeId].instances.size().
// Guard: if typeId not in s_typeRanges, returns 0.
uint32_t batcher_getTypeInstanceCount(uint32_t typeId);
```

**Source:** `s_typeRanges` is an `std::unordered_map<uint32_t, TypeRangeSsbo>`
defined at file scope in the anonymous namespace around flush(). It is
populated by `uploadAllBucketsIfNeeded()` which is called early in flush()
before the coalesce draw branch.

**Correctness:** `TypeRangeSsbo::instanceCount` = number of snapshot instances
for this type. It includes all instances in `s_bucketsByType[typeId]`, not just
GPU-cull-surviving instances. This matches the semantics of the legacy
`s_bucketsByType[typeId].instances.size()` draw. Accept this for v5.

### 6.2 `batcher_getBaseInstanceForSlot(uint32_t sortedSlot)`

```cpp
// Returns s_baseInstanceByCmdMap[coalesceFrameSlot * bytesPerFrame + sortedSlot].
// CPU read from the persistent-mapped SSBO (coherent; no stall).
// Guard: if sortedSlot >= (s_alphaOffCmdCount + s_alphaOnCmdCount), returns 0.
// Guard: if s_baseInstanceByCmdMap == nullptr, returns 0.
uint32_t batcher_getBaseInstanceForSlot(uint32_t sortedSlot);
```

**Source:** `s_baseInstanceByCmdSsbo` is allocated with
`GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT`
(confirmed at L727 of batcher.cpp). The CPU write in
`batcher_prepareBaseInstanceTable()` is coherent; no `glMemoryBarrier` or
fence is needed before a CPU read in the same frame. The GPU reads this SSBO
via the patch shader BEFORE flush(), so by flush() time the CPU write is
already fenced by the same `glClientWaitSync` that guards the coalesce ring
slot. Reads in the v5 loop are safe without additional synchronization.

**Binding:** `s_baseInstanceByCmdSsbo` is bound at `BASE_INSTANCE_SSBO_BINDING`
(= 16) for the GPU patch shader. The v5 per-draw loop reads from the CPU-mapped
pointer only; it does NOT need to rebind the SSBO for this purpose.

### 6.3 `batcher_getCoalesceProgram()`

```cpp
// Returns s_staticPropProgramCoalesce (the GL program handle).
// 0 if coalesce program failed to link or is disabled.
GLuint batcher_getCoalesceProgram();
```

v5 runs INSIDE the existing coalesce draw branch, which has already called
`glUseProgram(s_staticPropProgramCoalesce)`. This accessor is included for
completeness and diagnostic use, but the v5 loop does not need to call
`glUseProgram` again.

---

## 7. GL State Setup for the v5 Loop

The existing coalesce prologue (L3740–3886) runs unconditionally when the
coalesce branch is entered. By the time the v5 loop runs, the following state
is already established:

| State | Value | Set by |
|---|---|---|
| `GL_CURRENT_PROGRAM` | `s_staticPropProgramCoalesce` | L3754 `glUseProgram` |
| `GL_VERTEX_ARRAY_BINDING` | `s_sharedVao` | earlier in flush() |
| SSBO binding 0 | `s_coalesceInstanceSsbo` (covering both groups in global-pool mode) | L3881 `glBindBufferRange` |
| SSBO binding 4 | `s_perDrawSsbo` | L3799 `glBindBufferBase` |
| `GL_TEXTURE0` / `GL_TEXTURE_2D_ARRAY` | `s_texArrayOff` | L3886 `glBindTexture` |
| `GL_DRAW_INDIRECT_BUFFER` | `gpu_cull::compute_getIndirectCmdBuf()` | L3887 `glBindBuffer` |
| `u_drawIDBase` uniform | 0 (alpha-OFF group) | L3873 `glUniform1i` |
| Various MVP/fog uniforms | uploaded | L3757–3795 |

The v5 loop adds one `glUniform1i` per packet (for `u_drawIDBase`) and one
`glBindTexture` at the group boundary (when `i == s_alphaOffCmdCount`). The
existing `glBindBuffer(GL_DRAW_INDIRECT_BUFFER, ...)` remains from the prologue
but is irrelevant for the per-draw-call loop (indirect buffer is not read by
`glDrawElementsInstancedBaseVertexBaseInstance`). v5 does NOT unbind it — the
existing restore at L3917 handles cleanup.

**State the v5 loop adds that needs save/restore:**
- `u_drawIDBase` uniform: set once per packet. The existing restore path
  (L3919–3923) restores SSBO slots 4 and texture, but does NOT restore the
  uniform to 0. Since the coalesce epilogue doesn't re-use `u_drawIDBase`,
  this is acceptable — the next flush will set it correctly from the prologue.
  No additional save/restore needed.

---

## 8. `u_drawIDBase` Uniform Protocol

This is the most important v5 correctness invariant.

**In multidraw mode:** `gl_DrawID` = within-group draw index (0..N-1). The
uniform is set once per group: 0 for alpha-OFF, `s_alphaOffCmdCount` for
alpha-ON. The shader reads `entries[gl_DrawID + u_drawIDBase]`.

**In per-draw-call mode:** `gl_DrawID` is always 0 (only one draw at a time).
The uniform must be set to the absolute sorted slot for each packet:

```
// alpha-OFF slot i (i in [0, s_alphaOffCmdCount)):
u_drawIDBase = i         (absolute slot = i + 0)

// alpha-ON slot j (j in [0, s_alphaOnCmdCount)):
u_drawIDBase = s_alphaOffCmdCount + j   (absolute slot)
```

The loop variable is the absolute sorted slot, so `u_drawIDBase = loop_index`
for every packet. One `glUniform1i` per packet — this is the primary per-packet
overhead cost versus multidraw.

---

## 9. Risks

| # | Risk | Mitigation / Investigation needed |
|---|---|---|
| R1 | `s_typeRanges` not yet populated when v5 loop accesses it | `uploadAllBucketsIfNeeded()` is called before the coalesce branch; `s_typeRanges` is valid by L3727. Verify at insertion point. |
| R2 | `batcher_getBaseInstanceForSlot` coherency | SSBO mapped coherent — no stall needed. Verified at L727: `GL_MAP_COHERENT_BIT`. |
| R3 | `glDrawElementsInstancedBaseVertexBaseInstance` not available | Requires GL 4.2 or `ARB_base_instance`. The coalesce path already requires `ARB_draw_parameters` (for `gl_DrawIDARB`). Add explicit extension check; fail gracefully to multidraw if absent. |
| R4 | `packet.baseVertex` field name | `GpuStaticPropPacket` struct fields must be confirmed. If field is named differently (e.g. `baseVertexOffset`), the call is wrong. Grep `GpuStaticPropPacket` before coding. |
| R5 | `packet.firstIndex` is an element index, not a byte offset | `glDrawElementsInstancedBaseVertexBaseInstance` offset parameter is a byte offset into the IBO. Must multiply by `sizeof(uint32_t)`. Confirmed from `StaticPropDrawPacketCandidate::firstIndex` doc: "IBO element index (NOT byte offset)". |
| R6 | GPU cull count divergence | When v5 gate is ON, GPU cull benefit is lost (draws all snapshot instances). This is documented and accepted. Smoke gate must confirm visual parity (all instances rendered; same pixel output when frustum matches snapshot). |
| R7 | `u_drawIDBase` one-per-draw overhead | On mc2_01 with 134 packets, this is 134 `glUniform1i` calls per frame. Negligible for v5 correctness proof. |
| R8 | Instance data binding in legacy mode | v5 only runs inside the coalesce branch (`IsCoalesceEnabled()`). Legacy mode (s_globalPoolLegacy) uses per-group binding. The existing prologue at L3874 handles this correctly before v5 loop runs. v5 inherits the correct binding. |

---

## 10. Assumptions Requiring Verification Before Coding

1. **`GpuStaticPropPacket` struct fields:** Confirm field names `baseVertex`,
   `firstIndex`, `indexCount`, `owningTypeID`. Grep `struct GpuStaticPropPacket`.

2. **`s_typeRanges` is the correct count source:** `TypeRangeSsbo::instanceCount`
   = `s_bucketsByType[typeId].instances.size()`. Confirm at the
   `uploadAllBucketsIfNeeded()` populate site. There must not be a second
   per-packet instance count that differs from this.

3. **Coalesce branch entry condition:** The v5 code path is only reached when
   `IsCoalesceEnabled()` returns true AND `s_staticPropProgramCoalesce != 0`.
   Confirm that `s_typeRanges` is always populated by the time the branch executes
   (even on first frame before any draws).

4. **`drawIDBase` uniform name and location:** Confirmed as `u_drawIDBase`
   cached in `s_locsCoalesce.drawIDBase`. `-1` if absent in shader — calls to
   `glUniform1i(-1, ...)` are no-ops per GL spec.

---

## 11. Diagnostics / Logging

v5 adds one stderr log per gate-ON first flush:

```
[DRAW_PACKET_V5] event=armed draws_off=%u draws_on=%u total_draws=%u
```

Per-frame diagnostics are gated behind `MC2_DRAW_PACKET_COALESCE_V5_TRACE=1`:

```
[DRAW_PACKET_V5] slot=%u type=%u inst=%u base_inst=%u draw_id_base=%u
[DRAW_PACKET_V5] event=skip_zero_inst slot=%u type=%u
[DRAW_PACKET_V5] gl_error=%u (only logged if nonzero)
```

---

## 12. Hard Invariants (Smoke Gate Shape)

All five must hold after v5 implementation is complete:

| Invariant | Formula | Expected value |
|---|---|---|
| Draw count | `v5_draws_issued == s_alphaOffCmdCount + s_alphaOnCmdCount` | Total sorted packets (0-instance slots count as draws; see R6 — or skip them: then `v5_draws_issued <= total`) |
| GL error | `glGetError()` after both groups | `GL_NO_ERROR` |
| Invalid slot | `batcher_getBaseInstanceForSlot(i) returns 0 only for i >= total_cmds` | `invalid_slot_count == 0` |
| Tier1 | 5/5 missions PASS | All missions |
| No visual regression | Screenshot compare (or manual inspection) vs gate-OFF baseline | Same props rendered |

**Note on zero-instance draws:** If `instanceCount == 0` for a slot, the v5
loop should skip the draw (emit no GL call for that slot). This is safe because
the multidraw path uses GPU-cull counts which can be zero for invisible types;
the per-draw skip is the correct analogue. Adjust the draw-count invariant
accordingly: `v5_draws_issued <= total_cmds`.

---

## 13. Prerequisites

- v4C PASS: slot-level coverage soak confirmed `unmatched=0` on all 5 missions.
- `batcher_getBaseInstanceForSlot()` accessor added and unit-validated against
  known v4C slot values before wiring into GL dispatch.
- `GpuStaticPropPacket` field names confirmed by code inspection.
- `ARB_base_instance` extension check added to the coalesce-arm path.

---

## 14. Out of Scope for v5

- Multidraw (v6+).
- GPU-cull instance count integration (v6.1 potential).
- Shadow pass (`GpuStaticPropBatcher::flushShadow()` always uses legacy per-type loop; not changed here).
- `StaticPropDispatchMeta` sidecar struct (introduced in v5.5 between v5 and v6; see v6 arch doc).
- `batcher_setOpaqueDispatchCandidates()` retirement (v6).
