# Modern Terrain TexResolveTable (Shape A / M0a) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Hoist the 96M-call `MC_TextureNode::get_gosTextureHandle` hot path into a per-frame lazy-memoized indirection table, behind `MC2_MODERN_TEX_RESOLVE` (default OFF), without changing any shader, GPU buffer, or submission code.

**Architecture:** Single CPU sidecar (`mclib/tex_resolve_table.{h,cpp}`) holding `DWORD handles[MC_MAXTEXTURES]`. `beginFrameTexResolve()` runs once per frame from the **top of `Terrain::geometry` in `mclib/terrain.cpp`** (the earliest terrain frame boundary; runs in mission-update phase, before any converted setup-time read). `endFrameTexResolve()` runs immediately after `mcTextureManager->renderLists()` returns in `GameCamera::render`, clearing `frameActive` so out-of-frame callers fall through to legacy until the next terrain frame begins. A `frameActive` flag (set by begin, cleared by end) protects mission-load, UI, and any other out-of-window inline callers of the `terrtxm{,2}.h` accessors. `tex_resolve(nodeId)` is a free-function inline that falls through to legacy when killswitch OFF, OOB, or `!frameActive`, otherwise lazily first-touches through the legacy accessor on first use that frame, memoizes, and returns indexed loads thereafter. Validate-mode runs both paths and asserts equality on **every** call (not only first-touch), to catch within-frame eviction by §7.2 legacy callsites.

**Tech Stack:** C++ (mclib), env-gated debug instrumentation per worktree CLAUDE.md, Tracy zones, Python smoke harness (`tests/smoke/run_smoke.py`). RelWithDebInfo build, MSVC 2022, CMake.

**Spec:** [`docs/superpowers/specs/2026-04-27-modern-terrain-tex-resolve-table-design.md`](../specs/2026-04-27-modern-terrain-tex-resolve-table-design.md). The spec is the source of truth for design decisions; this plan implements it.

**Operator constraints (from spec sign-off):**
1. First implementation default OFF.
2. `beginFrameTexResolve()` and `tex_resolve()` in one commit with instrumentation.
3. Convert §7.1 callsites mechanically.
4. Run validate mode before any default-ON decision.
5. Keep the residual-call census in the closing report (**grep-only; no per-callsite runtime counter** — see Task 4 Step 6).
6. Do not fold in `txmmgr.cpp:1228` static-shadow yet — separate follow-up if at all.

**Advisor corrections folded in (round 2):**
- Init site moved from `GameCamera::render` to top of `Terrain::geometry` (mission-update phase) because the converted setup-time callsites in `quad.cpp:setupTextures`, `mapdata.cpp:434`, and the `terrtxm{,2}.h` inline accessors fire during update, before render.
- Added `frameActive` flag — set by `beginFrameTexResolve` at the top of `Terrain::geometry`; out-of-frame inline-accessor callsites (mission-load, UI) fall through to legacy.
- Validate mode now runs both paths on **every** call (not only first-touch) to catch within-frame stale-memoization caused by §7.2 legacy reads triggering eviction.
- `MC2_MODERN_TEX_RESOLVE_VALIDATE` implies `enabled` (so a single env var is enough to trigger the bake-mode behavior).
- Added OOB guard: `nodeId >= MC_MAXTEXTURES` → fall through to legacy, print `event=oob_node` once per offending nodeId in trace mode.
- Residual-call census is grep-only; no per-callsite runtime counter (advisor: scope reduction).
- Closing report does not commit `.tracy` binary snapshots — only the extracted Tracy table and paths/SHA-256 of locally-stored captures.

**Advisor corrections folded in (round 3):**
- Added `endFrameTexResolve()` paired with `beginFrameTexResolve()`. Without it, `frameActive` was set but never cleared, so out-of-frame protection only held before the very first terrain frame ever ran — a half-implemented invariant. `endFrameTexResolve` is called immediately after `mcTextureManager->renderLists()` in `GameCamera::render` and clears `frameActive`.
- Moved 600-frame summary emission from `beginFrameTexResolve` to `endFrameTexResolve` so the summary counts *completed* frames, not *started* frames. `shutdownTexResolveTable` handles the edge case of teardown while a frame is still mid-flight.
- Fixed commit-message wording from "Always-on: 600-frame summary" to "Always-on: startup + shutdown; when enabled: per-frame summary every 600 frames" — when the killswitch is OFF, `beginFrameTexResolve` short-circuits and no summary fires.

**Note on testing.** This codebase has no unit-test framework wired up; verification uses the smoke harness, validate-mode runs, and Tracy captures. The TDD-shaped "write failing test → implement → green" loop is replaced by "write the validate-mode assert → run with killswitch ON+VALIDATE → confirm zero `event=mismatch` lines → smoke gate." Build verification is `mc2-build` (RelWithDebInfo per CLAUDE.md), then `mc2-deploy`, then `run_smoke.py`.

---

## Files

**Create:**
- `mclib/tex_resolve_table.h` — table struct, sentinel constants, `tex_resolve()` inline, `beginFrameTexResolve()` declaration, instrumentation env-var globals.
- `mclib/tex_resolve_table.cpp` — `g_texResolveTable` definition, `beginFrameTexResolve()` body with Tracy zone + `[TEX_RESOLVE v1]` startup/summary/shutdown prints.

