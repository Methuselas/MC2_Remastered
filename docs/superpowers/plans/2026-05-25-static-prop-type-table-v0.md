# StaticPropTypeTable v0 — CPU Immutable Type Table Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a CPU-side immutable type table (`StaticPropTypeDesc`) that exposes per-type static-prop metadata (`firstPacket`, `packetCount`, `alphaClass`) across the GameOS/RenderCore seam without duplicating these facts per-instance.

**Architecture:** A 16-byte `StaticPropTypeDesc` struct lives in `RenderCore/`. A file-scope vector `s_typeDescTable` in `gos_static_prop_batcher.cpp` is populated at `finalizeGeometry()` after the alpha-class OR-reduce (line ~1622). Three free-function accessors follow the established `batcher_*` pattern in `gos_static_prop_batcher.h`. A candidate-log function (`batcher_buildCandidateLog`) iterates the table and proves correctness before any dispatch path is switched. `ExtractedStaticProp` fields are NOT removed this slice.

**Tech Stack:** C++17, OpenGL, no `std::span` (C++20 only — use raw pointer + count matching existing `batcher_getTypeDrawInfo` style). No SSBO, no shader changes, no cull-path changes.

**Build config:** `RelWithDebInfo` throughout — canonical validation config for this repo.

---

## File Map

| Action | File | Purpose |
|---|---|---|
| **Create** | `RenderCore/StaticPropTypeDesc.h` | 16-byte immutable per-type descriptor struct |
| **Modify** | `GameOS/gameos/gos_static_prop_batcher.h` | Forward-declare `StaticPropTypeDesc`; add 3 accessor + 1 log function declarations after existing `batcher_*` block (line ~335) |
| **Modify** | `GameOS/gameos/gos_static_prop_batcher.cpp` | Add `#include "../../RenderCore/StaticPropTypeDesc.h"` with other includes (line ~5); add `s_typeDescTable` vector near `s_types` (line ~196); populate in `finalizeGeometry()` after line 1657; implement accessors + log function; clear in `onMapUnload()` / `onMapLoad()` |

Do NOT touch:
- `render_snapshot.h` / `ExtractedStaticProp` — kept unchanged this slice
- Any shader file — no changes
- `gpu_cull_patch.comp` — no changes
- The coalesce flush path (lines ~3754–3819) — no changes
- The legacy flush loop (lines ~3822+) — only adds `batcher_buildCandidateLog()` call under env gate

---

## Task 1: `RenderCore/StaticPropTypeDesc.h`

**Files:**
- Create: `RenderCore/StaticPropTypeDesc.h`

- [ ] **Step 1.1: Create the header**

```cpp
// RenderCore/StaticPropTypeDesc.h
//
// Immutable per-type descriptor for static props.
// Lifetime: stable from GpuStaticPropBatcher::finalizeGeometry() to onMapUnload().
// Populated by gos_static_prop_batcher.cpp; consumed across the
// GameOS/RenderCore seam without pulling batcher internals into other TUs.
//
// v0: CPU-only. No SSBO binding, no shader access.
// See: docs/observations/2026-05-25-static-prop-type-table-design.md
#pragma once
#include <cstdint>

namespace RenderCore {

struct alignas(16) StaticPropTypeDesc {
    uint32_t typeId;       // dense index into s_types[]; matches GpuStaticPropInstance::typeID
    uint32_t firstPacket;  // index into s_packets[] (the shared IBO packet table)
    uint32_t packetCount;  // number of draw-packets for this type
    uint32_t alphaClass;   // 0=alpha-OFF, 1=alpha-ON (OR-reduce over packet materialFlags + textureAlpha)
};
static_assert(sizeof(StaticPropTypeDesc) == 16,
              "StaticPropTypeDesc must be 16 bytes");
static_assert(alignof(StaticPropTypeDesc) == 16,
              "StaticPropTypeDesc must be 16-byte aligned");

} // namespace RenderCore
```

- [ ] **Step 1.2: Commit**

```
git add RenderCore/StaticPropTypeDesc.h
git commit -m "feat(rendercore): add StaticPropTypeDesc 16-byte immutable per-type descriptor"
```

---

## Task 2: Wire `StaticPropTypeDesc` into the batcher

