# DrawPacket v5 — Execution Plan

**Date:** 2026-05-26
**Spec:** docs/superpowers/specs/2026-05-26-drawpacket-v5-spec.md
**Gate:** MC2_DRAW_PACKET_COALESCE_V5=1
**Target file:** GameOS/gameos/gos_static_prop_batcher.cpp

---

## Pre-coding verification (run before Task 1)

The following identifiers were grepped from the batcher before writing this plan.
Re-run to confirm they still exist at these locations before any coding session starts.

| Symbol | Line | Notes |
|---|---|---|
| `s_coalesceFrameSlot` | L220 | `static uint32_t`, ring-slot index |
| `s_baseInstanceByCmdBytesPerFrame` | L393 | `size_t`, bytes per ring slot |
| `s_baseInstanceByCmdMap` | L392 | `void*`, persistent mapped pointer |
| `s_globalPoolLegacy` | L76 | `static const bool`, pool mode gate |
| `s_alphaOffCmdCount` | L329 | `uint32_t`, OFF group packet count |
| `s_alphaOnCmdCount` | L330 | `uint32_t`, ON group packet count |
| `s_sortedPacketOrder` | L328 | `std::vector<uint32_t>`, sorted slot → global packet index |
| `s_packets` | L198 | packet array |
| `s_typeRanges` | L3065 | `std::unordered_map<uint32_t, TypeRangeSsbo>` |
| `TypeRangeSsbo::instanceCount` | L194 | CPU-snapshot instance count per type |
| `s_locsCoalesce.drawIDBase` | L416 | `GLint`, uniform location (default -1) |
| `s_texArrayOff` | L298 | `GLuint`, alpha-OFF texture array |
| `s_texArrayOn` | L299 | `GLuint`, alpha-ON texture array |
| `s_staticPropProgramCoalesce` | L301 | `GLuint`, coalesce shader program |
| `s_coalesceInstanceSsbo` | coalesce SSBO | used in `glBindBufferRange` at L3875/L3881 |
| `fr_off_bytes_d` | local in flush() ~L3729 | `size_t`, ring-slot byte offset for SSBO bind |
| `off_total_bytes` / `on_total_bytes` | local in flush() ~L3733 | size values for legacy-mode per-group bind |
| `s_totalUsedBytesThisFrame` | L399 | `size_t`, global-pool mode total bytes |
| `glMultiDrawElementsIndirect` alpha-OFF | ~L3888 | insertion point — wrap in `else` |
| `glMultiDrawElementsIndirect` alpha-ON | ~L3910 | insertion point — wrap in `else` |

**Grep commands to run before coding:**
```powershell
# 1. Confirm ring-slot identifiers still present
grep -n "s_coalesceFrameSlot\|s_baseInstanceByCmdBytesPerFrame\|s_baseInstanceByCmdMap" GameOS/gameos/gos_static_prop_batcher.cpp | head -15

# 2. Confirm GpuStaticPropPacket field names
grep -n "firstIndex\|indexCount\|baseVertex\|owningTypeID" GameOS/gameos/gos_static_prop_batcher.h | head -20

# 3. Confirm both glMultiDrawElementsIndirect insertion points
grep -n "glMultiDrawElementsIndirect" GameOS/gameos/gos_static_prop_batcher.cpp

# 4. Confirm s_locsCoalesce.drawIDBase location
grep -n "drawIDBase" GameOS/gameos/gos_static_prop_batcher.cpp | head -10

# 5. Confirm GLEW_ARB_base_instance include present
grep -n "GL/glew.h" GameOS/gameos/gos_static_prop_batcher.cpp | head -5
```

---

## Tasks

### Task 1: Gate + extension latch

**Purpose:** Introduce the two static bool gates in the anonymous namespace.
No logic yet — just the variables. Every later task tests these booleans.

**Insertion point:** `GameOS/gameos/gos_static_prop_batcher.cpp` anonymous
namespace, near the other static const bools (around L76–L100, after
`s_globalPoolLegacy`).

