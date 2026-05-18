# Water v2 S6 - Armed-Water Draw-Side Decouple (reframe B) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development (recommended) or superpowers:executing-plans. Steps use `- [ ]` checkboxes.

**Goal:** On armed frames, stop the legacy water path in `TerrainQuad::setupTextures` from doing the per-frame draw-side work (`wx/wy/wz/ww` writes + water texture-handle resolution + the two `addTriangleBulk(water/waterDetail)` enqueues) that the armed GPU water path already renders - SUBSTITUTIVELY (delete the redundant CPU enqueue on armed frames, do not add a path beside it).

**Architecture:** Split the 4 per-water-vertex sub-blocks of `quad.cpp` setupTextures into (i) KEEP-UNCONDITIONAL (the up-to-4 `projectForTerrainAdmission` + the `leastZ/mostZ/leastW/mostW/leastWY/mostWY` 6-tuple min/max + the `clipInfo = clipData` write - M2a locked) and (ii) ARM-GATE (the `wx..ww` writes + handle-resolution + `addTriangleBulk` x2, only inside the `if (clipped1||clipped2)` body; the `else` 0xffffffff sentinel stays unconditional). The gate is a single shared predicate `WaterFastPathOwnsArmedDraw()` (M1a) that `Terrain::renderWater` is refactored to also call (retires the fragile `terrain.cpp:~1184` hand-copy contract).

**Tech Stack:** C++ MSVC RelWithDebInfo, isolated worktree + dedicated `mc2-win64-water` deploy mirror, kill-aware `mc2_01` smoke, env-gated probes.

**Spec:** `docs/superpowers/specs/2026-05-17-water-v2-s6-armed-water-drawside-decouple-design.md` (PLAN-READY; read it - it carries the dual-adversarial outcome 8b, the resolved forks 8c, and the substitutivity proof Section 4). Read it before Task 0.

**Verification model (no GLSL; C++ + probe/smoke + a user-driven cost gate):**
- The wired `[WATER_INVPROJ v1]` probe (`terrain.cpp`, env `MC2_WATER_INVPROJ_PARITY`) is the IN-PLACE SUBSTITUTIVE CANARY: it MUST stay `result=identical` after S6 (because (i) incl. `clipInfo=clipData` is untouched). Any `result=divergent` post-S6 = the gate leaked into (i) = HARD FAIL, abort - not a tuning step.
- New `[WATER_S6 v1]` probe proves (ii) is actually skipped on armed frames (not silently always running).
- Cost proof = a USER-DRIVEN worst-case total-frame Tracy (Task 5) - per-quad `std::chrono` COST_SPLIT scopes are observer-effect-poisoned and disqualified.

**Hard rules:** no emoji any file; grep-verify every file:line at write-time (Rule 0); deploy ONLY to `A:/Games/mc2-opengl/mc2-win64-water/`, NEVER `mc2-win64-v0.4/`; build ALWAYS `--config RelWithDebInfo`; full relink (load-bearing `terrain.cpp`/`quad.cpp`); branch isolated (user integrates separately); kill-aware smoke (wait if a `*v0.4*` mc2.exe runs, never `--kill-existing` it; `mc2_01` only, 30s; marker-gated not exit-code-gated).

---

### Task 0: Plan-stage Rule-0 grounding (close spec V1-V6 + M1a home)

No code change. Produces `docs/superpowers/plans/2026-05-17-water-v2-s6-GROUNDING.md`, the line-ref source of truth for Tasks 1-3.

**Files:** Create `docs/superpowers/plans/2026-05-17-water-v2-s6-GROUNDING.md`

