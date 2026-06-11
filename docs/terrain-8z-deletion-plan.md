# Phase 8z: Terrain Legacy Deletion Plan

**Branch:** `claude/terrain-gen-pcg` (nifty-mendeleev worktree)
**Date:** 2026-06-10
**Status:** READ-ONLY PLAN — no source modifications.

**Backbone recon docs (do not re-derive from source; read these first):**
- `docs/terrain-8z-fastpath-drop-recon.md` — H2 T-table (T1..T20), Sections 5-7
- `docs/terrain-t3-map-audit.md` — T3 gate status (140 maps audited)
- `docs/editor-chunk-path-parity.md` — editor legacy dependency analysis

---

## Preconditions (all must be green before any deletion)

| # | Gate | Status | Evidence / Notes |
|---|---|---|---|
| P1 | **T3 audit CLEARS** — 0 non-atlas maps of 140 audited | **DONE** | `terrain-t3-map-audit.md` §C: 57 base + 24 DarkRain + 31 PicturesOfARebeliion + 20 TangoMaster + 8 cveg — every map has at least one `.burnin.{jpg,tga,ktx2}` atlas; `ShouldArmGpuTerrain` returns true for all. |
| P2 | **Automated FASTPATH steady-state green** — `MC2_FASTPATH_DROP_LOG=1` shows 0 non-warmup drops over tier1 5/5 | **DONE** (per H2 recon Section 7 stop-conditions; no continuous drop found in automated smoke) | Run `MC2_FASTPATH_DROP_LOG=1` on tier1 smoke to re-confirm before branching 8z. |
| P3 | **Interactive FASTPATH confirm** — gameplay panning, camera rotation, water tiles, mine tiles, no steady-state DROP log | **PENDING — user** | Headless smokes are blind to interactive arming edge cases. User must run `MC2_FASTPATH_DROP_LOG=1` in an interactive session (pan full map, fire weapons, trigger mine tiles) and confirm 0 steady-state drops. This is a hard gate before 8z-A. |
| P4 | **Editor arming visible-session confirm** — editor with passability map OFF shows GPU-indirect armed (no FASTPATH_DROP_LOG in editor) | **PENDING — user** | Editor sessions with headless have degenerate frustum; recipe/atlas readiness cannot be confirmed without a live editor session. User must open the editor, load a map, confirm `[FASTPATH_DROP]` log is silent (passability overlay OFF), and that the terrain surface renders correctly. |
| P5 | **T16/T19 loud-fail defined and implemented** — `s_processArmingDisabled` / `ForceDisableArmingForProcess` paths must have a loud assert/log BEFORE their `setupTextures` safety net is removed | **DECISION-NEEDED** | Currently silent fallback (recon Section 5). See the Loud-fail Requirements section below for the required change and exact sites. Must be implemented as a pre-8z-A commit. |
| P6 | **`MC2_TERRAIN_ACTIVE_AB` retired or converted** — the diagnostic resurrects `makeLists` under chunk=ON; silently breaks 8z-A if left alive | **DECISION-NEEDED** | See `MC2_TERRAIN_ACTIVE_AB` section below. Must be resolved before `makeLists` deletion. |
| P7 | **First-frame / warmup answer defined** — T2 (recipe not ready), T10 (WaterStream not ready), T12 (intro-legacy pan) are transient; must decide: accept brief black-terrain on load, or ensure chunk path renders frame 1 | **DECISION-NEEDED** | Currently the transient fallback silently covers frames 1-N during GPU preflight. After `setupTextures` deletion, these frames render nothing. The chunk LOD path already renders from frame 1 (it has no separate warm-up fence); the GPU-indirect solid path has its own IsDenseRecipeReady gate. Recommended decision: accept brief black-terrain (≤2 frames at 60fps) and document it; add a visible log `[WARMUP_BLACK] frame=N reason=T2/T10/T12` that fires during that window so it is diagnosable. |
| P8 | **Editor quarantine (8z-B) LANDED** — `setupTextures`, per-quad `draw()` loop, and `slimReduce` compile-gated out of game build | **PENDING — 8z-B is a prerequisite for 8z-A** | See 8z-B section below. Must be its own commit, tier1 green, before any 8z-A deletion begins. |
| P9 | **Kill-switch semantics decided** for T1/T4/T5/T6/T14/T15/T20 after 8z (remove, no-op, or fatal) | **DECISION-NEEDED** | These env vars become meaningless or dangerous after deletion. Minimum: add a startup log `[8Z_DEAD_KNOB] MC2_TERRAIN_LOD_CHUNK=0 has no effect` for T20 (the opt-out that becomes impossible), and remove the env-parse code for T6 (`MC2_SETUPTEXTURES_LEGACY_FORCE`) since it will have no body to force. Others (T1/T4/T5) are GPU-driven gates that survive 8z. T14/T15 (`NarrowEnabled`/`armedSkipOn`) are internal optimization knobs that remain valid. |

