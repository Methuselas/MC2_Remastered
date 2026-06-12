# Rescue Branch Triage: 2026-06-12

**Summary:** 10 branches rescued during worktree prune (2026-06-11). All cherry-picked from older branches as single WIP commits onto current HEAD. Nifty-mendeleev at commit 48bceeca (2026-06-12). 3 HIGH-value branches worth recon, 4 MEDIUM (partial/superseded), 3 LOW (experimental/dead-end).

## Triage Buckets

### DELETE-SAFE (3 branches)
Low uniqueness, experimental dead-ends, or heavily superseded by nifty work:
- `rescue/shore-reconciler-wip` — 101 files, mostly 3rdparty headers (GLEW, SDL2, zlib). Bulk external includes w/ 2 real files (gos_terrain_arm_logic.h). Nifty has moved past this; terrain ARM work integrated.
- `rescue/track-rv-VFX-wip` — 2 files, VFX baseline test data only. Snapshot artifact, no code. Already superseded by VFX oracle commits on nifty (e.g., acad80e7, e0d064e0).
- `rescue/colormap-modern-wip` — 1 file, 7 LOC terrtxm2.cpp. Tiny WIP, likely stale given terrain-8z maturity on nifty.

### KEEP-FOR-RECON (3 branches)
Unique unfinished work, high file counts, active development areas:
- `rescue/mc2-model-override-recon-wip` — **HIGH VALUE** — 159 files, 22.8 KB deltas. Foliage/impostor recon: cook scripts, tree-export tools, seam tests, GLB packing recon. 146 real files touched (.claude/ recon artifacts + scripts). Work on model-override, foliage impostors, GLB cook pipeline. Already some model-override fixes on nifty (f191354b, b2d7e6bc), but this branch contains the full foliage impostor recon artifact trail.
- `rescue/mech-skinning-import-wip` — **HIGH VALUE** — 10 files, 1 KB insertions. Mech animation skinning: new HANDOFF_ANIM_DEBUG.md, gltf joint/weight fixers, assimp importer expansion (+277 LOC), gos_mech_batcher tuning. Track D animation import phase. Related commits on nifty exist (ff4ff392 full GLB/FBX pipeline), but this may capture intermediate recon or debug artifacts worth preserving.
- `rescue/engine-standalone-wip` — **MEDIUM-HIGH VALUE** — 8 files, 591 insertions. Editor bringup: run-editor.bat, symbolize_editor_dump.py, editor-bringup-handoff.md (144 LOC), BdActor/Mech3D object init fixes. Related merged work on nifty (6470e53f recon doc, 3a56b9ba import Mission Editor, ec3b3de3 RenderCore init spike). This may duplicate or complement those; worth checking.

### NEEDS-OWNER-EYES (4 branches)
Partial/unclear value, possible overlap with nifty work, needs author context:
- `rescue/editor-robustness-pass-wip` — 3 files, Eraser.h + Objective/eraser.cpp robustness (12 ins + 18 ins + 9 ins). Aim: QoL + graceful missing-asset. Likely superseded by dad3093c on nifty (feat: robustness + QoL — graceful missing-asset, MRU, status HUD). Candidate for deletion if nifty version is comprehensive.
- `rescue/parallel-amdahl-wip` — 5 files, 264 insertions. Terrain indirect + mission/objmgr parallelism: gos_terrain_indirect, code/mission.cpp, objmgr.cpp. Early-stage perf tuning. No matching commits on nifty yet, but unclear if work is stale or WIP mid-refactor. Needs owner context.
- `rescue/terrain-normal-array-wip` — 1 file, 48 LOC in gos_terrain_arm_logic.h. Shader ARM normal array. Tiny, unclear scope. Likely experimental; check if superseded by terrain-8z finalization commits on nifty.

---

## Per-Branch Triage Table

| Branch | Tip Date | Files | Areas | Commits Ahead | Superseded? | Value | Action |
|--------|----------|-------|-------|---|---|---|---|
| **colormap-modern-wip** | 2026-06-11 | 1 | terrain/shader | 1 | Likely | LOW | **DELETE** — stale WIP, terrain-8z mature on nifty |
| **editor-robustness-pass-wip** | 2026-06-11 | 3 | editor/UI | 1 | Likely (dad3093c) | MED | **CONDITIONAL DELETE** — check if nifty dad3093c covers this |
| **engine-standalone-wip** | 2026-06-11 | 8 | editor/engine/docs | 10 | Partial overlap | MEDIUM-HIGH | **TRIAGE-RECON** — cross-check against 6470e53f, 3a56b9ba, ec3b3de3 |
| **mc2-model-override-recon-wip** | 2026-06-11 | 159 | foliage/impostor/cook | 1 | Partial (f191354b) | **HIGH** | **CHERRY-PICK CANDIDATE** — unique foliage impostor recon + cook scripts |
| **mc2-staticprop-material-orm-normal-recon-wip** | 2026-06-11 | 2 | staticprop/baseline | 1 | Unknown | LOW-MED | **TRIAGE-RECON** — minimal content, baseline artifact only |
| **mech-skinning-import-wip** | 2026-06-11 | 10 | mech/animation/assimp | 27 | Partial (ff4ff392) | **HIGH** | **CHERRY-PICK CANDIDATE** — Track D anim debug + gltf fixers |
| **parallel-amdahl-wip** | 2026-06-11 | 5 | perf/terrain/objmgr | 1 | Unknown | MED | **NEEDS-OWNER-EYES** — unclear if WIP mid-refactor or stale |
| **shore-reconciler-wip** | 2026-06-11 | 101 | 3rdparty/terrain | 2 | Yes (headers bulk) | LOW | **DELETE** — mostly 3rdparty includes, terrain work moved on |
| **terrain-normal-array-wip** | 2026-06-11 | 1 | shader/terrain | 1 | Likely | LOW | **DELETE** — tiny ARM shader WIP, unclear scope |
| **track-rv-VFX-wip** | 2026-06-11 | 2 | vfx/test | 7 | Yes (acad80e7, e0d064e0) | LOW | **DELETE** — baseline artifact only, VFX oracle superseded |