**Modify:**
- `mclib/terrain.cpp` (top of `Terrain::geometry` body, near line 982 per findings §2.1) — call `beginFrameTexResolve(++s_frameCounter)` as the first statement of the function. This is the earliest terrain frame boundary and runs in the mission-update phase, before `quadSetupTextures` and any converted setup-time read.
- `code/gamecam.cpp` (immediately after the `mcTextureManager->renderLists()` call in `GameCamera::render`) — call `endFrameTexResolve()` to clear `frameActive` and emit the 600-frame summary tick. This closes the per-frame window so out-of-window inline-accessor callers (mission-load, UI, post-render screen capture) see `frameActive=false` and fall through to legacy.
- `mclib/quad.cpp:185, 193, 314, 322, 387, 395, 1647, 1791, 2005, 2147` — swap `mcTextureManager->get_gosTextureHandle(...)` → `tex_resolve(...)`.
- `mclib/mapdata.cpp:317, 323, 329, 335, 434` — same swap.
- `mclib/terrtxm.h:277, 285, 305, 313` — same swap.
- `mclib/terrtxm2.h:134, 142, 150, 157, 166` — same swap.
- `mclib/terrtxm2.cpp:2384` — same swap.
- `mclib/txmmgr.cpp:1316, 1321` — `masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle()` → `tex_resolve(masterVertexNodes[i].textureIndex)`. Only these two lines (Render.TerrainSolid arm). Other `txmmgr.cpp` sites stay legacy per spec §7.2.
- `CMakeLists.txt` (or the mclib sub-list, whichever owns mclib/) — add `tex_resolve_table.cpp` to the mclib target.

**Build / deploy:** `mc2-build` skill, then `mc2-deploy` skill (`A:/Games/mc2-opengl/mc2-win64-v0.2/`).

**Smoke:** `py -3 .claude/worktrees/nifty-mendeleev/scripts/run_smoke.py --tier tier1 --with-menu-canary --kill-existing`. Plus a Wolfman canary and a Magic-install canary per spec §13.

---

## Task 1: Create the sidecar header and implementation (commit 1, part A)

**Files:**
- Create: `mclib/tex_resolve_table.h`
- Create: `mclib/tex_resolve_table.cpp`
- Modify: the CMake list that owns mclib/ (run `grep -rn 'txmmgr\.cpp' --include=CMakeLists.txt --include='*.cmake'` to locate)

- [ ] **Step 1: Read the spec sections that drive this file**

Read `docs/superpowers/specs/2026-04-27-modern-terrain-tex-resolve-table-design.md` §5 (data structure), §6.2 (algorithm), §7 (read helper), §8 (killswitch + validate + frame-generation), §9 (instrumentation events), §10 AR6 + AR7 (sentinel and cap risks). The exact code below mirrors §6.2 / §7 from the spec.

- [ ] **Step 2: Write `mclib/tex_resolve_table.h`**

```cpp
//===========================================================================
// tex_resolve_table.h — Shape A (M0a) per-frame texture-handle memoization.
// Spec: docs/superpowers/specs/2026-04-27-modern-terrain-tex-resolve-table-design.md
//
// Lazy first-touch design: tex_resolve(nodeId) goes through the legacy
// MC_TextureManager::get_gosTextureHandle(nodeId) accessor on the first read
// of that node this frame, memoizes the result in handles[], and returns
// indexed loads for subsequent reads. Killswitch OFF reverts to legacy.
//===========================================================================
#pragma once

#include "txmmgr.h"   // for MC_MAXTEXTURES, mcTextureManager
#include "dstd.h"     // DWORD

struct TexResolveTable {
    static constexpr DWORD kSentinel = 0xFFFFFFFFu;

    DWORD       handles[MC_MAXTEXTURES];
    uint64_t    buildGeneration;
    uint32_t    resolvedThisFrame;
    bool        enabled;        // MC2_MODERN_TEX_RESOLVE on, OR validate on (validate implies enabled)
    bool        validate;       // MC2_MODERN_TEX_RESOLVE_VALIDATE — run both paths every call, compare
    bool        trace;          // MC2_MODERN_TEX_RESOLVE_TRACE — per-frame begin_frame line + oob_node prints
    bool        frameActive;    // set on first beginFrameTexResolve(); guards out-of-frame inline-accessor callers
};

extern TexResolveTable g_texResolveTable;
extern uint64_t        g_currentFrameId;

// Bounds-checked at compile time.
static_assert(sizeof(((TexResolveTable*)0)->handles) / sizeof(DWORD) >= MC_MAXTEXTURES,
              "g_texResolveTable.handles too small for MC_MAXTEXTURES");

// Reset table to all-sentinel + bump generation + set frameActive=true.
// Called once per frame as the FIRST statement of Terrain::geometry
// (mission-update phase — runs before any converted setup-time read).
void beginFrameTexResolve(uint64_t frameId);

// Clear frameActive and emit the 600-frame summary tick when due.
// Called immediately after mcTextureManager->renderLists() returns in
// GameCamera::render. Pairs with beginFrameTexResolve to make the
// "this is a terrain frame" window an explicit invariant.
void endFrameTexResolve(void);

// Initialize from env vars; print [TEX_RESOLVE v1] event=startup. Called
// once at engine init.
void initTexResolveTable(void);

// Print [TEX_RESOLVE v1] event=shutdown summary. Called from atexit / engine teardown.
void shutdownTexResolveTable(void);

// Out-of-line cold-path helpers (declared here, defined in .cpp).
void texResolveLogMismatch(DWORD nodeId, DWORD table_h, DWORD legacy_h);
void texResolveLogOOB(DWORD nodeId);

// Lazy first-touch resolve. The hot path.
inline DWORD tex_resolve(DWORD nodeId)
{
    // Killswitch OFF, or table not yet initialized this run, or out-of-frame
    // (mission-load / UI / non-terrain inline-accessor caller). Fall through
    // to legacy with bit-exact semantics.
    if (!g_texResolveTable.enabled || !g_texResolveTable.frameActive) {
        return mcTextureManager->get_gosTextureHandle(nodeId);
    }
    if (nodeId == 0xFFFFFFFFu) {
        return nodeId;   // matches MC_TextureManager::get_gosTextureHandle wrapper at txmmgr.h:528–531
    }
    // OOB guard. MC_MAXTEXTURES is the table cap; any nodeId beyond it would
    // index off the end of handles[]. Fall through to legacy and log once.
    if (nodeId >= MC_MAXTEXTURES) {
        texResolveLogOOB(nodeId);
        return mcTextureManager->get_gosTextureHandle(nodeId);
    }

    if (g_texResolveTable.validate) {
        // Validate mode: run both paths on EVERY call (not only first-touch),
        // compare, fall through to legacy result. This catches within-frame
        // stale-memoization caused by §7.2 out-of-scope legacy callsites
        // triggering CACHED_OUT_HANDLE eviction on a node we already memoized.
        // Performance does not matter in validate mode.
        DWORD legacy = mcTextureManager->get_gosTextureHandle(nodeId);
        DWORD memo   = g_texResolveTable.handles[nodeId];
        if (memo == TexResolveTable::kSentinel) {
            // First touch this frame — store and continue.
            g_texResolveTable.handles[nodeId] = legacy;
            g_texResolveTable.resolvedThisFrame++;
        } else if (memo != legacy) {
            texResolveLogMismatch(nodeId, memo, legacy);
            g_texResolveTable.handles[nodeId] = legacy;  // self-heal so subsequent reads converge
        }
        return legacy;
    }

    // Production hot path: lazy first-touch memoization.
    DWORD h = g_texResolveTable.handles[nodeId];
    if (h != TexResolveTable::kSentinel) {
        return h;   // memoized hit
    }
    h = mcTextureManager->get_gosTextureHandle(nodeId);
    g_texResolveTable.handles[nodeId] = h;
    g_texResolveTable.resolvedThisFrame++;
    return h;
}
```