**Code:**
```cpp
// DrawPacket v5: per-draw-call substitutive dispatch.
// Gate: MC2_DRAW_PACKET_COALESCE_V5=1
// Extension: ARB_base_instance (GL 4.2 core, available on all tier1 GPUs).
static const bool s_v5Enabled = []() -> bool {
    const char* v = std::getenv("MC2_DRAW_PACKET_COALESCE_V5");
    return v && v[0] == '1';
}();

// GLboolean: GL_TRUE (1) on any GL 4.2+ context; GL_FALSE on older/absent.
// Evaluated at first use of the anonymous namespace (before any GL call).
// Safe: GLEW is initialized before batcher code runs (gos_Init() order).
static const bool s_baseInstanceSupported = (GLEW_ARB_base_instance == GL_TRUE);
```

**Commit message:** `feat(draw-packet-v5): add s_v5Enabled + s_baseInstanceSupported gate bools`

**Smoke check:** Build succeeds. No new log lines in tier1 with gate off.
`grep "DRAW_PACKET_V5" <smoke-log>` returns empty.

---

### Task 2: Gate-arm checks + armed log

**Purpose:** On the first gate-ON flush, verify that the two preconditions
for safe v5 dispatch are met. If either fails, set a session flag that
permanently disarms v5 and falls through to multidraw. Log the arm outcome
once.

**Insertion point:** Inside `flush()`, inside the `IsCoalesceEnabled()` branch,
just before the `if (s_alphaOffCmdCount > 0u)` block at ~L3871.
Insert after the diagnostic log block ending at ~L3863.

**New file-local variables to add (anonymous namespace, near other frame
state variables ~L398–L420):**
```cpp
static bool s_v5Armed      = false;  // true once gate-arm checks have run
static bool s_v5Disarmed   = false;  // true if gate-arm check failed for session
```

**Code (in flush(), before L3871):**
```cpp
        // DrawPacket v5: gate-arm checks (run once per process on first gate-ON flush).
        if (s_v5Enabled && !s_v5Armed && !s_v5Disarmed) {
            s_v5Armed = true;
            bool disarm = false;

            // Precondition 1: base-instance map must be non-null.
            // Under MC2_STATIC_PROP_GLOBAL_POOL_LEGACY=1, s_baseInstanceByCmdSsbo is
            // never allocated and s_baseInstanceByCmdMap stays nullptr.
            // Do NOT check this per-slot — that silently produces base_instance_missing
            // for every slot and hides the real cause.
            if (s_baseInstanceByCmdMap == nullptr) {
                std::fprintf(stderr,
                    "[DRAW_PACKET_V5] event=disarmed reason=legacy_pool_no_baseinst_map\n");
                disarm = true;
            }

            // Precondition 2: u_drawIDBase uniform location must be valid.
            // If -1, glUniform1i is a GL no-op and the shader reads the wrong PerDrawEntry.
            if (!disarm && s_locsCoalesce.drawIDBase < 0) {
                std::fprintf(stderr,
                    "[DRAW_PACKET_V5] WARNING event=disarmed reason=drawid_loc_missing"
                    " loc=%d\n",
                    (int)s_locsCoalesce.drawIDBase);
                disarm = true;
            }

            if (disarm) {
                s_v5Disarmed = true;
            } else {
                const uint32_t totalSlots = s_alphaOffCmdCount + s_alphaOnCmdCount;
                std::fprintf(stderr,
                    "[DRAW_PACKET_V5] event=armed slots=%u ext_supported=%d"
                    " drawid_loc=%d\n",
                    totalSlots,
                    (int)s_baseInstanceSupported,
                    (int)s_locsCoalesce.drawIDBase);
            }
        }
```

**Commit message:** `feat(draw-packet-v5): add gate-arm precondition checks and armed log`

**Smoke check (gate OFF):** No `[DRAW_PACKET_V5]` lines in tier1 logs.

**Smoke check (gate ON, `MC2_DRAW_PACKET_COALESCE_V5=1`):**
- If `MC2_STATIC_PROP_GLOBAL_POOL_LEGACY` is NOT set: log shows
  `event=armed slots=<N> ext_supported=1 drawid_loc=<positive>`.
- `event=disarmed` should NOT appear in the normal test environment.
- The two `glMultiDrawElementsIndirect` calls are not yet gated — props render normally.

---

### Task 3: SSBO binding 0 + per-draw loop (core)

**Purpose:** Implement the actual substitutive dispatch. This is the only
task that changes rendering output. Both `glMultiDrawElementsIndirect` calls
move into an `else` branch; the `if` branch runs the per-draw loop.