---

## 8z-B FIRST: Editor Compile-Gate Quarantine (must land before 8z-A)

### Why compile-gate, not runtime `drawTerrainGrid`

H2 recon Finding 5 (`terrain-8z-fastpath-drop-recon.md:151-153`) and editor recon Option B
analysis (`editor-chunk-path-parity.md:156-158`) both rule out runtime reliance on
`drawTerrainGrid`:

- `drawTerrainGrid` is a plain `bool` global in the **shared** binary (`terrain.cpp:258`).
  Any stray write (stale env, debug tool, future code) permanently suppresses the quad-skip
  optimization in production builds.
- The compile-gate has zero runtime cost and no regression surface.

### Existing hook: `MC2_IS_EDITOR=1`

The editor CMake target already defines `MC2_IS_EDITOR=1`:

```
editor/CMakeLists.txt:130  target_compile_definitions(EditRel PRIVATE MC2_IMGUI=1 MC2_IS_EDITOR=1)
```

**Proposed macro:** `MC2_IS_EDITOR` (already present — no new define needed).
The game binary is built WITHOUT this define, so `#ifdef MC2_IS_EDITOR` blocks are
stripped at compile time from all game-build objects.

### Symbols to gate behind `#ifdef MC2_IS_EDITOR`

| Symbol | File | Lines (approx) | Notes |
|---|---|---|---|
| `TerrainQuad::setupTextures()` body | `mclib/quad.cpp` | 684–~988 | Entire method body. Declaration in `mclib/quad.h` also behind gate. |
| `TerrainQuad::draw()` body | `mclib/quad.cpp` | ~1000–~1060 | Per-quad immediate raster draw. Already suppressed at terrain.cpp:2221 under chunk=ON, but the symbol must not link in game builds post-8z-A. |
| Per-quad `draw()` loop | `mclib/terrain.cpp` | 2221–2270 | The `if (!mc2TerrainLodChunkEnabled())` block that calls `currentQuad->draw()`. After 8z-A game builds will never reach this; gating it removes dead call-site linkage. |
| `slimReduce` loop | `mclib/terrain.cpp` | 3052–3276 | The `ZoneScopedN("Terrain::geometry slimReduce")` block. Already a no-op (zero iterations) when `makeLists` was skipped; gating removes the Tracy zone and iteration scaffolding from game builds. |
| `setupTextures` call site | `mclib/terrain.cpp` | 3806 | The `currentQuad->setupTextures()` call inside the `geometry()` quad-setup loop. The entire `if (!skipSetup) { ... }` block containing it. |
| `drawTerrainGrid` global and its read sites | `mclib/terrain.cpp` | 258, 2401, 2407, 3115, 3724 | Gate the declaration and all read sites. In editor builds these remain live; in game builds they compile away. |
| `makeLists` call | `mclib/terrain.cpp` | 1523–1526 | The `if (!mc2TerrainLodChunkEnabled() || getenv("MC2_TERRAIN_ACTIVE_AB"))` branch. In game builds after 8z-A this is dead. Gate behind `#ifndef MC2_IS_EDITOR` (after `MC2_TERRAIN_ACTIVE_AB` is retired — see below). |

### Why NOT Option C yet

Editor recon recommends Option A first, Option C as a separate future slice.
Option C (add `flushDrawCommands()` to `EditorCamera::render()`, decouple `drawLine()` from
`setupTextures()` clipInfo) is valuable editor polish but:

- Requires visual confirmation that chunk LOD mesh renders correctly in editor (PENDING user).
- Porting `drawLine()` off `clipInfo` is a non-trivial surgical change with its own risk surface.
- Does not unblock 8z-A at all.

Option C is tracked as a SEPARATE future slice. Do NOT mix into 8z-B.