- [ ] **Step 3: Write `mclib/tex_resolve_table.cpp`**

```cpp
//===========================================================================
// tex_resolve_table.cpp — sidecar implementation. See tex_resolve_table.h.
//===========================================================================
#include "tex_resolve_table.h"
#include "gos_profiler.h"   // ZoneScopedN
#include <stdio.h>
#include <stdlib.h>          // getenv
#include <string.h>          // memset

TexResolveTable g_texResolveTable = {};
uint64_t        g_currentFrameId  = 0;

namespace {
    uint64_t s_totalFrames         = 0;
    uint64_t s_totalResolves       = 0;
    constexpr uint64_t kSummaryEveryNFrames = 600;

    // Cold-path counters; throttled prints.
    uint64_t s_mismatchCount       = 0;
    uint64_t s_oobCount            = 0;
    constexpr uint64_t kMaxMismatchPrints = 32;
    constexpr uint64_t kMaxOobPrints      = 16;
}

void texResolveLogMismatch(DWORD nodeId, DWORD table_h, DWORD legacy_h)
{
    if (s_mismatchCount++ < kMaxMismatchPrints) {
        printf("[TEX_RESOLVE v1] event=mismatch frame=%llu nodeId=%u table=0x%08x legacy=0x%08x\n",
               (unsigned long long)g_texResolveTable.buildGeneration,
               (unsigned)nodeId, (unsigned)table_h, (unsigned)legacy_h);
        fflush(stdout);
    }
}

void texResolveLogOOB(DWORD nodeId)
{
    if (s_oobCount++ < kMaxOobPrints) {
        printf("[TEX_RESOLVE v1] event=oob_node frame=%llu nodeId=%u max=%d\n",
               (unsigned long long)g_texResolveTable.buildGeneration,
               (unsigned)nodeId, (int)MC_MAXTEXTURES);
        fflush(stdout);
    }
}

void initTexResolveTable(void)
{
    // Validate implies enabled — otherwise setting only MC2_MODERN_TEX_RESOLVE_VALIDATE
    // would print "validate" at startup but tex_resolve() would short-circuit on !enabled.
    g_texResolveTable.validate = (getenv("MC2_MODERN_TEX_RESOLVE_VALIDATE") != nullptr);
    g_texResolveTable.enabled  = g_texResolveTable.validate
                              || (getenv("MC2_MODERN_TEX_RESOLVE") != nullptr);
    g_texResolveTable.trace    = (getenv("MC2_MODERN_TEX_RESOLVE_TRACE") != nullptr);

    memset(g_texResolveTable.handles, 0xFF, sizeof(g_texResolveTable.handles));
    g_texResolveTable.buildGeneration   = 0;
    g_texResolveTable.resolvedThisFrame = 0;
    g_texResolveTable.frameActive       = false;  // armed by first beginFrameTexResolve

    const char* mode = "off";
    if (g_texResolveTable.validate)      mode = "validate";
    else if (g_texResolveTable.enabled)  mode = "on";

    printf("[TEX_RESOLVE v1] event=startup mode=%s max_textures=%d\n",
           mode, (int)MC_MAXTEXTURES);
    fflush(stdout);
}

void beginFrameTexResolve(uint64_t frameId)
{
    ZoneScopedN("Terrain.BeginFrameTexResolve");

    if (!g_texResolveTable.enabled) {
        // Killswitch OFF — table never read; skip the memset and leave
        // frameActive false so tex_resolve falls through bit-exactly to legacy.
        return;
    }

    g_currentFrameId                  = frameId;
    g_texResolveTable.buildGeneration = frameId;

    // Reset the running per-frame counter at frame open. End-of-frame will
    // accumulate it into s_totalResolves before zeroing.
    g_texResolveTable.resolvedThisFrame = 0;

    memset(g_texResolveTable.handles, 0xFF, sizeof(g_texResolveTable.handles));
    g_texResolveTable.frameActive = true;

    if (g_texResolveTable.trace) {
        printf("[TEX_RESOLVE v1] event=begin_frame frame=%llu\n",
               (unsigned long long)frameId);
        fflush(stdout);
    }
}

void endFrameTexResolve(void)
{
    ZoneScopedN("Terrain.EndFrameTexResolve");

    if (!g_texResolveTable.enabled || !g_texResolveTable.frameActive) {
        // Killswitch OFF, or beginFrameTexResolve never ran this frame
        // (e.g. mission-load path that gets to GameCamera::render without
        // having called Terrain::geometry). Nothing to close.
        return;
    }

    // Accumulate this frame's resolution count into the lifetime total
    // and bump the completed-frame counter. Counting at end (not begin)
    // means s_totalFrames reflects fully-rendered frames only.
    s_totalResolves += g_texResolveTable.resolvedThisFrame;
    s_totalFrames++;

    g_texResolveTable.frameActive = false;

    if ((s_totalFrames % kSummaryEveryNFrames) == 0) {
        const double avg_resolved = (double)s_totalResolves / (double)s_totalFrames;
        printf("[TEX_RESOLVE v1] event=summary frames=%llu resolved_per_frame_avg=%.1f mismatches=%llu oob=%llu\n",
               (unsigned long long)s_totalFrames,
               avg_resolved,
               (unsigned long long)s_mismatchCount,
               (unsigned long long)s_oobCount);
        fflush(stdout);
    }
}

void shutdownTexResolveTable(void)
{
    printf("[TEX_RESOLVE v1] event=shutdown total_frames=%llu total_resolves=%llu mismatches=%llu oob=%llu\n",
           (unsigned long long)s_totalFrames,
           (unsigned long long)s_totalResolves,
           (unsigned long long)s_mismatchCount,
           (unsigned long long)s_oobCount);
    fflush(stdout);
}
```

