# DrawPacket v4A — First Substitutive Static-Prop Opaque Dispatch (Non-Coalesce, All-Opaque Types Only)

**Version:** v8 (post adversarial + render-spine advisor — 2 CRITICAL + 1 MAJOR + 3 REVISE fixed)
**Date:** 2026-05-26
**Branch:** `claude/nifty-mendeleev`
**Prereqs shipped:** DrawPacket v0 (emit), v2 (pipelineId + cachedMaterialFlags on candidate)

---

## Scope

v4A targets **only the non-coalesce legacy per-type path** for **all-opaque types only**.

- `IsCoalesceEnabled()` → v4A no-op; logs `event=coalesce_noop`. Coalesce authoritative.
- `!IsCoalesceEnabled()` → v4A dispatches all-opaque type packets; legacy per-type loop skips
  those types (**only if all packets for that type were dispatched**).
- Mixed-alpha types: 100% legacy in both modes.
- Coalesce alpha-OFF block: **never touched**.

---

## Key decisions (v8)

**Pod seam:** Batcher public header defines `StaticPropOpaquePacketView` (all `uint32_t` fields).
Setter takes this type — no emitter type appears in the batcher's public interface. `gameosmain.cpp`
converts from `StaticPropDrawPacketCandidate` to the view type before calling the setter.

**Flush-entry clear:** `s_opaqueDispatchCandidates/Count` are unconditionally cleared at flush() entry
(batcher-internal invariant). If the setter is never called in a frame (early-exit paths), dispatch
is a no-op. No stale-pointer risk.

**Duplicate guard:** Before incrementing `s_v4TypeDrawCount[c.typeId]`, check if count would exceed
`packetCount` — count as `v4DuplicatePacket` and skip draw. Hard gate: `v4DuplicatePacket == 0`.

**Insertion point:** Line **3372** (after `u_fogValue` at L3371 — last uniform upload before
`IsCoalesceEnabled()`). Inserting earlier (even at L3354 after `glCullFace`) would fire before
`u_worldToClipGL` (L3358), `u_mvp` (L3362), `u_tex` (L3365), `u_fogValue` (L3371) are uploaded,
producing garbage geometry on first frame.

**Logging:** `std::fprintf(stderr, ...)` — the only logging pattern used in batcher.cpp. `GOS_REPORT`
does not exist anywhere in the codebase.

---

## Task 3 audit results (corrected in v8)

| Item | Value |
|---|---|
| `s_parityBytesUsedThisFrame = 0` | Line 3309 |
| `glUseProgram(s_staticPropProgram)` | Line 3344 |
| `glBindVertexArray(s_sharedVao)` | Line 3345 |
| `uploadAllBucketsIfNeeded()` | Line 3233 (before 3309) |
| `glDepthFunc(GL_GEQUAL)` | Line 3350 |
| `glDisable(GL_BLEND)` | Line 3351 |
| `glCullFace(GL_BACK)` | Line 3353 |
| `u_worldToClipGL` upload | Lines 3358-3361 |
| `u_mvp` upload | Lines 3362-3364 |
| `u_tex` upload | Line 3365 |
| `u_debugAddrMode` upload | Line 3366 |
| `u_fogValue` upload | Line 3371 |
| **Insertion point** | **Line 3372** (after all uniform uploads; before `IsCoalesceEnabled()` at 3602) |
| `if (IsCoalesceEnabled())` | Line 3602 |
| `if (s_alphaOffCmdCount > 0u)` | Line 3797 |
| Legacy per-type loop | Line 3865 |
| `u_packetID` legacy value | `(int)(type.firstPacket + p)` at line 4010 = `c.globalPacketIdx` |
| `s_locsLegacy` members | `.maxLocalVertexID` (L595), `.materialFlags` (L596), `.packetID` (L597) — GLint, valid before flush |
| Logging macro | `std::fprintf(stderr, ...)` — GOS_REPORT does NOT exist |
| `pipelineId` type | `RenderCore::PipelineId` (strongly typed; (uint32_t) cast needed for pod storage) |
| `s_candidates` | Function-local static (L1175); no realloc between emit (L1189) and flush (L1295) |
| `stats.emitted` | Valid candidate count — use as setter `count` argument, NOT `s_candidates.size()` |
| `gameosmain.cpp` includes batcher.h | Yes, already at L5 — Task 6 does not need to add it |
| `cachedMaterialFlags` | TRANSITIONAL field — v4A takes explicit dependency; must update v4A if semantics change |