### 8z-B commit structure

Single commit, independently tier1-gated:

1. Add `#ifdef MC2_IS_EDITOR` guards around the symbols listed above in `quad.cpp` and `terrain.cpp`.
2. Verify the editor build still compiles and renders correctly with passability overlay on/off.
3. Verify the game build compiles and links without `setupTextures` / `draw` symbol references.
4. Tier1 5/5 pass.

---

## 8z-A: Game-Build Legacy Deletion — Ordered Steps

Each step is independently committable and tier1-gated. Do NOT mix steps.
**Do not start any 8z-A step until 8z-B is landed and all Preconditions are green.**

### Step A1: Retire `MC2_TERRAIN_ACTIVE_AB` (prerequisite for A2)

**What it is:** The env var forces `makeLists` under chunk=ON (terrain.cpp:1523-1526).
It was a Phase 8a A/B diagnostic for the chunk active-set producer. The diagnostic is no
longer needed (8a/8b/8c shipped, FN=0 validated per memory entry 2026-06-08).

**Sites:**
- `mclib/terrain.cpp:1523` — the `makeLists` branch condition `if (!mc2TerrainLodChunkEnabled() || getenv("MC2_TERRAIN_ACTIVE_AB"))`
- `mclib/terrain.cpp:3337` — `static const bool s_activeABForce = (getenv("MC2_TERRAIN_ACTIVE_AB") != nullptr);`
- `mclib/terrain.cpp:3450` — `static const bool s_activeAB = (getenv("MC2_TERRAIN_ACTIVE_AB") != nullptr);`
- `mclib/terrain.cpp:3441-3550` — the `if (s_activeAB)` A/B comparison block (FN diagnostics)
- `mclib/terrain.cpp:3602` — reference inside `MC2_TERRAIN_SOLID_AB` block (reads `s_shVert` populated by the above A/B path)

**What has been extracted:** Phase 8a (`0372eee6`) shipped the chunk active-set producer
(`produceActiveSetFromChunks`). Phase 8b (`b4346b32`) shipped chunk solid-window. Phase 8c
(`df20f557`) made them production-default under `MC2_TERRAIN_LOD_CHUNK=1`. All three have
been validated FN=0 (memory HANDOFF 2026-06-08). The `MC2_TERRAIN_ACTIVE_AB` diagnostic
consumer is the ONLY remaining live user of the `makeLists` call under chunk=ON.

**Risk:** LOW. The active-set producer (`s_lodChunkProd` block, terrain.cpp:3325-3400) is
already the production path. Removing the A/B diagnostic removes a diagnostic-only branch.

**Action:** Convert the `makeLists` call condition at terrain.cpp:1523 from
`if (!mc2TerrainLodChunkEnabled() || getenv("MC2_TERRAIN_ACTIVE_AB"))` to simply
`if (!mc2TerrainLodChunkEnabled())`. Then delete the `s_activeABForce`, `s_activeAB`
statics, and the A/B comparison block (terrain.cpp:3441-3602 range containing FN diagnostics).
Add a one-line startup log: `[8Z_RETIRED_ENV] MC2_TERRAIN_ACTIVE_AB has no effect (8a/8b/8c shipped)`.

**Gate:** Tier1 5/5 with `MC2_TERRAIN_ACTIVE_AB=1` set (must not crash — the env is now ignored).

---

### Step A2: Delete `makeLists` call under chunk=ON

**What it is:** `Terrain::mapData->makeLists(vertexList, numberVertices, quadList, numberQuads)`
at terrain.cpp:1526. This builds the O(n²) vertex/quad lists used by `slimReduce` and the
legacy `TerrainQuad::draw()` path.

**Consumers and extraction status:**
- `objBlockInfo.active` / `objVertexActive` — re-homed to chunk producer in Phase 8c (`df20f557`). DONE.
- Solid-window / `s_shVert` shadow set — re-homed in Phase 8b (`b4346b32`). DONE.
- `slimReduce` loop (terrain.cpp:3052) — iterates `vertexList[0..numberVertices]`; already a no-op when `numberVertices==0` (which is exactly the state after this deletion). The loop body becomes dead. DONE (no-op confirmed by H2 recon and memory HANDOFF 2026-06-08 perf inversion note).
- `TerrainQuad::draw()` call (terrain.cpp:2250) — already gated behind `if (!mc2TerrainLodChunkEnabled())` (terrain.cpp:2221); suppressed in chunk=ON mode. DONE.
- `MC2_TERRAIN_ACTIVE_AB` diagnostic — retired in A1. DONE.

