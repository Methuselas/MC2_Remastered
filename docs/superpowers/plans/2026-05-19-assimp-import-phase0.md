# Assimp Import — Phase 0 Implementation Plan (Plan 1 of 2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land the existing `claude/assimp-testing` glB/FBX import path onto `claude/nifty-mendeleev` as a gated, surgical 8-commit cherry-pick, proving stock ASE/TGL load is behaviorally identical before any cluster-LOD work builds on it.

**Architecture:** `claude/assimp-testing` is a stale branch (forked at `566a0f0`, lacks all later nifty work — a branch merge would revert hundreds of advances). The entire Assimp integration is 8 self-contained "track-d" commits (`feeaeaa..857c965`) already gated behind an `ENABLE_ASSIMP_IMPORTER` CMake option and `MC2_ASSIMP_TRACE` env var. We cherry-pick those 8 commits in order onto nifty, resolve per-commit conflicts caused by nifty's drift, build with the importer gate ON, and verify stock load identity (the load-bearing risk) plus a positive glB-import proof.

**Tech Stack:** git cherry-pick, CMake (RelWithDebInfo, full-relink discipline), vendored Assimp under `3rdparty/assimp/`, `run_smoke.py` tier1, user-driven zoomed-out-big-map visual identity check.

**Scope:** This is Plan 1 of 2. Plan 2 (the substitutive static-prop cluster-LOD arc) is written AFTER this plan's Phase-0 gate (Task 7) passes — its tasks depend on Phase-0 outputs and on spec-deferred traces, and would otherwise be placeholder-laden. Spec: `docs/superpowers/specs/2026-05-19-static-prop-cluster-lod-poc-design.md`.

**The 8 cherry-pick commits (on `claude/assimp-testing`, in order):**

| SHA | Subject | Risk |
|---|---|---|
| `feeaeaa` | build(track-d): add ENABLE_ASSIMP_IMPORTER + vendor Assimp + stubs | CMakeLists.txt conflict; bulk vendored tree |
| `7753d9d` | feat(track-d): narrow construction API for format-agnostic mech import | msl.h conflict (nifty drift) |
| `e843911` | feat(track-d): ImportGeometryFromFile body + BuildTextureList helper | new files, low |
| `d4176d2` | feat(track-d): TG_TypeMultiShape::LoadFromFile probe-only entry point | msl.cpp conflict (nifty drift) |
| `70a8a88` | feat(track-d): wire mech3d.cpp:286 to LoadFromFile + parse [Import] Source= | HIGH — site drifted to mech3d.cpp:304/:322 |
| `066e6a9` | feat(track-d): probe .fbx as well as .glb in LoadFromFile | low |
| `f19ce2e` | fix(track-d): actually wire ENABLE_ASSIMP_IMPORTER into mclib build | mclib/CMakeLists.txt conflict |
| `857c965` | chore(track-d): env-gate ASSIMP_TRACE behind MC2_ASSIMP_TRACE | low |

Engine-glue delta (excluding vendored tree): ~965 lines across `CMakeLists.txt`, `mclib/CMakeLists.txt`, `mclib/assimp_importer.{cpp,h}`, `mclib/mech3d.cpp`, `mclib/msl.{cpp,h}`, `mclib/tgl.{cpp,h}`. All import symbols confirmed ABSENT on nifty HEAD (no conflicting prior definitions).

---

### Task 1: Pre-flight — safety tag, clean tree, conflict dry-run

**Files:** none modified (inspection + tag only)

- [ ] **Step 1: Confirm clean working tree on nifty**

Run:
```bash
cd A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev
git status --porcelain
git rev-parse --abbrev-ref HEAD
```
Expected: empty porcelain output; branch `claude/nifty-mendeleev`. If not clean, STOP and resolve before proceeding.

- [ ] **Step 2: Create a safety tag to roll back to**

Run:
```bash
git tag pre-assimp-phase0 HEAD
git rev-parse --short pre-assimp-phase0
```
Expected: prints the current HEAD short SHA (e.g. `48ba0d8`). This is the rollback point if the gate fails.

- [ ] **Step 3: Capture the pre-cherry-pick smoke baseline (for the identity gate)**