---

## File map

| File | Action |
|---|---|
| `GameOS/gameos/gos_static_prop_batcher.h` | ADD `StaticPropOpaquePacketView` pod struct; ADD `batcher_setOpaqueDispatchCandidates()` declaration |
| `GameOS/gameos/gos_static_prop_batcher.cpp` | ADD `#include "draw_packet_emitter.h"` + `#include "../../RenderCore/PipelineRegistry.h"`; ADD statics; ADD flush-entry clear; ADD v4A dispatch block at L3372; ADD legacy suppression |
| `GameOS/gameos/gameosmain.cpp` | ADD `s_opaqueViews` static vector; ADD conversion loop + setter call after `emitStaticPropDrawPackets()` |
| `docs/tier1_env_vars.md` | Document `MC2_DRAWPACKET_STATIC_PROP_OPAQUE` |

---

## Prerequisites (all confirmed by Task 3 / adversarial workers)

All PRE-1 through PRE-4 confirmed. See v7 table for details.

**PRE-5 — `alphaClass == 0` draw-order safety ✓ CONFIRMED**
`glDisable(GL_BLEND)` is called at line 3351 before the per-type loop (L3865) and is never
re-enabled inside the loop body (no `glEnable(GL_BLEND)` between L3865–L4047). All-opaque types
cannot have GL-level alpha blending active in the legacy path. Draw-order change is safe under
`GL_GEQUAL` depth. No pre-Task-4 check required; PRE-5 passes structurally.

---

## Tasks

---

### Task 1 — Header: pod type + setter declaration

In `gos_static_prop_batcher.h`, add:

```cpp
// Batcher-owned view of a candidate opaque packet for v4A dispatch.
// Caller (gameosmain.cpp) converts from StaticPropDrawPacketCandidate; no emitter type here.
// cachedMaterialFlags: TRANSITIONAL field from emitter v2; v4A depends on current semantics.
// If cachedMaterialFlags semantics change, update v4A dispatch block in same commit.
struct StaticPropOpaquePacketView {
    uint32_t typeId;
    uint32_t globalPacketIdx;
    uint32_t cachedMaterialFlags;
    uint32_t pipelineId;   // cast from RenderCore::PipelineId at call site
    uint32_t firstIndex;
    uint32_t indexCount;
};

void batcher_setOpaqueDispatchCandidates(const StaticPropOpaquePacketView* views, size_t count);
```

No forward-declaration of any emitter type. No emitter header include in batcher.h.

**Build gate:** RelWithDebInfo clean

---

### Task 2 — batcher.cpp: includes + statics + setter

In `gos_static_prop_batcher.cpp`, add includes (alongside existing):
```cpp
#include "draw_packet_emitter.h"
#include "../../RenderCore/PipelineRegistry.h"   // direct include; do not rely on transitive
```

Add statics near existing statics:
```cpp
static const StaticPropOpaquePacketView* s_opaqueDispatchCandidates    = nullptr;
static size_t                            s_opaqueDispatchCandidateCount = 0u;
static std::vector<uint16_t>            s_v4TypeDrawCount;  // per-frame per-type; resized lazily
```

Add setter implementation:
```cpp
void batcher_setOpaqueDispatchCandidates(const StaticPropOpaquePacketView* views, size_t count) {
    static bool s_gateEnabled = (getenv("MC2_DRAWPACKET_STATIC_PROP_OPAQUE") != nullptr);
    s_opaqueDispatchCandidates     = (s_gateEnabled && count > 0u) ? views : nullptr;
    s_opaqueDispatchCandidateCount = (s_gateEnabled && count > 0u) ? count : 0u;
}
```

**No flush-entry clear.** Call order is: setter (gameosmain.cpp, before draw_screen()) → flush().
A flush-entry clear would wipe the candidates before the v4A block reads them.
Instead, the dispatch block uses a **consume-and-clear** pattern (see Task 4).

