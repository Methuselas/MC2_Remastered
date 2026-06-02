# MODEL-OVERRIDE-MVP-1 — Implementation Notes

Running validation/decision log. See plan: `docs/model-override-mvp-plan.md`.

---

## Slice 1 — `MODEL-OVERRIDE-REGISTRY-0` — SHIPPED

- Commits: `19acd6a1` (vendor nlohmann/json + isolation guard), `4d7c5285` (registry + tests + empty manifest), `8ebbeb6e` (review fixes).
- `mclib/model_override_registry.{h,cpp}` — engine-independent parse → `ModelOverrideRecord`, case-insensitive `resolve(class,name)`, lazy `std::call_once` singleton.
- Validation (logged + dropped, never crash): non-object entries, `type==model`, `renderOnly==true`, `fallback=="stock"`, `scale==1.0`, typed `<class>:<name>` split on first `:` + normalized (trim+lowercase), class ∈ {staticProp,tree}, class-field agreement, safe relative `.glb/.gltf` source (no absolute/drive/`..`), duplicate first-wins.
- Per-entry try/catch added (review C1): nlohmann `type_error` on mismatched field types is caught/dropped, not propagated.
- Tests: 10 blocks / 9 fixtures, MSVC + g++ C++14-clean, `ALL TESTS PASSED`. json isolation OK.
- Shipped default `data/model_overrides/models.json` = `{ "overrides": [] }` (no-mod identity).

---

## Task 0 — `MODEL-OVERRIDE-COLLISION-AUTHORITY-PROOF-0` — VERDICT: **CASE A**

**Collision/footprint is derived from the SAME `TG_TypeMultiShape` pointer the override would replace. A single-shape hook WOULD change gameplay. Render-only requires a DUAL-SHAPE architecture.**

### Evidence (read-only proof)
1. **Buildings:** `BldgAppearance::calcCellsCovered` reads `appearType->bldgShape[currentLOD]` (`bdactor.cpp:2779`, vertex loop `:2786`) — the **same array** the base loader fills (`bdactor.cpp:211,235` `bldgShape[i]->LoadTGMultiShapeFromASE`).
2. **Trees:** `TreeAppearance::markTerrain` reads `appearType->treeShape[currentLOD]` (`bdactor.cpp:4266`, vertex loop `:4271`) — the **same array** the tree base loader fills (`bdactor.cpp:3385,3408`).
3. **Timing/cache:** `calcCellsCovered` runs once at scenario load (`objmgr.cpp:1569` → `terrobj.cpp:1373`), result cached in `cellsCovered` (`terrobj.cpp:1442`), never re-derived at runtime.
4. **No separate authority:** `BldgAppearanceType` holds only `bldgShape[MAX_LODS]` + `bldgDmgShape` (visual damage); no `.FITS` footprint, no `collisionOffsetX/Y` consumer, no collision-only mesh. Trees identical. Passability is purely vertex-derived from the visual shape.

### Consequence — Slice 2 REDESIGNED to dual-shape (supersedes plan §Slice 2 single-shape redirect)
Do **NOT** redirect the base loader (`bdactor.cpp:217,235,3391,3414`) to `LoadFromFile`. That would overwrite the collision-authority shape.

Instead:
- **Keep** the existing stock load (`bldgShape[i]/treeShape[i] = LoadTGMultiShapeFromASE(stock)`) UNCHANGED → `calcCellsCovered`/`markTerrain` stay bound to stock geometry → collision/footprint identical to stock.
- **Add** a separate render-authority shape — e.g. `bldgRenderShape[MAX_LODS]` / `treeRenderShape[MAX_LODS]` (override-or-null), loaded via `LoadFromFile(aseFileName, "staticProp"/"tree")` AFTER the stock load, only when the registry resolves an override.
- **Route render/cull only** to the render shape when present, else stock: `render()`, `getRadius()`, `OBBRadius`/`highZ` recompute (`bdactor.cpp:693-744`), and static-prop registry submission read the render shape. `calcCellsCovered`/`markTerrain` MUST continue to read the **stock** `bldgShape/treeShape` — never the render shape.
- Bounds recompute then derives from the render (override) mesh → cull/HZB/shadow track the visible geometry; collision stays stock. This satisfies render-only.

### Open design points for Slice 2 planning (next session)
- Where to store the render shape + null-guard every render/cull consumer to fall back to stock (audit all `bldgShape`/`treeShape` readers; split render-vs-collision readers).
- `OBBRadius`/`highZ` currently computed from `bldgShape` in `init` — must compute from the render shape when an override exists.
- Damage states stay stock (out of MVP).
- Memory/lifetime of the extra shape (destroy alongside stock).

**Task 0 gate: CLEARED — CASE A, dual-shape adjustment recorded. No loader code touched.**

---