---

## Per-Branch Notes

### High-Value Branches (Worth Preserving)

**rescue/mc2-model-override-recon-wip**
- 159 files captured in .claude/ (recon artifacts, screenshots, test outputs).
- Foliage impostor cook pipeline: cook_stock_foliage.py, cook_stock_impostor.py, foliage_crop.py, tree-export tools, seam fix scripts.
- GLB packing recon: gltfpack build artefacts, meshoptimizer proof-of-concept.
- Unique work: foliage seam tests, impostor texture views, LOD baseline comparisons, cook metrics.
- Nifty has model-override fixes (f191354b, b2d7e6bc) but this branch's full impostor recon + cook scripts are not yet on main.
- **Verdict:** CHERRY-PICK CANDIDATE — extract cook scripts + seam recon docs, discard .png artifacts during cherry-pick.

**rescue/mech-skinning-import-wip**
- 10 files, 27 commits ahead of base (earliest work date likely 2026-05 or earlier).
- Animation debug: HANDOFF_ANIM_DEBUG.md (199 LOC), gltf_joints.py, gltf_weights.py fixers, mc2-deploy.md skill update.
- Core work: assimp_importer.cpp (+277 LOC for joint/weight handling), gos_mech_batcher tuning, mech3d.cpp (+76 LOC anim helpers).
- Nifty has ff4ff392 (full GLB/FBX mech body + anim import pipeline), but this branch may contain intermediate recon or debug hooks not yet merged.
- **Verdict:** CHERRY-PICK CANDIDATE — anim debug handoff + gltf fixers have unique value for ongoing Track D work.

**rescue/engine-standalone-wip** 
- 8 files, 591 insertions, ~10 commits ahead.
- Editor bringup: run-editor.bat launch script, symbolize_editor_dump.py (212 LOC), editor-bringup-handoff.md (144 LOC), Editor/EditorGameOS/EditorObjectMgr fixes.
- Nifty has related: 6470e53f (engine-standalone-seams recon doc Slice 1+2), 3a56b9ba (RenderCore init spike), 3a56b9ba (import MC2 Mission Editor from engine-standalone).
- Likely partial overlap; handoff doc + symbolize script may still be valuable.
- **Verdict:** TRIAGE-RECON — requires detailed diff against nifty's 6470e53f/3a56b9ba to assess uniqueness.

---

## Deletion Safety Summary

| Bucket | Count | Branches | Safe to Delete? |
|--------|-------|----------|---|
| **DELETE-SAFE** | 3 | shore-reconciler-wip, track-rv-VFX-wip, colormap-modern-wip | YES — low uniqueness, superseded or artifact-only |
| **CONDITIONAL** | 1 | editor-robustness-pass-wip | YES if dad3093c on nifty is comprehensive |
| **EXPERIMENTAL** | 3 | parallel-amdahl-wip, terrain-normal-array-wip, mc2-staticprop-material-orm-normal-recon-wip | MAYBE — needs owner context; low risk if deleted |
| **KEEP** | 3 | mc2-model-override-recon-wip, mech-skinning-import-wip, engine-standalone-wip | NO — HIGH recon value, cherry-pick candidates |

---

## Next Steps

1. **Immediate Delete (safe):** shore-reconciler-wip, track-rv-VFX-wip, colormap-modern-wip, terrain-normal-array-wip.
2. **Conditional Delete:** editor-robustness-pass-wip (if nifty dad3093c verified as superset).
3. **Detailed Recon:**
   - engine-standalone-wip: diff .claude/handoffs/editor-bringup vs nifty 6470e53f, validate symbolize script uniqueness.
   - parallel-amdahl-wip: author interview — is this mid-refactor WIP or stale experiment?
   - mc2-staticprop-material-orm-normal-recon-wip: minimal content, low priority, likely discard.
4. **Cherry-Pick Workflow:**
   - mc2-model-override-recon-wip: extract cook_stock_foliage.py, cook_stock_impostor.py, tree_export_impostor.py, seam recon snapshots (as docs, not binaries).
   - mech-skinning-import-wip: cherry-pick HANDOFF_ANIM_DEBUG.md, gltf fixers, assimp recon, skip smoke test tuning if nifty equivalent exists.

---

**Report Generated:** 2026-06-12  
**Base Branch:** claude/nifty-mendeleev @ 48bceeca  
**Rescue Branches:** All from 2026-06-11 worktree prune  
**Caveman Mode:** HIGH=2 (model-override-recon, mech-skinning), MEDIUM-HIGH=1 (engine-standalone), DELETE-SAFE=4, CONDITIONAL=1, EXPERIMENTAL=3.