- [ ] **Step 4: Add the new TU to the mclib build target**

Locate the CMake target that owns `mclib/txmmgr.cpp`:

```bash
grep -rn 'txmmgr.cpp' --include=CMakeLists.txt --include='*.cmake' .
```

Add `tex_resolve_table.cpp` immediately after `txmmgr.cpp` in that list. Match indentation/style of the surrounding lines.

- [ ] **Step 5: Verify it compiles in isolation**

`mc2-build` skill (RelWithDebInfo). Expected: clean build, no warnings about the new file. If a warning fires about the `static_assert`, the cap shape has drifted — read `txmmgr.h` for the current `MC_MAXTEXTURES` definition before continuing.

---

## Task 2: Wire startup, frame-init, and shutdown call sites (commit 1, part B)

**Files:**
- Modify: `mclib/terrain.cpp` — top of `Terrain::geometry()` body (find with `grep -n 'void Terrain::geometry\|^Terrain::geometry' mclib/terrain.cpp`). Findings doc places it at `mclib/terrain.cpp:980–1193`. Insert as the **first statement** of the function, before the existing `vertexProjectLoop`/`quadSetupTextures` Tracy zones.
- Modify: a one-time engine-init site to call `initTexResolveTable()` (likely `MC_TextureManager::start()` at `mclib/txmmgr.cpp` — `grep -n 'void MC_TextureManager::start' mclib/txmmgr.cpp`).
- Modify: an engine-teardown site to call `shutdownTexResolveTable()` — pair with `MC_TextureManager::flush()` shutdown path or use `atexit(&shutdownTexResolveTable)` from `initTexResolveTable()`.

**Why not `GameCamera::render`:** the converted callsites in `quad.cpp:setupTextures`, `mapdata.cpp:434`, and the `terrtxm{,2}.h` inline accessors fire during `Terrain::geometry`, which runs from `mission->update()` *before* `GameCamera::render`. Initializing in render would leave the first wave of reads looking at last-frame state.

- [ ] **Step 1: Locate `Terrain::geometry`**

```bash
grep -n 'void Terrain::geometry\|^Terrain::geometry' mclib/terrain.cpp | head -5
```

Confirm the line number; the body begins shortly after the function signature. Read the first 30 lines of the function — confirm the existing `ZoneScopedN("Terrain::geometry")` (or equivalent parent zone) is at the top, since `beginFrameTexResolve` should land *before* any work in that function but *inside* its scope so the new sub-zone nests correctly.

- [ ] **Step 2: Add `beginFrameTexResolve` call at top of `Terrain::geometry`**

Insert as the first statement of the function body. Use a function-local static counter so the frame number is monotonic per call:

```cpp
void Terrain::geometry(/* existing args */)
{
    // existing top-of-function ZoneScoped (do not move)

    // Shape A (M0a) — per-frame texture-handle memoization. Initialized at
    // the EARLIEST terrain frame boundary because converted setup-time reads
    // in TerrainQuad::setupTextures, ensureTerrainFaceCacheEntryResident, and
    // terrtxm{,2}.h accessors fire during this function (mission-update phase),
    // before GameCamera::render. See
    // docs/superpowers/specs/2026-04-27-modern-terrain-tex-resolve-table-design.md.
    {
        static uint64_t s_texResolveFrameCounter = 0;
        beginFrameTexResolve(++s_texResolveFrameCounter);
    }

    // ... existing function body ...
}
```

Add `#include "tex_resolve_table.h"` at the top of `mclib/terrain.cpp` near the existing `#include "txmmgr.h"`.

- [ ] **Step 3a: Wire `endFrameTexResolve()` after the per-frame `renderLists()` flush**

The render-side close bracket lives in `code/gamecam.cpp:242`:

```cpp
mcTextureManager->renderLists();   // existing — sends triangles down to the card
```

Insert immediately after this line, before the closing `}` of the surrounding block:

```cpp
mcTextureManager->renderLists();
endFrameTexResolve();              // close the per-frame window — clears frameActive,
                                   // accumulates resolved-count, emits 600-frame summary
                                   // when due. No-op when killswitch OFF or already inactive.
```

Add `#include "tex_resolve_table.h"` near the existing mclib includes at the top of `gamecam.cpp` if not already present. (It is not — Task 2 Step 2 placed the begin call in `mclib/terrain.cpp`, not `gamecam.cpp`.)

