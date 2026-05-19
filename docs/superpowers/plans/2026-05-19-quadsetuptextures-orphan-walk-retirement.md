# quadSetupTextures Orphan-Walk Retirement Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Retire the per-frame O(N) `setupTextures` recipe->member orphan producer (consumer `draw` is already default-dead via 60f2ef8), and re-home the still-load-bearing water 6-tuple onto slimReduce as its literal sole producer. (DRAWALPHA-reservation deletion was dropped 2026-05-19 - no valid dead-pixel proof; see Slice-1 scope-change note.)

**Architecture:** Slice 1 deletes the recipe->member assignments and repoints every reader to the static Shape-C cache (`getTerrainFaceCacheEntry`), compile-enforced by deleting/poisoning the five `TerrainQuad` fields. Slice 2 folds an unconditional water-tile-Z visit into slimReduce reusing the exact old predicate/source/helper/accumulator, then deletes the quad.cpp water-projection reduction. Slice 3 is prep/spec-only.

**Tech Stack:** C++ (mclib/quad.cpp, quad.h, terrain.cpp; GameOS/gameos/gos_terrain_indirect.cpp), CMake RelWithDebInfo + full relink, run_smoke.py, Tracy, `MC2_WATER_INVPROJ_PARITY` probe.

**Spec:** `docs/superpowers/specs/2026-05-19-quadsetuptextures-orphan-walk-retirement-design.md` (authoritative; read it). All file:line below re-pinned at HEAD 306e641; the executor MUST re-grep before editing (numbers drift; bare-identifier member reads are compiler-discovered, NOT grep-enumerable).

---

## File Structure

- `mclib/quad.cpp` - `setupTextures` member assignments (~1004-1008), the water-projection reduction block (`CostSplitWaterVertProjScope` ~1038-~1287), `TerrainQuad::draw` (~2062) reader repoints. (DRAWALPHA sub-emit NOT touched - out of scope.)
- `mclib/quad.h` - the five `TerrainQuad` fields (`terrainDetailHandle` ~68, `overlayHandle` ~71, `uvData` ~76, plus `terrainHandle`, `isCement`; ctor inits ~100-107).
- `mclib/terrain.cpp` - slimReduce per-vertex visit (`ZoneScopedN("Terrain::geometry slimReduce")` ~1686, `projectForTerrainAdmission` ~1799), the drawPass `=0` revert loop (~1109+), probe A/B (~1924/2067), `setInverseProject` (~2119).
- `GameOS/gameos/gos_terrain_indirect.cpp` - `Counters_GetLegacyDrawAlphaDetailQuads()` (~189).
- Build/deploy/smoke per worktree CLAUDE.md.

---

## Slice 1 - Retire the recipe->member orphan producer

> **SCOPE CHANGE 2026-05-19 (user-ruled): DRAWALPHA-reservation deletion DROPPED.** The original Task 1.1 (counter pre-gate) + Task 1.2 (delete reservation) are REMOVED. Reason: `legacy_drawalpha_detail_quads` instruments the draw()-internal DRAWALPHA site (trivially 0 armed because draw() is skipped wholesale), NOT the `addTerrainTriangles` reservation; txmmgr.cpp has live `MC2_ISTERRAIN & MC2_DRAWALPHA` passes - no valid dead-pixel proof. Reservation stays as-is (pre-existing, not a regression). See `memory/drawalpha_counter_instruments_wrong_site.md`. Slice 1 is now a single task: the recipe->member retirement.

### Task 1.1: CANCELLED - Slice 1 is DEAD (premise falsified at root)

> **DO NOT EXECUTE.** `setupTextures`'s per-quad walk IS the per-frame camera-dependent terrain visibility cull (handle `0xffffffff` sentinel = cull channel: `!isTerrainQuadVisible` writes it quad.cpp:955-967, `draw()` early-outs on it quad.cpp:2064). A static cache has no camera; repointing defeats per-quad cull (full-map render). Not an orphan, not Narrow-A, not anything. See `memory/setuptextures_is_a_multiwriter_tangle_not_a_clean_shuttle.md`. Skip to Slice 2 (the live deliverable). Original (void) steps follow for history only.