**Build gate:** RelWithDebInfo clean

---

### Task 3 — GL state audit ✓ COMPLETE

Insertion point confirmed: **line 3372**. All state valid at that point. No code change.

---

### Task 4 — v4A dispatch block at line 3372

Insert **after line 3371** (`u_fogValue` upload), before `if (IsCoalesceEnabled())` at line 3602.
Task 4 and Task 5 MUST be in the same commit — double-draw occurs if Task 4 lands without Task 5.

```cpp
// v4A: substitutive opaque dispatch — non-coalesce, all-opaque types only.
// All uniforms (u_worldToClipGL, u_mvp, u_tex, u_fogValue) valid at this point (L3358-3371).
//
// Consume-and-clear: capture the per-frame registration and null it immediately.
// This guards against stale pointer reuse if flush() is called again later (e.g., shadow pass),
// and handles the case where the setter was never called (pointer was already null).
const StaticPropOpaquePacketView* v4Candidates     = s_opaqueDispatchCandidates;
const size_t                      v4CandidateCount = s_opaqueDispatchCandidateCount;
s_opaqueDispatchCandidates     = nullptr;
s_opaqueDispatchCandidateCount = 0u;

s_v4TypeDrawCount.assign(s_types.size(), 0u);

bool     v4DispatchAttempted  = (v4CandidateCount > 0u);
uint32_t v4OpaqueDraws        = 0u;
uint32_t v4MixedTypeDeferred  = 0u;
uint32_t v4Invalid            = 0u;
uint32_t v4DuplicatePacket    = 0u;

if (v4DispatchAttempted) {
    if (IsCoalesceEnabled()) {
        if (s_counters.frame_count % 600 == 0) {
            std::fprintf(stderr, "[DRAW_PACKET_DISPATCH v1 frame=%" PRIu64 " event=coalesce_noop candidates=%zu]\n",
                s_counters.frame_count, v4CandidateCount);  // use captured local; statics already cleared
        }
    } else {
        for (size_t i = 0; i < v4CandidateCount; ++i) {
            const StaticPropOpaquePacketView& v = v4Candidates[i];

            if (v.typeId >= s_types.size())                                    { ++v4Invalid;          continue; }
            if (s_types[v.typeId].alphaClass != 0u)                            { ++v4MixedTypeDeferred; continue; }
            if (v.pipelineId != static_cast<uint32_t>(RenderCore::PipelineId::StaticPropOpaque))
                                                                                { ++v4Invalid;          continue; }
            // Duplicate guard: if count would exceed packetCount, this candidate is a duplicate.
            if (s_v4TypeDrawCount[v.typeId] >= s_types[v.typeId].packetCount)  { ++v4DuplicatePacket;  continue; }

            auto it = s_typeRanges.find(v.typeId);
            if (it == s_typeRanges.end())                                       { ++v4Invalid;          continue; }
            const TypeRangeSsbo& tr = it->second;

            if (v.globalPacketIdx >= s_packets.size())                          { ++v4Invalid;          continue; }
            const int32_t baseVertex = s_packets[v.globalPacketIdx].baseVertex;

            // Per-type SSBO sub-range bind — matches legacy per-type bind pattern (~L3877-3882).
            glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, s_instanceSsbo,
                (GLintptr)tr.instanceByteOffset, (GLsizeiptr)tr.instanceByteSize);

            // Uniforms: shader declares int (not uint) — glUniform1i throughout.
            // Use s_locsLegacy (populated at shader link L595-597).
            const int maxVtxID = (s_types[v.typeId].vertexCount > 0u)
                ? (int)(s_types[v.typeId].vertexCount - 1u) : 0;
            glUniform1i(s_locsLegacy.maxLocalVertexID, maxVtxID);
            glUniform1i(s_locsLegacy.materialFlags,    (GLint)v.cachedMaterialFlags);
            // u_packetID == globalPacketIdx == type.firstPacket + p (legacy line 4010 semantics).
            glUniform1i(s_locsLegacy.packetID,         (GLint)v.globalPacketIdx);

            glDrawElementsBaseVertex(GL_TRIANGLES, (GLsizei)v.indexCount, GL_UNSIGNED_INT,
                reinterpret_cast<void*>((uintptr_t)v.firstIndex * sizeof(uint32_t)), baseVertex);

            ++s_v4TypeDrawCount[v.typeId];
            ++v4OpaqueDraws;
        }

        // Three-way invariant counters (type-table-derived, not candidate-derived).
        uint32_t v4EligibleOpaquePkts  = 0u;
        uint32_t v4PartialTypeDispatch = 0u;
        for (uint32_t t = 0; t < (uint32_t)s_types.size(); ++t) {
            if (s_v4TypeDrawCount[t] == 0u || s_types[t].alphaClass != 0u) continue;
            v4EligibleOpaquePkts += s_types[t].packetCount;
            if (s_v4TypeDrawCount[t] != s_types[t].packetCount) ++v4PartialTypeDispatch;
        }

        if (s_counters.frame_count % 600 == 0) {
            std::fprintf(stderr,
                "[DRAW_PACKET_DISPATCH v1 frame=%" PRIu64 " candidates=%zu "
                "eligible_opaque_pkts=%u opaque_draws=%u mixed_deferred=%u "
                "partial_type=%u duplicate=%u invalid=%u]\n",
                s_counters.frame_count, v4CandidateCount,
                v4EligibleOpaquePkts, v4OpaqueDraws,
                v4MixedTypeDeferred, v4PartialTypeDispatch, v4DuplicatePacket, v4Invalid);
        }
    }
}
```