- [ ] **Step 1 (V1):** grep `mclib/quad.cpp` for the 4 water sub-blocks. Record, per sub-block, the EXACT current lines of: the `projectForTerrainAdmission` call, the `clipData =` assignment + the `if(!isVisible) clipData=false`, the `clipInfo = clipData` if/else pair (both branches - MINOR-1: identical, both stay unconditional), the `leastZ/mostZ/leastW/mostW/leastWY/mostWY` min/max, the `wx/wy/wz/ww` writes, the `if (clipped1||clipped2)` open + its body (handle resolution `getTextureHandle/getWaterTextureHandle/getWaterDetailHandle/setDetail` + the two `addTriangleBulk`) + the `else { waterHandle=0xffffffff; waterDetailHandle=0xffffffff; }` sentinel + the block close. Mark each statement KEEP-(i) vs GATE-(ii). Confirm `clipData` is wholly (i)-produced (M2a clean).
- [ ] **Step 2 (V2, M1a):** grep `Terrain::renderWater` early-return conjunction (`terrain.cpp` ~:1209-1217) + the `s_fastPath` definition (~:1194-1196: `getenv("MC2_RENDER_WATER_FASTPATH")!=nullptr || gpu_driven::IsWaterEnabled()`) + the `~:1184` "byte-identical" contract comment. Record all sub-terms verbatim + exact order. Decide + record the home for the new `WaterFastPathOwnsArmedDraw()` (a `Terrain::` static method vs a `gos_terrain_indirect` free fn) such that BOTH `terrain.cpp` renderWater AND `quad.cpp` setupTextures can call it (check includes/TU visibility for both).
- [ ] **Step 3 (V3):** confirm `gos_terrain_indirect::IsFrameSolidArmed()` + `gpu_driven::IsWaterEnabled()` + `WaterStream::IsReady()/GetRecipeCount()` + `Terrain::terrainTextures2` are all reachable from the chosen helper's TU AND from `quad.cpp`. Re-confirm `IsFrameMaskWaterArmed()` still has ZERO consumers (do not regress to it).
- [ ] **Step 4 (V4):** grep every reader of `->wx/->wy/->wz/->ww` and water-quad `clipInfo`. Record: `drawWater` (`quad.cpp` ~:3264, `wx..ww` reads ~:3313-3614) is the sole `wx..ww` reader and is skipped under the same predicate via `renderWater`'s loop early-return; `drawLine` (~:3891) reads `clipInfo` but NOT `wx..ww` and is debug-grid (NOT same-predicate) - confirm it never runs on the armed gameplay path or note it as out-of-scope debug. Confirm slimReduce `clipInfo` write (`terrain.cpp` ~:1668) runs before the setupTextures loop (cull-cascade immune).
- [ ] **Step 5 (V5):** confirm `addTriangleBulk` (`txmmgr.h` ~:850) is the legacy `masterVertexNodes` path and its reservation (setupTextures) + fill (`drawWater` via `renderWater`) are both inside the same predicate (no half-pair when (ii) skipped).
- [ ] **Step 6:** record the existing env-gated probe idiom in `quad.cpp`/`terrain.cpp` (cached `static const bool` + latched `static int` + raw printf/fflush, e.g. `[WATER_REFL v1]`/`[WATER_INVPROJ v1]`) for Task 3's `[WATER_S6 v1]`.
- [ ] **Step 7:** write the addendum with every recorded actual; commit ONLY it: `git add docs/superpowers/plans/2026-05-17-water-v2-s6-GROUNDING.md && git commit -m "docs(water-v2/S6): plan-stage Rule-0 grounding addendum (V1-V6 + M1a home)"`

---

### Task 1: Extract the shared `WaterFastPathOwnsArmedDraw()` predicate (M1a)

**Files:** Modify `mclib/terrain.cpp` (+ its header if the helper is a `Terrain::` method) ONLY.

- [ ] **Step 1:** At the home chosen in GROUNDING Step 2, add the helper returning the FULL conjunction verbatim from GROUNDING (incl. the `s_fastPath` definition inlined: `(getenv("MC2_RENDER_WATER_FASTPATH") != nullptr || gpu_driven::IsWaterEnabled()) && gos_terrain_indirect::IsFrameSolidArmed() && WaterStream::IsReady() && WaterStream::GetRecipeCount() > 0 && Terrain::terrainTextures2 != nullptr`). Preserve the `s_fastPath` getenv-once caching semantics if the original used a function-static (use a function-static inside the helper so the getenv is still evaluated once).

- [ ] **Step 2:** Refactor `Terrain::renderWater`'s early-return (`terrain.cpp` ~:1209-1217 per GROUNDING) to call the helper instead of the inline conjunction. Replace the `~:1184` "MUST stay byte-identical" hand-copy contract comment with a one-line note that the predicate is now single-sourced in the helper (retire the contract).

- [ ] **Step 3: Full relink build**
```
rm -f build64/RelWithDebInfo/mc2.exe
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -20
```
Expected: clean (pre-existing FFmpeg LNK4199 warnings OK).