#### (VOID) NARROW-A steps - historical, do not run

> Compile-enforce is OFF (the sole-consumer model was falsified at HEAD - `setupTextures` is a 720-2061 multi-writer tangle; see `memory/setuptextures_is_a_multiwriter_tangle_not_a_clean_shuttle.md`). KEEP the 5 `TerrainQuad` fields. This is a narrow structural decoupling, not a compile-enforced META-FIX, not a perf claim.

**Files:** Modify `mclib/quad.cpp` (delete `setupTextures` assigns ~1004-1008; repoint `draw` reads), `mclib/terrain.cpp` (the ~1120 drawPass hoist consumer). Do NOT modify `mclib/quad.h`.

- [ ] **Step 1: Re-grep the assignment block + both consumers**

```bash
grep -n "isCement            = recipe\|terrainHandle       = recipe\|terrainDetailHandle = recipe\|overlayHandle       = recipe\|uvData *= recipe" mclib/quad.cpp
grep -nE "^[[:space:]]*(if[[:space:]]*\()?[^a-zA-Z_]*(terrainHandle|terrainDetailHandle|overlayHandle|uvData|isCement)\b" mclib/quad.cpp | sed -n '1,80p'   # bare-id reads in draw (2062-3309) - NOT ->member
grep -n "currentQuad->terrainHandle\|currentQuad->overlayHandle\|currentQuad->terrainDetailHandle" mclib/terrain.cpp   # the ~1120 hoist consumer
```

- [ ] **Step 2: Delete ONLY the five `recipe`->member assignments at quad.cpp:1004-1008.** Keep `tryGetCachedTerrainRecipe`/`recipe` (feeds `addTerrainTriangles(recipe)`). Do NOT touch quad.h, the legacy `!terrainTextures2` self-consuming branch (789-891), the sentinel resets (741-746/858-966), the dead-1410 guard, the water block, the DRAWALPHA reservation.

- [ ] **Step 3: Repoint every `TerrainQuad::draw` member read (2062-3309) to the cache entry**

For each bare-identifier read of the 5 members in `draw`:
```cpp
const MapData::WorldQuadTerrainCacheEntry* e =
    Terrain::mapData ? Terrain::mapData->getTerrainFaceCacheEntry(tileR, tileC) : NULL;
// e->terrainHandle / e->terrainDetailHandle / e->overlayHandle / e->uvData / e->isCement()
```
Key on the STABLE `tileR/tileC`. If `draw` lacks `tileR/tileC` in scope, derive IDENTICALLY to how `setupTextures` derives them before its `getTerrainFaceCacheEntry` call (re-grep quad.cpp ~979; lift the same derivation; do NOT invent a new tile-identity source). Guard `e == NULL` exactly as the pre-existing `cachedEntry` site (quad.cpp ~979). Reuse `enqueueCachedTerrainTriangles(const MapData::WorldQuadTerrainCacheEntry&)` (quad.cpp:502) where a site is an enqueue. (The 5 fields still exist, so this is a deliberate read-source switch in `draw`, NOT compiler-forced.)

- [ ] **Step 4: Resolve the terrain.cpp ~1120 drawPass hoist consumer**

`if (currentQuad->terrainHandle == 0 && currentQuad->overlayHandle == 0xffffffff && currentQuad->terrainDetailHandle == 0xffffffff) continue;` runs on the `=0` revert path. After Step 2, modern-path members are SENTINEL (0xffffffff) not recipe values, changing this skip decision. EITHER repoint this hoist to read the cache entry (behavior-preserving - preferred), OR prove `draw()`'s internal early-out is exactly behavior-equivalent for the formerly-hoist-skipped (pure-water) quads and document the proof. The `=0`-revert smoke (Step 7) is the gate.

- [ ] **Step 5: Build foreground until clean**

```bash
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo 2>&1 | tail -15
```
(No compiler-forced reader list - fields are KEPT. Correctness is by the `=0`-revert smoke, not the compiler.)

- [ ] **Step 6: Full relink + deploy**

