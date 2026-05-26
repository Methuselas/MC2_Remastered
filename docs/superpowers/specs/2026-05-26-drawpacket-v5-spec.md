# DrawPacket v5 — Substitutive Coalesce Dispatch Spec

**Date:** 2026-05-26
**Status:** SPEC (pre-implementation; requires render-spine-advisor + adversarial review)
**Env gate:** `MC2_DRAW_PACKET_COALESCE_V5=1`
**Predecessor specs:**
- v4B: `2026-05-26-drawpacket-v4b-spec.md` (count coverage soak, shipped da7eb1e3)
- v4C: `2026-05-26-drawpacket-v4c-spec.md` (slot coverage soak, shipped 64254839)

---

## 1. Goal

Replace the two `glMultiDrawElementsIndirect` calls in `flush()` (alpha-OFF at ~L3888,
alpha-ON at ~L3910 of `gos_static_prop_batcher.cpp`) with a per-draw-call loop when
`MC2_DRAW_PACKET_COALESCE_V5=1`. v5 runs entirely inside `gos_static_prop_batcher.cpp`
— no gameosmain changes, no new batcher public API.

This is the first dispatch-changing slice. It proves: program, uniform, SSBO bindings,
instance count source, base-instance source, and texture array group boundary — before
multidraw optimization in v6.

**Anti-goals:**
- No multidraw (defer to v6 if perf demands).
- No GPU-cull instance count integration (CPU-snapshot counts only; same as legacy path).
- No new shader programs, no SSBO layout changes, no texture array changes.
- No new public batcher accessors (v5 reads file-local state inside batcher.cpp directly).
- No shadow-pass changes (`GpuStaticPropBatcher::flushShadow()` excluded).
- No `StaticPropDispatchMeta` sidecar (defined in v5.5 after v5 proves which fields are needed).

---

## 2. Classification

| Axis | Value |
|---|---|
| Kind | Dispatch-changing |
| Pixels changed | Yes (may render more instances than GPU-cull path; same as legacy mode) |
| GL calls added | Yes (one `glDrawElementsInstancedBaseVertexBaseInstance` + one `glUniform1i` per slot) |
| GL calls replaced | Yes (suppresses `glMultiDrawElementsIndirect` when gate ON) |
| Files changed | 1 (`gos_static_prop_batcher.cpp`) |
| New public batcher API | None — all state is file-local |
| New env vars | `MC2_DRAW_PACKET_COALESCE_V5`, `MC2_DRAW_PACKET_COALESCE_V5_TRACE` |

---

## 3. Insertion Point

Inside `flush()` → coalesce branch (`IsCoalesceEnabled()` == true) → after the full coalesce
prologue has run (program switch, uniform upload, SSBO bindings, texture bindings).

Target replacement lines (approximate; grep to confirm):
```
// alpha-OFF multidraw (~L3888):
glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, (void*)0u,
    (GLsizei)s_alphaOffCmdCount, 0);

// alpha-ON multidraw (~L3910):
glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, (void*)alphaOnOffset,
    (GLsizei)s_alphaOnCmdCount, 0);
```

Gate placement:
```cpp
static const bool s_v5Enabled = [] {
    const char* v = std::getenv("MC2_DRAW_PACKET_COALESCE_V5");
    return v && v[0] == '1';
}();
```

Extension check (at coalesce-arm time or first gate-ON flush):
```cpp
// GLEW_ARB_base_instance is a GLboolean flag (0/1, not an enum).
// GL 4.2 core mandates ARB_base_instance, so GLEW_ARB_base_instance == GL_TRUE
// on any 4.2+ context — no separate version check needed.
static const bool s_baseInstanceSupported = (GLEW_ARB_base_instance == GL_TRUE);
```

If `!s_v5Enabled || !s_baseInstanceSupported`:
- Log once: `[DRAW_PACKET_V5] event=unsupported` (if v5 requested but extension absent)
- Fall through to existing `glMultiDrawElementsIndirect` calls — no suppression, no regression.

