# Inverse-Projection Consumer-Collapse Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stub the already-broken TacMap minimap viewport-rect overlay, then delete the orphaned `Camera::setInverseProject` / `inverseProjectZ` chain + the per-frame CPU terrain-projection reduction (slimReduce RED + the DIVERGENT water-block reduction) it existed to feed.

**Architecture:** 5 compile-safe, bisectable phases. Phase 1 removes the SOLE live consumer (the broken viewport-rect block), making everything downstream orphaned-but-still-compilable. Phases 2-5 delete the chain in dependency order so no commit ever leaves dangling calls or dead fields. Each phase: grep checklist -> edit -> build foreground -> deploy -> 20s 1-2-mission `--keep-logs` smoke -> USER confirms minimap still renders -> atomic commit.

**Tech Stack:** C++ (`mclib/` `code/` `GameOS/`), CMake RelWithDebInfo + full relink, `run_smoke.py`, Tracy. No new files; only deletions and one edit.

**Spec:** `docs/superpowers/specs/2026-05-19-inverseproject-consumer-collapse-design.md` (HEAD a89e4db) - read it. All file:line below were grep-verified at HEAD 9a2f67d; **re-pin every line at task time** (drift; bare-identifier reads).

---

## Hard rules for every task

- **Grep checklist BEFORE editing** (spec §5 Mandatory grep checklist). For each symbol about to be touched, sweep declarations, definitions, inline wrappers, member-field access (BOTH `->member` and bare-identifier `this->`), comments, external/mod headers across `code/ mclib/ GameOS/ shaders/ data/ docs/`. If any unexpected reader/writer exists, STOP and escalate (premise change = 7th-collision guard).
- **Build FOREGROUND** (incremental link of 1-2 TUs is ~minutes): `"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo 2>&1 | tail -15`. Do NOT background it. If link-stale risk: `rm -f build64/RelWithDebInfo/mc2.exe` + the changed `.obj` first.
- **Deploy after every successful build:** `cp -f build64/RelWithDebInfo/mc2.exe "A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe" && diff -q build64/RelWithDebInfo/mc2.exe "A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe"`.
- **Smoke after every deploy:** `py -3 scripts/run_smoke.py --mission mc2_01 --mission mc2_10 --duration 20 --keep-logs --kill-existing`. Expect `result=PASS (2/2 passed)`, no `GL_INVALID_*` in `tests/smoke/artifacts/<latest>/*.log`. USER is the visual observer per CLAUDE.md - surface and ask "does the minimap base + mover/team markers still render?" (Phase 1 onwards) and "any unrelated UI regression?" (every phase).
- **Stage files BY NAME** (concurrent sessions share this worktree - never `git add -A`/`.`). Each phase = one atomic commit.
- **No commit may leave dangling calls or dead fields** still required by later code. If a phase breaks any invariant (build, smoke, USER visual gate): STOP, do not chain forward, escalate.
- **No emoji** in any file (`feedback_no_emoji_in_files.md`).
- **Carve-out (NEVER touch):** the angular `onScreenR` object cull cascade (terrain.cpp ~1811 `clipInfo` + ~1813-1837 `objBlockInfo`/`objVertexActive` writes + their defs `Terrain::setObjBlockActive`/`setObjVertexActive` ~2268/2282 + `mission.cpp:505-506 clearObjBlocksActive/clearObjVerticesActive` + `objmgr.cpp:1703/1840/2040` consumers). Out of scope. Bundling it = the campaign-death rock.

---

## File Structure (modify-only; no new files)

- **`code/gametacmap.cpp`** - Phase 1 deletes the broken viewport-rect block (~212-276). Minimap base bitmap (~128-170) + mover/team draw (~278+) preserved.
- **`mclib/camera.h`** - Phase 2 deletes `inverseProjectForPicking` inline (~634-636) + the `[[deprecated]] inverseProjectZ` decl (~626). Phase 3 deletes `setInverseProject` (~1115) + the 4 scalar member fields `startZInverse/startWInverse/zPerPixel/wPerPixel` (~141-144).
- **`mclib/camera.cpp`** - Phase 2 deletes the `inverseProjectZ` body (~1941+).
- **`mclib/terrain.cpp`** - Phase 4 deletes `setInverseProject` call (~2119) + `yzRange/ywRange` compute (~2051-2055) + slimReduce RED reduction (~1878-1898). Phase 5 deletes the 6-global per-frame reset (~1600-1602) + storage definition (~1533-1535).
- **`mclib/quad.cpp`** - Phase 5 deletes the water-block 6-global writers in `CostSplitWaterVertProjScope` (~1101-1287, per spec §2 self-contained rule) + the 6 file-scope `extern float` decls (~540-545). `worldToTacMap` deletion is conditional (Phase 1 grep-decides).