**Insertion point:** Replace the alpha-OFF draw block (~L3871–L3891) and the
alpha-ON draw block (~L3894–L3913) with the structure below.

The existing code to wrap in `else`:
```cpp
        if (s_alphaOffCmdCount > 0u) {
            ...
            glMultiDrawElementsIndirect(...);
        }
        if (s_alphaOnCmdCount > 0u) {
            ...
            glMultiDrawElementsIndirect(...);
        }
```

**New variables to add in anonymous namespace (near error-counter state):**
```cpp
static uint32_t s_v5FrameDrawsIssued       = 0u;
static uint32_t s_v5FrameZeroInstSkips     = 0u;
static uint32_t s_v5FrameSortedOob         = 0u;
static uint32_t s_v5FramePacketOob         = 0u;
static uint32_t s_v5FrameTypeOob           = 0u;
static uint32_t s_v5FrameBaseInstMissing   = 0u;
static uint32_t s_v5FrameGlErrors          = 0u;
static uint32_t s_v5TotalFrameCount        = 0u;
```

**Full replacement code (drops in at the site of the two if-blocks):**
```cpp
        // DrawPacket v5: per-draw-call substitutive dispatch vs. multidraw.
        const bool runV5 = s_v5Enabled && s_baseInstanceSupported && !s_v5Disarmed;

        if (runV5) {
            // Reset per-frame counters.
            s_v5FrameDrawsIssued     = 0u;
            s_v5FrameZeroInstSkips   = 0u;
            s_v5FrameSortedOob       = 0u;
            s_v5FramePacketOob       = 0u;
            s_v5FrameTypeOob         = 0u;
            s_v5FrameBaseInstMissing = 0u;
            s_v5FrameGlErrors        = 0u;
            ++s_v5TotalFrameCount;

            const uint32_t totalCmds =
                s_alphaOffCmdCount + s_alphaOnCmdCount;

            // Compute base-instance map pointer for this ring slot.
            // s_baseInstanceByCmdMap is non-null (gate-arm check Task 2 guarantees this).
            // Arithmetic is in BYTES: multiply ring-slot index by bytes-per-frame, then
            // byte-cast the void* before offset. Do NOT use index arithmetic on void*.
            const size_t fr_off_bi =
                static_cast<size_t>(s_coalesceFrameSlot) *
                s_baseInstanceByCmdBytesPerFrame;
            const uint32_t* baseInstanceMap = reinterpret_cast<const uint32_t*>(
                static_cast<const uint8_t*>(s_baseInstanceByCmdMap) + fr_off_bi);

            // Unconditionally bind SSBO slot 0 before the loop.
            // The prologue at L3871 only binds inside (s_alphaOffCmdCount > 0u).
            // If all props are alpha-ON, slot 0 would be stale from a prior frame.
            //
            // v5 is always in global-pool mode here: gate-arm (Task 2) disarms v5
            // when s_baseInstanceByCmdMap==nullptr, which is the indicator that
            // s_globalPoolLegacy==true (legacy mode never allocates the SSBO).
            // So the global-pool bind path is the only reachable path.
            {
                const size_t totalUsed = s_totalUsedBytesThisFrame;
                glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, s_coalesceInstanceSsbo,
                                  static_cast<GLintptr>(fr_off_bytes_d),
                                  static_cast<GLsizeiptr>(
                                      totalUsed > 0 ? totalUsed
                                                    : sizeof(GpuStaticPropInstance)));
            }

            // Bind alpha-OFF texture array (may already be bound by prologue if
            // s_alphaOffCmdCount > 0; redundant here but safe).
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOff);

            bool enteredOnGroup = false;

            for (uint32_t i = 0u; i < totalCmds; ++i) {
                // Bounds guard: sorted-packet order array.
                if (i >= static_cast<uint32_t>(s_sortedPacketOrder.size())) {
                    ++s_v5FrameSortedOob;
                    continue;
                }
                const uint32_t globalPktIdx = s_sortedPacketOrder[i];

                // Bounds guard: packet array.
                if (globalPktIdx >= static_cast<uint32_t>(s_packets.size())) {
                    ++s_v5FramePacketOob;
                    continue;
                }
                const GpuStaticPropPacket& pkt = s_packets[globalPktIdx];

                // Bounds guard: type range map.
                const auto typeIt = s_typeRanges.find(pkt.owningTypeID);
                if (typeIt == s_typeRanges.end()) {
                    ++s_v5FrameTypeOob;
                    continue;
                }
                const uint32_t instanceCount = typeIt->second.instanceCount;

                // Zero-instance skip: type had no snapshot instances this frame.
                // Not a bug — expected for types present in layout but absent from snapshot.
                if (instanceCount == 0u) {
                    ++s_v5FrameZeroInstSkips;
                    continue;
                }

                // Base-instance bounds guard.
                if (i >= totalCmds) {
                    // Tautologically false here (loop condition), but kept as
                    // explicit guard matching spec §5 wording.
                    ++s_v5FrameBaseInstMissing;
                    continue;
                }
                const GLuint baseInstance = baseInstanceMap[i];

                // Group boundary: switch texture array when entering alpha-ON group.
                if (i == s_alphaOffCmdCount) {
                    glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOn);
                    enteredOnGroup = true;
                }

                // Per-draw uniform: absolute sorted slot.
                // In multidraw: u_drawIDBase = group-start offset, gl_DrawID = within-group index.
                // In per-draw:  u_drawIDBase = i,                  gl_DrawID = always 0.
                // Net shader index (gl_DrawID + u_drawIDBase) is identical in both paths.
                glUniform1i(s_locsCoalesce.drawIDBase, static_cast<GLint>(i));

                // Per-draw-call dispatch.
                // firstIndex * sizeof(uint32_t): cast to uintptr_t first to avoid
                // 32-bit overflow before the multiply (risk R5 from spec §11).
                glDrawElementsInstancedBaseVertexBaseInstance(
                    GL_TRIANGLES,
                    static_cast<GLsizei>(pkt.indexCount),
                    GL_UNSIGNED_INT,
                    reinterpret_cast<const void*>(
                        static_cast<uintptr_t>(pkt.firstIndex) * sizeof(uint32_t)),
                    static_cast<GLsizei>(instanceCount),
                    static_cast<GLint>(pkt.baseVertex),
                    baseInstance);

                ++s_v5FrameDrawsIssued;
            }

            // Restore texture array if loop entered the alpha-ON group.
            // Epilogue (L3919+) restores prevTex2DArray but we restore here
            // for state hygiene before the epilogue runs.
            if (enteredOnGroup) {
                glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOff);
            }

            // GL error check (gate-ON only — one glGetError per flush).
            {
                const GLenum err = glGetError();
                if (err != GL_NO_ERROR) {
                    ++s_v5FrameGlErrors;
                    std::fprintf(stderr,
                        "[DRAW_PACKET_V5] event=gl_error frame=%u err=0x%x\n",
                        s_v5TotalFrameCount, (unsigned)err);
                }
            }

        } else {
            // ---- Existing multidraw path (unchanged) ----
            // 11.7.g — alpha-OFF group draw.
            if (s_alphaOffCmdCount > 0u) {
                if (s_locsCoalesce.drawIDBase >= 0)
                    glUniform1i(s_locsCoalesce.drawIDBase, 0);
                if (s_globalPoolLegacy) {
                    glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, s_coalesceInstanceSsbo,
                                      static_cast<GLintptr>(fr_off_bytes_d + 0u),
                                      static_cast<GLsizeiptr>(off_total_bytes));
                } else {
                    const size_t totalUsed = s_totalUsedBytesThisFrame;
                    glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, s_coalesceInstanceSsbo,
                                      static_cast<GLintptr>(fr_off_bytes_d),
                                      static_cast<GLsizeiptr>(totalUsed > 0 ? totalUsed : sizeof(GpuStaticPropInstance)));
                }
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOff);
                glBindBuffer(GL_DRAW_INDIRECT_BUFFER, gpu_cull::compute_getIndirectCmdBuf());
                glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT,
                    reinterpret_cast<const void*>(static_cast<uintptr_t>(0)),
                    static_cast<GLsizei>(s_alphaOffCmdCount),
                    0);
            }

            // 11.7.h — alpha-ON group draw.
            if (s_alphaOnCmdCount > 0u) {
                if (s_locsCoalesce.drawIDBase >= 0)
                    glUniform1i(s_locsCoalesce.drawIDBase,
                                static_cast<GLint>(s_alphaOffCmdCount));
                if (s_globalPoolLegacy) {
                    glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, s_coalesceInstanceSsbo,
                                      static_cast<GLintptr>(fr_off_bytes_d + off_total_bytes),
                                      static_cast<GLsizeiptr>(on_total_bytes));
                }
                glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOn);
                glBindBuffer(GL_DRAW_INDIRECT_BUFFER, gpu_cull::compute_getIndirectCmdBuf());
                const uintptr_t alphaOnOffset =
                    static_cast<uintptr_t>(s_alphaOffCmdCount) *
                    static_cast<uintptr_t>(gpu_cull::kDrawElementsIndirectCommandSize);
                glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT,
                    reinterpret_cast<const void*>(alphaOnOffset),
                    static_cast<GLsizei>(s_alphaOnCmdCount),
                    0);
            }
        }
```

