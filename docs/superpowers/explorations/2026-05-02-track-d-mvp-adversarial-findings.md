# Track D — MVP Adversarial Review Findings + Execution Plan

**Date:** 2026-05-02
**Reviewer:** Track D MVP execution session
**Plan reviewed:** `docs/superpowers/plans/2026-04-27-assimp-mech-importer.md` (1547 lines, 12 tasks)
**Spec reviewed:** `docs/superpowers/specs/2026-04-27-assimp-mech-importer-design.md`
**Trigger:** New infrastructure + lifecycle/cull-adjacent risks (TGL pool, texture handles). Lightweight code-grounding pass per `.claude/skills/adversarial-plan-review.md` — every cited symbol grep'd against source.

---

## Findings

### CRITICAL (will not compile / will run wrong)

1. **Plan Task 8 invents `_TG_Animation::QuatType`** — fictional typedef.
   - Plan line 1021: `out->quat = (_TG_Animation::QuatType*)TG_Shape::tglHeap->Malloc(...)`
   - Reality (`mclib/tgl.h:344`): `Stuff::UnitQuaternion *quat;` — no inner typedef.
   - **Fix:** cast to `(Stuff::UnitQuaternion*)`. Out of MVP scope (anim is M2) but worth flagging now.

2. **Plan treats `TG_AnimateShape::listOfAnimation` as array of pointers (`[i]->`); reality is array of values (`[i].`).**
   - Plan Task 4 line 422: `if (listOfAnimation[i]) listOfAnimation[i]->SaveBinaryCopy(&binFile);`
   - Reality (`mclib/msl.h:591`): `TG_AnimationPtr listOfAnimation;` where `TG_AnimationPtr = TG_Animation*` (a pointer to a contiguous **array of values**, indexed by `[i].field`).
   - Confirmed at `mclib/msl.cpp:2092`: `listOfAnimation[i].LoadBinaryCopy(&binFile);` (dot, not arrow).
   - **Cascading effect:** Plan Task 8 line 1112 allocates `sizeof(_TG_Animation*) * out->count` — wrong size; correct is `sizeof(_TG_Animation) * out->count`.
   - Out of MVP scope (anim is M2) but the cache write/read must match if/when M2 lands.

3. **Plan's per-shape `TG_TypeShape::listOfTextures` has no `textureName` field.**
   - Plan spec §6 implies `listOfTextures[n].textureName` exists per-shape. Reality (`mclib/tgl.h:558`): per-shape uses `TG_TinyTexturePtr`, fields `{mcTextureNodeIndex, gosTextureHandle, textureAlpha}` — no name.
   - The texture **name** lives on the multi-shape's `TG_TexturePtr listOfTextures` (`mclib/msl.h:77`), populated via `CreateListOfTextures(TG_TexturePtr list, DWORD numTxms)` (`mclib/tgl.h:653`).
   - **Implication for `BuildTextureList` gap fill:** must populate the multi-shape's `TG_Texture[]` (with names + alpha) AND link per-shape `TG_TinyTexture` entries via `mcTextureNodeIndex` back-pointers.

4. **TG_TypeShape data members are `protected`**, not friended to a hypothetical `assimp_importer` translation unit.
   - `mclib/tgl.h:551-565`: `numTypeVertices, listOfTypeVertices, listOfTypeTriangles, listOfTextures, hotPinkRGB, alphaTestOn, filterOn` — all protected.
   - Friend list (`mclib/tgl.h:544-547`): `TG_TypeMultiShape, TG_MultiShape, TG_Shape, GpuStaticPropBatcher`. Not `assimp_importer`.
   - **Fix options (in order of cheapness):** (a) write the importer as a member function of `TG_TypeMultiShape` (it already accesses these via `listOfTypeShapes[i]->...`); (b) friend `assimp_importer` from both `TG_TypeShape` and `TG_TypeMultiShape`; (c) add public mutator methods. The plan implicitly assumes (b) by writing free functions that touch `ts->numTypeVertices = ...` directly. **Decision: friend declaration in tgl.h + msl.h.** Lowest churn.

### MAJOR (executor will hit walls)

5. **Line numbers throughout the plan are stale.**
   | Plan claim | Actual |
   |---|---|
   | `mclib/tgl.h:528` (TG_TypeShape) | `tgl.h:536` |
   | `mclib/tgl.h:614, 631` (ParseASEFile, LoadTGShapeFromASE) | `:623, :640` |
   | `mclib/msl.h:570` (TG_AnimateShape) | `:580` |
   | `mclib/msl.cpp:182-230` (LoadBinaryCopy) | `:184-238` |
   | `mclib/msl.cpp:291-320` (SaveBinaryCopy) | `:335-?` |
   | `mclib/mech3d.cpp:277` (mech LOD0 load) | `:286` |
   | `mclib/mech3d.cpp:393` (anim loop) | `:419` (call), `:403` (name format) |
   | `bdactor.cpp:179,197,211,227,247,3085,3108,3127,3147,3167` | `:187,205,219,235,255,3186,3209,3228,3248,3268` |
   | `gvactor.cpp:147,165,179,194,346,357` | `:153,171,185,200,352,363` |
   | `genactor.cpp:105,127` | `:110,132` |
   - **Implication:** executor must grep at task-time, not trust the plan. (Worktree CLAUDE.md "Documentation Discipline" mandates this anyway.)

