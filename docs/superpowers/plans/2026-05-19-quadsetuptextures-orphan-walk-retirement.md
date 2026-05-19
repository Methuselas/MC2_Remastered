# quadSetupTextures Orphan-Walk Retirement Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Retire the per-frame O(N) `setupTextures` recipe->member orphan producer (consumer `draw` is already default-dead via 60f2ef8) + the 521d83a-dead DRAWALPHA reservation, and re-home the still-load-bearing water 6-tuple onto slimReduce as its literal sole producer.

**Architecture:** Slice 1 deletes the recipe->member assignments and repoints every reader to the static Shape-C cache (`getTerrainFaceCacheEntry`), compile-enforced by deleting/poisoning the five `TerrainQuad` fields. Slice 2 folds an unconditional water-tile-Z visit into slimReduce reusing the exact old predicate/source/helper/accumulator, then deletes the quad.cpp water-projection reduction. Slice 3 is prep/spec-only.

**Tech Stack:** C++ (mclib/quad.cpp, quad.h, terrain.cpp; GameOS/gameos/gos_terrain_indirect.cpp), CMake RelWithDebInfo + full relink, run_smoke.py, Tracy, `MC2_WATER_INVPROJ_PARITY` probe.

**Spec:** `docs/superpowers/specs/2026-05-19-quadsetuptextures-orphan-walk-retirement-design.md` (authoritative; read it). All file:line below re-pinned at HEAD 306e641; the executor MUST re-grep before editing (numbers drift; bare-identifier member reads are compiler-discovered, NOT grep-enumerable).

---

## File Structure

- `mclib/quad.cpp` - `setupTextures` member assignments (~1004-1008), `addTerrainTriangles` DRAWALPHA sub-emit (~688/704), the water-projection reduction block (`CostSplitWaterVertProjScope` ~1038-~1287), `TerrainQuad::draw` (~2062) reader repoints.
- `mclib/quad.h` - the five `TerrainQuad` fields (`terrainDetailHandle` ~68, `overlayHandle` ~71, `uvData` ~76, plus `terrainHandle`, `isCement`; ctor inits ~100-107).
- `mclib/terrain.cpp` - slimReduce per-vertex visit (`ZoneScopedN("Terrain::geometry slimReduce")` ~1686, `projectForTerrainAdmission` ~1799), the drawPass `=0` revert loop (~1109+), probe A/B (~1924/2067), `setInverseProject` (~2119).
- `GameOS/gameos/gos_terrain_indirect.cpp` - `Counters_GetLegacyDrawAlphaDetailQuads()` (~189).
- Build/deploy/smoke per worktree CLAUDE.md.

---

## Slice 1 - Retire the recipe->member orphan producer + dead DRAWALPHA

### Task 1.1: DRAWALPHA pre-delete telemetry gate (substitutive, BEFORE any deletion)

**Files:** none modified (measurement only).

- [ ] **Step 1: Build current HEAD + deploy** (baseline binary the gate runs on)

```bash
cd A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo
cp -f build64/RelWithDebInfo/mc2.exe "A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe" && diff -q build64/RelWithDebInfo/mc2.exe "A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe"
```

- [ ] **Step 2: Run an armed parity-summary smoke (2 missions, 20s, keep-logs)**

Run:
```bash
py -3 scripts/run_smoke.py --mission mc2_01 --mission mc2_10 --duration 20 --keep-logs --kill-existing
```

- [ ] **Step 3: Confirm the DRAWALPHA path is cold**

Grep the kept logs for the legacy DRAWALPHA detail counter (the `legacy_drawalpha_detail_quads` summary; emitter `Counters_GetLegacyDrawAlphaDetailQuads()` gos_terrain_indirect.cpp:189):
```bash
grep -aErn "legacy_drawalpha_detail|drawalpha_detail_quads" tests/smoke/artifacts/$(ls -1t tests/smoke/artifacts | head -1)/
```
Expected: counter == 0 (path cold). **If non-zero: STOP - DRAWALPHA is NOT dead; do not delete the reservation; escalate to the user (the 521d83a dead-claim is falsified).**

- [ ] **Step 4: Record the gate result** in the task notes (counter value + artifact dir). No commit (measurement task).

### Task 1.2: Delete the DRAWALPHA dead reservation

**Files:** Modify `mclib/quad.cpp` - the `addTriangleBulk(r.terrainDetailHandle, MC2_ISTERRAIN | MC2_DRAWALPHA, 2)` sub-emits in `addTerrainTriangles` (re-grep: ~688 non-cement, ~704 alpha-cement; the `else // pure cement` at ~708 has NO DRAWALPHA - leave it).

- [ ] **Step 1: Re-grep the exact DRAWALPHA sites**

```bash
grep -n "MC2_DRAWALPHA, 2)\|addTriangleBulk(r.terrainDetailHandle" mclib/quad.cpp
```

- [ ] **Step 2: Delete ONLY the `DRAWALPHA, 2` detail `addTriangleBulk` reservation lines in `addTerrainTriangles`.** Do NOT touch the `MC2_DRAWSOLID` emits, the pure-cement branch, or any mine `MC2_DRAWALPHA` (those are different). Preserve the SOLID/cement contract (spec carve-out).

- [ ] **Step 3: Build RelWithDebInfo + full relink + deploy**