**Commit message:** `feat(draw-packet-v5): per-draw loop substituting glMultiDrawElementsIndirect when gate ON`

**Smoke check (gate OFF):** tier1 5/5 PASS. No `[DRAW_PACKET_V5]` lines except `event=armed` if
gate is on. No rendering regression vs. pre-Task-3 baseline.

**Smoke check (gate ON):** Props render. No blank world. Check
`[DRAW_PACKET_V5] event=dispatch_summary` (from Task 4) once it lands;
for now check `event=armed` present, no `event=gl_error` lines.

---

### Task 4: Diagnostics — per-frame summary

**Purpose:** Every 600 frames, emit one summary log line so soak runs can
verify all invariants from spec §10 without manual tracing.

**Insertion point:** Inside `flush()`, inside the `if (runV5)` block, after
the GL error check, before the closing brace of the `if (runV5)` block.

**Code:**
```cpp
            // Per-frame summary log: every 600 frames.
            if ((s_v5TotalFrameCount % 600u) == 0u) {
                const int ok = (s_v5FrameSortedOob       == 0u &&
                                s_v5FramePacketOob        == 0u &&
                                s_v5FrameTypeOob          == 0u &&
                                s_v5FrameBaseInstMissing  == 0u &&
                                s_v5FrameGlErrors         == 0u) ? 1 : 0;
                std::fprintf(stderr,
                    "[DRAW_PACKET_V5] frame=%u event=dispatch_summary"
                    " slots_considered=%u draws_issued=%u zero_instance_skips=%u"
                    " sorted_oob=%u packet_oob=%u type_oob=%u base_instance_missing=%u"
                    " gl_errors=%u ok=%d\n",
                    s_v5TotalFrameCount,
                    s_alphaOffCmdCount + s_alphaOnCmdCount,
                    s_v5FrameDrawsIssued,
                    s_v5FrameZeroInstSkips,
                    s_v5FrameSortedOob,
                    s_v5FramePacketOob,
                    s_v5FrameTypeOob,
                    s_v5FrameBaseInstMissing,
                    s_v5FrameGlErrors,
                    ok);
            }
```