6. **`_TG_Animation` lives in `tgl.h:337`, not `msl.h`.**
   - Status doc line 45 says "TG_AnimateShape (mclib/msl.h:570) — animation: per-node `quat[]` and `pos[]` dense arrays at a fixed frameRate" which conflates the per-node `_TG_Animation` struct (in `tgl.h`) with the per-mech `TG_AnimateShape` collection (in `msl.h`). Both exist; the per-node one is what gets baked.
   - Out of MVP scope (anim is M2).

7. **Plan Task 6 writes `tri.localTextureHandle = mesh->mMaterialIndex;` — but `localTextureHandle` is the per-shape index into `listOfTextures` (TG_TinyTexture array), not the scene-wide material index.**
   - These coincide IFF every material maps to exactly one texture and no shape-level deduplication happens. For MadCat (single material per mesh), this works. For mechs with shared atlases, may produce wrong texture binding.
   - **MVP scope:** acceptable for MadCat; document as a follow-up.

8. **Plan Task 5 `LoadFromFile` builds full paths via `tglPath` and probes — but the call site at `mech3d.cpp:286` already passes a constructed `FullPathFileName` (with `.ase` baked in)**, not a base name.
   - Reality (`mech3d.cpp:283-286`):
     ```cpp
     FullPathFileName mechName;
     mechName.init(tglPath, aseFileName, ".ase");
     mechShape[i]->LoadTGMultiShapeFromASE(mechName);
     ```
     `aseFileName` is the base name (e.g. `"madcat"`). The `.ase` path is constructed locally then passed.
   - **Wiring change for MVP Task 10:** must pass `aseFileName` (base name) to `LoadFromFile`, not `mechName`. `LoadFromFile` constructs the .glb candidate path internally, falls through to `LoadTGMultiShapeFromASE(asePath)` if no .glb exists.

### MINOR (will work, but worth flagging)

9. **Task 6 `ts->alphaTestOn = false; ts->filterOn = true;` is redundant** with `TG_TypeShape::init()` defaults (`tgl.h:584-586`) which set the same values when the constructor runs. Removable.

10. **Plan Task 9's `[Import]` parsing reads **inside the same FitIniFile session** — fine, but plan doesn't show a `seekBlock("Import")` first.** Without that, `readIdString("Source", ...)` reads from whatever the last `seekBlock` set (likely `[TGLData]` or `[Gestures24]` per the loop at `mech3d.cpp:209-249`). Need explicit `seekBlock("Import")` before the reads.

11. **Plan does not call out that `mclib` is a parent-scope `set(SOURCES ${SOURCES} ...)` extension** (`mclib/CMakeLists.txt:4` then `add_library(mclib ${SOURCES})` at `:103`). The right insertion is to append `tgl_cook.cpp` (always) and `assimp_importer.cpp` (conditional) to the SOURCES list before the `add_library` call, not after.

---

## Constraints checklist (load-bearing memory)