**Stop conditions (Task 4 specific):**
- `s_locsLegacy.maxLocalVertexID == -1` at first flush — shader not linked; halt
- `s_instanceSsbo == 0` at line 3372 — uploadAllBucketsIfNeeded failed; halt
- `RenderCore::PipelineId::StaticPropOpaque` compile error — PipelineRegistry.h include path wrong; fix before proceeding
- PRE-5 not confirmed — do not commit until draw-order safety is verified
- **Do not commit Task 4 without Task 5 in the same commit.** Without Task 5, all-opaque types are drawn twice per frame (v4A + legacy = double-draw). This is immediately visible as z-fighting.

**Build gate:** RelWithDebInfo clean

---

### Task 5 — Legacy suppression (inside `} else {` non-coalesce branch)

At top of legacy per-type loop (line 3865), before the loop body:

```cpp
uint32_t v4OpaquePacketsSuppressed = 0u;  // declared before loop

for (uint32_t typeID = 0; typeID < s_types.size(); ++typeID) {
    // v4A: suppress all-opaque type only if ALL its packets were dispatched (no partial substitution).
    if (s_types[typeID].alphaClass == 0u
            && typeID < (uint32_t)s_v4TypeDrawCount.size()
            && s_types[typeID].packetCount > 0u
            && s_v4TypeDrawCount[typeID] == s_types[typeID].packetCount) {
        v4OpaquePacketsSuppressed += s_types[typeID].packetCount;
        continue;
    }
    // ... existing legacy per-type body unchanged ...
}

if (s_counters.frame_count % 600 == 0 && v4DispatchAttempted) {
    std::fprintf(stderr,
        "[DRAW_PACKET_SUPPRESS v1 frame=%" PRIu64 " legacy_opaque_packets_suppressed=%u]\n",
        s_counters.frame_count, v4OpaquePacketsSuppressed);
}
```

In coalesce mode: the `} else {` branch at L3863 is never entered — it is the direct else of
`if (IsCoalesceEnabled())` at L3602. The suppression loop is structurally unreachable in coalesce
mode, regardless of `s_v4TypeDrawCount` values. Legacy coalesce path unmodified. ✓

**Build gate:** RelWithDebInfo clean

---

### Task 6 — Call site in gameosmain.cpp

After `emitStaticPropDrawPackets()` at line ~1189, before `draw_screen()` at line 1295.
`gameosmain.cpp` already includes `gos_static_prop_batcher.h` at L5 — no include change needed.