**Files:**
- Modify: `GameOS/gameos/gos_static_prop_batcher.cpp`
- Modify: `GameOS/gameos/gos_static_prop_batcher.h`

- [ ] **Step 2.1: Add include at the top of `gos_static_prop_batcher.cpp`**

The file's include block at lines 1–27 already contains:
```cpp
#include "../../RenderCore/MaterialGpu.h"   // MaterialGpu-2: sidecar upload
```
Add immediately after it (line ~5):
```cpp
#include "../../RenderCore/StaticPropTypeDesc.h"  // v0: cross-seam immutable type table
```

- [ ] **Step 2.2: Add `s_typeDescTable` near `s_types`**

Find the geometry table block at line ~194:
```cpp
// Geometry table (immutable after finalizeGeometry).
std::vector<GpuStaticPropPacket>                   s_packets;
std::vector<GpuStaticPropType>                     s_types;
std::unordered_map<const TG_TypeShape*, uint32_t>  s_typeIndex;
```
Add one line immediately after `s_typeIndex`:
```cpp
// Geometry table (immutable after finalizeGeometry).
std::vector<GpuStaticPropPacket>                   s_packets;
std::vector<GpuStaticPropType>                     s_types;
std::unordered_map<const TG_TypeShape*, uint32_t>  s_typeIndex;
std::vector<RenderCore::StaticPropTypeDesc>        s_typeDescTable; // v0: cross-seam mirror
```

- [ ] **Step 2.3: Forward-declare in `gos_static_prop_batcher.h`**

Line 14 already has:
```cpp
namespace RenderCore { struct MaterialGpu; }
```
Add immediately after:
```cpp
namespace RenderCore { struct StaticPropTypeDesc; }
```

- [ ] **Step 2.4: Add clears in `onMapUnload()` and `onMapLoad()`**

In `GpuStaticPropBatcher::onMapUnload()`, alongside the existing clears of `s_types`, `s_packets`, `s_typeIndex`:
```cpp
s_typeDescTable.clear();
```

In `GpuStaticPropBatcher::onMapLoad()`, at the top (defensive — prevents stale data if finalize is re-entered):
```cpp
s_typeDescTable.clear();
```

- [ ] **Step 2.5: Build clean**

```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64" --config RelWithDebInfo --target mc2 2>&1 | Select-Object -Last 30
```
Expected: zero errors, zero warnings.

- [ ] **Step 2.6: Commit**

```
git add GameOS/gameos/gos_static_prop_batcher.h GameOS/gameos/gos_static_prop_batcher.cpp
git commit -m "feat(batcher): add s_typeDescTable vector, include, forward-decl, and map clears"
```

---

## Task 3: Populate `s_typeDescTable` in `finalizeGeometry()`

**Files:**
- Modify: `GameOS/gameos/gos_static_prop_batcher.cpp`

- [ ] **Step 3.1: Find the insertion point**

In `finalizeGeometry()`, find lines 1655–1659 (after sort finishes, before byte-cursor loop):
```cpp
    s_alphaOnCount = static_cast<uint32_t>(s_sortedTypeOrder.size())
                   - s_alphaOffCount;

    {
        size_t offByteCursor = 0;
```
Insert between `s_alphaOnCount = ...` and the `{` that opens the byte-cursor loop.

- [ ] **Step 3.2: Insert population block**

```cpp
    // v0: Populate cross-seam type desc table.
    // Must run AFTER alpha-class OR-reduce (Step 5.6, line ~1621)
    // and sort (Step 5.7, line ~1647-1657) so all s_types[i] fields are stable.
    {
        s_typeDescTable.clear();
        s_typeDescTable.reserve(s_types.size());
        uint32_t invalid = 0u;
        uint32_t alphaOn = 0u;
        for (uint32_t i = 0u; i < static_cast<uint32_t>(s_types.size()); ++i) {
            const GpuStaticPropType& t = s_types[i];
            RenderCore::StaticPropTypeDesc desc{};
            desc.typeId      = i;
            desc.firstPacket = t.firstPacket;
            desc.packetCount = t.packetCount;
            desc.alphaClass  = static_cast<uint32_t>(t.alphaClass);
            if (t.packetCount == 0u) { ++invalid; }
            if (t.alphaClass  == 1u) { ++alphaOn; }
            s_typeDescTable.push_back(desc);
        }
        std::fprintf(stderr,
            "[STATIC_PROP_TYPE_TABLE v0] types=%u packet_ranges=%u"
            " alpha_on=%u invalid=%u\n",
            static_cast<uint32_t>(s_typeDescTable.size()),
            static_cast<uint32_t>(s_packets.size()),
            alphaOn, invalid);
    }
```