**Defensive add at the second camera path:** `code/simplecamera.cpp:209` also calls `mcTextureManager->renderLists()` (a separate path used during briefing/cinematic cameras). Whether that path is invoked between `Terrain::geometry` and the main `GameCamera::render` flush is path-dependent; rather than reason about it, mirror the `endFrameTexResolve()` call after the simplecamera flush as well. Because `endFrameTexResolve()` short-circuits on `!frameActive` (the early-return guard in the cpp body), the call is a no-op when the simplecamera path isn't following an active terrain frame. Cost: one branch.

```cpp
if ( !drawOldWay )
    mcTextureManager->renderLists();
endFrameTexResolve();              // defensive — see plan Task 2 Step 3a.
```

Same `#include` line at top of `simplecamera.cpp`.

- [ ] **Step 3b: Add `initTexResolveTable()` to engine startup**

```bash
grep -n 'void MC_TextureManager::start' mclib/txmmgr.cpp
```

Add `initTexResolveTable();` at the end of `MC_TextureManager::start()`, after the existing init body. Add `#include "tex_resolve_table.h"` to `mclib/txmmgr.cpp`.

- [ ] **Step 3c: Add `shutdownTexResolveTable()` to teardown**

Preferred: register via `atexit(&shutdownTexResolveTable)` at the end of `initTexResolveTable()`. This avoids hunting for the right teardown site and is appropriate here because the function only prints a summary line — no resource ownership.

If `atexit` is forbidden by the engine's policy (read `mclib/`/`code/` for the convention; if any other subsystem registers `atexit` you're fine), pair with `MC_TextureManager::flush(false)` or the engine-quit banner.

- [ ] **Step 4: Build and run with killswitch OFF**

`mc2-build`, then `mc2-deploy`, then launch `mc2_01` from the deploy directory (NOT the source tree per memory `feedback_deploy_path.md`). Expected console output:

```
[TEX_RESOLVE v1] event=startup mode=off max_textures=3000
```

No `event=begin_frame`, no `event=summary`, no `event=oob_node` (because the table is short-circuited when `enabled=false`). Confirm the game runs identically to baseline — this is the bit-exact-legacy path.

- [ ] **Step 5: Build and run with killswitch ON**

```
set MC2_MODERN_TEX_RESOLVE=1
set MC2_MODERN_TEX_RESOLVE_TRACE=1
```

Launch. Expected:

```
[TEX_RESOLVE v1] event=startup mode=on max_textures=3000
[TEX_RESOLVE v1] event=begin_frame frame=1
[TEX_RESOLVE v1] event=begin_frame frame=2
...
[TEX_RESOLVE v1] event=summary frames=600 resolved_per_frame_avg=0.0 mismatches=0 oob=0
```

`resolved_per_frame_avg` is 0.0 at this point because no callsites are converted yet — the table is initialized every frame but never read. **This is the correct intermediate state.** Game must still render correctly because every read still goes through the legacy path.

**Validation observation (advisor-flagged):** confirm `Terrain::geometry` is called exactly once per rendered frame. If `mission->update()` runs a multi-tick catch-up loop, `beginFrameTexResolve` could fire multiple times between `renderLists()` flushes, which would memset the table mid-frame and silently lose memoizations made by setup-time reads in earlier ticks.

To check: after a 60-second `mc2_01` standard-zoom run with both `MC2_MODERN_TEX_RESOLVE=1` and `MC2_MODERN_TEX_RESOLVE_TRACE=1`, count begin lines and summary frame counts:

```bash
grep -c 'event=begin_frame' run.log    # B
grep    'event=summary'    run.log     # last frames=N
```

If `B == N` (within ±a small handful from setup-only frames), the 1:1 invariant holds and the design is fine. If `B > N + small`, `Terrain::geometry` is firing more than once per render frame; in that case, before proceeding to Task 3, gate `beginFrameTexResolve` on a "first geometry call since last `endFrameTexResolve`" predicate (track via a `bool s_geometryRanThisRender` static set in `beginFrameTexResolve` and cleared in `endFrameTexResolve`). Document the finding in the closing report regardless of which way it goes.

- [ ] **Step 6: Commit (part 1 of commit 1)**

Hold the commit until Task 3 is also done, so the sidecar lands together with at least the first batch of converted callsites. (Per operator constraint 2: "in one commit with instrumentation.")

---

## Task 3: Convert the §7.1 callsites mechanically (commit 1, part C)

**Files (modify, in this order):**
- `mclib/quad.cpp` (10 sites)
- `mclib/mapdata.cpp` (5 sites)
- `mclib/terrtxm.h` (4 sites)
- `mclib/terrtxm2.h` (5 sites)
- `mclib/terrtxm2.cpp` (1 site)
- `mclib/txmmgr.cpp` (2 sites — Render.TerrainSolid only; lines 1316, 1321)

Each file gets `#include "tex_resolve_table.h"` once.

- [ ] **Step 1: Pre-conversion grep audit (baseline for residual-call census)**

Save the baseline list of every `get_gosTextureHandle` callsite to a file in the worktree:

```bash
grep -rn 'get_gosTextureHandle\s*(' mclib/ GameOS/gameos/ code/ \
    | grep -v 'tex_resolve_table' \
    > docs/superpowers/plans/progress/2026-04-27-tex-resolve-baseline-callsites.txt
```

This is the pre-Shape-A snapshot. Commit this file alongside the implementation so future audits can diff.

- [ ] **Step 2: Convert `mclib/quad.cpp`**

The 10 sites are at lines 185, 193, 314, 322, 387, 395, 1647, 1791, 2005, 2147. All are of the form `mcTextureManager->get_gosTextureHandle(<expr>)`. Mechanical replacement:

| Before | After |
|---|---|
| `mcTextureManager->get_gosTextureHandle(TerrainQuad::mineTextureHandle);` | `tex_resolve(TerrainQuad::mineTextureHandle);` |
| `mcTextureManager->get_gosTextureHandle(TerrainQuad::blownTextureHandle);` | `tex_resolve(TerrainQuad::blownTextureHandle);` |
| `mcTextureManager->get_gosTextureHandle(mineTextureHandle);` | `tex_resolve(mineTextureHandle);` |
| `mcTextureManager->get_gosTextureHandle(blownTextureHandle);` | `tex_resolve(blownTextureHandle);` |
| `const DWORD overlayTexId = mcTextureManager->get_gosTextureHandle(overlayHandle);` | `const DWORD overlayTexId = tex_resolve(overlayHandle);` |