If `s_v5Enabled && s_baseInstanceSupported`:
- Run gate-arm checks: `s_baseInstanceByCmdMap != nullptr`, `s_locsCoalesce.drawIDBase != -1`
  (both disarm v5 for session if failed — see §5 and §7)
- Skip the two `glMultiDrawElementsIndirect` calls
- Run v5 per-draw loop

---

## 4. Instance Count Authority

**v5 uses CPU-snapshot instance counts only.** Do not reason about GPU-cull counts.

Source: `s_typeRanges[typeId].instanceCount` (file-local `std::unordered_map<uint32_t, TypeRangeSsbo>`
inside batcher.cpp anonymous namespace). Populated by `uploadAllBucketsIfNeeded()` before the
coalesce branch — **snapshot types only** (types with instances this frame). Types with zero
instances are NOT inserted; querying them yields `s_typeRanges.end()`.

`TypeRangeSsbo::instanceCount` = `s_bucketsByType[typeId].instances.size()` = number of snapshot
instances submitted for that type this frame. This is the same source used by the legacy per-type
loop. Zero means the type has no instances in the current snapshot.

**Important:** `s_sortedPacketOrder` includes ALL texture-assigned packets built at coalesce
finalization (map-load time), including many types with no instances in the current snapshot.
A miss on `s_typeRanges.find()` is NOT a type-OOB error — it means zero instances this frame.
Genuine OOB is checked against `s_types.size()` (the static all-types table).

**Not used:**
- GPU cull indirect buffer instance counts (GPU-write-only, no CPU copy without stall)
- `s_offGroupCountThisFrame` (global accumulator, not per-type)
- Any emitter candidate `instanceCount` field (not passed into batcher.cpp)

**Zero-instance skip:** If type not in `s_typeRanges` OR `instanceCount == 0`, skip the draw.
This is the CPU-snapshot analogue to the GPU-cull producing instanceCount=0. The slot is
counted in `zero_instance_skips`.

Zero-instance types in `s_sortedPacketOrder` are valid and expected (types that had submissions
in a prior frame but not this frame may still appear in the coalesce layout). They are NOT bugs.

---

## 5. Base Instance Authority

Source: `s_baseInstanceByCmdMap` — the CPU-mapped `void*` of `s_baseInstanceByCmdSsbo`.

`s_baseInstanceByCmdSsbo` is allocated with `GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT`.
The CPU write in `batcher_prepareBaseInstanceTable()` (called before flush()) is coherent;
no `glMemoryBarrier` or fence needed before CPU reads in the v5 loop.

**Ring-slot byte arithmetic (exact — do not use index arithmetic):**
```cpp
// s_baseInstanceByCmdMap is void* mapped at SSBO start.
// Each ring slot occupies s_baseInstanceByCmdBytesPerFrame bytes.
// Use the same arithmetic as prepareBaseInstanceTable():
const size_t fr_off = s_coalesceFrameSlot * s_baseInstanceByCmdBytesPerFrame;  // bytes
const uint32_t* baseInstanceMap = reinterpret_cast<const uint32_t*>(
    static_cast<const uint8_t*>(s_baseInstanceByCmdMap) + fr_off);
// Then access: baseInstanceMap[i]  for slot i in [0, totalCmds)
```

**Gate-arm null check (CRITICAL — run once at arm time, not per-slot):**
Under `MC2_STATIC_PROP_GLOBAL_POOL_LEGACY=1`, `s_baseInstanceByCmdSsbo` is never allocated and
`s_baseInstanceByCmdMap` stays `nullptr`. If `s_baseInstanceByCmdMap == nullptr` at gate-arm time:
log `event=unsupported reason=legacy_pool_no_baseinst_map`, set `s_v5Enabled = false` for the
session, fall through to legacy multidraw. Do NOT detect this per-slot — that silently drops all
draws (ok=0, user sees blank world) and hides the real problem.

**Per-slot bounds guard:** If `i >= totalCmds`: increment `base_instance_missing`; continue.
Do NOT treat `baseInstanceMap[0]` = 0 as a null sentinel — 0 is a valid base instance for slot 0.

---