- [ ] **Step 3.3: Build clean**

```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64" --config RelWithDebInfo --target mc2 2>&1 | Select-Object -Last 30
```

- [ ] **Step 3.4: Quick single-mission smoke to verify log line**

```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --missions mc2_01 --duration 15 --kill-existing --keep-logs
```
Check the artifact log for:
```
[STATIC_PROP_TYPE_TABLE v0] types=N packet_ranges=P alpha_on=A noGeom=N
```
- `noGeom` is informational — non-visual prop types (collision volumes, triggers, sound emitters) register with no geometry. Expect noGeom > 0 on larger maps.
- `types > 0` is required — if 0, `finalizeGeometry()` returned early before the insertion point.

- [ ] **Step 3.5: Commit**

```
git add GameOS/gameos/gos_static_prop_batcher.cpp
git commit -m "feat(batcher): populate StaticPropTypeDesc table in finalizeGeometry + validation log"
```

---

## Task 4: Add accessors to header and implement in .cpp

**Files:**
- Modify: `GameOS/gameos/gos_static_prop_batcher.h`
- Modify: `GameOS/gameos/gos_static_prop_batcher.cpp`

- [ ] **Step 4.1: Add accessor declarations to the header**

Find the end of the existing `batcher_*` accessor block in `gos_static_prop_batcher.h` (after line ~335, where `batcher_getSortedPacketCount()` is declared). Add a new section:

```cpp
// ---------------------------------------------------------------------------
// Type-desc table accessors (v0: CPU-side only, no SSBO).
// All functions return safe sentinels (0 / nullptr / false) before
// GpuStaticPropBatcher::finalizeGeometry() runs or after onMapUnload().
// See: RenderCore/StaticPropTypeDesc.h for struct definition.
// See: docs/observations/2026-05-25-static-prop-type-table-design.md
// ---------------------------------------------------------------------------

// Number of entries in the type desc table (equals batcher_getTypeCount() post-finalize).
// Returns 0 if geometry not yet finalized.
uint32_t batcher_getStaticPropTypeDescCount();

// Copy one descriptor by typeId. typeId must be in [0, batcher_getStaticPropTypeDescCount()).
// Returns false if typeId is out of range or the table is empty.
// Returns the desc as-is regardless of packetCount (caller validates).
// out must not be null.
bool batcher_getStaticPropTypeDesc(uint32_t typeId, RenderCore::StaticPropTypeDesc* out);

// Direct read-only pointer to the entire table. outCount receives the entry count.
// Pointer is valid until the next onMapUnload(). outCount must not be null.
// Returns nullptr if geometry not finalized or table is empty.
const RenderCore::StaticPropTypeDesc* batcher_getStaticPropTypeDescTable(uint32_t* outCount);
```

- [ ] **Step 4.2: Implement accessors in .cpp**

Find the section where the existing `batcher_getTypeCount` and `batcher_getTypeDrawInfo` are implemented (grep for `batcher_getTypeCount` in the .cpp to locate them). Add the three new implementations immediately after:

```cpp
// ---------------------------------------------------------------------------
// Type-desc table accessors (v0)
// ---------------------------------------------------------------------------

uint32_t batcher_getStaticPropTypeDescCount() {
    return static_cast<uint32_t>(s_typeDescTable.size());
}

bool batcher_getStaticPropTypeDesc(uint32_t typeId, RenderCore::StaticPropTypeDesc* out) {
    if (!out) return false;
    if (typeId >= static_cast<uint32_t>(s_typeDescTable.size())) return false;
    *out = s_typeDescTable[typeId];
    return true;
}

const RenderCore::StaticPropTypeDesc* batcher_getStaticPropTypeDescTable(uint32_t* outCount) {
    if (!outCount) return nullptr;
    *outCount = static_cast<uint32_t>(s_typeDescTable.size());
    if (s_typeDescTable.empty()) return nullptr;
    return s_typeDescTable.data();
}
```