Run:
```bash
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```
Expected: exit `0`. Note the artifacts dir printed (`tests/smoke/artifacts/<timestamp>/`). Record per-mission GL-error counts from `report.*` — this is the BASELINE the Task 7 identity gate compares against. Do NOT add `--with-menu-canary`.

- [ ] **Step 4: Dry-run the cherry-pick conflict surface (no apply)**

Run:
```bash
for c in feeaeaa 7753d9d e843911 d4176d2 70a8a88 066e6a9 f19ce2e 857c965; do echo "== $c =="; git cherry-pick --no-commit -n $c 2>&1 | tail -3; git cherry-pick --abort 2>/dev/null; git reset --hard HEAD >/dev/null; done
```
Expected: a per-commit summary of which apply clean vs conflict. Record the conflicting files. (This is informational — the real applies happen in Tasks 2-5 with resolution.)

- [ ] **Step 5: Commit (tag only — nothing to add)**

No code commit. The `pre-assimp-phase0` tag is the artifact. Proceed.

---

### Task 2: Cherry-pick `feeaeaa` — vendor Assimp + build stub

**Files:**
- Add (bulk, mechanical): `3rdparty/assimp/**` (entire vendored tree)
- Modify: `CMakeLists.txt` (ENABLE_ASSIMP_IMPORTER option + vendor wiring)

- [ ] **Step 1: Apply the cherry-pick**

Run:
```bash
git cherry-pick feeaeaa
```
Expected: either clean, or a conflict in `CMakeLists.txt` (nifty has added targets since `566a0f0`).

- [ ] **Step 2: Resolve the CMakeLists.txt conflict if present**

Open `CMakeLists.txt`. The incoming hunk adds an `option(ENABLE_ASSIMP_IMPORTER ...)` and an Assimp `add_subdirectory`/include block. Keep BOTH sides: nifty's existing targets AND the new Assimp block. The Assimp block must be placed so it does not precede nifty's `project()`/global `-DLINUX_BUILD` (CMakeLists.txt defines `-DLINUX_BUILD` globally). Resolution rule: take nifty's version of every non-Assimp hunk; take the incoming version for every Assimp-only hunk.

Run after editing:
```bash
git diff --check
git add CMakeLists.txt 3rdparty/assimp
git cherry-pick --continue
```
Expected: cherry-pick completes; commit recorded.

- [ ] **Step 3: Verify the vendored tree and option landed**

Run:
```bash
git grep -n 'ENABLE_ASSIMP_IMPORTER' -- CMakeLists.txt | head -3
ls 3rdparty/assimp/CMakeLists.txt 3rdparty/assimp/include 2>/dev/null | head
```
Expected: `ENABLE_ASSIMP_IMPORTER` present in `CMakeLists.txt`; vendored Assimp `CMakeLists.txt` and `include/` exist.

- [ ] **Step 4: Commit**

Already committed by `git cherry-pick --continue` (preserves the original `feeaeaa` message). No extra commit.

---

### Task 3: Cherry-pick `7753d9d`, `e843911`, `d4176d2` — importer core

**Files:**
- Modify: `mclib/msl.h` (narrow construction API), `mclib/msl.cpp` (`LoadFromFile` probe), `mclib/tgl.cpp`, `mclib/tgl.h`
- Add: `mclib/assimp_importer.cpp`, `mclib/assimp_importer.h`

- [ ] **Step 1: Cherry-pick the construction-API commit**

Run:
```bash
git cherry-pick 7753d9d
```
Expected: clean, or a conflict in `mclib/msl.h` / `mclib/tgl.h` (nifty drift). Resolution rule: the incoming change ADDS a format-agnostic construction entry point; nifty has no conflicting symbol (confirmed absent). Keep nifty's existing declarations and ADD the incoming new declarations adjacent. Then:
```bash
git add mclib/msl.h mclib/tgl.h mclib/tgl.cpp
git cherry-pick --continue
```

- [ ] **Step 2: Cherry-pick the ImportGeometryFromFile body**