---

## Task 1 (Phase 1): Stub the broken minimap viewport-rect consumer

**Files:**
- Modify: `code/gametacmap.cpp` (delete the ~212-276 viewport-rect block in `GameTacMap::render`)

This makes the entire downstream chain (`inverseProjectForPicking` -> `inverseProjectZ` -> `setInverseProject` scalars -> terrain `slimReduce` RED reduction -> `quad.cpp` water-block reduction) orphaned-but-compilable, so Phases 2-5 can delete it safely.

- [ ] **Step 1: Re-pin the block at HEAD**

Run:
```bash
cd A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev
grep -n "this is the little viewing rect\|worldToTacMap\|inverseProjectForPicking\|gos_DrawQuads( &corners\[0\], 4 )" code/gametacmap.cpp | head -20
```
Expected: the start anchor (`// this is the little viewing rect`), 4 `worldToTacMap(world, corners[N])`, 4 `eye->inverseProjectForPicking(nScreen, world)`, and ONE `gos_DrawQuads(&corners[0], 4)` near the end of the block (~276). The end of the block is the `gos_DrawQuads` line. If anchors moved: pin the new line range before editing.

- [ ] **Step 2: Confirm `worldToTacMap` caller set**

Run:
```bash
grep -rn "worldToTacMap" code/ mclib/ GameOS/
```
Expected: only the 4 calls inside the viewport-rect block + the function definition. If the 4 are sole callers, `worldToTacMap` itself can be deleted in this same commit; if other callers exist, leave it.

- [ ] **Step 3: Delete the viewport-rect block in `GameTacMap::render`**

In `code/gametacmap.cpp`, delete from the `// this is the little viewing rect` comment (~line 212) through and including the `gos_DrawQuads( &corners[0], 4 );` line (~276). Preserve all code before and after that block. If Step 2 confirmed `worldToTacMap` has no other callers, also delete its definition.

DO NOT touch the minimap base bitmap setup/draw (~128-170, the `gos_DrawTriangles( corners, 3 )` pair). DO NOT touch the mover/team draw loops (~278+).

- [ ] **Step 4: Build foreground**

Run:
```bash
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo 2>&1 | tail -15
```
Expected: zero errors. If any error references `inverseProjectForPicking`/`worldToTacMap`/`corners[]`/`nScreen`/`world` after this deletion, you over-deleted - inspect and narrow.

- [ ] **Step 5: Deploy**

Run:
```bash
cp -f build64/RelWithDebInfo/mc2.exe "A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe" && diff -q build64/RelWithDebInfo/mc2.exe "A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe"
```
Expected: no output from `diff -q` (byte-identical = deploy OK).

- [ ] **Step 6: Smoke**

Run:
```bash
py -3 scripts/run_smoke.py --mission mc2_01 --mission mc2_10 --duration 20 --keep-logs --kill-existing
```
Expected: `result=PASS (2/2 passed)`. Grep the latest artifact dir for failures:
```bash
A=$(ls -1t tests/smoke/artifacts | head -1); grep -aE "GL_INVALID|FATAL|SEGV|ASSERT" tests/smoke/artifacts/$A/*.log | head
```
Expected: no matches.

- [ ] **Step 7: USER visual gate**

Surface to USER: "Phase 1 deployed. Please confirm during the smoke (or a follow-up `mc2.exe` launch) that: (a) the minimap base bitmap renders, (b) mover/team markers render on it, (c) the (already-broken) viewport-rect trapezoid overlay is gone, (d) no other UI regression." Do not advance to Phase 2 until USER confirms.

- [ ] **Step 8: Commit**