- [ ] **Step 4: Deploy + parity-regression smoke** (this is a pure refactor; renderWater's predicate value must be identical -> the existing water + parity behaviour must be unchanged). Deploy exe to `mc2-win64-water` (`cp -f` + `diff -q`). Kill-aware `mc2_01` 30s with `MC2_WATER_INVPROJ_PARITY=1`; gate: `[SMOKE v1] event=summary result=pass` AND `[WATER_INVPROJ v1] result=identical` (refactor must not perturb it) AND no `0(N): error`.

- [ ] **Step 5: Commit**
```
git add mclib/terrain.cpp <header-if-any>
git commit -m "refactor(water-v2/S6): single-source WaterFastPathOwnsArmedDraw() predicate (M1a; retires :1184 hand-copy)"
```

---

### Task 2: Interleaved (ii) arm-gate in `quad.cpp` setupTextures

**Files:** Modify `mclib/quad.cpp` (+ `mclib/terrain.cpp` for the one stale-comment fix) ONLY.

- [ ] **Step 1:** In each of the 4 water sub-blocks (exact lines per GROUNDING Step 1), wrap ONLY the (ii) statements in `if (WaterFastPathOwnsArmedDraw()) { /* legacy CPU draw-side */ }` ... actually the inverse: execute (ii) only when the fast path does NOT own the armed draw. Use the exact sense so legacy runs when armed-GPU is NOT active: gate as `if (!WaterFastPathOwnsArmedDraw()) { <wx..ww writes>; <the if(clipped1||clipped2){ handle-resolution + addTriangleBulk x2 } > } else { /* armed: GPU water path owns the draw; skip legacy enqueue */ }`. KEEP UNCONDITIONAL (outside the gate): the `projectForTerrainAdmission` call, the `clipData`/`isVisible` logic, the `clipInfo = clipData` if/else pair (M2a), the `leastZ/mostZ/...` 6-tuple min/max, AND the `else { waterHandle=0xffffffff; waterDetailHandle=0xffffffff; }` sentinel (sonnet MAJOR-2 - sentinel stays unconditional so armed frames always write it; never stale). Mirror the exact KEEP/GATE split GROUNDING Step 1 recorded for all 4 sub-blocks (8 clipInfo branch points stay; do not gate them).

- [ ] **Step 2:** Fix the stale anticipatory comment at `terrain.cpp` ~:1821 ("Armed: setupTextures() gated, waterHandle never set") - it was written for the dead old S6 design. Replace with an accurate note: under reframe-B the (ii) draw-side is skipped on armed frames but (i) projection+reduction+clipInfo and the 0xffffffff sentinel still run.

- [ ] **Step 3: Full relink build** (per Task 1 Step 3 commands).

- [ ] **Step 4: Deploy + canary smoke.** Deploy exe to `mc2-win64-water` (`cp -f`+`diff -q`; NEVER v0.4). Kill-aware `mc2_01` 30s with `MC2_WATER_INVPROJ_PARITY=1`:
```
LOG=$(ls -dt tests/smoke/artifacts/*/ | head -1)
grep -nE "\[SMOKE v1\] event=summary result=pass" "$LOG"mc2_01*.log
grep -nE "\[WATER_INVPROJ v1\]" "$LOG"mc2_01*.log | tail -5
grep -nE "0\([0-9]+\): error" "$LOG"mc2_01*.log
```
GATE (HARD): `result=pass` present AND the LATEST `[WATER_INVPROJ v1]` state is `result=identical` (NOT `divergent`) AND no `0(N): error`. **`result=divergent` => the gate leaked into (i) => REVERT this task's quad.cpp change and re-do Step 1 (do NOT proceed).**

- [ ] **Step 5: Commit**
```
git add mclib/quad.cpp mclib/terrain.cpp
git commit -m "feat(water-v2/S6): arm-gate the (ii) water draw-side; keep (i) projection/reduction/clipInfo unconditional (M2a)"
```

---

### Task 3: `[WATER_S6 v1]` env-gated armed-skip probe

**Files:** Modify `mclib/quad.cpp` ONLY.

- [ ] **Step 1:** At the (ii) gate site (one representative point, e.g. first sub-block, immediately at the `WaterFastPathOwnsArmedDraw()` branch), add an env-gated latched probe matching the GROUNDING Step 6 idiom (cached `static const bool` on `MC2_WATER_S6_TRACE`, `static int s_lastS6=-1` edge latch, raw printf+fflush):
```cpp
if (s_waterS6Trace) {
    int s6 = WaterFastPathOwnsArmedDraw() ? 1 : 0;   // 1 = armed: (ii) skipped
    if (s6 != s_lastS6) {
        printf("[WATER_S6 v1] event=state armedSkip=%d (1=GPU owns draw, (ii) legacy enqueue skipped)\n", s6);
        fflush(stdout);
        s_lastS6 = s6;
    }
}
```
(Do NOT call `WaterFastPathOwnsArmedDraw()` an extra time per quad in the hot path outside the env gate - the probe's call is inside the cached env bool + latched, so it is O(1)/edge only. If GROUNDING shows setupTextures is per-quad hot, hoist the probe to a once-per-frame site that still observes the same predicate.)

- [ ] **Step 2:** Full relink + deploy (per Task 1 Step 3-4 commands, exe only).

- [ ] **Step 3: Commit**
```
git add mclib/quad.cpp
git commit -m "feat(water-v2/S6): [WATER_S6 v1] env-gated armed-skip probe"
```

---

### Task 4: Integrated autonomous gate (canary + armed-skip)

No code change. The autonomously-verifiable acceptance gate.

- [ ] **Step 1:** Kill-aware `mc2_01` 30s with BOTH probes:
```
MC2_SMOKE_MODE=1 MC2_WATER_INVPROJ_PARITY=1 MC2_WATER_S6_TRACE=1 py -3 scripts/run_smoke.py --mission mc2_01 --duration 30 --keep-logs --exe A:/Games/mc2-opengl/mc2-win64-water/mc2.exe
```
- [ ] **Step 2:** Marker-gate the latest artifact: `[SMOKE v1] event=summary result=pass` present; latest `[WATER_INVPROJ v1]` = `result=identical` (substitutive canary - (i) untouched); `[WATER_S6 v1] armedSkip=1` present (proves (ii) IS skipped on armed frames, not silently always running); no `0(N): error`. Any failure => stop, diagnose, do not advance.

---

### Task 5: USER-DRIVEN execute-gate - worst-case total-frame Tracy (the substitutive cost proof)

No code. **This is a user-driven gate; the autonomous loop PAUSES here.** S6 is NOT "done" until this passes.

- [ ] **Step 1:** User captures a clean TOTAL-FRAME Tracy on the deployed `mc2-win64-water` exe, armed, at a user-positioned worst-case wolfman zoom-out over a water-heavy mission (autonomous default-camera under-attributes per `memory/feedback_cost_split_worst_case_camera.md`). Read the coarse `ZoneScopedN("Terrain::geometry quadSetupTextures")` zone (`terrain.cpp` ~:1755).
- [ ] **Step 2:** SUBSTITUTIVE PROOF: vs a pre-S6 capture of the same camera, the `quadSetupTextures` zone must drop on armed frames with NO displaced cost elsewhere (not ~0; a real reduction). If the drop is ~0 / cost merely moved, S6 is non-substitutive in practice -> reassess (do not ship a ~0 slice). The per-quad COST_SPLIT std::chrono scopes are disqualified - total-frame zone only.

---

### Task 6: Final whole-implementation review

- [ ] Dispatch a final code-grounded reviewer over commits Task1..Task3 (BASE = GROUNDING commit, HEAD = Task 3 commit): cross-task coherence - predicate single-sourced & both sites call it; (ii) gate sense correct (legacy runs when NOT armed-owned); (i)+clipInfo+sentinel unconditional; no blocked-path/cull-cascade/dual-queue regression; canary stayed identical; stale comment fixed.

---

## Self-Review

**Spec coverage:** M1a (shared predicate) -> Task 1. M2a (clipInfo unconditional in (i)) -> Task 2 Step 1 KEEP list. (ii) arm-gate + else-sentinel-unconditional + stale-comment (sonnet MAJOR-2) -> Task 2 Steps 1-2. usePerspective consumer caveat (sonnet MAJOR-1) -> covered by the canary staying identical (the consumer is the 6-tuple; (i) untouched) + GROUNDING records it; no separate task needed since (i) is unconditional. M2b-i (accept 1-frame pop, no transition probe) -> no task (declined by design). [WATER_S6 v1] -> Task 3. [WATER_INVPROJ v1] canary gate -> Tasks 1/2/4. Spec V1-V6 -> Task 0. Section 7 execute-gates -> Task 4 (parity, autonomous) + Task 5 (Tracy, user). Final review -> Task 6. Covered.

**Placeholder scan:** none. The helper home, exact line ranges, and probe-hoist decision are explicitly deferred to GROUNDING actuals (Rule 0), not placeholders. Task 2 Step 1 states the gate SENSE explicitly (legacy runs when `!WaterFastPathOwnsArmedDraw()`).

**Type/name consistency:** `WaterFastPathOwnsArmedDraw()` used identically Task 1->2->3->6; `[WATER_S6 v1] armedSkip` and `[WATER_INVPROJ v1] result=identical` consistent across Tasks 3/4. KEEP-(i) vs GATE-(ii) split deferred to GROUNDING Step 1 (single source).