## 6. Per-Draw Loop Structure

```
sorted_count = s_alphaOffCmdCount + s_alphaOnCmdCount

// SSBO binding 0 precondition:
// The coalesce prologue binds slot 0 (s_coalesceInstanceSsbo) ONLY inside
// if (s_alphaOffCmdCount > 0u). If OFF count is 0 (all props alpha-ON),
// slot 0 is unbound from a prior frame. The v5 implementation MUST bind
// SSBO slot 0 unconditionally here (using the same bind call as the prologue)
// before entering the loop. The multidraw path shares this latent risk.
// In practice s_alphaOffCmdCount > 0 on all tier1 missions, but do not rely on this.
glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, s_coalesceInstanceSsbo, <ring_offset>, <ring_size>)

// Bind alpha-OFF texture array (already done by prologue at L3886; redundant here if OFF>0 but safe)
// u_drawIDBase: v5 sets per draw via glUniform1i — no prologue value relied upon

for i in [0, sorted_count):
    // Bounds guard
    if i >= s_sortedPacketOrder.size():
        ++sorted_oob; continue

    globalPktIdx = s_sortedPacketOrder[i]

    if globalPktIdx >= s_packets.size():
        ++packet_oob; continue

    packet = s_packets[globalPktIdx]   // GpuStaticPropPacket

    typeId = packet.owningTypeID
    // Genuine OOB: typeId outside the static all-types table (s_types).
    if typeId >= s_types.size():
        ++type_oob; continue

    // s_typeRanges only has snapshot (instance-bearing) types.
    // Missing from s_typeRanges = zero instances this frame; not an error.
    typeIt = s_typeRanges.find(typeId)
    instanceCount = typeIt->instanceCount if typeIt != s_typeRanges.end() else 0

    if instanceCount == 0:
        ++zero_instance_skips; continue

    baseInstance = baseInstanceMap[i]   // index within current ring slot

    // Group boundary: switch texture array when entering alpha-ON group
    if i == s_alphaOffCmdCount:
        glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOn)
        // u_drawIDBase will be set below for this slot

    // Per-draw uniform: absolute sorted slot (gl_DrawID is always 0 in non-multidraw)
    glUniform1i(s_locsCoalesce.drawIDBase, (GLint)i)

    glDrawElementsInstancedBaseVertexBaseInstance(
        GL_TRIANGLES,
        (GLsizei)packet.indexCount,
        GL_UNSIGNED_INT,
        (const void*)(uintptr_t)(packet.firstIndex * sizeof(uint32_t)),  // byte offset
        (GLsizei)instanceCount,
        (GLint)packet.baseVertex,
        (GLuint)baseInstance)

    ++draws_issued

// Restore texture array to alpha-OFF state if loop ran the ON group
// (existing epilogue handles other state; u_drawIDBase doesn't need restore —
//  next frame's prologue sets it fresh from L3873)
if s_alphaOnCmdCount > 0:
    glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOff)  // restore for consistency

// GL error check (gate-ON only)
GLenum err = glGetError()
if err != GL_NO_ERROR: ++gl_errors; log immediately
```

---

## 7. `u_drawIDBase` Uniform Protocol

Critical correctness point. The coalesce shader reads `entries[gl_DrawID + u_drawIDBase]`.

| Path | gl_DrawID | u_drawIDBase |
|---|---|---|
| Multidraw (existing) | within-group index [0, N) | 0 for OFF group; `s_alphaOffCmdCount` for ON group |
| Per-draw-call (v5) | always 0 | absolute sorted slot `i` |

In v5: `u_drawIDBase = i` for every packet. One `glUniform1i` per packet.
On mc2_24 (753 total packets): 753 `glUniform1i` calls per frame — negligible for correctness proof.

The uniform location `s_locsCoalesce.drawIDBase` must exist. If it is `-1` (absent from shader),
`glUniform1i(-1, x)` is a no-op per GL spec — the draw reads wrong entry data silently.
Verify `s_locsCoalesce.drawIDBase != -1` at gate-arm time. **If -1: log WARNING, set `s_v5Enabled
= false` for the session, and fall through to legacy multidraw permanently.** Do not proceed with
v5 draws when this location is missing — silent SSBO entry corruption is not acceptable.

