# Arc 3: Building + Tree Blob Shadow Transform Retirement

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Delete the per-frame `TransformMultiShape` calls for building and tree blob shadow shapes in `bdactor.cpp`, which are gated dead by `!gos_IsTerrainTessellationActive()` (always false in normal in-mission gameplay). This is a dead-code cleanup arc — confirmed by a preflight instrumentation probe before any deletion.

**Architecture:** Two guarded `if` blocks in `BldgAppearance::update()` and `TreeAppearance::update()` perform shadow shape transforms that can never run in-mission (tessellation is always active, making the guard always false). A temporary probe is added first to measure whether the blocks are actually entered, then removed, then the blocks are deleted. The `renderShadows()` early-returns already in place are not touched.

**Tech Stack:** C++ only. No new files. One file modified: `mclib/bdactor.cpp`. Build: `cmake --build build64 --config RelWithDebInfo`. Smoke: `run_smoke.py`.

**Spec:** `docs/superpowers/specs/2026-05-23-building-tree-shadow-dead-write-retirement.md`

---

## Files

- Modify (then revert instrumentation): `mclib/bdactor.cpp`

---

## Task 0: Pre-flight — verify deletion targets exist

**Files:**
- Read: `mclib/bdactor.cpp`

- [ ] **Step 1: Grep for the tessellation guards**

Run from the worktree root (`A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev`):

```bash
grep -n "!gos_IsTerrainTessellationActive" mclib/bdactor.cpp
```

Expected output — exactly 2 matches:
```
2387:		if (bldgShadowShape && useShadows && !gos_IsTerrainTessellationActive())
4620:		if (treeShadowShape && useShadows && !gos_IsTerrainTessellationActive())
```

(Line numbers may drift slightly; confirm one match is inside `BldgAppearance::update` and one inside `TreeAppearance::update`.)

If 0 matches: the blocks were already deleted — skip to Task 3 to verify gates.
If more than 2 matches: STOP and report; the spec did not anticipate additional sites.

- [ ] **Step 2: Confirm surrounding context for building block**

```bash
grep -n -A 6 "bldgShadowShape && useShadows && !gos_IsTerrainTessellationActive" mclib/bdactor.cpp
```

Expected: the `if` block containing `SetRecalcShadows`, `SetLightList`, and `TransformMultiShape` for `bldgShadowShape`.

- [ ] **Step 3: Confirm surrounding context for tree block**

```bash
grep -n -A 6 "treeShadowShape && useShadows && !gos_IsTerrainTessellationActive" mclib/bdactor.cpp
```

Expected: same pattern for `treeShadowShape`.

---

## Task 1: Add ARC3_PROBE instrumentation, build, run, verify decision gate

The probe must prove that the outer `bldgShadowShape && useShadows` check fires (path exercised) but the inner `!gos_IsTerrainTessellationActive()` check never fires (transform body dead). Both conditions together are the safety proof.

**Files:**
- Modify: `mclib/bdactor.cpp` (temporary; reversed in Task 2)

- [ ] **Step 1: Replace the building shadow block with the probe**

Find and replace this exact block in `mclib/bdactor.cpp` (the comment block plus the `if`):

```cpp
		// Skip the legacy-blob-shadow per-frame transform when shadow maps are
		// active. BldgAppearance::renderShadows() at line 2010 already early-
		// returns under the same condition (gos_IsTerrainTessellationActive),
		// meaning bldgShadowShape's transformed state is never consumed in this
		// pipeline. Per-actor saving = 1 TransformMultiShape call per visible
		// building per frame (~3 µs of pure waste).
		if (bldgShadowShape && useShadows && !gos_IsTerrainTessellationActive())
		{
			bldgShadowShape->SetRecalcShadows(checkShadows);
			bldgShadowShape->SetLightList(eye->getWorldLights(),eye->getNumLights());
			bldgShadowShape->TransformMultiShape (&xlatPosition,&rot);
		}
```

Replace with:

```cpp
		// [ARC3_PROBE v1] temporary probe — remove before deletion commit
		{
			static long long s_bldg_candidates    = 0;
			static long long s_bldg_xform_entries = 0;
			static long long s_bldg_tess_true     = 0;
			static long long s_bldg_tess_false    = 0;
			if (bldgShadowShape && useShadows) {
				++s_bldg_candidates;
				const bool tessActive = gos_IsTerrainTessellationActive();
				if (tessActive) {
					++s_bldg_tess_true;
				} else {
					++s_bldg_tess_false;
					++s_bldg_xform_entries;
					bldgShadowShape->SetRecalcShadows(checkShadows);
					bldgShadowShape->SetLightList(eye->getWorldLights(), eye->getNumLights());
					bldgShadowShape->TransformMultiShape(&xlatPosition, &rot);
				}
				if ((s_bldg_candidates % 500) == 0)
					fprintf(stderr, "[ARC3_PROBE v1] bldg candidates=%lld xform_entries=%lld tess_true=%lld tess_false=%lld\n",
					        s_bldg_candidates, s_bldg_xform_entries, s_bldg_tess_true, s_bldg_tess_false);
			}
		}
```

- [ ] **Step 2: Replace the tree shadow block with the probe**

Find and replace this exact block:

```cpp
		// Skip the legacy-blob-shadow per-frame transform when shadow maps are
		// active — same rationale as BldgAppearance::update. TreeAppearance::
		// renderShadows() at line 4544 already early-returns under the same
		// condition; treeShadowShape's transformed state is never consumed.
		if (treeShadowShape && useShadows && !gos_IsTerrainTessellationActive())
		{
			treeShadowShape->SetRecalcShadows(checkShadows);
			treeShadowShape->SetLightList(eye->getWorldLights(),eye->getNumLights());
			treeShadowShape->TransformMultiShape (&xlatPosition,&rot);
		}
```

Replace with:

```cpp
		// [ARC3_PROBE v1] temporary probe — remove before deletion commit
		{
			static long long s_tree_candidates    = 0;
			static long long s_tree_xform_entries = 0;
			static long long s_tree_tess_true     = 0;
			static long long s_tree_tess_false    = 0;
			if (treeShadowShape && useShadows) {
				++s_tree_candidates;
				const bool tessActive = gos_IsTerrainTessellationActive();
				if (tessActive) {
					++s_tree_tess_true;
				} else {
					++s_tree_tess_false;
					++s_tree_xform_entries;
					treeShadowShape->SetRecalcShadows(checkShadows);
					treeShadowShape->SetLightList(eye->getWorldLights(), eye->getNumLights());
					treeShadowShape->TransformMultiShape(&xlatPosition, &rot);
				}
				if ((s_tree_candidates % 500) == 0)
					fprintf(stderr, "[ARC3_PROBE v1] tree candidates=%lld xform_entries=%lld tess_true=%lld tess_false=%lld\n",
					        s_tree_candidates, s_tree_xform_entries, s_tree_tess_true, s_tree_tess_false);
			}
		}
```

- [ ] **Step 3: Confirm `fprintf` header is available**

```bash
grep -n "#include.*stdio\|#include.*cstdio" mclib/bdactor.cpp | head -5
```

If `<stdio.h>` or `<cstdio>` is not already included, add it near the top of the file's existing `#include` block. If it is present, skip.

- [ ] **Step 4: Build with probe**

```bash
cmake --build "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64" --config RelWithDebInfo 2>&1 | tail -20
```

Expected: build succeeds, no errors. The probe uses only standard C stdio — no unusual dependencies.

- [ ] **Step 5: Deploy the probed exe**

```bash
cp -f "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/RelWithDebInfo/mc2.exe" "A:/Games/mc2-opengl/mc2-win64-water/mc2.exe"
cp -f "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/RelWithDebInfo/mc2.pdb" "A:/Games/mc2-opengl/mc2-win64-water/mc2.pdb"
```

- [ ] **Step 6: Run probe smoke (mc2_01 — has visible buildings and trees)**