```bash
rm -f build64/RelWithDebInfo/mc2.exe
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo
cp -f build64/RelWithDebInfo/mc2.exe "A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe" && diff -q build64/RelWithDebInfo/mc2.exe "A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe"
```

- [ ] **Step 7: Slice-1 acceptance smokes (`=0` revert is LOAD-BEARING)**

```bash
# LOAD-BEARING: the =0 revert path is where draw() + the hoist actually run
MC2_TERRAIN_INDIRECT_OVERLAY=0 py -3 scripts/run_smoke.py --mission mc2_01 --mission mc2_10 --duration 20 --keep-logs --kill-existing
# regression check: default-armed (draw() skipped wholesale - exercises little of this change)
py -3 scripts/run_smoke.py --mission mc2_01 --duration 20 --keep-logs --kill-existing
```
Expected: PASS both. USER visually confirms on the `=0` path: terrain/decal/detail parity, cement canary scene unchanged, map-edge / zoomed-out camera-window pass (tileR/tileC identity holds), AND default-armed shows no regression. Surface to USER for the visual gate (this is the substitutive proof for Narrow-A - the `=0` path is where the change is observable).

- [ ] **Step 8: Commit**

```bash
git add mclib/quad.cpp mclib/terrain.cpp
git commit -m "feat(terrain): Narrow-A - draw reads Shape-C cache directly; delete setupTextures recipe->member shuttle (modern path)

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

- [ ] **Step 9: Slice-1 Tracy (USER-DRIVEN, no-regression)** - hand to user: clean non-COST_SPLIT total-frame Tracy at worst-case zoomed-out-big-map; confirm NO regression (negligible delta - this is a decoupling, not a measurable win; do NOT assert "net-shrink"/"zone-gone"). Record.

---

## Slice 2 - Collapse the water 6-tuple onto slimReduce

### Task 2.1: Characterize the old water-projection reduction (no edits)

**Files:** read `mclib/quad.cpp` `CostSplitWaterVertProjScope` block (~1038-~1287).

- [ ] **Step 1: Document, from the code, the exact old-block contract**: the admission predicate (`vertices[i]->pVertex->water & 1`, quad.cpp ~1039-1042 + the `&128`/`&64` elevation selectors ~1062-1066), the water-elevation source, the projection helper used, and each accumulator update (`if (screenPos.z < leastZ) leastZ = screenPos.z;` etc. for the six globals `leastZ/mostZ/leastW/mostW/leastWY/mostWY`, declared `extern` quad.cpp:540-545). Write this as the cardinality-equivalence reference (spec Slice-2 must-fix). No commit.

### Task 2.2: Fold an unconditional water-Z visit into slimReduce

**Files:** Modify `mclib/terrain.cpp` slimReduce per-vertex visit (re-grep `projectForTerrainAdmission` ~1799 inside `ZoneScopedN("Terrain::geometry slimReduce")` ~1686).

- [ ] **Step 1: Re-grep the slimReduce per-vertex site**

```bash
grep -n "ZoneScopedN(\"Terrain::geometry slimReduce\")\|projectForTerrainAdmission(vertex3D\|pVertex->water" mclib/terrain.cpp
```

- [ ] **Step 2: Add the water-Z visit** inside the existing per-vertex loop, AFTER the existing `projectForTerrainAdmission`, reusing the EXACT predicate / elevation source / projection result / accumulator semantics characterized in Task 2.1 - co-located only, no second pass, no second projection call. Fold into the SAME six globals so slimReduce becomes their literal sole producer. (Cardinality-equivalence: it must visit the identical water-vertex candidate set the old block did.)

- [ ] **Step 3: Build + full relink + deploy** (same commands as Task 1.3 Step 6).

- [ ] **Step 4: Parity gate - probe MUST flip to identical**

```bash
MC2_WATER_INVPROJ_PARITY=1 py -3 scripts/run_smoke.py --mission mc2_01 --mission mc2_10 --duration 20 --keep-logs --kill-existing
A=$(ls -1t tests/smoke/artifacts | head -1); grep -aErh "event=parity result=" tests/smoke/artifacts/$A/ | sed -E 's/.*result=([a-z]+).*/\1/' | sort | uniq -c
```
Expected: `identical` only, ZERO `divergent`. **If any `divergent`: the fold is not cardinality-equivalent - fix the visit (do NOT delete the old block yet), iterate.** This gate is BEFORE Task 2.3 (old block still present as the B-snapshot reference).

- [ ] **Step 5: Commit**

```bash
git add mclib/terrain.cpp
git commit -m "feat(terrain): fold unconditional water-Z visit into slimReduce (6-tuple sole producer)

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