**Risk:** LOW. All consumers have been extracted or are already no-ops under chunk=ON.
The `makeLists` call itself under chunk=ON has been a no-op since Phase 8c; A2 removes the
dead call.

**Action:** After A1 is landed, simplify terrain.cpp:1523-1526:
```cpp
// Before A2:
if (!mc2TerrainLodChunkEnabled())  // (after A1 stripped the MC2_TERRAIN_ACTIVE_AB branch)
    Terrain::mapData->makeLists(vertexList, numberVertices, quadList, numberQuads);

// After A2: delete the entire if-block. numberVertices stays 0 under chunk=ON.
// The slimReduce loop (A3) is now unconditionally a no-op; delete it next.
```

**Gate:** Tier1 5/5. Verify `[TerrainLOD prod]` telemetry still reports non-zero `objBlocks` on all 5 missions.

---

### Step A3: Delete `slimReduce` loop from `Terrain::geometry()`

**What it is:** The `ZoneScopedN("Terrain::geometry slimReduce")` block at terrain.cpp:3052-3277.
This is the O(n) per-vertex loop (over `numberVertices`) that produced `objBlockInfo.active`,
`objVertexActive`, `s_shVert` shadow vertices, and the solid-window. After A2, `numberVertices==0`
unconditionally under chunk=ON, making this loop a confirmed no-op.

**Consumers and extraction status:**
- `objBlockInfo.active` — Phase 8c. DONE.
- `objVertexActive` — Phase 8c. DONE.
- `s_shVert` / shadow set — Phase 8b. DONE.
- Solid-window feed — Phase 8b/8c. DONE.
- `SlimSplitRollAndMaybeEmit()` (terrain.cpp:3277) — Tracy RDTSC diagnostic for slimReduce cost decomposition (terrain.cpp:2811-2823). Remove the call and the associated SLIMSPLIT machinery if desired (or leave as a dead-code stub guarded by `#ifdef MC2_IS_EDITOR` if the cost-split tooling is still useful in the editor context). Flag for separate decision.
- `MC2_BLOCK_FRUSTUM_FALLBACK` block (terrain.cpp:3280-3322) — post-slimReduce block-level frustum pass. This runs after the slimReduce loop and reads `objBlockInfo.active`. After A3, if this block reads the chunk-produced `objBlockInfo.active` (set by `s_lodChunkProd` block above it in geometry()), it may be retained independently. **VERIFY** that this block does not depend on slimReduce intermediate state (e.g., `objVertexActive`); if it does, it must also be gated or removed.

**Risk:** LOW for the core loop. The `MC2_BLOCK_FRUSTUM_FALLBACK` dependency on `objVertexActive`
is a BLOCKED item — must verify before deletion.

**Action:** Delete terrain.cpp:3052-3277 (the slimReduce zone and its SlimSplitRollAndMaybeEmit call).
Retain the `MC2_BLOCK_FRUSTUM_FALLBACK` block only if it is confirmed to use only
`objBlockInfo.active` (chunk-produced). If it also reads `objVertexActive`, it must be updated
or deleted.

**Gate:** Tier1 5/5. Verify `[TerrainLOD prod] objBlocks` counts unchanged; verify no
TRACY `Terrain::geometry slimReduce` zone appears.

---

### Step A4: Delete `TerrainQuad::draw()` and the per-quad draw loop

**What it is:** `TerrainQuad::draw()` (quad.cpp, ~1000-1060) and the call site at
terrain.cpp:2221-2270.