## Slice 2 — DUAL-SHAPE (APPROVED) — see `docs/model-override-dual-shape-plan.md`

Reader audit table written (gate cleared). Key refinement vs raw audit: two `bldgShape` members exist — type-level `BldgAppearanceType::bldgShape[]` (collision authority, what LoadFromFile fills) vs per-instance `BldgAppearance::bldgShape` (render instance via CreateFrom). Per-instance render readers inherit authority from their CreateFrom source, so the real edit set is ~6 route sites (CreateFrom source + type bounds) + a nullable type-level render shape + accessor. Collision/damage rows all "leave". Full table + Slice 2 Tasks A/B/C in the dual-shape plan.

## Slice 2 — DUAL-SHAPE — SHIPPED

- Commits: `7114fcca` (dual-shape impl), `d4941557` (review fixes).
- Type-level `bldgRenderShape`/`treeRenderShape` (nullable) on the *Type classes + `getBldgRenderShape`/`getTreeRenderShape` (render-or-stock) + `getBldg/TreeCollisionShape` (always stock). Freed null-safe in type `destroy()`.
- Override loaded (Option 1: direct `ImportGeometryFromFile(manifestDir + sourceRelPath, renderShape)` under `#ifdef ENABLE_ASSIMP_IMPORTER`) AFTER the UNCHANGED stock `LoadTGMultiShapeFromASE`. Base-name captured into `bldgBaseName`/`treeBaseName` before the damage block clobbers `aseFileName`. Failure/throw/zero-shapes → delete+NULL → stock fallback (try/catch around import).
- Routed ONLY the 4 per-instance CreateFrom sources (bldg init + setObjStatus NORMAL; tree init + setObjStatus NORMAL) to the render accessor. OBBRadius/highZ reads the per-instance shape → auto-inherits. ALL collision (calcCellsCovered/markMoveMap/markLOS/markTerrain/calcAdjCell) + damage readers UNCHANGED on stock type array (verified by diff: zero collision/damage tokens).
- `assimp_importer.cpp` AABB made vertex-tight AND mesh-local (matches zero-node-pivot render) so routed cull/HZB/shadow bounds are correct.
- Reviews: spec ✅ (render-only invariant held), code-quality ✅ after fixes (#1 mesh-local AABB, #2 registry `manifestDir()` getter replaces hardcoded literal, #3 snprintf, #4 assimp try/catch, #5 cross-ref comment).
- **Build: `mclib` compiles clean (exit 0, mclib.lib, 0 errors)** via VS2022 cmake; worktree provisioned by copying nifty's `3rdparty/cmake`+`lib` (SDL2) — gitignored build deps.
- No-mod identity: empty manifest → resolve NULL → render shapes NULL → accessors return stock → byte-identical.

### Open for Slice 3 (flagged by quality review)
- GPU static-prop batcher registers the stock TYPE shape at setObjStatus, not `bldgRenderShape`. The per-instance render shape (CreateFrom result) is what draws, so likely fine — but confirm the override actually renders in the Slice 3 applied-override smoke (render-completeness, not collision).

## Slice 3 — STATICPROP-MODEL-OVERRIDE-PROOF — PARTIAL (plumbing proven, RENDER INCOMPLETE)

**Verdict:** the override RESOLVE + IMPORT + dual-shape collision-safety path all work and the engine is stable (exit 0, 0 GL errors), BUT the override geometry is NOT yet rasterized — the GPU static-prop batcher draws the stock type shape (see Visual CORRECTION below). Remaining work: register the render shape into the batcher.

Runtime validated on the canonical runtime **`A:/Games/mc2-opengl/mc2-win64-v0.4`**, mission **`mc2_01`** (the validate-skill default `mis0101` does not exist in any deploy dir; the project smoke harness `scripts/run_smoke.py` uses `mc2_01` on v0.4 — that is the real target). Built full `mc2` (0 errors) and deployed exe+pdb+dlls+63 shaders + `data/model_overrides/`.

Asset: reused `tests/fixtures/assets/cube.gltf` (self-contained base64 GLB-equivalent) as `data/model_overrides/source/props/box.gltf` (unit cube).

Discovery: added env-gated `MC2_MODOVERRIDE_TRACE` log of every staticProp/tree appearance name at the resolve site (bdactor.cpp, matches `MC2_ASSIMP_TRACE` convention). One trace run on `mc2_01` listed 56 static props + 11 trees actually loaded.

Proof run (manifest overriding 14 real props → box, `MC2_MODOVERRIDE_TRACE` off):
- **All 14 `[MODOVERRIDE] staticProp 'X': render override applied`** (crateswithtarp, dumpster, geodesicdome, hangar, hqtent, junkpile, lookouttower, Quonset, quonset2, sandbagbunker, sandbagwall, tent, wirefence, woodencrates).
- `--validate --frames 30 -mission mc2_01` → **exit 0, gl_errors=[], shader_errors=[]**, avg 11.4ms. No destroys, no crash.
- **Visual — CORRECTION (initial conclusion was WRONG):** the hangar VANISHED in the override shot, and I first read that as "box rasterized." It is NOT. The GPU static-prop batcher (`g_useGpuObjects`/`static_prop_registry` ON) draws the **registered STOCK type shape** — `registerMultiShape(appearType->bldgShape[i])` at `bdactor.cpp:855` (trees: `treeShape[i]` at `:3781`). The dual-shape override fills `bldgRenderShape`/`treeRenderShape` + the per-instance shape, but the **batcher never registers the render shape**, so the override geometry is never rasterized. The hangar disappeared because the override shrank the per-instance render bounds → the actor was culled / drew degenerate — NOT because the box was drawn. **The Slice-2 quality-review render-completeness flag is CONFIRMED OPEN, not resolved.** Same root cause = override trees don't render. (The bare trees in the shots are unmodified `*dead` variants, which were never overridden.)
- **Fallback:** manifest pointing `hangar` at a missing `.glb` → `[MODOVERRIDE] staticProp 'hangar': import failed … using stock`, exit 0, gl_errors=[]. Stock renders, no crash.
- **No-mod identity:** discovery run with empty manifest = stock baseline (`registry loaded: 0 override(s)`, byte-equivalent to v0.4 stock validate `vcheck.json`).
- **Collision:** code-guaranteed (dual-shape; collision reads stock type array, diff-proven Slice 2) + stable run (+0 destroys). Visual gameplay-collision probe is out of `--validate` scope.

Deploy manifest restored to `{"overrides":[]}` (install not left modded). Trace code is permanent (env-gated, zero-cost when unset).

Limitation: unit-cube box is tiny at MC2 world scale, so the override reads as "prop disappeared" rather than "prop is a visible box". The render-replacement is nonetheless proven by the hangar's disappearance + the applied logs. A larger box asset would make it visually obvious (deferred; not needed for proof).

## Slice 4 — TREE-MODEL-OVERRIDE-PROOF — RENDERS ✅ (override trees draw in-game)

**RESOLVED 2026-06-02.** Override trees render in-game via the GPU static-prop batcher, collision stays stock (dual-shape), exit 0, 0 GL errors. Confirmed on v0.4 / mc2_01 with the real PBR `tree_small.glb` (42k-tri, 20×): `registry loaded: 6`, all 6 tree types `render override applied`, pools 78% of 16M (fits), trees visible across the map (`.claude/slice4_forest_final.png`).

### What made it work
1. **Register the override RENDER shape into the batcher BEFORE `finalizeGeometry`** — added `registerMultiShape(getBldg/TreeRenderShape(i))` at the TOP of `BldgAppearance::registerStatic` / `TreeAppearance::registerStatic` (`bdactor.cpp`). The mission-load pre-pass (`registerStaticPropsForMissionLoad`, default-on) runs before `StaticProp::finalizeGeometry` (mission.cpp:3198), so the override type-shapes enter the immutable VBO (`s_typeIndex`) and `buildRecipeFromShape` succeeds → GPU-instanced draw. (The 2 `[GPUPROPS] late registerType` lines are unrelated pre-existing stock types — verified: their pointers don't match any override leaf.)
2. **Pool caps sized for override forests** — `code/mission.cpp` pools 16M (vertex/color/shadow) / 8M (face/triangle), `mission2.cpp` startVertices 16M, `MC_MAXFACES` 8M. Per-instance lighting/transform buffers scale with override mesh complexity × instances; the detailed tree (~21k verts × ~147 instances of the 6 overridden types) peaks ~12.6M.
3. **gltfpack for mesh weight** — built the vendored `gltfpack` (`3rdparty/meshoptimizer/gltf/gltfpack.cpp`; standalone build in `.claude/gltfpack_build`). `gltfpack -si 0.1 -sa -kn` crushes leaf-card foliage (which Blender collapse-decimate can't) → `tree_light.glb` (~4.3k tris) for cases needing many instances. The detailed `tree_small.glb` (42k) also fits the 16M pools.

### CRITICAL debugging lesson (cost hours)
A hung `mc2.exe` (validate process that didn't exit) held a **lock on the deployed `mc2.exe`**, so `cp`/`Copy-Item` of new builds **silently failed** → runs used a STALE exe → intermittent "no override / no trees" that looked like code bugs. Fix: `taskkill /F /IM mc2.exe /T` before every deploy-copy, and verify the copy. Also: `strings` is broken in this env (0 output) and bash `grep` mis-reads the UTF-16 stderr logs — use PowerShell `Select-String` for engine logs.

### Remaining polish (not blockers)
- **Untextured** → trees render dark (geometry only; importer sets texture NAMES, MC_TextureManager resolves by name — the glTF's embedded leaf/bark textures aren't wired into MC2's texture set). Material/texture binding is a follow-on.
- **Leaf alpha**: tree leaf material is glTF BLEND (engine supports MASK/alpha-test only) — set CLIP in the Blender export; full leaf cutout needs the leaf texture's alpha bound.
- **Perf**: detailed tree × instances → avg ~66ms (heavy per-instance lighting). Lighter tree (gltfpack) or a GPU per-instance-lighting path (avoids per-vertex pool storage) is the perf follow-on.
- Add a **model-override** row to `docs/asset-pipeline.md` (§7 mandate) once finalized.

---

## (superseded) earlier RENDER-BLOCKED analysis — kept for history

Tree asset: real PBR tree `tree_small_02_1k.gltf` (user-supplied) → Blender headless decimate (`.claude/tree_export.py`) → `data/model_overrides/source/trees/tree_small.glb`. Decimate floored at ~42,885 tris (leaf cards are disconnected quads — collapse-decimate can't reduce; **meshopt_simplifySloppy / gltfpack would** — vendored at `3rdparty/meshoptimizer`, CLI `gltf/gltfpack.cpp`). Leaf material set CLIP/MASK.

Runtime (v0.4 / mc2_01): override resolves + imports + 6× `render override applied`, exit 0, 0 GL errors — **but the override geometry does NOT render** (confirmed visually, repeatedly).

### ROOT CAUSE — one-shot immutable GPU VBO
`GpuStaticPropBatcher::finalizeGeometry()` (`GameOS/gameos/gos_static_prop_batcher.cpp`): `if (s_geometryFinalized) return;` then `glBufferStorage(..., flags=0)` → a **single, fully-immutable** shared VBO/IBO built once. The batcher draws static props/trees from this VBO via per-type recipes (`s_typeIndex: unordered_map<const TG_TypeShape*,uint32_t>`). A type registered **after** finalize → `[GPUPROPS] late registerType … CPU-fallback for this type`. The override render-shape is new geometry; it is not in the immutable VBO, so it falls to the per-instance CPU path, which:
  (a) does not visibly rasterize under `g_useGpuObjects`, and
  (b) retains a per-instance `CreateFrom` copy → TGL pools saturate deterministically (4,939,908/5,000,000 = ~235 of ~1000 trees before overflow). The pools are now 10x'd (5M/2M) but the per-instance-retention model still can't hold ~1000 heavy instances — and the GPU path is the only viable one for that count (geometry stored once).

### What this means
The dual-shape design is correct (collision stays stock — proven), and resolve/import/registration are wired, but **a render-only model override of a GPU-batched static prop/tree requires its geometry to be in the immutable batcher VBO**, i.e. registered **before the first `finalizeGeometry`**. Fixes attempted (register render shape at `init` 855/3787 + at `registerStatic` top, before the pre-pass `finalizeGeometry` at mission.cpp:3198) did NOT land it — 2 `late registerType` persist, implying finalize is already done by the time the override type is first seen (likely an earlier menu/shell finalize, or staging-VBO sizing reserved pre-override). Needs deeper batcher work, NOT another blind patch.

### Candidate real fixes (for the expert/next session)
1. **Pre-register override geometry before the first `finalizeGeometry`** — resolve the mod manifest at mission/menu boot and register all override render-shapes into the staging VBO before it's frozen. (Cleanest; matches the immutable-VBO design.)
2. **Batcher VBO rebuild hook** — allow `finalizeGeometry` to re-stage when a mission with overrides loads (there's a `resetForRestore()` path in `GameAdapters/StaticPropRenderAdapter` / saveload.cpp:1628 worth reusing).
3. **meshopt runtime clamp** (task b) — clamp override meshes to a budget so even CPU-fallback is survivable; helps but does not fix the no-render.
Align any solution with `docs/asset-pipeline.md` (canonical; render-owner `GpuStaticPropBatcher`, gamedata-owner `BldgAppearance`/`TreeAppearance`) and add a **model-override** row there per its §7.

### Committed this session (correct + endorsed, even though render incomplete)
- Caps 10x: `code/mission.cpp:3303-3316` (5M/2M), `code/mission2.cpp:111`, `MC_MAXFACES` `mclib/txmmgr.h:49`. (User: caps were artificial CPU-era limits.)
- Batcher registers the RENDER shape: `bdactor.cpp` init sites (855/3787) + `registerStatic()` tops (both bldg + tree).
- Env-gated `MC2_MODOVERRIDE_TRACE` discovery log.
NOTE: the Slice-3 "static-prop render replaced" claim was a misread (bounds-cull, not rasterization) — same root cause. Static props have the same limitation; only the dual-shape collision-safety + resolve/import are proven.