### Task 2.3: Delete the quad.cpp water-projection reduction

**Files:** Modify `mclib/quad.cpp` (`CostSplitWaterVertProjScope` block ~1038-~1287).

- [ ] **Step 1: Delete the water-projection reduction block** that writes the six globals from `setupTextures`. Keep any non-reduction side-effects the spec residue list protects (water fast-path narrow-append predicate, `clipInfo`, `calcThisFrame` - verify each against Task 2.1's doc; delete ONLY the 6-tuple reduction writers). slimReduce is now the sole producer.

- [ ] **Step 2: Build + full relink + deploy.**

- [ ] **Step 3: Post-delete parity re-confirm + Slice-2 acceptance**

```bash
MC2_WATER_INVPROJ_PARITY=1 py -3 scripts/run_smoke.py --mission mc2_01 --duration 20 --keep-logs --kill-existing
py -3 scripts/run_smoke.py --mission mc2_01 --mission mc2_10 --duration 20 --keep-logs --kill-existing
```
Expected: probe still `identical` (now trivially - one writer); PASS. USER drives: cursor->ground / camera / move / cull smoke on a water-heavy map; clean Tracy shows the old water-reduction zone GONE and slimReduce up only the expected per-water-vertex branch (net neutral-or-negative).

- [ ] **Step 4: Commit**

```bash
git add mclib/quad.cpp
git commit -m "feat(terrain): delete setupTextures water-projection reduction (slimReduce literal sole producer)

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Slice 3 - SSBO-authoritative lighting repoint (PREP / SPEC ONLY)

### Task 3.1: Write the Slice-3 prep design (interface-shape only, no engine edits)

**Files:** Create `docs/superpowers/specs/2026-05-19-slice3-ssbo-lighting-repoint-prep.md`.

- [ ] **Step 1: Document** the end-state: indirect thin-record packer (quad.cpp ~2389) + water-overlay (quad.cpp ~2465) source lighting from the GPU lighting SSBO ring via the existing non-blocking 3-slot tryConsume, deleting the `CopyResultsToVertexPool` CPU scatter (gos_terrain_lighting.cpp:834) while KEEPING the BAR->DRAM-shadow sequential indirection (anti-write-combining; never random WC, never `glGetBufferSubData` hot-path). Vulkan-prep: SSBO consumer binding as explicit device-mediated binding (`device.bindShaderStorageBuffer(...)`, no implicit cross-call GL state) per `memory/vulkan_prep_explicit_device_discipline.md`. Carve-out: must not remove the legacy raster path that still produces class-3 cement. Enumerate the consumer-repoint blast radius (quad.cpp 2389/2465/2578). NO engine code in this effort.

- [ ] **Step 2: Commit**

```bash
git add docs/superpowers/specs/2026-05-19-slice3-ssbo-lighting-repoint-prep.md
git commit -m "docs(spec): Slice-3 SSBO-authoritative lighting repoint prep (interface-shape only)

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Notes for the executor

- Subagent-driven: fresh subagent per task, two-stage review between tasks.
- Stage files BY NAME only (concurrent sessions active in this worktree - never `git add -A`/`.`).
- Smoke is USER-DRIVEN: when a task needs a visual/Tracy gate, surface to the user; do not claim visual parity from logs alone.
- Build is long: main agent runs it backgrounded; do not `--clean-first` unless link-stale (full relink = `rm` the exe + changed `.obj`, per CLAUDE.md).
- Never weaken a gate to make a task pass. A failed parity/visual gate = STOP + escalate, not iterate-around.
- Class-3 pure cement is the carve-out: no task removes SOLID-emit for cement.