---

## 8. GL State Inherited from Prologue

When v5 loop runs, the following state is already set by the coalesce prologue (L3740–3886):

| State | Value |
|---|---|
| `GL_CURRENT_PROGRAM` | `s_staticPropProgramCoalesce` (set at L3754) |
| VAO | `s_sharedVao` |
| SSBO binding 0 | `s_coalesceInstanceSsbo` ring-slot range — v5 **rebinds unconditionally** before loop (prologue only binds inside `if (s_alphaOffCmdCount > 0u)`) |
| SSBO binding 4 | `s_perDrawSsbo` (PerDrawEntry data, one entry per sorted slot) |
| Texture unit 0 | `s_texArrayOff` (alpha-OFF texture array, set at L3886) |
| `GL_DRAW_INDIRECT_BUFFER` | indirect command buffer (GPU-cull written; not used by per-draw-call) |
| Various uniforms | MVPs, fog, etc. |

Note: SSBO binding 7 is NOT a prologue-inherited state for the draw pass. It is bound only during
the compute patch pass (`gpu_cull_compute.cpp`) and restored immediately. The static_prop shaders
do not declare binding 7. Do not rely on binding 7 being set when v5 loop runs.

Note: `u_drawIDBase` prologue value (L3873) is conditional on `s_alphaOffCmdCount > 0`. V5 sets
`u_drawIDBase` unconditionally per draw via `glUniform1i` — no prologue value is relied upon.

v5 loop mutates:
- `u_drawIDBase` per packet — no restore needed; next frame prologue resets it.
- `GL_TEXTURE_2D_ARRAY` binding at the group boundary — restore to `s_texArrayOff` after loop.
- SSBO binding 0 — rebind before loop; epilogue restores overall state.

v5 does NOT unbind `GL_DRAW_INDIRECT_BUFFER` — the existing epilogue handles this.

---

## 9. Diagnostics / Logging

### Gate-arm log (once per process on first gate-ON flush)

```
[DRAW_PACKET_V5] event=armed slots=%u ext_supported=%d drawid_loc=%d
```

### Per-frame summary (every 600 frames, gate-ON)

```
[DRAW_PACKET_V5] frame=%u event=dispatch_summary
  slots_considered=%u draws_issued=%u zero_instance_skips=%u
  sorted_oob=%u packet_oob=%u type_oob=%u base_instance_missing=%u
  gl_errors=%u ok=%d
```

`ok=1` iff all error counters are 0.

### Per-slot trace (every frame, `MC2_DRAW_PACKET_COALESCE_V5_TRACE=1`)

```
[DRAW_PACKET_V5] slot=%u type=%u inst=%u base_inst=%u draw_id_base=%u
[DRAW_PACKET_V5] event=skip slot=%u reason=<zero_inst|base_missing|packet_oob|type_oob>
```

Verbose gate fires only when master gate is also on.

---

## 10. Hard Invariants (Smoke Gate Shape)

### Gate OFF (default)

- `tier1 5/5 PASS`
- No `[DRAW_PACKET_V5]` lines in smoke logs

### Gate ON

```
slots_considered == s_alphaOffCmdCount + s_alphaOnCmdCount
draws_issued + zero_instance_skips == slots_considered
sorted_oob == 0
packet_oob == 0
type_oob == 0
base_instance_missing == 0
gl_errors == 0
ok == 1
tier1 5/5 PASS
```

Note: `draws_issued <= slots_considered` is always true (zero-instance slots are skipped).
`draws_issued == slots_considered` only if ALL types have ≥1 snapshot instance this frame,
which is unlikely on large missions. Do NOT require equality.

### Optional visual gate (non-blocking for smoke, but verify manually)

On mc2_01 and mc2_24: all props visible, no alpha-test fence/building flicker, no missing
geometry. Accept that GPU-cull instance count benefit is lost (may draw slightly more instances
than the multidraw path, but no under-draw).