**Status:** After 8z-B (compiler gate), `TerrainQuad::draw()` and its call site are already
removed from game builds. A4 is the physical deletion from source (removing the `#ifdef`
block and the method body entirely from non-editor source, or deleting from quad.cpp with
the understanding that the editor's `#ifdef MC2_IS_EDITOR` copy is what survives in editor builds).

**Consumers and extraction status:**
- Surface terrain render — Phase 10 (chunk frag shader, colormap, lighting, shadows, water,
  skirts). DEFAULT-ON as of commit `a7b090be`. DONE.
- Decal/overlay fallback — T8 (`MC2_GPU_DRIVEN_OVERLAY=0`). This is a KILL-SWITCH SEMANTICS
  DECISION (P9 above). If the overlay env var is retained as a valid opt-out, `draw()` would
  need to survive in that mode. If the overlay is declared always-GPU after 8z, `draw()` is
  safe to delete. **DECISION-NEEDED** — confirm with the M2d decal plan (memory HANDOFF
  2026-06-05 mentions M2d as a future slice).

**Risk:** MEDIUM. Decal/overlay fallback status must be confirmed (see above). Mark this
step **BLOCKED until M2d/T8 decision is resolved** unless T8 is declared dead.

**Gate:** Tier1 5/5 (all 5 missions render correctly). Interactive visual confirm (user) that
terrain surface, decals, and overlays all render.

---

### Step A5: Delete `TerrainQuad::setupTextures()` from game builds

**What it is:** `TerrainQuad::setupTextures()` body (quad.cpp:684-~988) and the
`geometry()` call site (terrain.cpp:3806 inside the `if (!skipSetup)` block).

**Consumers and extraction status:**
- Terrain surface texture/color — GPU-indirect path (armed). DONE.
- Water per-quad waterHandle — armed water fast path (T9). DONE for armed path.
- `clipInfo` (clip-space vertex coords for `drawLine()`) — EDITOR ONLY (8z-B quarantine). DONE via 8z-B.
- Mine texture handle lazy-load — `setupTextures` previously lazy-loaded mine handles; mine
  enqueue was extracted (quad.cpp:480-488 notes the mine enqueue was moved OUT of setupTextures).
  **VERIFY** no remaining mine-handle load depends on `setupTextures` running first.
  `gos_terrain_indirect.cpp:3799-3801` documents the "R7 timing trap" (mine handles loaded
  by `setupTextures` before `RebuildMineStaticVBOIfDirty`). This R7 trap site must be confirmed
  resolved before A5. **BLOCKED** until R7 timing trap is confirmed safe.
- Overlay tex handle lazy-load — `gos_terrain_indirect.cpp:4060` notes "overlay tex handles
  lazy-load in TerrainQuad::setupTextures during the overlay tex block". **VERIFY** that
  overlay handle loading was migrated to the GPU-indirect init path.

**Risk:** HIGH for the R7 mine-handle and overlay-handle lazy-load concerns. These are the
biggest remaining BLOCKED items in 8z-A. Do not delete A5 until both are verified migrated.

**Gate:** Tier1 5/5. Interactive visual confirm (user): mine fields render correctly (no missing
mine tiles), overlay/decal tiles render correctly on a map that uses them (e.g., `torrin`,
`mc2_01`).

---

### Step A6: Clean up dead kill-switch env-var parsing

After A1-A5 are complete, remove:

| Env var | File | Lines | Action |
|---|---|---|---|
| `MC2_SETUPTEXTURES_LEGACY_FORCE` | `quad.cpp:739-741` | `s_legacyForce` static and its gate | Delete: no body to force after A5 |
| `MC2_QUADSETUP_ARMED_SKIP` | `terrain.cpp:3649-3661` | `s_armedSkipOn` and `skipSetup` | Delete: always-skip after 8z-A |
| `MC2_TERRAIN_LOD_CHUNK=0` | `terrain.cpp:135-142` | opt-out gate | Convert to startup fatal: `if (getenv("MC2_TERRAIN_LOD_CHUNK") ...) PANIC("Legacy chunk path deleted in 8z")` |

Kill-switch semantics for T1/T4/T5 (`MC2_TERRAIN_INDIRECT`, `MC2_GPU_DRIVEN`, `MC2_GPU_DRIVEN_TERRAIN_SOLID`)
are GPU-driven gates that survive 8z — they disable the GPU-indirect path entirely, which post-8z
means black terrain. Leave them in place but add a prominent comment: "8z: no legacy fallback;
disabling this = black terrain".

**Risk:** LOW.
**Gate:** Tier1 5/5.

---

## Loud-Fail Requirements (T16/T19)

The two hard-GL-failure paths currently fall back SILENTLY to `setupTextures`. After A5
removes `setupTextures`, these paths produce silent black terrain with no diagnostic. They
must be converted to loud-fails BEFORE A5 (as a prerequisite commit in the same branch).

### Sites and required change

**T16 / T19 — `s_processArmingDisabled` set by `ForceDisableArmingForProcess()`**

| Site | File:line | Current behavior | Required change |
|---|---|---|---|
| `s_processArmingDisabled` declaration | `gos_terrain_indirect.cpp:1659` | static bool, starts false | No change to declaration |
| `ForceDisableArmingForProcess()` body | `gos_terrain_indirect.cpp:2396-2401` | Sets flag, prints one log line | Add `MC2_HARD_FAIL_ON_ARMING_DISABLED` env-gated `gosASSERT(false)` or `__debugbreak()` after the log; the log itself must be guaranteed visible even in RelWithDebInfo |
| `ComputePreflight()` early-return | `gos_terrain_indirect.cpp:2476` | `if (s_processArmingDisabled) return false;` | After 8z-A: change to `if (s_processArmingDisabled) { MC2_PANIC_LOG("[TERRAIN_ARMING_FATAL] setupTextures deleted; no fallback. GL arming permanently disabled. Terrain will be black."); return false; }` |
| `DrawIndirect()==false` path (T19) | `gos_terrain_indirect.cpp:2396-2401` (called from ~2613, ~3118, ~3527) | Calls `ForceDisableArmingForProcess` and falls through | Same: the `ForceDisableArmingForProcess` log must be loud enough to surface in a crash/log dump |

**Timing:** This loud-fail commit must land in the 8z branch BEFORE A5 (`setupTextures` deletion).
It can land simultaneously with A5 if done carefully, but it is safer as a separate preceding commit.

**Finding 2 (GL-fail latch):** `gos_terrain_indirect.cpp:1789, 1820` — `s_resourcesAllocated`
is set true even on GL allocation failure, then `ResourcesReady()` returns false permanently
with no further throttled log. Add a throttled warning log at the `if (s_resourcesAllocated) return false` guard (line 1774) that fires once per session when it detects that allocation was previously attempted but buffers are invalid (`g_solidVBO == 0` etc.). This is a pre-8z hardening commit, not strictly part of 8z-A ordering.

---

## `MC2_TERRAIN_ACTIVE_AB` Retirement

Per Section 4 Finding 1 of the fastpath-drop recon: `MC2_TERRAIN_ACTIVE_AB` causes
`makeLists` to run even under chunk=ON (terrain.cpp:1523). This must be resolved in **Step A1**
(the first 8z-A step) before `makeLists` deletion (A2).

**All sites:**

| File | Line | Role |
|---|---|---|
| `mclib/terrain.cpp` | 1523 | `makeLists` call gate — the resurrection trigger |
| `mclib/terrain.cpp` | 3337 | `s_activeABForce = getenv("MC2_TERRAIN_ACTIVE_AB")` — suppresses production chunk producer |
| `mclib/terrain.cpp` | 3450 | `s_activeAB = getenv("MC2_TERRAIN_ACTIVE_AB")` — enables FN-comparison block |
| `mclib/terrain.cpp` | 3441–3550 | A/B comparison body (blockFN / vertexFN counters, logging) |
| `mclib/terrain.cpp` | 3602 | Reference inside `MC2_TERRAIN_SOLID_AB` block (reads `s_shVert` populated by the A/B path) — also remove if `MC2_TERRAIN_SOLID_AB` is retired in A1 |

**`MC2_TERRAIN_SOLID_AB`** — similar diagnostic for the solid-window producer (Phase 8b). Check
for equivalent retirement need when resolving `MC2_TERRAIN_ACTIVE_AB`. If `MC2_TERRAIN_SOLID_AB`
also forces legacy paths under chunk=ON, retire it in the same A1 commit.

**Action in A1:** Delete the `s_activeABForce`, `s_activeAB` statics and the A/B block.
Convert the `makeLists` gate to `if (!mc2TerrainLodChunkEnabled())` (pure flag-off path
retained for the non-chunk legacy code path — the game-build compiler gate handles the
physical deletion in 8z-A final cleanup).

---

## Risk and Validation

### Per-step summary

| Step | Risk | Blocked on | Validation |
|---|---|---|---|
| 8z-B (compile-gate) | LOW | P4 (editor visual confirm) | Editor build compiles + renders + passability overlay works; tier1 5/5 |
| A1 (retire ACTIVE_AB) | LOW | None (diagnostic-only removal) | Tier1 5/5 with `MC2_TERRAIN_ACTIVE_AB=1` set (env ignored, no crash) |
| A2 (delete makeLists under chunk=ON) | LOW | A1 | Tier1 5/5; verify `[TerrainLOD prod] objBlocks` non-zero |
| A3 (delete slimReduce loop) | LOW (core), MEDIUM (BLOCK_FRUSTUM_FALLBACK) | A2; verify BLOCK_FRUSTUM_FALLBACK `objVertexActive` dependency | Tier1 5/5; no TRACY slimReduce zone |
| A4 (delete TerrainQuad::draw) | MEDIUM | M2d/T8 overlay decision | Tier1 5/5; interactive terrain + decal visual confirm (user) |
| A5 (delete setupTextures from game) | HIGH | R7 mine-handle + overlay-handle migration verified; loud-fail commit landed | Tier1 5/5; interactive mine field + overlay tile confirm (user) |
| A6 (dead env-var cleanup) | LOW | A1-A5 | Tier1 5/5 |

### Rollback approach

Each step is a standalone commit. Rollback is `git revert <step-commit>`. The compile-gate
in 8z-B ensures editor builds retain the legacy path throughout 8z-A, so editor users are
never exposed to an incomplete deletion state in the editor binary.

### Do-not-mix warning

**8z must be its own branch.** Do not combine with:
- S2 / F1 (projection unification — different risk domain, touches uniform paths that interact
  with `clipInfo` / `worldToClipGL`)
- HZB (occlusion-cull slice — reads depth buffer after terrain draw; deletion changes draw-call
  ordering assumptions)
- Water/shadows arcs (M2a/M2b/M2d — overlay and water paths are directly involved in T8/T9
  fallback decisions)

Any of these merging before the 8z gates are green creates hard-to-bisect compound failures.
Open a clean `claude/terrain-8z` branch from `claude/nifty-mendeleev` HEAD and land 8z-B +
A1-A6 there; merge back to nifty after tier1 green + interactive confirms.

---

## Ordered Execution Checklist

```
Pre-8z-A (all gates):
  [ ] P3 — user: interactive FASTPATH confirm (MC2_FASTPATH_DROP_LOG=1)
  [ ] P4 — user: editor visual confirm (passability ON + OFF)
  [ ] P5 — developer: loud-fail commit for T16/T19 (gos_terrain_indirect.cpp:1659,2396-2401,2476)
  [ ] P6 — covered by A1
  [ ] P7 — decision recorded: accept brief black or add warmup log
  [ ] P8 — 8z-B compile-gate landed, tier1 green
  [ ] P9 — kill-switch semantics decision documented

8z-A (in order, tier1 gate between each):
  [ ] A1 — retire MC2_TERRAIN_ACTIVE_AB (terrain.cpp:1523, 3337, 3450, 3441-3550)
  [ ] A2 — delete makeLists call under chunk=ON (terrain.cpp:1523-1526 simplified)
  [ ] A3 — delete slimReduce loop (terrain.cpp:3052-3277; verify BLOCK_FRUSTUM_FALLBACK first)
  [ ] A4 — delete TerrainQuad::draw (quad.cpp + terrain.cpp:2221-2270; M2d decision required)
  [ ] A5 — delete setupTextures from game build (quad.cpp:684-988; R7 + overlay-handle verified)
  [ ] A6 — clean dead env-var parsing (quad.cpp:739-741, terrain.cpp:3649-3661)
```

---

## Single Biggest Remaining Blocker

**A5 is the load-bearing deletion (setupTextures) and it has two unverified lazy-load
dependencies: (1) the R7 mine-handle timing trap** (`gos_terrain_indirect.cpp:3799-3801`,
which documents that mine handles must be loaded by `setupTextures` before `RebuildMineStaticVBOIfDirty`
fires) **and (2) the overlay texture handle lazy-load** (`gos_terrain_indirect.cpp:4060`).
Until both are confirmed migrated to the GPU-indirect init path, A5 is BLOCKED regardless of
all other gates being green. This is the critical-path item for completing Phase 8z.

---

*Read-only plan. No source code modified. All line-number citations should be grep-verified before use; the recon docs note line numbers were verified at recon time on the nifty-mendeleev worktree (2026-06-09/10). Citations: `terrain-8z-fastpath-drop-recon.md`, `terrain-t3-map-audit.md`, `editor-chunk-path-parity.md`.*