Note: `batcher_getStaticPropTypeDesc` returns the desc as-is (including `packetCount==0` slots) and lets the caller validate. `packetCount==0` is already flagged at finalizeGeometry via the `invalid` counter; the accessor does not re-filter.

- [ ] **Step 4.3: Build clean**

```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64" --config RelWithDebInfo --target mc2 2>&1 | Select-Object -Last 30
```

- [ ] **Step 4.4: Commit**

```
git add GameOS/gameos/gos_static_prop_batcher.h GameOS/gameos/gos_static_prop_batcher.cpp
git commit -m "feat(batcher): add batcher_getStaticPropTypeDesc* accessor trio"
```

---

## Task 5: Add `batcher_buildCandidateLog()` — prove the table path

This function iterates the type table, computes the expected packet coverage, and logs a per-frame summary plus optional detailed per-candidate lines. It does NOT issue GL calls. It proves the type table covers the same packet set as `s_types`.

**Files:**
- Modify: `GameOS/gameos/gos_static_prop_batcher.h`
- Modify: `GameOS/gameos/gos_static_prop_batcher.cpp`

- [ ] **Step 5.1: Add declaration to header**

In the same type-desc accessor section added in Task 4, append:

```cpp
// Emit candidate draw log derived from the type desc table.
// Per-frame: always logs a one-line [DRAW_CAND v0] summary (emitted/invalid/expected).
// Detailed per-packet [DRAW_CAND v0 detail] lines: first-frame only,
// OR every frame when MC2_TYPE_TABLE_CAND_VERBOSE=1.
// Does NOT issue GL calls; does NOT modify any state.
// gate: MC2_TYPE_TABLE_CAND_LOG=1
void batcher_buildCandidateLog();
```

- [ ] **Step 5.2: Implement in .cpp**

Add next to the other accessor implementations:

```cpp
void batcher_buildCandidateLog() {
    static const bool gate    = (getenv("MC2_TYPE_TABLE_CAND_LOG")     != nullptr);
    static const bool verbose = (getenv("MC2_TYPE_TABLE_CAND_VERBOSE") != nullptr);
    if (!gate) return;

    if (s_typeDescTable.empty()) {
        std::fprintf(stderr, "[DRAW_CAND v0] table_empty\n");
        return;
    }

    // Compute expected: sum of packetCount over all non-zero-packet entries.
    uint32_t expected = 0u;
    for (const auto& d : s_typeDescTable) { expected += d.packetCount; }

    static bool s_detailDone = false;
    const bool doDetail = verbose || !s_detailDone;

    uint32_t emitted = 0u;
    uint32_t invalid = 0u;
    for (const RenderCore::StaticPropTypeDesc& desc : s_typeDescTable) {
        if (desc.packetCount == 0u) { ++invalid; continue; }
        for (uint32_t p = 0u; p < desc.packetCount; ++p) {
            const uint32_t pktIdx = desc.firstPacket + p;
            if (pktIdx >= static_cast<uint32_t>(s_packets.size())) {
                if (doDetail) {
                    std::fprintf(stderr,
                        "[DRAW_CAND v0 detail] typeId=%u pkt=%u BOUNDS_OVERFLOW\n",
                        desc.typeId, p);
                }
                ++invalid;
                continue;
            }
            if (doDetail) {
                const GpuStaticPropPacket& pkt = s_packets[pktIdx];
                std::fprintf(stderr,
                    "[DRAW_CAND v0 detail] typeId=%u pkt=%u firstIndex=%u"
                    " indexCount=%u baseVertex=%d alphaClass=%u\n",
                    desc.typeId, p,
                    pkt.firstIndex, pkt.indexCount, pkt.baseVertex,
                    desc.alphaClass);
            }
            ++emitted;
        }
    }
    s_detailDone = true;

    // Summary line emitted every frame.
    // Gate: emitted==expected, invalid==0, no BOUNDS_OVERFLOW.
    std::fprintf(stderr,
        "[DRAW_CAND v0] emitted=%u expected=%u invalid=%u%s\n",
        emitted, expected, invalid,
        (emitted == expected && invalid == 0) ? " OK" : " MISMATCH");
}
```

- [ ] **Step 5.3: Wire call into `flush()` under env gate**