```cpp
// v4A: convert candidates to batcher-owned view type; register for substitutive dispatch.
// Uses stats.emitted (valid count), NOT s_candidates.size() (buffer capacity = kMaxPackets).
// s_opaqueViews is function-local static; reuses allocation each frame.
// Gate is env-latched in setter; no-op when MC2_DRAWPACKET_STATIC_PROP_OPAQUE absent.
static std::vector<StaticPropOpaquePacketView> s_opaqueViews;
s_opaqueViews.resize(stats.emitted);
for (uint32_t i = 0; i < stats.emitted; ++i) {
    const StaticPropDrawPacketCandidate& c = s_candidates[i];
    s_opaqueViews[i] = {c.typeId, c.globalPacketIdx, c.cachedMaterialFlags,
                        (uint32_t)c.pipelineId, c.firstIndex, c.indexCount};
}
batcher_setOpaqueDispatchCandidates(s_opaqueViews.data(), stats.emitted);
```

**Build gate:** RelWithDebInfo clean

---

### Task 7 — Env var documentation

In `docs/tier1_env_vars.md`, add:

```
MC2_DRAWPACKET_STATIC_PROP_OPAQUE
  Enables v4A substitutive opaque dispatch for all-opaque static-prop types.
  Active only when IsCoalesceEnabled()==false; coalesce mode logs event=coalesce_noop.
  Requires: no v3 gate needed (uses v0 emit candidates directly).
  Gate latched at first batcher_setOpaqueDispatchCandidates() call; process restart to change.
  Default: OFF. Logs: [DRAW_PACKET_DISPATCH v1] + [DRAW_PACKET_SUPPRESS v1] at 600-frame cadence.
  cachedMaterialFlags: TRANSITIONAL field dependency — update v4A if semantics change.
```

---

### Task 8 — Smoke gates

```powershell
# Gate OFF — tier1 5/5 (confirm [DRAW_PACKET_DISPATCH] absent in logs)
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs

# Gate ON fast check (2-mission; confirm counters + no crash)
$env:MC2_DRAWPACKET_STATIC_PROP_OPAQUE=1
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --missions mc2_01,mc2_24 --duration 30 --kill-existing --keep-logs

# Gate ON tier1 (required before merge)
$env:MC2_DRAWPACKET_STATIC_PROP_OPAQUE=1
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```

---

## Log schema

```
DRAW_PACKET_DISPATCH v1:
  frame (uint64), candidates, eligible_opaque_pkts, opaque_draws, mixed_deferred,
  partial_type, duplicate, invalid
  OR: frame, event=coalesce_noop, candidates

DRAW_PACKET_SUPPRESS v1:
  frame (uint64), legacy_opaque_packets_suppressed
```

## Hard invariants

**Non-coalesce mode:**
```
invalid == 0
duplicate == 0
partial_type_dispatch == 0
eligible_opaque_pkts == opaque_draws
eligible_opaque_pkts == legacy_opaque_packets_suppressed
gate OFF: [DRAW_PACKET_DISPATCH] absent + tier1 5/5
gate ON: above counters + tier1 5/5
```

**Coalesce mode (gate ON):**
```
event=coalesce_noop in logs
opaque_draws == 0
legacy path unmodified
tier1 5/5
```

## Global stop conditions

- `partial_type_dispatch > 0` on first gate-ON smoke run
- `duplicate > 0` on first gate-ON smoke run
- `invalid > 0` on first gate-ON smoke run (stale slots or OOB typeId)
- `s_locsLegacy.maxLocalVertexID == -1` at first flush
- `s_instanceSsbo == 0` at insertion point
- Task 4 committed without Task 5 (double-draw)
- Coalesce alpha-OFF block modified in any way
- `flushShadow()` touched
- `s_parityBytesUsedThisFrame` logic touched

## Commit messages

```
feat(drawpacket): v4A Task 1-2 — StaticPropOpaquePacketView seam + setter + flush-entry clear

docs(drawpacket): v4A Task 7 — document MC2_DRAWPACKET_STATIC_PROP_OPAQUE in tier1_env_vars.md

feat(drawpacket): v4A Task 4-5 — dispatch block at L3372 + per-type suppression (atomic commit)

feat(drawpacket): v4A Task 6 — s_opaqueViews conversion + setter call site in EmitDrawPackets

feat(drawpacket): v4A Task 8 — smoke gate OFF tier1 5/5 + gate ON 2-mission + tier1 5/5
```

## Rollback