Add `#include "tex_resolve_table.h"` at the top of `mclib/quad.cpp` near the existing `#include "txmmgr.h"`.

- [ ] **Step 3: Convert `mclib/mapdata.cpp`**

5 sites at lines 317, 323, 329, 335, 434. All the same shape. Replace and add include.

- [ ] **Step 4: Convert `mclib/terrtxm.h` and `mclib/terrtxm2.h`**

4 sites in `terrtxm.h` (277, 285, 305, 313); 5 sites in `terrtxm2.h` (134, 142, 150, 157, 166). These are inline accessor methods. Replace `mcTextureManager->get_gosTextureHandle(...)` with `tex_resolve(...)`. Add the include at the top of each header.

**Subtlety:** because these are headers, the include propagates to every TU that uses them. That's fine — `tex_resolve_table.h` already includes `txmmgr.h`, so the existing dependency footprint is unchanged.

- [ ] **Step 5: Convert `mclib/terrtxm2.cpp`**

1 site at line 2384. Same shape. Add include.

- [ ] **Step 6: Convert `mclib/txmmgr.cpp` Render.TerrainSolid arm only**

Lines 1316 and 1321 — both:
```cpp
gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
```
become:
```cpp
gos_SetRenderState( gos_State_Texture, tex_resolve(masterVertexNodes[i].textureIndex));
```

**Do NOT convert any other site in `txmmgr.cpp` in this commit.** Per operator constraint 6, line 1228 (Shadow.StaticAccum) stays legacy. Lines 1114/1125 (Render.3DObjects), 1429+ (water/alpha/decals/overlays) all stay legacy per spec §7.2.

The include is already present (`tex_resolve_table.h` is in the same directory); add it near the existing local includes if not.

- [ ] **Step 7: Build and verify clean compile**

`mc2-build`. If any TU fails to find `tex_resolve`, the include is missing — add it.

- [ ] **Step 8: Post-conversion grep audit (residual-call census, raw)**

```bash
grep -rn 'get_gosTextureHandle\s*(' mclib/ GameOS/gameos/ code/ \
    | grep -v 'tex_resolve_table' \
    > docs/superpowers/plans/progress/2026-04-27-tex-resolve-residual-callsites.txt
```

Diff against the baseline:

```bash
diff docs/superpowers/plans/progress/2026-04-27-tex-resolve-baseline-callsites.txt \
     docs/superpowers/plans/progress/2026-04-27-tex-resolve-residual-callsites.txt
```

Expected: every removed line is one of the §7.1 conversions; every retained line is one of the §7.2 intentional out-of-scope sites. If anything else differs, find it and fix it before committing.

- [ ] **Step 9: Run with killswitch OFF, then ON**

OFF: identical to baseline behavior (the converted callsites fall through `tex_resolve` to legacy).
ON: game still renders correctly; `[TEX_RESOLVE v1] event=summary` shows `resolved_per_frame_avg` greater than zero (probably tens to a few hundred — one entry per distinct terrain material per frame).

If Wolfman or `mc2_01` shows visible regression at this point, the conversion is wrong somewhere. Bisect by reverting individual files (commit them separately if uncertain).

- [ ] **Step 10: Commit (single commit for sidecar + conversions + instrumentation)**