```bash
git add code/gametacmap.cpp
git commit -m "$(cat <<'EOF'
refactor(gametacmap): stub broken minimap viewport-rect overlay (Phase 1)

Removes the GameTacMap::render viewport-rect trapezoid block. The
overlay was already broken (camera vertices way off, user-acknowledged
2026-05-19) and is being killed and reimplemented separately. This
detaches the sole live consumer of the Camera::inverseProjectZ /
setInverseProject chain so Phases 2-5 can delete it without dangling
calls. Minimap base bitmap + mover/team markers untouched. Spec:
docs/superpowers/specs/2026-05-19-inverseproject-consumer-collapse-design.md.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2 (Phase 2): Delete `Camera::inverseProjectForPicking` + `Camera::inverseProjectZ`

**Files:**
- Modify: `mclib/camera.h` (delete `inverseProjectForPicking` inline ~634-636 + `[[deprecated]] inverseProjectZ` decl ~626)
- Modify: `mclib/camera.cpp` (delete `inverseProjectZ` body ~1941+)

After Phase 1 the only call to `inverseProjectForPicking` was inside the deleted block, and `inverseProjectForPicking` was the only path to `inverseProjectZ`. This phase deletes both, leaving `setInverseProject` + the 4 scalars as the next orphan (Phase 3 target).

- [ ] **Step 1: Grep checklist (spec §5)**

Run, in this order:
```bash
grep -rn "inverseProjectForPicking" code/ mclib/ GameOS/ shaders/ docs/
grep -rn "inverseProjectZ" code/ mclib/ GameOS/ shaders/ docs/
```
Expected, `inverseProjectForPicking`: ONLY (a) the inline def in `camera.h` (~634-636), (b) the deprecated banner in `camera.h:622-633` (the `[[deprecated]]` decl above `inverseProjectZ`), and (c) maybe a comment in `camera.cpp:1947` / `txmmgr.cpp:1887-1909`. Zero remaining CALLERS (the 4 gametacmap calls are gone after Phase 1).
Expected, `inverseProjectZ`: the decl in `camera.h:~626`, the def in `camera.cpp:~1941`, comments in `terrain_depth_bias.h:48/70` + `terrain.cpp:1549` + `camera.cpp:1947/1956`, the call from inside `inverseProjectForPicking` inline. Zero other CALLERS.

If any unexpected caller exists, STOP and escalate.

- [ ] **Step 2: ABI evidence (per spec §5, Phase-2-specific)**

Record positive evidence for the Phase-2 commit message:

```bash
grep -rn "__declspec\|dllexport\|GAMEOS_API\|MCLIB_API\|MC_API\|visibility(\"default\"\)" mclib/camera.h | grep -i "inverseProject\|setInverseProject\|inverseProjectZ"
```
Expected: empty. Then opposite-direction: are camera.h symbols mass-exported elsewhere?
```bash
grep -n "Camera::" mclib/camera.h | grep -i "inverseProject" | head
ls -1 *.def 2>/dev/null; find . -maxdepth 3 -name "*.def" -not -path "./.claude/*" 2>/dev/null | head
```
Expected: no .def-exported `Camera::inverseProject*` symbols. Then SDK/mod-header consumer:
```bash
grep -rn "inverseProjectForPicking\|inverseProjectZ" --include="*.h" code/ | grep -v "^mclib/"
```
Expected: empty (no public-header consumer outside mclib internals).

Record the three findings (no `__declspec`/exports, no .def export, no public-header consumer outside mclib) verbatim in the Phase-2 commit message. A stock-mission smoke (Step 6 below) is the third leg.

- [ ] **Step 3: Delete from `camera.h`**

In `mclib/camera.h` around line 622-636: delete the `[[deprecated(...)]]` decl block for `inverseProjectZ` and the `inverseProjectForPicking` inline that follows it. Preserve everything before and after. Re-grep to confirm both names are gone from the header:
```bash
grep -n "inverseProjectZ\|inverseProjectForPicking" mclib/camera.h
```
Expected: only inline comments / dead references remain (if any), no live decls.

- [ ] **Step 4: Delete from `camera.cpp`**

In `mclib/camera.cpp` around line 1941: delete the `Camera::inverseProjectZ` function body (from the function header through its closing brace). The function ends around line 1995 - re-pin its closing brace before deleting. Preserve everything before and after.

Verify the comment-only references at `camera.cpp:1947/1956` (Tracy event names and the "projectForTerrainAdmission extrema" annotation) - since their containing function is going away, they go with it (they're inside the body). External comments (`terrain.cpp:1549`, `terrain_depth_bias.h:48/70`) become stale references; clean them up in Phase 4/5 commits when those files are touched (do NOT touch them here - keeps the diff focused).

- [ ] **Step 5: Build + deploy**

```bash
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo 2>&1 | tail -15
cp -f build64/RelWithDebInfo/mc2.exe "A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe" && diff -q build64/RelWithDebInfo/mc2.exe "A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe"
```
Expected: zero errors. If any error references `inverseProjectZ`/`inverseProjectForPicking`, an unexpected caller existed - STOP, do NOT silently delete the caller; escalate (Step 1 should have caught it).

- [ ] **Step 6: Smoke (stock-mission included; the ABI-evidence third leg)**

```bash
py -3 scripts/run_smoke.py --mission mc2_01 --mission mc2_10 --duration 20 --keep-logs --kill-existing
A=$(ls -1t tests/smoke/artifacts | head -1); grep -aE "GL_INVALID|FATAL|SEGV|ASSERT" tests/smoke/artifacts/$A/*.log | head
```
Expected: `result=PASS (2/2 passed)`, no errors. `mc2_01` is a stock mission - it passing IS the stock-mission ABI smoke.

- [ ] **Step 7: USER visual gate**

Surface: "Phase 2 deployed. The deprecated inverseProjectZ chain is half-deleted. Please confirm minimap base + movers + general gameplay still render with no regression."

- [ ] **Step 8: Commit (with ABI evidence)**

```bash
git add mclib/camera.h mclib/camera.cpp
git commit -m "$(cat <<'EOF'
refactor(camera): delete deprecated inverseProjectForPicking + inverseProjectZ (Phase 2)

The chain's sole live caller was the broken minimap viewport-rect block,
removed in Phase 1. inverseProjectZ was already [[deprecated]]
(camera.h:626). ABI evidence: no __declspec/dllexport on
Camera::inverseProject* (grep mclib/camera.h); no .def-exported symbol;
no public-header consumer outside mclib (grep --include=*.h excluding
mclib/). Stock-mission smoke mc2_01 PASS (2/2). The 3 legs are
satisfied (no exports, no public consumer, stock smoke passing) -
deletion is ABI-safe.

External comments in terrain.cpp:1549 + terrain_depth_bias.h:48/70 are
left for now (cleaned in Phases 4/5 to keep this diff focused).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3 (Phase 3): Delete `Camera::setInverseProject` + the 4 scalar fields

**Files:**
- Modify: `mclib/camera.h` (delete `setInverseProject` ~1115-1121 + member fields `startZInverse/startWInverse/zPerPixel/wPerPixel` ~141-144)

After Phase 2 the only reader of the 4 scalars (`inverseProjectZ` body) is gone. The only writer remaining is `terrain.cpp:2119` (still present - Phase 4 deletes it). To delete the storage in this phase compile-safely, we must delete `setInverseProject` first - the call site at terrain.cpp:2119 then needs replacement (or removal) too. So Phase 3 carries the terrain.cpp:2119 call-site deletion as a paired change (the alternative - delete the scalars while leaving the call - leaves a dangling write to non-existent fields = build break).

**Paired files:**
- Modify: `mclib/terrain.cpp` (delete the `eye->setInverseProject(...)` call at ~2119; the producing 6-tuple + yzRange/ywRange remain and are dead-stored until Phase 4)

- [ ] **Step 1: Grep checklist**

```bash
grep -rn "setInverseProject\|startZInverse\|startWInverse\|zPerPixel\|wPerPixel" code/ mclib/ GameOS/ shaders/ docs/
```
Expected: `setInverseProject` - decl at `camera.h:~1115`, call at `terrain.cpp:~2119`, comments in `terrain.cpp:1629/1641/1911/2059` and `terrain_depth_bias.h`. Scalars - decls at `camera.h:141-144`, and they were ONLY read inside the deleted `inverseProjectZ` body (Phase 2). Bare-identifier sweep:
```bash
grep -rnE "\b(startZInverse|startWInverse|zPerPixel|wPerPixel)\b" mclib/ code/ GameOS/
```
Expected: only the 4 lines in `camera.h:141-144` + the 4 lines inside the `setInverseProject` setter body (camera.h:~1117-1120). No bare-identifier reads anywhere else (the `inverseProjectZ` body was the sole reader and is gone post-Phase-2).

If any reader remains, STOP and escalate.

- [ ] **Step 2: Delete the call site at `terrain.cpp:2119`**

Re-pin with `grep -n "setInverseProject" mclib/terrain.cpp`. Delete the single line `eye->setInverseProject(mostZ, leastW, yzRange, ywRange);` (~2119). The producer variables (`leastZ/mostZ/leastW/mostW/leastWY/mostWY` + `yzRange/ywRange`) become dead-stored (no readers) but still exist - that's fine for compile; Phase 4 deletes the producer.

- [ ] **Step 3: Delete `setInverseProject` from `camera.h`**

Delete the `setInverseProject(float sZ, float sW, float zPP, float wPP)` inline setter and its body (~camera.h:1115-1121). Preserve everything around.

- [ ] **Step 4: Delete the 4 scalar member fields**

In `mclib/camera.h` around line 141-144, delete the four member declarations:
```cpp
float startZInverse;
float startWInverse;
float zPerPixel;
float wPerPixel;
```
Preserve adjacent fields. Re-grep to confirm zero remaining references:
```bash
grep -rnE "\b(setInverseProject|startZInverse|startWInverse|zPerPixel|wPerPixel)\b" code/ mclib/ GameOS/
```
Expected: empty (or comments only).

- [ ] **Step 5: Build + deploy**

```bash
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo 2>&1 | tail -15
cp -f build64/RelWithDebInfo/mc2.exe "A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe" && diff -q build64/RelWithDebInfo/mc2.exe "A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe"
```
Expected: zero errors. If any error references the deleted symbols, an unexpected reference existed - STOP and escalate.

- [ ] **Step 6: Smoke + USER visual gate**

```bash
py -3 scripts/run_smoke.py --mission mc2_01 --mission mc2_10 --duration 20 --keep-logs --kill-existing
A=$(ls -1t tests/smoke/artifacts | head -1); grep -aE "GL_INVALID|FATAL|SEGV|ASSERT" tests/smoke/artifacts/$A/*.log | head
```
Expected: PASS (2/2), no errors. Surface to USER: "Phase 3 deployed. The Camera inverse-projection state + setter are gone. Please confirm gameplay + minimap + UI still render."

- [ ] **Step 7: Commit**

```bash
git add mclib/camera.h mclib/terrain.cpp
git commit -m "$(cat <<'EOF'
refactor(camera,terrain): delete setInverseProject + 4 scalar fields (Phase 3)

After Phase 2 deleted inverseProjectZ (the sole reader of the 4
scalars), the storage was dead. Phase 3 deletes Camera::setInverseProject
(camera.h ~1115) + the four scalar member fields startZInverse/
startWInverse/zPerPixel/wPerPixel (camera.h:141-144) + the paired
setInverseProject call at terrain.cpp:~2119. Bare-identifier grep
confirms zero remaining readers. The producer variables (6-tuple +
yzRange/ywRange) remain dead-stored in terrain.cpp until Phase 4
deletes the producer.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4 (Phase 4): Delete the terrain RED reduction + yzRange/ywRange

**Files:**
- Modify: `mclib/terrain.cpp` (delete `yzRange/ywRange` compute ~2051-2055 + slimReduce RED reduction ~1878-1898 + clean stale comments)

Phase 3 deleted the call at terrain.cpp:2119; the variables it consumed (`mostZ/leastW/yzRange/ywRange`) are now dead-stored. This phase deletes the producers. The 6 file-scope globals + their definition site + per-frame reset survive to Phase 5 (the water block in `quad.cpp` still writes them; deleting the storage now would break that compile).

- [ ] **Step 1: Re-pin the terrain producer sites**

```bash
grep -n "yzRange\|ywRange\|^[[:space:]]*leastZ *= \|mostZ *= \|leastW *= \|mostW *= \|leastWY *= \|mostWY *= " mclib/terrain.cpp | sed -n '1,30p'
```
Expected: yzRange/ywRange compute around 2051-2055 (`yzRange = (mostZ - leastZ) / (mostWY - leastWY); ywRange = (mostW - leastW) / (mostWY - leastWY);`); slimReduce RED writes at ~1878-1898 (six `if (... < / > leastZ/mostZ/leastW/mostW)` blocks setting the globals + `leastWY/mostWY = sp.y` partners); per-frame reset ~1600-1602 (Phase 5).

- [ ] **Step 2: Delete the yzRange/ywRange compute**

In `mclib/terrain.cpp` around line 2051-2055, delete the two `yzRange = ...` and `ywRange = ...` lines (and any local declarations of those that have no other use). Preserve any surrounding code that has unrelated purpose.

- [ ] **Step 3: Delete the slimReduce RED reduction**

In `mclib/terrain.cpp` around line 1878-1898 (inside the slimReduce loop), delete the six `if`-blocks that write `leastZ/mostZ/leastW/mostW/leastWY/mostWY` from `sp.z/sp.w/sp.y`. PRESERVE everything else in the slimReduce loop: the `clipInfo` write (~1811), the angular `onScreenR` cull-cascade write (~1813-1837 - the carve-out), the per-vertex projection call itself if it has other consumers (re-grep `eye->projectForTerrainAdmission` within the loop; if only the deleted reduction consumed `sp`, the projection call can also go - decide by grep, not by guess).

- [ ] **Step 4: Clean stale external comments in terrain.cpp**

Re-grep:
```bash
grep -n "inverseProjectZ\|setInverseProject\|inverseProjectForPicking" mclib/terrain.cpp
```
Stale comment references (e.g. `:1549 RED : leastZ/mostZ/leastW/mostW reduction (feeds dead inverseProjectZ)`, `:1629 setInverseProject. The extremes of projected screenPos.z/.w/.y`, `:1641 feeding CPU setInverseProject`, `:1911 reduction that feeds eye->setInverseProject below`, `:2059 strictly BEFORE the setInverseProject consumer`) describe deleted code. Either delete each stale comment or rewrite to current state ("RED reduction retired 2026-05-19 - see slice1-postmortem"). Pick deletion if the surrounding code no longer needs the explanation.

- [ ] **Step 5: Build + deploy**

```bash
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo 2>&1 | tail -15
cp -f build64/RelWithDebInfo/mc2.exe "A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe" && diff -q build64/RelWithDebInfo/mc2.exe "A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe"
```
Expected: zero errors. The `quad.cpp` water block still writes the globals - that's fine; they still exist.

- [ ] **Step 6: Smoke + USER visual gate**

```bash
py -3 scripts/run_smoke.py --mission mc2_01 --mission mc2_10 --duration 20 --keep-logs --kill-existing
A=$(ls -1t tests/smoke/artifacts | head -1); grep -aE "GL_INVALID|FATAL|SEGV|ASSERT" tests/smoke/artifacts/$A/*.log | head
```
Expected: PASS (2/2), no errors. Surface to USER: "Phase 4 deployed. The slimReduce RED reduction is retired. Please confirm gameplay + UI still render."

- [ ] **Step 7: Commit**

```bash
git add mclib/terrain.cpp
git commit -m "$(cat <<'EOF'
refactor(terrain): delete slimReduce RED reduction + yzRange/ywRange (Phase 4)

Producers for the Camera::setInverseProject 4 scalars deleted in Phase
3. The slimReduce per-vertex extrema reduction (terrain.cpp ~1878-1898)
and the yzRange/ywRange compute (~2051-2055) are now retired. The
slimReduce loop's clipInfo write + the angular onScreenR object
cull-cascade are preserved (carve-out per spec). Stale external comments
referencing setInverseProject/inverseProjectZ cleaned up.

The 6 file-scope globals (leastZ/leastW/mostZ/mostW/leastWY/mostWY) +
their per-frame reset + their definition site remain because the quad.cpp
water-block still writes them; Phase 5 deletes both together.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5 (Phase 5): Delete water-block writers + per-frame reset + extern decls + storage

**Files:**
- Modify: `mclib/quad.cpp` (delete water-block 6-global writers in `CostSplitWaterVertProjScope` ~1101-1287 + 6 extern decls ~540-545)
- Modify: `mclib/terrain.cpp` (delete per-frame reset ~1600-1602 + storage definition ~1533-1535)

This is the last phase. Per spec §2 self-contained water-block deletion rule: delete ONLY writes/computations whose sole purpose is feeding the six globals; preserve every unrelated water-block side effect (`clipInfo`, `calcThisFrame |= 2`, the `legacyWaterDraw`-gated `wx/wy/wz/ww` writes, fast-path predicates, `addTriangleBulk` setup).

- [ ] **Step 1: Re-pin all targets + characterize the water block**

```bash
grep -n "leastZ\|mostZ\|leastW\b\|mostW\b\|leastWY\|mostWY\|CostSplitWaterVertProjScope\|legacyWaterDraw\|calcThisFrame.*2\|clipInfo *=" mclib/quad.cpp | sed -n '1,60p'
grep -n "^float leastZ\|^float mostZ\|^float leastWY\|leastZ *= *1\.0f\|leastZ *= *.0f" mclib/terrain.cpp | head
```
Expected: globals defined `terrain.cpp:1533-1535` (`float leastZ = 1.0f, leastW = 1.0f; float mostZ = -1.0f, mostW = -1.0; float leastWY = 0.0f, mostWY = 0.0f;`); per-frame reset `terrain.cpp:1600-1602`; externs `quad.cpp:540-545`; water-block writers in 4 per-vertex sub-blocks inside `CostSplitWaterVertProjScope` at quad.cpp ~1058-1124, ~1126-1194, ~1196-1264, ~1266-1334 - within each sub-block, find the `if (clipData) { if (screenPos.z < leastZ) leastZ = screenPos.z; ... }` reduction (the lines that touch the 6 globals).

- [ ] **Step 2: Per-vertex-block decision on `projectForTerrainAdmission`**

For each of the 4 per-vertex water sub-blocks, grep within the sub-block:
```bash
sed -n '1058,1334p' mclib/quad.cpp | grep -n "screenPos\|projectForTerrainAdmission"
```
For each sub-block, decide:
- If `screenPos` is consumed ONLY by the 6-global writes within that sub-block (no `legacyWaterDraw` write of `wx/wy/wz/ww`, no other use): delete the `projectForTerrainAdmission` call too.
- If `screenPos` is consumed by `legacyWaterDraw`-gated `wx/wy/wz/ww` writes (or any other side effect): KEEP the `projectForTerrainAdmission` call (and `screenPos`), delete ONLY the 6-global writes inside the `if (clipData) { ... }` block.

Record the decision per sub-block (1 through 4) in the commit message.

- [ ] **Step 3: Delete the water-block 6-global writers**

In each of the 4 per-vertex sub-blocks, delete the `if (clipData) { if (screenPos.z < leastZ) ... mostWY = screenPos.y; }` reduction (the ~22-line block per vertex that touches the six globals - see the original block 1099-1122 as the pattern). Per Step 2, also delete the per-block `projectForTerrainAdmission` call where the decision was "delete." Preserve `vertices[i]->clipInfo = clipData` writes, `vertices[i]->calcThisFrame |= 2`, `legacyWaterDraw`-gated `wx/wy/wz/ww` writes, and every fast-path/setup statement outside the 6-global reduction.

If the entire `CostSplitWaterVertProjScope` block is now empty after these deletions, also delete the scope wrapper itself.

- [ ] **Step 4: Delete the 6 extern decls in `quad.cpp`**

In `mclib/quad.cpp` around line 540-545, delete the 6 lines:
```cpp
extern float leastZ;
extern float leastW;
extern float mostZ;
extern float mostW;
extern float leastWY;
extern float mostWY;
```
(plus any blank line they collectively occupy).

- [ ] **Step 5: Delete the per-frame reset in `terrain.cpp`**

In `mclib/terrain.cpp` around line 1600-1602, delete the three lines:
```cpp
leastZ = 1.0f; leastW = 1.0f;
mostZ = -1.0f; mostW = -1.0;
leastWY = 0.0f; mostWY = 0.0f;
```
(check exact line endings/format; re-grep to confirm).

- [ ] **Step 6: Delete the storage definition in `terrain.cpp`**

In `mclib/terrain.cpp` around line 1533-1535, delete the three definition lines:
```cpp
float leastZ = 1.0f, leastW = 1.0f;
float mostZ = -1.0f, mostW = -1.0;
float leastWY = 0.0f, mostWY = 0.0f;
```

- [ ] **Step 7: Final grep gate (per spec §5)**

Run, expecting EMPTY output for each:
```bash
grep -rnE "\b(setInverseProject|inverseProjectZ|inverseProjectForPicking|startZInverse|startWInverse|zPerPixel|wPerPixel)\b" code/ mclib/ GameOS/
grep -rnE "\b(leastZ|mostZ|leastW|mostW|leastWY|mostWY)\b" code/ mclib/ GameOS/
```
If any match remains, the deletion is incomplete - inspect and fix in this same commit. (Hits inside comments are still hits worth resolving; rewrite or delete.)

- [ ] **Step 8: Build + deploy**

```bash
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo 2>&1 | tail -15
cp -f build64/RelWithDebInfo/mc2.exe "A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe" && diff -q build64/RelWithDebInfo/mc2.exe "A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe"
```
Expected: zero errors. If anything still references the deleted globals/symbols, fix in this commit before proceeding.

- [ ] **Step 9: Smoke**

```bash
py -3 scripts/run_smoke.py --mission mc2_01 --mission mc2_10 --duration 20 --keep-logs --kill-existing
A=$(ls -1t tests/smoke/artifacts | head -1); grep -aE "GL_INVALID|FATAL|SEGV|ASSERT" tests/smoke/artifacts/$A/*.log | head
```
Expected: PASS (2/2), no errors.

- [ ] **Step 10: USER substitutive proof - worst-case zoomed-out-big-map clean Tracy**

Surface to USER: "Phase 5 deployed - the chain is fully retired. Please capture a clean non-COST_SPLIT total-frame Tracy at worst-case zoomed-out-big-map (tier1 default camera is structurally blind to the regression class). Expected: `Terrain::geometry slimReduce` zone shrinks by the RED reduction's share; `Terrain::geometry quadSetupTextures` shrinks by the water-block reduction's share; no displaced cost elsewhere. Minimap still renders (base + movers; no viewport-rect overlay). Confirm both."

Do NOT advance / claim success without USER's Tracy + visual confirmation.

- [ ] **Step 11: Commit**

```bash
git add mclib/quad.cpp mclib/terrain.cpp
git commit -m "$(cat <<'EOF'
refactor(terrain,quad): delete water-block 6-tuple reduction + 6 globals (Phase 5)

Final phase. Deletes:
- The quad.cpp water-block 6-global reduction writers in
  CostSplitWaterVertProjScope (per-vertex sub-blocks; per-block
  projectForTerrainAdmission decision recorded below).
- The 6 extern float decls in quad.cpp:540-545.
- The per-frame reset in terrain.cpp:1600-1602.
- The storage definition in terrain.cpp:1533-1535.

Per-vertex-block projectForTerrainAdmission decisions:
- Vert 0 (~quad.cpp:1058-1124): <KEEP or DELETE; record per Step 2>
- Vert 1 (~1126-1194): <KEEP or DELETE>
- Vert 2 (~1196-1264): <KEEP or DELETE>
- Vert 3 (~1266-1334): <KEEP or DELETE>

Final grep confirms zero remaining writers, readers, or references to
{setInverseProject, inverseProjectZ, inverseProjectForPicking, 4 scalars,
6 globals} across code/ mclib/ GameOS/. The Slice-2 DIVERGENT-water
nemesis is now MOOT (no reduction -> nothing to diverge). Substitutive
proof: USER worst-case zoomed-out-big-map clean Tracy - slimReduce zone
shrinks by the RED reduction's share, quadSetupTextures shrinks by the
water-block reduction's share, no displaced cost.

Spec: docs/superpowers/specs/2026-05-19-inverseproject-consumer-collapse-design.md
Postmortem: docs/superpowers/specs/2026-05-19-slice1-postmortem.md

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Self-review

**Spec coverage (every section/requirement):**
- Spec §1 finding (sole consumer = 4 TacMap corners; chain = setInverseProject -> inverseProjectZ): Phases 1-3 implement the consumer-then-chain deletion in order.
- Spec §2 deletes (stub consumer; chain in order; 6 globals incl per-frame reset + storage definition; water-block self-contained rule): Phases 1 (consumer), 2 (inlines), 3 (state), 4 (terrain producer), 5 (water + globals + reset + storage) cover all. The §2 water-block preserve-list + per-vertex-block screenPos-use decision are Task 5 Step 2.
- Spec §3 carve-out (angular onScreenR object cull cascade): "Hard rules" + Task 4 Step 3 explicitly preserve it.
- Spec §4 done-criterion (deletion not flagging; clean worst-case Tracy; minimap still renders; no displaced cost): Task 5 Step 10 + Step 11 commit message.
- Spec §5 grep checklist (decls, defs, inline wrappers, member-field access incl bare-identifier, comments, external headers): Task 1 Steps 1-2, Task 2 Step 1, Task 3 Step 1, Task 4 Step 1, Task 5 Steps 1+7 enforce it. Bare-identifier grep `\b<symbol>\b` is used.
- Spec §5 ABI evidence (no exports, no public-header consumer, stock smoke): Task 2 Step 2 + Step 6 + commit Step 8 record all three.
- Spec §5 compile-safe sequencing (no commit leaves dangling): each Phase's build step is the gate; Task 3 explicitly pairs the call-site deletion with the storage so it doesn't dangle; Task 4 leaves the producer-of-storage alive because quad.cpp still writes it; Task 5 deletes water-writers BEFORE storage in one commit.

**Placeholder scan:** the commit message for Task 5 has `<KEEP or DELETE; record per Step 2>` placeholders for the per-vertex decisions - those are explicitly meant to be filled in by the implementer at commit time based on the Step 2 grep result (decision can't be made at plan-write time; it depends on HEAD code). Acceptable: the procedure for filling them in is concrete (Step 2 grep-decision). No other placeholders.

**Type consistency:** all symbol names match across tasks (`setInverseProject`, `inverseProjectZ`, `inverseProjectForPicking`, `startZInverse/startWInverse/zPerPixel/wPerPixel`, `leastZ/leastW/mostZ/mostW/leastWY/mostWY`, `CostSplitWaterVertProjScope`, `clipInfo`, `calcThisFrame`, `legacyWaterDraw`, `worldToTacMap`, `onScreenR`). File paths consistent (`code/gametacmap.cpp`, `mclib/camera.h`, `mclib/camera.cpp`, `mclib/terrain.cpp`, `mclib/quad.cpp`). Line numbers match the spec.