In `GpuStaticPropBatcher::flush()`, near the top (before the coalesce/legacy branch), add:
```cpp
batcher_buildCandidateLog();
```
The function is internally gated by `MC2_TYPE_TABLE_CAND_LOG`, so it is a no-op in production.

- [ ] **Step 5.4: Validate candidate log**

Run with the env gate on one mission:
```powershell
$env:MC2_TYPE_TABLE_CAND_LOG = "1"
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --missions mc2_01 --duration 10 --kill-existing --keep-logs
```
In the artifact log, check:
- First-frame detail lines present (no `BOUNDS_OVERFLOW`)
- Summary line every frame shows `OK`: `[DRAW_CAND v0] emitted=E expected=E invalid=0 OK`

The `emitted==expected` check is deterministic: `expected` is computed from the table as `sum(packetCount)`, and `emitted` counts successfully-walked packets. If they differ, a packet range is out of bounds.

Unset env var:
```powershell
Remove-Item env:MC2_TYPE_TABLE_CAND_LOG
```

- [ ] **Step 5.5: Build clean**

```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64" --config RelWithDebInfo --target mc2 2>&1 | Select-Object -Last 30
```

- [ ] **Step 5.6: Commit**

```
git add GameOS/gameos/gos_static_prop_batcher.h GameOS/gameos/gos_static_prop_batcher.cpp
git commit -m "feat(batcher): add batcher_buildCandidateLog — type table packet coverage proof"
```

---

## Task 6: Tier-1 smoke gate

**Goal:** Confirm zero regression across all 5 tier-1 missions.

- [ ] **Step 6.1: Run full tier-1 smoke**

```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```
Expected: exit 0, all 5 missions pass (`mc2_01`, `mc2_03`, `mc2_10`, `mc2_17`, `mc2_24`).

- [ ] **Step 6.2: Verify type table log in each mission**

In the smoke artifact logs, each mission's stderr must contain:
```
[STATIC_PROP_TYPE_TABLE v0] types=N packet_ranges=P alpha_on=A invalid=0
```
`invalid=0` is the hard gate. If non-zero, find which type has `packetCount==0` and trace through `registerType()` for that shape.

- [ ] **Step 6.3: Triage guide**

| Symptom | Likely cause | Fix |
|---|---|---|
| `types=0` | `finalizeGeometry()` returned early or insertion point is past an early-return | Add one stderr print before the population block to confirm entry |
| `noGeom > 0` | Types with no renderable geometry (collision, trigger, sound) — expected and normal. Hard failure only if `noGeom >= types` (all types empty). | Verify with `registerType()` trace if suspicious. |
| `emitted != expected` in candidate log | A packet's `firstPacket + p` overflows `s_packets.size()` | Check `finalizeGeometry()` packet range computation for that type |
| Smoke crash | Type table population or accessor touched freed memory | Check that `onMapUnload()` clear runs before the destructor of any accessor consumer |

- [ ] **Step 6.4: Commit fix if triage was needed**

```
git commit -m "fix(batcher): <specific cause>"
```

---

## Gates Summary

| Gate | Criterion |
|---|---|
| Build | Zero errors, zero warnings, `RelWithDebInfo` |
| Type table log (all 5 missions) | `noGeom` informational; `BOUNDS_OVERFLOW`=0 is hard gate |
| Candidate log (`MC2_TYPE_TABLE_CAND_LOG=1`, mc2_01) | `emitted==expected`, `invalid=0`, no `BOUNDS_OVERFLOW` |
| Tier-1 smoke | Exit 0, all 5 missions pass |
| `ExtractedStaticProp` | UNCHANGED (`alphaClass`, `firstPacket`, `packetCount` fields still present) |
| Shader files | UNCHANGED (zero shader diff) |
| GPU cull + coalesce paths | UNCHANGED |

---

## NOT in this slice

- Remove `ExtractedStaticProp.alphaClass`, `firstPacket`, `packetCount` — v3
- Upload type desc SSBO — v1 (separate plan)
- Per-frame instance range table (`StaticPropTypeFrame`) — v2 (separate plan)
- VS type-table indexing — v3
- Switch legacy flush loop source from `s_types` to type desc table — v0.5 (after this slice proves the table)
- `meshHandle` / `materialTableBase` pad fields — Stage B

---

## Design reference

`docs/observations/2026-05-25-static-prop-type-table-design.md`