| Memory | Applies? | Addressed by plan? |
|---|---|---|
| `stock_install_must_remain_playable.md` | YES — Track D adds opt-in import path | YES — `.glb` is opt-in, ASE path unchanged when `.glb` absent |
| `cull_gates_are_load_bearing.md` | INDIRECT — new mechs flow through normal cull | NO new bypass; legacy mechs continue. **Risk: if .glb mech has higher poly count, TGL pool exhaustion** — see next |
| `tgl_pool_exhaustion_is_silent.md` | YES — vertex pool capped at 500K | NOT addressed in plan; flag for MVP integration test |
| `mc2_texture_handle_is_live.md` | YES — importer must not cache mc2TextureNodeIndex value | Plan stores `mcTextureNodeIndex` per shape — that IS the slot index, not the live handle. Correct per memory. |
| `feedback_offload_scope_stock_only.md` | INDIRECT — MVP gate is stock vs new-format equivalence | YES — comparison is against stock MadCat ASE |
| `feedback_deploy_path.md` | YES — deploy is `mc2-win64-v0.3` (user confirmed, supersedes user task's "v0.2" reference) | n/a (deploy concern) |

**No load-bearing memory contradicts the MVP scope.** Pool exhaustion risk is the one to monitor at integration time.

---

## Adjusted MVP execution plan

User-stated MVP: Task 1 + geometry-only Task 6 + minimal Task 9/10. Defer Task 2-5 (cache), Task 7-8 (anim), Task 11-12 (cook tool). Frozen stand pose, single LOD (LOD0), no shadow/arm/damage shapes for the .glb path.

### Implementation sequence

| Step | Commit | Files | Purpose |
|---|---|---|---|
| 1 | build: add ENABLE_ASSIMP_IMPORTER opt + Assimp 5.3.1 stubs | `CMakeLists.txt`, `mclib/CMakeLists.txt`, `mclib/assimp_importer.{h,cpp}` (stubs) | Wire dependency, prove FetchContent compiles |
| 2 | feat: friend assimp_importer in TG_TypeShape / TG_TypeMultiShape | `mclib/tgl.h`, `mclib/msl.h` | Unblock data-member writes from importer translation unit |
| 3 | feat: BuildTextureList — multi-shape `listOfTextures` + per-shape `TG_TinyTexture` index linkage | `mclib/assimp_importer.cpp` | Plan-side gap from §Self-review |
| 4 | feat: ImportGeometryFromFile (geometry only, MadCat-shaped) | `mclib/assimp_importer.cpp` | Task 6 MVP slice — verts/tris/UVs/coord transform/V-flip/validator |
| 5 | feat: TG_TypeMultiShape::LoadFromFile (probe-only, no cache) | `mclib/msl.h`, `mclib/msl.cpp` | Stripped Task 5: probe `.glb` → `ImportGeometryFromFile`; else legacy `.ase` |
| 6 | feat: parse [Import] Source= INI override | `mclib/mech3d.cpp` | Task 9 partial |
| 7 | feat: wire mech3d.cpp:286 LOD0 to LoadFromFile | `mclib/mech3d.cpp` | Task 10 partial — single LOD0 site only |
| 8 | docs: track_d_assimp_mvp_done.md memory + index update | `~/.claude/projects/.../memory/` | Per user task deliverable |

### Skipped from full plan (deferred to M2)

- **TG_AnimateShape::Clone()** gap (plan §Self-review). User task asked for this in commit 2 — but Clone is only needed for shared-gesture copy in animation loading, which is M2. **Defer.** Note this back to the user.
- All animation work (Tasks 7, 8): MVP renders frozen stand pose; `mechAnim[*]` left untouched (legacy ASE-loaded if `.ase` siblings exist, else null — engine guards on null).
- `.tglc`/`.aglc` cache (Tasks 2-4): re-cook every run for MVP. Acceptable at MadCat geometry size; cache is a perf opt, not correctness.
- LOD1/LOD2, shadow shape, arms, damage shapes for .glb: legacy ASE path remains for those at MVP — `LoadFromFile` falls through to `LoadTGMultiShapeFromASE` when no `.glb` exists. This means a partial deploy (only `madcat.glb` + stock `madcat.ase`) loads LOD0 from glb, others from ase. Acceptable for proving the seam works.
- `mc2_assetcook` offline tool (Task 11): cook-at-startup is sufficient for MVP.

### Verification gate

1. **Pre-baseline tier1 smoke (no canary)**: capture FPS per mission. (In progress.)
2. Build with `ENABLE_ASSIMP_IMPORTER=ON` (default).
3. Smoke without any `.glb` present: tier1 must match pre-baseline ±1 FPS — proves opt-in path doesn't regress legacy.
4. Smoke with `madcat.glb` deployed: load `mc2_01` and observe MadCat in stand pose with correct hardpoint geometry and textures.
5. Visual diff: side-by-side same camera angle, stock ASE MadCat vs .glb MadCat — indistinguishable at frozen-pose.

### Test asset acquisition

The user task says the MadCat .glb is "hand-authored / dropped next to madcat.ini" but does **not** specify a known-good source path. **This is the one open dependency.** Options:
- (a) Convert from existing community FBX (e.g. `A:/Games/mc2-opengl/MC2 Conversions/MadCat/MadCat.FBX` per plan Task 12 line 1442). Fastest.
- (b) Hand-author in Blender per spec §5 conventions. Slowest but cleanest.
- (c) Use a stock ASE → glb conversion via an external tool. Couples MVP to converter quality.

**Decision deferred to user.** I'll prioritize getting the import path working end-to-end with a minimal hand-fabricated test before touching the real MadCat. If the user confirms an FBX source, even simpler — Assimp imports both, no separate test asset needed.

---

## Architectural decisions that need user/advisor sign-off before any revision pass

1. **Confirm: defer `TG_AnimateShape::Clone()` until M2.** User task asked for it in MVP "commit 2"; my read is it's anim-coupled and unnecessary for frozen-pose MVP. If user disagrees, I'll land a stub-with-deep-copy now.
2. **Confirm: friend declaration is the right access mechanism for `assimp_importer`.** Alternative is making the importer a TG_TypeMultiShape member function. Friend keeps the importer code cleanly isolated; member is more conventional. Friend is my default.
3. **Confirm: deploy target is `mc2-win64-v0.3`** (user confirmed). User task wrote `v0.2` in several places — defer to user message.
4. **Confirm: test-asset sourcing path** (Options (a)/(b)/(c) above). My default is (a) — convert from existing FBX, since Assimp handles both equivalently and the seam-proof is what matters.