```bash
git add mclib/tex_resolve_table.h mclib/tex_resolve_table.cpp \
        mclib/terrain.cpp code/gamecam.cpp code/simplecamera.cpp \
        mclib/quad.cpp mclib/mapdata.cpp \
        mclib/terrtxm.h mclib/terrtxm2.h mclib/terrtxm2.cpp \
        mclib/txmmgr.cpp \
        docs/superpowers/plans/progress/2026-04-27-tex-resolve-baseline-callsites.txt \
        docs/superpowers/plans/progress/2026-04-27-tex-resolve-residual-callsites.txt \
        <CMakeLists.txt path>

git commit -m "$(cat <<'EOF'
feat: Shape A TexResolveTable — per-frame lazy memoization of MC_TextureNode handles

Hoists the 96M-call MC_TextureNode::get_gosTextureHandle hot path
(per the Tracy baseline snapshot in the findings doc) into a
per-frame lazy-memoized table keyed by the existing DWORD textureIndex.

Each tex_resolve(nodeId) goes through the legacy accessor on first
use that frame and memoizes the result for the rest of the frame.
Repeated reads of the same texture index collapse to one legacy
call per frame instead of N. No GPU buffer change, no shader change,
no submission change. Default OFF behind MC2_MODERN_TEX_RESOLVE.

Conversion list (terrain-solid read path): mclib/quad.cpp (10 sites),
mclib/mapdata.cpp (5), mclib/terrtxm.h (4), mclib/terrtxm2.h (5),
mclib/terrtxm2.cpp (1), mclib/txmmgr.cpp Render.TerrainSolid arm
(2 sites: 1316, 1321). Other txmmgr arms (Render.3DObjects, water/
alpha/decals/overlays, Shadow.StaticAccum) intentionally stay legacy
per the spec §7.2.

Instrumentation env-gated per worktree CLAUDE.md "Debug
Instrumentation Rule for reworks": [TEX_RESOLVE v1] startup /
begin_frame / summary / shutdown / mismatch / oob_node events.
Always-on: startup + shutdown. When MC2_MODERN_TEX_RESOLVE is
enabled (or implied via MC2_MODERN_TEX_RESOLVE_VALIDATE): per-frame
summary every 600 completed frames, emitted from endFrameTexResolve.

Spec: docs/superpowers/specs/2026-04-27-modern-terrain-tex-resolve-table-design.md
Plan: docs/superpowers/plans/2026-04-27-modern-terrain-tex-resolve-table.md

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Validate-mode capture + Tracy A/B + smoke + closing report (commit 2)

**Files (create):**
- `docs/superpowers/plans/progress/2026-04-27-tex-resolve-closing-report.md` — validate-mode result, Tracy A/B numbers, residual-call census interpretation, promotion recommendation.

- [ ] **Step 1: Run validate mode on `mc2_01` standard zoom**

```bash
set MC2_MODERN_TEX_RESOLVE=1
set MC2_MODERN_TEX_RESOLVE_VALIDATE=1
set MC2_HEARTBEAT=1
```

Launch `mc2_01` from `A:/Games/mc2-opengl/mc2-win64-v0.2/`. Play passively for at least 60 seconds. Save the console log to `docs/superpowers/plans/progress/2026-04-27-validate-mc2_01.log`.

Pass criterion: zero `[TEX_RESOLVE v1] event=mismatch` lines, zero `event=stale_generation` lines. The `event=summary` line shows `mismatches=0`.

If any mismatch fires: read the spec AR7 (sentinel collision) and AR1 (live-handle violation) — one of those is the explanation. Stop; do not promote until root-caused.

- [ ] **Step 2: Run validate mode in Wolfman zoom**

Same env. Launch `mc2_01`, switch to Wolfman zoom (the standard hotkey), play passively 60 seconds. Save log to `docs/superpowers/plans/progress/2026-04-27-validate-wolfman.log`.

Same pass criterion. Wolfman is the perf claim target — if mismatch is going to fire anywhere, it's here (40000 visible verts, more material variety per frame).

- [ ] **Step 3: Run validate mode on a Magic mission**

Per spec §13 and worktree CLAUDE.md memory `magic_abl_contamination_rule.md`: do NOT ship loose `corebrain.abx` in the deploy. Use the standard Magic install canary mission. 60 seconds passive. Save log.

- [ ] **Step 4: Tracy capture A/B**

Two captures, identical `mc2_01` Wolfman 60s session. **Do not commit `.tracy` binary snapshots into the repo** — they're large opaque blobs and the project has no convention for storing them. Save them locally and reference by path + SHA-256 in the closing report.

Local-only paths:
- A: `MC2_MODERN_TEX_RESOLVE=0` (or unset). Save to `tracy_captures/2026-04-27-tracy-A-killswitch-off.tracy` (gitignored or outside the worktree).
- B: `MC2_MODERN_TEX_RESOLVE=1`. Save to `tracy_captures/2026-04-27-tracy-B-killswitch-on.tracy`.

Open both in Tracy GUI. Record the Self-time per zone for the spec §11.1 zones in a table that goes into the closing report:

| Zone | A (off) | B (on) | Δ |
|---|---|---|---|
| `MC_TextureNode::get_gosTextureHandle` | | | |
| `TerrainQuad::setupTextures resolveFallback` | | | |
| `TerrainQuad::setupTextures cachedVisibleSubmission` | | | |
| `TerrainColorMap::getTextureHandle realizeTexture` | | | |
| `Terrain.DrawPatches` (GPU) | | | |
| `Render.TerrainSolid` (CPU) | | | |
| `Terrain.BeginFrameTexResolve` | n/a | | |

Compute the combined delta. Pass threshold per spec §11.1: combined (`get_gosTextureHandle` + `resolveFallback` + `realizeTexture`) self-time delta ≥ 0.20 ms/frame at Wolfman, no other tracked zone regressing >5%, `Terrain.BeginFrameTexResolve` <5 µs/frame.

Capture SHA-256 of the two `.tracy` files (`certutil -hashfile <file> SHA256` on Windows) and embed those in the closing report so future readers can verify the captures referenced.

- [ ] **Step 5: Run the smoke gate**

```bash
py -3 .claude/worktrees/nifty-mendeleev/scripts/run_smoke.py \
    --tier tier1 --with-menu-canary --kill-existing
```

Exit 0 = pass. Exit nonzero = inspect `tests/smoke/artifacts/<timestamp>/`. Tier1 covers `mc2_01, mc2_03, mc2_10, mc2_17, mc2_24` plus the menu canary (per worktree CLAUDE.md).

Run smoke twice: once with `MC2_MODERN_TEX_RESOLVE=0`, once with `=1`. Both must exit 0.

- [ ] **Step 6: Write the closing report**

Create `docs/superpowers/plans/progress/2026-04-27-tex-resolve-closing-report.md` with:

1. **Validate-mode results.** Three log filenames + zero-mismatch confirmation per run. Or, if mismatch was found, the root-cause analysis. Note: validate mode now compares **every** call (not only first-touch), so any within-frame eviction by §7.2 legacy callers would surface here.
2. **Tracy A/B table** filled in. Pass / fail vs the spec §11.1 threshold. Include the SHA-256 of the local `.tracy` capture files referenced (the binary snapshots themselves are NOT committed).
3. **Smoke gate results.** Tier1 + menu canary outcome under both killswitch states.
4. **Residual-call census interpretation.** Diff between `2026-04-27-tex-resolve-baseline-callsites.txt` and `2026-04-27-tex-resolve-residual-callsites.txt` (committed in Task 3 Step 8 — these are grep-only census artifacts, not runtime counters). Confirm every retained legacy callsite is on the spec §7.2 list. Confirm no §7.1 callsite was missed.
5. **`oob` and `mismatch` counts** from the `[TEX_RESOLVE v1] event=summary` and `event=shutdown` lines. Both must be zero.
6. **Promotion recommendation.** One of:
   - "Promote to default-ON in a follow-up commit" (all gates pass).
   - "Hold at default-OFF; re-evaluate" (Tracy delta below threshold, but no correctness regression — Shape A still useful as infrastructure for Shape B).
   - "Revert" (correctness regression).
7. **Followups identified.** Notably: should `txmmgr.cpp:1228` Shadow.StaticAccum opt in (one-line follow-up commit)? Should the static-shadow path be left alone until a dedicated shadow slice? Plan-Open-Question OQ-Plan-5 from the spec.

- [ ] **Step 7: Commit the report (NOT the .tracy snapshots)**

```bash
git add docs/superpowers/plans/progress/2026-04-27-tex-resolve-closing-report.md \
        docs/superpowers/plans/progress/2026-04-27-validate-*.log