```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --missions mc2_01 --duration 30 --kill-existing --keep-logs --exe A:/Games/mc2-opengl/mc2-win64-water/mc2.exe
```

- [ ] **Step 7: Read probe output from smoke log**

```bash
LATEST=$(ls -td "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/tests/smoke/artifacts"/*/  | head -1)
grep "ARC3_PROBE" "${LATEST}mc2_01/mc2_01.log" 2>/dev/null || grep "ARC3_PROBE" "${LATEST}mc2_01.log" 2>/dev/null || echo "Check stderr output in smoke artifact"
```

Also check the ring_trace.log if the above finds nothing:
```bash
grep "ARC3_PROBE" "${LATEST}"*/ring_trace.log 2>/dev/null | head -20
```

The probe prints to stderr; the smoke runner captures stderr into the mission log. Look for lines matching `[ARC3_PROBE v1]`.

- [ ] **Step 8: Apply decision gate**

From the probe output, record the final values of all four counters for both actor types.

**PROCEED only if ALL of the following hold:**
- `bldg candidates > 0` (buildings were present and processed in mc2_01)
- `bldg xform_entries == 0` (building transform body never ran)
- `bldg tess_false == 0` (tessellation was always true for buildings)
- `tree candidates > 0` (trees were present and processed in mc2_01)
- `tree xform_entries == 0` (tree transform body never ran)
- `tree tess_false == 0` (tessellation was always true for trees)

**If `candidates == 0` for either type:** The smoke did not exercise that actor type. Extend the smoke duration or switch to a mission known to have those actors (`mc2_03` has many buildings). Do NOT treat zero candidates as proof of safety — rerun.

**If `xform_entries > 0` or `tess_false > 0` for either type:** STOP. Do not delete. Report the counter values; the non-tessellation path is active and the spec assumption is wrong.

Record the probe values — you will need them for the commit message in Task 5.

---

## Task 2: Remove probe, delete both transform blocks

**Files:**
- Modify: `mclib/bdactor.cpp`

- [ ] **Step 1: Replace building probe block with nothing (full deletion)**

Find and remove this entire block (the probe comment + braces + contents):

```cpp
		// [ARC3_PROBE v1] temporary probe — remove before deletion commit
		{
			static long long s_bldg_candidates    = 0;
			static long long s_bldg_xform_entries = 0;
			static long long s_bldg_tess_true     = 0;
			static long long s_bldg_tess_false    = 0;
			if (bldgShadowShape && useShadows) {
				++s_bldg_candidates;
				const bool tessActive = gos_IsTerrainTessellationActive();
				if (tessActive) {
					++s_bldg_tess_true;
				} else {
					++s_bldg_tess_false;
					++s_bldg_xform_entries;
					bldgShadowShape->SetRecalcShadows(checkShadows);
					bldgShadowShape->SetLightList(eye->getWorldLights(), eye->getNumLights());
					bldgShadowShape->TransformMultiShape(&xlatPosition, &rot);
				}
				if ((s_bldg_candidates % 500) == 0)
					fprintf(stderr, "[ARC3_PROBE v1] bldg candidates=%lld xform_entries=%lld tess_true=%lld tess_false=%lld\n",
					        s_bldg_candidates, s_bldg_xform_entries, s_bldg_tess_true, s_bldg_tess_false);
			}
		}
```

Replace with: *(nothing — the entire block is deleted)*

- [ ] **Step 2: Replace tree probe block with nothing (full deletion)**

Find and remove this entire block:

```cpp
		// [ARC3_PROBE v1] temporary probe — remove before deletion commit
		{
			static long long s_tree_candidates    = 0;
			static long long s_tree_xform_entries = 0;
			static long long s_tree_tess_true     = 0;
			static long long s_tree_tess_false    = 0;
			if (treeShadowShape && useShadows) {
				++s_tree_candidates;
				const bool tessActive = gos_IsTerrainTessellationActive();
				if (tessActive) {
					++s_tree_tess_true;
				} else {
					++s_tree_tess_false;
					++s_tree_xform_entries;
					treeShadowShape->SetRecalcShadows(checkShadows);
					treeShadowShape->SetLightList(eye->getWorldLights(), eye->getNumLights());
					treeShadowShape->TransformMultiShape(&xlatPosition, &rot);
				}
				if ((s_tree_candidates % 500) == 0)
					fprintf(stderr, "[ARC3_PROBE v1] tree candidates=%lld xform_entries=%lld tess_true=%lld tess_false=%lld\n",
					        s_tree_candidates, s_tree_xform_entries, s_tree_tess_true, s_tree_tess_false);
			}
		}
```

Replace with: *(nothing — the entire block is deleted)*

- [ ] **Step 3: Confirm no ARC3_PROBE markers remain**

```bash
grep -n "ARC3_PROBE" mclib/bdactor.cpp
```

Expected: 0 matches. If any remain, the probe was not fully removed.

---

## Task 3: Post-deletion grep verification

**Files:**
- Read: `mclib/bdactor.cpp`

All three gates must return 0 matches before proceeding to build.

- [ ] **Step 1: Gate A — tessellation guard gone**

```bash
grep -n "!gos_IsTerrainTessellationActive" mclib/bdactor.cpp
```

Expected: **0 matches**.

If any remain: locate the match and determine whether it is in `BldgAppearance::update` or `TreeAppearance::update`. If so, the deletion in Task 2 was incomplete — re-apply. If in a different function, that is a separate (unrelated) site and is not a blocker; document it.

- [ ] **Step 2: Gate B — TransformMultiShape calls gone from update paths**

```bash
grep -n "bldgShadowShape->TransformMultiShape\|treeShadowShape->TransformMultiShape" mclib/bdactor.cpp
```

Expected: **0 matches** in `BldgAppearance::update` and `TreeAppearance::update`. If a match appears in a different function (e.g. a one-time setup or lifecycle function), confirm via context that it is outside `update()` before marking as passed.

- [ ] **Step 3: Gate C — SetRecalcShadows calls gone from update paths**

```bash
grep -n "bldgShadowShape->SetRecalcShadows\|treeShadowShape->SetRecalcShadows" mclib/bdactor.cpp
```

Expected: **0 matches** in `BldgAppearance::update` and `TreeAppearance::update`. Same scoping note as Gate B.

---

## Task 4: Full clean build + deploy + tier1 smoke + visual canary

**Files:**
- Read: build output
- Read: smoke artifact logs

- [ ] **Step 1: Full clean build (remove exe to force relink)**

```bash
rm "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/RelWithDebInfo/mc2.exe"
cmake --build "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64" --config RelWithDebInfo 2>&1 | tail -20
```

Expected: build succeeds, `mc2.exe` produced, 0 errors.

- [ ] **Step 2: Deploy exe (NEVER `cp -r`; always `cp -f` per file)**

```bash
cp -f "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/RelWithDebInfo/mc2.exe" "A:/Games/mc2-opengl/mc2-win64-water/mc2.exe"
cp -f "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/RelWithDebInfo/mc2.pdb" "A:/Games/mc2-opengl/mc2-win64-water/mc2.pdb"
```

Verify deploy:
```bash
diff -q "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/RelWithDebInfo/mc2.exe" "A:/Games/mc2-opengl/mc2-win64-water/mc2.exe"
```
Expected: no output (files identical).

- [ ] **Step 3: Tier1 5/5 smoke**

```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs --exe A:/Games/mc2-opengl/mc2-win64-water/mc2.exe
```

Expected: exit code 0. All 5 missions pass (`mc2_01`, `mc2_03`, `mc2_10`, `mc2_17`, `mc2_24`).

If any mission fails, check:
```bash
LATEST=$(ls -td "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/tests/smoke/artifacts"/*/  | head -1)
cat "${LATEST}"*/ring_trace.log | grep -i "fail\|crash\|error\|FAIL" | head -20
```

- [ ] **Step 4: Visual canary — buildings and trees**

`mc2_01` and `mc2_03` both have visible buildings and trees in the smoke camera path.