**Commit message:** `feat(draw-packet-v5): add per-frame dispatch_summary log every 600 frames`

**Smoke check (gate ON, run 600+ frames):** Log contains
`event=dispatch_summary ... ok=1`. All oob/error counters zero.

---

### Task 5: Per-slot trace

**Purpose:** Verbose per-slot log for debugging individual draw decisions.
Only active when both `MC2_DRAW_PACKET_COALESCE_V5=1` and
`MC2_DRAW_PACKET_COALESCE_V5_TRACE=1` are set. Never fires with gate off.

**New file-local variable (anonymous namespace, near `s_v5Enabled`):**
```cpp
static const bool s_v5TraceEnabled = []() -> bool {
    const char* v = std::getenv("MC2_DRAW_PACKET_COALESCE_V5_TRACE");
    return v && v[0] == '1';
}();
```

**Insertion points inside the per-draw loop in Task 3:**

After `++s_v5FrameDrawsIssued;` (successful draw path):
```cpp
                if (s_v5TraceEnabled) {
                    std::fprintf(stderr,
                        "[DRAW_PACKET_V5] slot=%u type=%u inst=%u"
                        " base_inst=%u draw_id_base=%u\n",
                        i, pkt.owningTypeID, instanceCount,
                        (unsigned)baseInstance, i);
                }
```

After each `continue` in the skip paths (sorted_oob, packet_oob, type_oob,
zero_inst, base_instance_missing), add a corresponding trace line. Example
for zero_inst (add before the `continue`):
```cpp
                if (s_v5TraceEnabled) {
                    std::fprintf(stderr,
                        "[DRAW_PACKET_V5] event=skip slot=%u reason=zero_inst\n", i);
                }
```