# DO NOT git add the .tracy files — they are large binaries, kept local-only
# and referenced by SHA-256 in the closing report.

git commit -m "$(cat <<'EOF'
docs: TexResolveTable Shape A closing report — validate clean, Tracy A/B, smoke gate

Validate mode (mc2_01 std + mc2_01 Wolfman + Magic canary): zero
[TEX_RESOLVE v1] event=mismatch and event=oob_node lines across <N> frames.
Validate compares every call (not first-touch only) so within-frame
eviction by §7.2 legacy callers would have surfaced.

Tracy A/B at Wolfman shows <combined>ms/frame self-time reduction
across get_gosTextureHandle + resolveFallback + realizeTexture.
Terrain.BeginFrameTexResolve <X> µs/frame (under 5 µs target).
Tracy capture SHA-256 references in the closing report; binary
snapshots kept local-only.

Smoke gate tier1 + menu canary green under both MC2_MODERN_TEX_RESOLVE
=0 and =1.

Residual-call census diff (grep-only) matches spec §7.2 expected list exactly.

Promotion recommendation: <yes|hold|revert>.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

- [ ] **Step 8: Followup decision**

Based on the closing report's promotion recommendation:

- **If "Promote":** open a separate, single-line follow-up commit that flips the env-default from OFF to ON in `initTexResolveTable()`. The flip is one line; do not bundle it with anything else.
- **If "Hold":** leave default OFF and document in CLAUDE.md / memory why we stopped here. Shape A is still valuable as infrastructure for Shape B/C.
- **If "Revert":** `git revert` the implementation commit. Document the failure mode in a memory file under `~/.claude/projects/A--Games-mc2-opengl-src/memory/`.

Static-shadow Shadow.StaticAccum (`txmmgr.cpp:1228`) opt-in is **always** a separate commit per operator constraint 6, regardless of which branch above is taken.

---

## Self-review

**Spec coverage check:**

| Spec section | Plan coverage |
|---|---|
| §5 data structure | Task 1 Step 2 (header) defines the struct, including `frameActive` flag |
| §6.1 init site | Task 2 Step 2 — top of `Terrain::geometry` (mission-update phase, before any converted setup-time read). Earlier draft had this in `GameCamera::render`; corrected per advisor round-2. |
| §6.1 close site | Task 2 Step 3a — after `mcTextureManager->renderLists()` in both `gamecam.cpp:242` and `simplecamera.cpp:209`. Pairs begin/end so `frameActive` is a real invariant, not a one-way bit. Added per advisor round-3. |
| §6.2 algorithm | Task 1 Steps 2–3 (`tex_resolve` inline + `beginFrameTexResolve` + `endFrameTexResolve`) |
| §7.1 conversions | Task 3 Steps 2–6 cover all 27 sites |
| §7.2 out-of-scope | Task 3 Step 6 explicitly does NOT touch `txmmgr.cpp:1228` etc. |
| §8 killswitch + validate | Task 1 Step 2 inline (validate compares **every** call, not only first-touch) + Task 4 Steps 1–3. Validate implies enabled. |
| §9 instrumentation | Task 1 Step 3 (cpp body) implements `startup`, `begin_frame`, `summary`, `shutdown`, `mismatch`, `oob_node`. |
| §10 AR1 (cross-frame caching) | `frameActive` set on begin, cleared on end + sentinel-fill memset every `Terrain::geometry`. Begin/end pairing means out-of-frame inline-accessor callers (mission-load, UI, loading screen, post-render capture) reliably see `frameActive=false` and fall through. |
| §10 AR6 (cap miss) | Task 1 Step 2 has the `static_assert`; runtime OOB guard with `event=oob_node` |
| §10 AR7 (sentinel collision) | Validate-mode every-call comparison would surface it |
| §11 measurement plan | Task 4 Step 4 (locally-stored Tracy captures, SHA-256 in report) |
| §12 residual-call census | Task 3 Step 1 (baseline) + Step 8 (post) + Task 4 Step 6. **Grep-only**; no per-callsite runtime counter (advisor round-2 scope reduction). |
| §13 smoke gate | Task 4 Step 5 |

**Placeholder scan:** No "TBD," no "implement later," every step has either a code block or an exact command. Diffs use exact file:line citations from the spec.

**Type consistency:** `tex_resolve` is a free-function returning `DWORD` everywhere. `beginFrameTexResolve` takes `uint64_t frameId`. `g_currentFrameId` is `uint64_t`. The `static uint64_t s_texResolveFrameCounter` in `gamecam.cpp` matches. `kSentinel = 0xFFFFFFFFu` is used both in the inline and in the `memset(..., 0xFF, ...)` initialization.

---

## Estimated commit count and session shape

Two commits, as the operator predicted:

1. **Implementation commit** (Tasks 1–3): sidecar + 27 conversions + instrumentation. ~250 lines of new code (header + cpp), ~30 line edits across 7 existing files, +2 lines in CMake.
2. **Closing report commit** (Task 4): no code; logs + Tracy snapshots + report. Promotion decision lives here.

Optional follow-ups, each its own commit:
- Default-ON flip (one line) — only if Task 4 says "Promote."
- Shadow.StaticAccum opt-in (one line, `txmmgr.cpp:1316/1321`-style swap at `:1228`) — only if Task 4's followups recommend it.

Total session: one implementation pass, one validation pass. Both fit in a single agentic-development session if the smoke runs are scripted.