Confirm from the smoke logs that:
- No `crash` or `heartbeat_freeze` events appear in `mc2_01` or `mc2_03` ring_trace.log.
- No new failures vs the baseline at HEAD `d15dab5` (the pre-arc HEAD confirmed in spec).

Building/tree blob shadow visuals are **not** expected to regress — those blob shadows were already suppressed by the `renderShadows()` tessellation early-return that was in place before this arc. Confirm buildings and trees still render (geometry + texture) by checking that `mc2_01` and `mc2_03` complete without crash.

If vehicle or mech blob shadows are visible in any tier1 mission and the user reports them broken, that is out of scope for this arc (vehicle/mech paths are untouched). Note it and do not block the commit.

---

## Task 5: Commit

**Files:**
- Git

- [ ] **Step 1: Stage the modified file**

```bash
git add mclib/bdactor.cpp
```

- [ ] **Step 2: Verify staged diff contains only the two deletions**

```bash
git diff --cached mclib/bdactor.cpp | grep "^[+-]" | grep -v "^---\|^+++" | head -40
```

Expected: only removals (lines beginning with `-`). No additions. If any `+` lines appear (other than the `---`/`+++` headers), the probe was not fully cleaned up — go back to Task 2.

- [ ] **Step 3: Commit with probe results**

Use the probe counter values recorded in Task 1 Step 8. Replace `N_BLDG` and `N_TREE` with actual candidate counts:

```bash
git commit -m "$(cat <<'EOF'
retire dead building/tree blob shadow transform blocks (Arc 3)

ARC3_PROBE result:
  bldg candidates=N_BLDG xform_entries=0 tess_true=N_BLDG tess_false=0
  tree candidates=N_TREE xform_entries=0 tess_true=N_TREE tess_false=0
Conclusion: dead-code cleanup only; no runtime CPU win claimed.

Deletes two guarded if-blocks in BldgAppearance::update() and
TreeAppearance::update() that were already unreachable in normal
in-mission gameplay. The renderShadows() tessellation early-returns
already in place are not touched. Vehicle/mech blob shadow paths
and the ISSHADOWS flush loop in txmmgr.cpp are untouched.

MC_TextureManager dual-queue legacy retirement — Arc 3.
Spec: docs/superpowers/specs/2026-05-23-building-tree-shadow-dead-write-retirement.md

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

- [ ] **Step 4: Confirm commit landed**

```bash
git log --oneline -3
```

Expected: Arc 3 commit at HEAD with a clean one-line summary.

---

## Gates Summary

| Gate | Command | Expected |
|---|---|---|
| Pre-flight targets present | `grep -n "!gos_IsTerrainTessellationActive" mclib/bdactor.cpp` | Exactly 2 matches |
| Probe: candidates > 0 | ARC3_PROBE stderr output | bldg>0, tree>0 |
| Probe: xform_entries == 0 | ARC3_PROBE stderr output | bldg=0, tree=0 |
| Probe: tess_false == 0 | ARC3_PROBE stderr output | bldg=0, tree=0 |
| No probe markers remain | `grep -n "ARC3_PROBE" mclib/bdactor.cpp` | 0 matches |
| Gate A: tessellation guard gone | `grep -n "!gos_IsTerrainTessellationActive" mclib/bdactor.cpp` | 0 matches |
| Gate B: TransformMultiShape gone | `grep -n "bldgShadowShape->TransformMultiShape\|treeShadowShape->TransformMultiShape" mclib/bdactor.cpp` | 0 in update() |
| Gate C: SetRecalcShadows gone | `grep -n "bldgShadowShape->SetRecalcShadows\|treeShadowShape->SetRecalcShadows" mclib/bdactor.cpp` | 0 in update() |
| Build | `cmake --build build64 --config RelWithDebInfo` | 0 errors |
| Tier1 smoke | `run_smoke.py --tier tier1 --duration 30` | exit 0, 5/5 pass |
| Staged diff | `git diff --cached` | deletions only, no additions |