---

## 11. Risks

| # | Risk | Mitigation |
|---|---|---|
| R1 | `GpuStaticPropPacket` field names differ from spec | Grep `struct GpuStaticPropPacket` before coding. Confirm `baseVertex`, `firstIndex`, `indexCount`, `owningTypeID`. |
| R2 | `s_typeRanges` not populated at insertion point | `uploadAllBucketsIfNeeded()` runs before coalesce branch; verify at insertion point. |
| R3 | `ARB_base_instance` absent | Explicit check; fall through to legacy multidraw (no suppression). Log `event=unsupported`. |
| R4 | `glUniform1i(s_locsCoalesce.drawIDBase, ...)` location is -1 | Log WARNING at arm time. GL no-ops the call but draw reads wrong PerDrawEntry. Must fix shader or confirm uniform present. |
| R5 | `firstIndex * sizeof(uint32_t)` overflow for large meshes | firstIndex is uint32; sizeof(uint32_t) = 4; product is uint64 max for firstIndex=1B. Cast to `uintptr_t` first. |
| R6 | base instance map ring-slot offset calculation | Arithmetic is in **bytes**: `s_coalesceFrameSlot * s_baseInstanceByCmdBytesPerFrame`. Cast to `uint8_t*`, add offset, then cast to `uint32_t*`. Do NOT use index arithmetic — `s_baseInstanceByCmdMap` is `void*`. |
| R7 | `s_texArrayOff` restore after loop | If loop runs alpha-ON group, leaves texture bound to `s_texArrayOn`. Must restore to `s_texArrayOff` for existing epilogue. |
| R8 | `s_baseInstanceByCmdMap` is nullptr under `s_globalPoolLegacy` | Check at gate-arm time (not per-slot). If nullptr, disarm v5 for session, fall through to multidraw. Per-slot null check masks this as 753× base_instance_missing. |
| R9 | SSBO binding 0 not set when `s_alphaOffCmdCount == 0` | v5 must rebind SSBO 0 unconditionally before loop. Prologue only binds inside `if (s_alphaOffCmdCount > 0u)`. |

---

## 12. Assumptions Requiring Verification Before Coding

1. `GpuStaticPropPacket` struct field names — grep `gos_static_prop_batcher.h`. Confirmed by adversarial: `firstIndex`, `indexCount`, `baseVertex`, `owningTypeID` all present.
2. `s_typeRanges` populates before the coalesce branch — `uploadAllBucketsIfNeeded()` at L3215 before coalesce branch. Confirmed.
3. Ring-slot byte arithmetic — `s_coalesceFrameSlot * s_baseInstanceByCmdBytesPerFrame` (bytes). Grep `prepareBaseInstanceTable()` to confirm `s_coalesceFrameSlot` and `s_baseInstanceByCmdBytesPerFrame` identifiers. Spec §5 shows exact arithmetic.
4. `s_locsCoalesce.drawIDBase` is non-(-1) — confirmed field at L416; populated at L634. Confirm the coalesce shader declares `u_drawIDBase` uniform.
5. `GLEW_ARB_base_instance` is available — confirmed: `<GL/glew.h>` included at L20 of batcher.cpp.
6. `s_texArrayOff` / `s_texArrayOn` — confirmed at L298-299.
7. `s_baseInstanceByCmdMap` is non-null at gate-arm — confirm `MC2_STATIC_PROP_GLOBAL_POOL_LEGACY` is NOT set in the test environment. Adversarial finding: if it IS set, v5 must disarm.
8. `s_coalesceFrameSlot` and `s_baseInstanceByCmdBytesPerFrame` exist as file-local variables — grep before coding.

---

## 13. Out of Scope

- Multidraw (v6+)
- GPU-cull instance count integration (v6.1 potential)
- Shadow pass (`flushShadow()`)
- `StaticPropDispatchMeta` sidecar struct (v5.5)
- `batcher_setOpaqueDispatchCandidates()` retirement (v6)
- Any new public batcher accessors (file-local only in v5)
- Default-on gate flip (separate follow-up commit after soak)
