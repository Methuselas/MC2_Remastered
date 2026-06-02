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

## Slice 3 / 4 — PENDING — need a chosen stock prop/tree + author a test GLB + run the engine with the mod (runtime proof + smoke gates).