```bash
rm -f build64/RelWithDebInfo/mc2.exe build64/RelWithDebInfo/mclib.dir/quad.obj 2>/dev/null
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo
cp -f build64/RelWithDebInfo/mc2.exe "A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe" && diff -q build64/RelWithDebInfo/mc2.exe "A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe"
```

- [ ] **Step 4: Smoke - default-armed + the `=0` revert, both visual-parity**

```bash
py -3 scripts/run_smoke.py --mission mc2_01 --mission mc2_10 --duration 20 --keep-logs --kill-existing
MC2_TERRAIN_INDIRECT_OVERLAY=0 py -3 scripts/run_smoke.py --mission mc2_01 --duration 20 --keep-logs --kill-existing
```
Expected: both PASS, no `GL_INVALID_*`, user confirms no visual delta (decals/detail). USER is the visual observer (smoke is user-driven).

- [ ] **Step 5: Commit**

```bash
git add mclib/quad.cpp
git commit -m "feat(terrain): delete 521d83a-dead DRAWALPHA detail reservation (counter==0 pre-gated)

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

### Task 1.3: Delete the recipe->member assignments + compile-enforce the reader set

**Files:** Modify `mclib/quad.cpp` (`setupTextures` member assigns ~1004-1008), `mclib/quad.h` (the five fields + ctor inits).

- [ ] **Step 1: Re-grep the assignment block**

```bash
grep -n "isCement            = recipe\|terrainHandle       = recipe\|terrainDetailHandle = recipe\|overlayHandle       = recipe\|uvData *= recipe\|tryGetCachedTerrainRecipe(cachedEntry" mclib/quad.cpp
```

- [ ] **Step 2: Delete the five `recipe`->member assignments in `setupTextures`.** Keep the `tryGetCachedTerrainRecipe` call IF its `recipe` local still feeds `addTerrainTriangles(recipe)` (it does - that is separate, SOLID/cement contract). Delete ONLY the `this->`-member stores.

- [ ] **Step 3: Compile-enforce - delete the five fields from `TerrainQuad` (quad.h)**

Remove the declarations `terrainHandle`, `terrainDetailHandle`, `overlayHandle`, `uvData`, `isCement` and their ctor inits (quad.h ~68/71/76/100-107). This makes every surviving reader a compile error - the mechanical sole-consumer proof (spec must-fix #1).

- [ ] **Step 4: Compile; the compiler enumerates every reader**

```bash
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo 2>&1 | grep -E "error|terrainHandle|overlayHandle|isCement|uvData|terrainDetailHandle"
```
Expected: a finite list of compile errors = the exact reader set (in `TerrainQuad::draw` and possibly the `=0` revert loop / hoist check).

- [ ] **Step 5: Repoint each errored reader to the cache entry**

For each error site, fetch the static entry and read from it (the same source the deleted assignments copied FROM):
```cpp
const MapData::WorldQuadTerrainCacheEntry* e =
    Terrain::mapData ? Terrain::mapData->getTerrainFaceCacheEntry(tileR, tileC) : NULL;
// e->terrainHandle / e->terrainDetailHandle / e->overlayHandle / e->uvData / e->isCement()
```
Key on the STABLE `tileR/tileC` (NOT the camera-windowed quadList slot). If a reader site (e.g. inside `TerrainQuad::draw`) does not already have `tileR/tileC` in scope, derive them the same way `setupTextures` does before its `getTerrainFaceCacheEntry` call (re-grep the `tileR`/`tileC` computation feeding quad.cpp ~979 and lift the identical derivation; do NOT invent a new tile-identity source). Guard `e == NULL` exactly as the pre-existing `cachedEntry` sites do (quad.cpp ~979). Prefer reusing `enqueueCachedTerrainTriangles(const WorldQuadTerrainCacheEntry&)` (quad.cpp:502) if a site is an enqueue. Repeat Step 4/5 until zero errors.

- [ ] **Step 6: Full relink + deploy**

```bash
rm -f build64/RelWithDebInfo/mc2.exe
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo
cp -f build64/RelWithDebInfo/mc2.exe "A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe" && diff -q build64/RelWithDebInfo/mc2.exe "A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe"
```

- [ ] **Step 7: Slice-1 acceptance smokes (spec gates 4-7)**

```bash
py -3 scripts/run_smoke.py --mission mc2_01 --mission mc2_10 --duration 20 --keep-logs --kill-existing
MC2_TERRAIN_INDIRECT_OVERLAY=0 py -3 scripts/run_smoke.py --mission mc2_01 --duration 20 --keep-logs --kill-existing
```
Expected: PASS both. USER visually confirms: default-armed parity, `=0` revert parity, a cement canary scene unchanged, and a map-edge / zoomed-out camera-window pass (tileR/tileC identity holds). Surface to user for the visual gate.

- [ ] **Step 8: Commit**

```bash
git add mclib/quad.cpp mclib/quad.h
git commit -m "feat(terrain): retire setupTextures recipe->member orphan producer; cache-direct reads (compile-enforced)

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

- [ ] **Step 9: Slice-1 substitutive proof (USER-DRIVEN)** - hand to user: clean non-COST_SPLIT total-frame Tracy at worst-case zoomed-out-big-map; confirm `Terrain::geometry quadSetupTextures` net-shrinks with no displaced cost into `draw`/mission-load. Record. (Honest test = net-shrink, NOT zone-gone - residue stays per spec.)

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