Run:
```bash
git cherry-pick e843911
```
Expected: clean (adds new files `mclib/assimp_importer.{cpp,h}` + a `BuildTextureList` helper). If a conflict in `mclib/msl.cpp`, keep both sides (incoming adds a new helper; nifty's body unchanged).
```bash
git add mclib/assimp_importer.cpp mclib/assimp_importer.h mclib/msl.cpp
git cherry-pick --continue
```

- [ ] **Step 3: Cherry-pick the LoadFromFile probe entry point**

Run:
```bash
git cherry-pick d4176d2
```
Expected: conflict likely in `mclib/msl.cpp` (nifty heavily evolved this file — `LoadTGMultiShapeFromASE` region). Resolution rule: `LoadFromFile` is a NEW probe-only method that does NOT modify `LoadTGMultiShapeFromASE`. Keep nifty's entire existing `LoadTGMultiShapeFromASE`/cache logic verbatim; ADD the new `LoadFromFile` method body from the incoming side. Verify the new method does not alter the stock control flow.
```bash
git add mclib/msl.cpp mclib/msl.h
git cherry-pick --continue
```

- [ ] **Step 4: Verify importer symbols now present**

Run:
```bash
git grep -n 'LoadFromFile\|ImportGeometryFromFile\|kImportExts' -- mclib/msl.cpp mclib/msl.h mclib/assimp_importer.cpp | head
```
Expected: all three symbols present.

- [ ] **Step 5: Commit**

Already committed per cherry-pick (3 preserved commits). No extra commit.

---

### Task 4: Cherry-pick `70a8a88` — mech3d wiring (HIGH RISK: site drift)

**Files:**
- Modify: `mclib/mech3d.cpp` (wire shape load to `LoadFromFile`, parse `[Import] Source=`)

- [ ] **Step 1: Identify the drifted target site**

The commit wires `mech3d.cpp:286` in the old base. On nifty HEAD the equivalent stock-load sites are `mclib/mech3d.cpp:304` and `:322` (`mechShape[i]->LoadTGMultiShapeFromASE(mechName);` / `mechShape[0]->LoadTGMultiShapeFromASE(mechName);`).

Run:
```bash
git grep -n 'LoadTGMultiShapeFromASE' -- mclib/mech3d.cpp | head
```
Expected: confirm the load sites (around :304 / :322) before applying.

- [ ] **Step 2: Apply the cherry-pick (expect conflict)**

Run:
```bash
git cherry-pick 70a8a88
```
Expected: CONFLICT in `mclib/mech3d.cpp` (the `:286` context does not match nifty's `:304`).

- [ ] **Step 3: Resolve by intent, not by line**

The commit's intent: before the stock `LoadTGMultiShapeFromASE(mechName)` call, parse an optional `[Import] Source=` INI key; if present (and `ENABLE_ASSIMP_IMPORTER` built in), call `LoadFromFile` on the import source instead, falling back to the stock ASE/TGL path when absent. Apply that SAME intent at nifty's current site: wrap the `mechShape[i]->LoadTGMultiShapeFromASE(mechName);` at `mclib/mech3d.cpp:304` (and the single-LOD `:322`) with the incoming import-probe guard. Preserve nifty's surrounding LOD-loop logic verbatim. The stock path (no `[Import]` key) MUST be byte-identical to pre-cherry-pick behavior — this is the load-bearing invariant the Task 7 gate verifies.

Run after editing:
```bash
git diff --check
git add mclib/mech3d.cpp
git cherry-pick --continue
```

- [ ] **Step 4: Verify stock path untouched**

Run:
```bash
git diff pre-assimp-phase0 HEAD -- mclib/mech3d.cpp | grep -E '^\+' | grep -v 'Import\|LoadFromFile\|ASSIMP' | head
```
Expected: NO added line that changes stock LOD-load logic — every added `+` line is import-guard/`LoadFromFile`/gate code only. If a non-import stock line changed, the resolution is wrong; redo Step 3.

- [ ] **Step 5: Commit**

Already committed per cherry-pick. No extra commit.

---

### Task 5: Cherry-pick `066e6a9`, `f19ce2e`, `857c965` — fbx probe, mclib build, env-gate

**Files:**
- Modify: `mclib/msl.cpp` (add `.fbx` to probe exts), `mclib/CMakeLists.txt` (wire `ENABLE_ASSIMP_IMPORTER`), `mclib/assimp_importer.cpp` (env-gate trace)

- [ ] **Step 1: Cherry-pick the fbx-probe commit**

Run:
```bash
git cherry-pick 066e6a9
```
Expected: clean or trivial `mclib/msl.cpp` conflict (the `kImportExts[]` array — keep incoming `{".glb",".fbx"}`).
```bash
git add mclib/msl.cpp && git cherry-pick --continue 2>/dev/null || true
```

- [ ] **Step 2: Cherry-pick the mclib build-wiring fix**

Run:
```bash
git cherry-pick f19ce2e
```
Expected: conflict likely in `mclib/CMakeLists.txt` (nifty drift). Resolution rule: keep nifty's existing mclib sources/targets; ADD the incoming `ENABLE_ASSIMP_IMPORTER`-guarded `assimp_importer.cpp` source + Assimp link/include. Then:
```bash
git add mclib/CMakeLists.txt && git cherry-pick --continue
```

- [ ] **Step 3: Cherry-pick the env-gate commit**

Run:
```bash
git cherry-pick 857c965
```
Expected: clean (gates trace prints behind `MC2_ASSIMP_TRACE`).
```bash
git add -A && git cherry-pick --continue 2>/dev/null || true
```

- [ ] **Step 4: Verify all 8 commits landed**

Run:
```bash
git log --oneline pre-assimp-phase0..HEAD | grep -c 'track-d'
```
Expected: `8`.

- [ ] **Step 5: Commit**

Already committed per cherry-pick. No extra commit.

---

### Task 6: Build with importer gate ON (RelWithDebInfo, full relink) + deploy

**Files:** none (build/deploy only)

- [ ] **Step 1: Full-relink build with ENABLE_ASSIMP_IMPORTER ON**

Load-bearing functions in `mclib` changed (msl/tgl/mech3d), so a full relink is required (CMake incremental leaks stale linkage).

Run:
```bash
rm -f build64/RelWithDebInfo/mc2.exe
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" -B build64 -DENABLE_ASSIMP_IMPORTER=ON
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2 --clean-first
```
Expected: configure picks up `3rdparty/assimp` + `ENABLE_ASSIMP_IMPORTER=ON`; build succeeds; `build64/RelWithDebInfo/mc2.exe` exists and is newer than the cherry-picks.

- [ ] **Step 2: Deploy (per-file cp -f, never cp -r)**

Run:
```bash
cp -f build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe
diff -q build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe && echo DEPLOY_OK
```
Expected: `DEPLOY_OK`. Copy any new Assimp runtime DLL the same way if the build produced one (check `build64/RelWithDebInfo/*.dll` newer than baseline; deploy each with `cp -f` + `diff -q`).

- [ ] **Step 3: Commit (no source change — build artifact only)**

No commit. Build/deploy is verified by Task 7.

---

### Task 7: Stock-load identity gate (THE load-bearing verification)

**Files:** none (verification only)

The importer is probe-only and opt-in: it triggers only when an `[Import] Source=` INI key or a `.glb`/`.fbx` is present. Stock assets have none, so the stock ASE/TGL path must be behaviorally identical to the `pre-assimp-phase0` baseline. This task proves that.

- [ ] **Step 1: Automated tier1 smoke (importer built in, unused by stock)**

Run:
```bash
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```
Expected: exit `0`. Compare per-mission GL-error counts in the new `tests/smoke/artifacts/<timestamp>/report.*` against the Task 1 Step 3 BASELINE. PASS criterion: exit 0 AND GL-error counts equal to baseline (no new GL errors introduced by the cherry-pick). FPS is explicitly NOT a criterion.

- [ ] **Step 2: User-driven zoomed-out-big-map identity check**

tier1's default camera is structurally blind to zoomed-out regressions (recurred 3x historically). This step is USER-DRIVEN: the smoke window is live and user-observable.

Run:
```bash
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 60 --kill-existing --keep-logs
```
Ask the user to zoom out to big-map on at least one mission and confirm building/terrain rendering is visually identical to pre-change (no missing geometry, no flicker, no shape drop-outs). Record the user's first-hand visual confirmation. PASS criterion: user confirms visual identity zoomed-out.

- [ ] **Step 3: Decision gate**

If Step 1 OR Step 2 fails: the "shouldn't interfere" hypothesis is FALSIFIED. Roll back (`git reset --hard pre-assimp-phase0`), record which cherry-pick introduced the regression (re-bisect the 8 commits), and do NOT proceed to Plan 2. If both pass: Phase 0 stock-identity gate is GREEN.

- [ ] **Step 4: Commit**

No source commit. Record the gate result in the Task 8 tag message.

---

### Task 8: Positive glB-import proof + Phase-0-complete tag

**Files:**
- Create (test fixture): a minimal `.glb` + a test mech `.ini` with `[Import] Source=<that glb>` (use any existing tiny glB; if none, export a unit cube glB — the proof is that the import path executes and produces geometry, not asset fidelity)

- [ ] **Step 1: Stage a glB import fixture**

Place a known-good small `.glb` under the mech asset path used by a throwaway test mech entry, with an INI containing `[Import]` / `Source=<glb>`. (Exact asset dir: mirror an existing mech `.ini` location; the importer resolves via the same path machinery, `PATH_SEPARATOR`/forward-slash only — no `\\`.)

- [ ] **Step 2: Run with import trace ON**

Run:
```bash
set MC2_ASSIMP_TRACE=1
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs
```
Expected: in `tests/smoke/artifacts/<timestamp>/` logs, `[ASSIMP` trace lines showing `LoadFromFile` probed the `.glb` and `ImportGeometryFromFile` populated a `TG_TypeMultiShape` (non-zero vertices). PASS criterion: the import path executed and produced geometry without GL errors / crash.

- [ ] **Step 3: Tag Phase 0 complete**

Run:
```bash
git tag -a assimp-phase0-complete -m "Phase 0: assimp import cherry-picked (8 track-d commits); stock-identity gate GREEN (tier1 + user zoomed-out); glB import proven. Plan 2 (cluster-LOD arc) unblocked."
git log --oneline pre-assimp-phase0..assimp-phase0-complete
```
Expected: the 8 track-d commits listed; tag created.

- [ ] **Step 4: Commit**

The tag is the deliverable. Phase 0 is complete; Plan 2 may now be written against a concrete, importer-present nifty.

---

## Self-Review

**Spec coverage (vs spec "Phase 0 (PREREQUISITE)"):**
- "merge the assimp-testing import path" → Tasks 2-5 (cherry-pick, not branch-merge — safer, per the stale-branch finding).
- "blast-radius scope is a Phase-0 task" → Task 1 Step 4 (dry-run) + Task 4 (mech3d drift) + Task 7 Step 3 (falsification gate).
- "Assimp heavy dep, vendoring rides along" → Task 2 (vendored tree via `feeaeaa`); Task 6 (`ENABLE_ASSIMP_IMPORTER` build).
- "Phase-0 exit gate: tier1 zoomed-out green, stock load unchanged, BEFORE cooker work" → Task 7 (identity gate) + Task 8 (tag unblocks Plan 2).
- "stock ASE/TGL behaviorally unchanged (identity)" → Task 4 Step 4 + Task 7 Steps 1-2 (baseline comparison).

**Placeholder scan:** No TBD/TODO. Conflict resolutions are specified by intent + exact files + exact git commands. The one fixture not byte-specified (Task 8 Step 1 glB) is intentionally any-valid-glB — the proof is path execution, not a specific asset; criterion is concrete (trace + non-zero geometry).

**Type/command consistency:** Tag names (`pre-assimp-phase0`, `assimp-phase0-complete`) consistent across Tasks 1/4/7/8. Smoke command consistent with CLAUDE.md (`--tier tier1 --duration 30 --kill-existing`, no menu canary). Build command matches CLAUDE.md (RelWithDebInfo, full relink, per-file `cp -f` + `diff -q`). The 8 SHAs are consistent across the header table and Tasks 2-5.

**Out-of-scope confirmed excluded:** No cooker, `.cdag`, cluster compute, or ladder deletion here — those are Plan 2, correctly gated behind Task 8.