Similarly for each other skip reason:
```cpp
                // sorted_oob skip trace:
                if (s_v5TraceEnabled) {
                    std::fprintf(stderr,
                        "[DRAW_PACKET_V5] event=skip slot=%u reason=sorted_oob\n", i);
                }

                // packet_oob skip trace:
                if (s_v5TraceEnabled) {
                    std::fprintf(stderr,
                        "[DRAW_PACKET_V5] event=skip slot=%u reason=packet_oob\n", i);
                }

                // type_oob skip trace:
                if (s_v5TraceEnabled) {
                    std::fprintf(stderr,
                        "[DRAW_PACKET_V5] event=skip slot=%u reason=type_oob\n", i);
                }

                // base_missing skip trace:
                if (s_v5TraceEnabled) {
                    std::fprintf(stderr,
                        "[DRAW_PACKET_V5] event=skip slot=%u reason=base_missing\n", i);
                }
```

**Commit message:** `feat(draw-packet-v5): add MC2_DRAW_PACKET_COALESCE_V5_TRACE per-slot log`

**Smoke check (gate OFF):** No trace output.

**Smoke check (gate ON, trace OFF):** No per-slot lines (only armed + summary).

**Smoke check (gate ON, trace ON, mc2_01 short run):**
- Every slot in `[0, totalCmds)` appears as either `slot=<N> type=...` (draw) or
  `event=skip slot=<N> reason=...`.
- `draws_issued + zero_instance_skips == slots_considered` in the summary.
- No `sorted_oob`, `packet_oob`, `type_oob`, or `base_missing` skip lines.

---

### Task 6: Smoke gate verification

**Purpose:** Confirm tier1 baseline is intact (gate OFF) and all spec §10
hard invariants pass (gate ON). This is a verification task, not a code task.
No commit — just a run record.

**Gate OFF verification:**
```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```
Expected: exit 0, 5/5 PASS, no `[DRAW_PACKET_V5]` in logs.

**Gate ON verification:**
```powershell
$env:MC2_DRAW_PACKET_COALESCE_V5 = "1"
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
$env:MC2_DRAW_PACKET_COALESCE_V5 = $null
```
Expected: exit 0, 5/5 PASS.

Then inspect smoke logs for all five missions and verify:

```
grep "DRAW_PACKET_V5" <smoke-log>
```

Required log lines per mission:
- `event=armed` (once per process, first flush with gate ON)
- `event=dispatch_summary` (at or before frame 600 if mission runs long enough; skip if <600 frames)

If `event=dispatch_summary` is present, assert all of the following:
- `slots_considered == s_alphaOffCmdCount + s_alphaOnCmdCount` (consistent within the frame)
- `draws_issued + zero_instance_skips == slots_considered`
- `sorted_oob == 0`
- `packet_oob == 0`
- `type_oob == 0`
- `base_instance_missing == 0`
- `gl_errors == 0`
- `ok=1`

No `event=disarmed` lines.
No `event=gl_error` lines.
No crash or hang.

**Optional visual gate (non-blocking for smoke):**
Manually drive camera on mc2_01 and mc2_24. Verify all props visible, no
missing geometry, no alpha-test fence/building flicker. Accept that
GPU-cull instance count benefit is absent (may show more instances than
multidraw path, but no under-draw).

---

## Gate-ON smoke verification (spec §10 invariants)

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

Note: `draws_issued == slots_considered` is only true if ALL types have >=1
snapshot instance this frame. Do NOT require equality — only require
`draws_issued + zero_instance_skips == slots_considered`.

---

## Commit sequence

1. `feat(draw-packet-v5): add s_v5Enabled + s_baseInstanceSupported gate bools`
2. `feat(draw-packet-v5): add gate-arm precondition checks and armed log`
3. `feat(draw-packet-v5): per-draw loop substituting glMultiDrawElementsIndirect when gate ON`
4. `feat(draw-packet-v5): add per-frame dispatch_summary log every 600 frames`
5. `feat(draw-packet-v5): add MC2_DRAW_PACKET_COALESCE_V5_TRACE per-slot log`

Task 6 (smoke verification) produces no commit — it is the exit criterion for the slice.