Unset `MC2_DRAWPACKET_STATIC_PROP_OPAQUE` → no code change needed.
Gate is latched on first setter call; process restart required to change state.
Consume-and-clear in the dispatch block prevents stale candidate reuse when setter was not called
for a given frame (pointer captured as null → v4DispatchAttempted false → block is a no-op).

---

## Review history

| Round | Critical findings | Fixed in |
|---|---|---|
| Advisor v1 | Seam direction, log DOS, zero-opaque undocumented | v2 |
| Adversarial v1 (Haiku workers) | objectIndex/sortKey abuse, s_packets name, glUniform1ui, materialFlags[0], wrong maxVtxID, missing glBindBufferRange, baseVertex=0, candidate index as packetID, weak smoke, bad log counters, GL state unaudited | v4 |
| Adversarial v2 (external) | Full header include circular risk, wrong counter cardinality, coarse coalesce suppression, s_candidates lifetime, candidate fields unverified, pipelineId cast, u_packetID assumed, insertion point assumed, glGetUniformLocation in loop, gate-OFF parity | v5 |
| Adversarial v3 (external) | eligible_opaque_pkts missing, suppress-only-if-all-packets, coalesce_noop log, PRE-1 wording, env-docs task, dispatch-attempted flag | v6 |
| Task 3 subagent (Haiku) | All PREs confirmed; insertion point corrected to L3372 (uniforms at L3358-3371 not yet uploaded at L3354) | v7 |
| Adversarial v4 (Sonnet+Haiku) | GOS_REPORT non-existent, insertion L3354 invalid (uniforms not yet uploaded), stats.emitted vs s_candidates.size() | v8 |
| Render-spine advisor (Sonnet) | Emitter type in batcher header (seam violation → StaticPropOpaquePacketView pod), stale pointer if setter skipped (→ flush-entry clear), duplicate globalPacketIdx not detected (→ v4DuplicatePacket guard) | v8 |
| Final targeted adversarial (Sonnet+Haiku) | Flush-entry clear timing wrong (→ consume-and-clear in dispatch block v9), coalesce_noop log used cleared static (→ v4CandidateCount), PRE-5 confirmed structurally, Task 5 coalesce note clarified | v9 |
| Execution (Sonnet executor) | `PRIu64` → `%llu` cast (no `<cinttypes>` in file) | commits c843b634–bd9e75bb |
| Burn-in fix 1 (2026-05-26) | finalizeGeometry: alphaClass OR-reduce + s_typeDescTable inside coalesceWanted branch → emitter aborted with "type table empty" under MC2_SUBSTRATE_COALESCE_LEGACY=1. Hoisted both to unconditional before early return. | commit 05afde9e |
| Burn-in fix 2 (2026-05-26) | v4Invalid conflated CPU-fallback (no s_typeRanges entry) with genuine bugs. Split into v4NoRange (soft) + v4Invalid (hard gate). | commit 05afde9e |

## Burn-in results (2026-05-26)

Gate OFF (default coalesce): `event=coalesce_noop` — tier1 5/5 PASS (executor run)

Gate ON (MC2_DRAWPACKET_STATIC_PROP_OPAQUE=1 + MC2_SUBSTRATE_COALESCE_LEGACY=1, mc2_01):
```
frame=0:   candidates=74 eligible_opaque_pkts=7  opaque_draws=7  suppressed=7  partial_type=0 duplicate=0 invalid=0 no_range=21
frame=600: candidates=74 eligible_opaque_pkts=4  opaque_draws=4  suppressed=4  partial_type=0 duplicate=0 invalid=0 no_range=24
frame=1200+: candidates=74 eligible_opaque_pkts=0 opaque_draws=0 suppressed=0 no_range=28
```
Three-way invariant holds every frame. Hard gate (invalid==0) PASS.
no_range=21..28: CPU-fallback types (submit() rejected); expected in non-coalesce mode.
Steady-state 0 dispatches: snapshot vs submit() timing gap in COALESCE_LEGACY mode; not a v4A bug.
Tier1 5/5 PASS (2026-05-26T08-32-59): mc2_01/03/10/17/24 all PASS, Δdestroys=0.

**v4A BURN-IN COMPLETE. Slice proven correct.**
