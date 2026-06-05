# Model Override — Dual-Shape Plan (`MODEL-OVERRIDE-DUAL-SHAPE-PLAN-1`)

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Steps use `- [ ]` tracking. Supersedes the single-shape Slice 2 in `docs/model-override-mvp-plan.md` (rejected by Task 0 = CASE A).

**Goal:** Render-only model override for static props + trees: stock `bldgShape[]`/`treeShape[]` stay the collision/gameplay authority; a nullable type-level render shape carries the override; render/cull/bounds use render-or-stock via accessors; collision always uses stock.

**Why dual-shape (Task 0 = CASE A):** `calcCellsCovered`/`markTerrain`/`markMoveMap`/`markLOS` derive passability from the same `appearType->bldgShape[lod]`/`treeShape[lod]` the override would overwrite. Overwriting it changes gameplay. Proof: `docs/model-override-mvp-notes.md` Task 0; `bdactor.cpp:2779/2786` (bldg), `:4266/4271` (tree).

---

## Key architectural distinction (drives the small edit set)

Two members share the name `bldgShape` — do not conflate:

| Member | Class | Type | Role |
|---|---|---|---|
| `bldgShape[MAX_LODS]` | `BldgAppearanceType` (`bdactor.h:52`) | `TG_TypeMultiShapePtr` | **Type geometry.** What `LoadTGMultiShapeFromASE`/`LoadFromFile` fills. Collision reads this. |
| `bldgShape` | `BldgAppearance` (`bdactor.h:188`) | `TG_MultiShapePtr` | **Per-instance render shape**, built via `appearType->bldgShape[lod]->CreateFrom()` (`bdactor.cpp:599`). Every per-instance render/node/texture/transform use reads this. |

Same for `treeShape[MAX_LODS]` (`bdactor.h:455`) vs `treeShape` (`bdactor.h:505`).

**Consequence:** the per-instance render shape inherits whatever type shape its `CreateFrom` source points at. So we do **not** edit the ~86 per-instance read sites the audit found. We only:
1. add a nullable **type-level** render shape + accessor,
2. point the per-instance `CreateFrom` source and the per-instance bounds (`OBBRadius`/`highZ`) computation at the render accessor,
3. leave every collision/damage reader on the stock type array.

---

## Reader audit table (gate requirement)

Classification of all `bldgShape`/`treeShape`/`*DmgShape` readers. `consumer ∈ {collision/gameplay, render/draw, cull/bounds/HZB, damage, lifecycle}`, `authority ∈ {stock only, render-or-stock, stock}`, `action ∈ {leave, route via accessor, route CreateFrom source}`. Per-instance render/draw rows are marked **(inherits)** — they need NO direct edit because they read the per-instance `BldgAppearance::bldgShape`, which becomes render-authoritative once its `CreateFrom` source (the few **route** rows) is switched.

### `bldgShape` (buildings / static props)
| consumer | file:line | authority | action |
|---|---|---|---|
| lifecycle (type load) | bdactor.cpp:211-217, 229-235 | stock | leave (Task A loads render shape alongside) |
| **render instance build** | bdactor.cpp:599 `bldgShape = appearType->bldgShape[0]->CreateFrom()` | render-or-stock | **route CreateFrom source** |
| **type bounds OBBRadius/highZ** | bdactor.cpp:684-740 (GetRootNodeCenter/GetMinBox/GetMaxBox) | render-or-stock | **route via accessor** |
| **LOD reselect instance build** | bdactor.cpp:1312-1339, 1385-1391 (`appearType->bldgShape[currentLOD]->CreateFrom()`) | render-or-stock | **route CreateFrom source** |
| render/draw — textures | bdactor.cpp:603-634, 1343-1374, 1395-1426 | render-or-stock | leave (inherits) |
| render/draw — Render/Transform/Lights/nodes | bdactor.cpp:478-500, 939-965, 1536-1542, 1630-1672, 2104-2105, 2164-2350 | render-or-stock | leave (inherits) |
| cull — instance GetExtentRadius | bdactor.cpp:1104, getRadius bdactor.h:370-374 | render-or-stock | leave (inherits) |
| shadow | bdactor.cpp:1863-1866 (`bldgShadowShape` else `bldgShape`) | render-or-stock | leave (inherits) |
| damage | bdactor.cpp:229-232,257,406-409,765-774,2613-2718 (`bldgDmgShape`) | stock | leave |
| **collision — calcCellsCovered** | bdactor.cpp:2483-2562 (`appearType->bldgShape[lod]`, vertex loop) | stock only | leave |
| **collision — 2nd passability pass** | bdactor.cpp:2725-2733 | stock only | leave |
| **collision — markMoveMap** | bdactor.cpp:2799-2913 | stock only | leave |
| **collision — markLOS** | bdactor.cpp:2929-2950 | stock only | leave |
| **collision — calcAdjCell** | bdactor.cpp:3003-3020 | stock only | leave |

### `treeShape` (trees)
| consumer | file:line | authority | action |
|---|---|---|---|
| lifecycle (type load) | bdactor.cpp:3079-3090, 3102-3113 | stock | leave (Task A loads render shape alongside) |
| **render instance build** | bdactor.cpp:3254-3259 (`treeShape = appearType->treeShape[0]->CreateFrom()`) | render-or-stock | **route CreateFrom source** |
| **type bounds OBBRadius/highZ** | bdactor.cpp:3344-3398 | render-or-stock | **route via accessor** |
| **LOD reselect instance build** | bdactor.cpp:3780-3789 (+ associated CreateFrom) | render-or-stock | **route CreateFrom source** |
| render/draw — textures | bdactor.cpp:3263-3294, 3471-3544 | render-or-stock | leave (inherits) |
| render/draw — Render/shadow | bdactor.cpp:3920, 4062-4065 | render-or-stock | leave (inherits) |
| cull — instance GetExtentRadius | bdactor.cpp:3625, getRadius bdactor.h:601-604 | render-or-stock | leave (inherits) |
| damage | bdactor.cpp:3145-3152,3198-3201,3411-3450 (`treeDmgShape`) | stock | leave |
| **collision — markTerrain** | bdactor.cpp:4189-4239 (`appearType->treeShape[lod]`, vertex loop) | stock only | leave |
| **collision — markLOS** | bdactor.cpp:4244-4304 | stock only | leave |

### Audit conclusions
- **No `unknown` rows.** All readers classified.
- **No external readers.** Repo-wide search found no `bldgShape`/`treeShape` reads outside `bdactor.{h,cpp}` (GameAdapters/GameOS/RenderCore reference the per-instance `TG_MultiShape*` handed to the registry/draw path, not these members). The static-prop registry receives the instance render shape → inherits automatically.
- **Minimal route set:** building — `bdactor.cpp:599`, `684-740`, `1312-1339`, `1385-1391`; tree — `3254-3259`, `3344-3398`, `3780-3789` (+ its CreateFrom). Everything else is leave (collision/damage) or inherits (per-instance render).
- **LOD-under-override policy (MVP):** the override has no LODs. The render accessor returns the single override type shape for **every** LOD when an override is loaded (LOD collapse), so LOD reselect never pops between override and stock. Documented; revisit if modders supply LOD'd overrides.

---

## Accessor design (centralize null fallback)

On `BldgAppearanceType` / `TreeAppearanceType`, add the nullable render shape + accessors. Collision keeps reading `bldgShape[lod]` directly (no accessor) so it can never accidentally pick up the override.

```cpp
// BldgAppearanceType (bdactor.h, near bldgShape[MAX_LODS]):
TG_TypeMultiShapePtr bldgShape[MAX_LODS];        // stock — collision authority (unchanged)
TG_TypeMultiShapePtr bldgRenderShape;            // override render shape, NULL if none (MODEL-OVERRIDE-MVP)

// Render authority: override if present, else stock LOD. LOD-collapses under override (see policy).
TG_TypeMultiShape* getBldgRenderShape(long lod) {
    return bldgRenderShape ? (TG_TypeMultiShape*)bldgRenderShape
                           : (TG_TypeMultiShape*)bldgShape[lod];
}
// Collision authority: ALWAYS stock. (Call sites already use bldgShape[lod] directly;
//  this accessor exists for clarity / future-proofing — collision must never call the render one.)
TG_TypeMultiShape* getBldgCollisionShape(long lod) { return (TG_TypeMultiShape*)bldgShape[lod]; }
```
Tree mirror: `treeRenderShape`, `getTreeRenderShape(lod)`, `getTreeCollisionShape(lod)`.

---

## Slice 2 tasks — `MODEL-OVERRIDE-DUAL-SHAPE`

### Task A: type-level render shape member + accessors + lifecycle
**Files:** `mclib/bdactor.h`, `mclib/bdactor.cpp`
- [ ] Add `bldgRenderShape` / `treeRenderShape` (`TG_TypeMultiShapePtr`, init `NULL` in the type ctors at `bdactor.h:84-88` / `:470-477`).
- [ ] Add `getBldgRenderShape(lod)`/`getBldgCollisionShape(lod)` and tree mirrors (inline, as above).
- [ ] Destroy: free `bldgRenderShape`/`treeRenderShape` wherever `bldgShape[]`/`treeShape[]` are deleted (`~BldgAppearanceType` ~`:393-409`, tree ~`:3191-3201`). Null-safe.
- [ ] Build mclib. Commit.

### Task B: load the override into the render shape (registry-keyed)
**Files:** `mclib/bdactor.cpp` (building type load `:211-236`; tree type load `:3079-3113`), include `model_override_registry.h`.
- [ ] After the stock base shape is loaded (`bldgShape[i]`/`treeShape[i]` filled via the UNCHANGED `LoadTGMultiShapeFromASE`), resolve `ModelOverrideRegistry::instance().resolve("staticProp"/"tree", aseFileName)`.
- [ ] On hit: `bldgRenderShape = new TG_TypeMultiShape;` then `bldgRenderShape->LoadFromFile(aseFileName, "staticProp")`. On failure/non-zero or zero shapes, `delete` and set `NULL` (→ stock render fallback). Log applied/fallback. (Tree: `"tree"`.)
- [ ] Stock load path is byte-identical when no override resolves (render shape stays NULL).
- [ ] Build. Commit.
> Note: `LoadFromFile` already probes override source? No — the registry resolve is done HERE and we call `LoadFromFile(aseFileName, class)` which (per `msl.cpp`) will itself resolve the same override and import the `.glb`. To avoid a double-resolve, prefer calling `ImportGeometryFromFile` directly on the resolved `source` path here, OR keep the `LoadFromFile` override block (Slice-2-original Task 6) but call it ONLY for the render shape. **Decide during implementation; the render shape is the only thing that ever receives override geometry.** Stock shapes never call the override-aware path.

### Task C: route the render-instance build + type bounds through the accessor
**Files:** `mclib/bdactor.cpp`
- [ ] `:599` and the LOD-reselect CreateFrom sites (`:1312-1339`, `:1385-1391`): change `appearType->bldgShape[lod]->CreateFrom()` → `appearType->getBldgRenderShape(lod)->CreateFrom()`. Tree: `:3254-3259`, `:3780-3789` → `getTreeRenderShape(lod)`.
- [ ] OBBRadius/highZ blocks (`:684-740` bldg, `:3344-3398` tree): compute box from `getBldgRenderShape(lod)`/`getTreeRenderShape(lod)` instead of the stock member, so cull/HZB/shadow bounds track the rendered (override) mesh.
- [ ] **Do NOT touch** any collision row (`calcCellsCovered`/`markMoveMap`/`markLOS`/`markTerrain`/`calcAdjCell`) or any `*DmgShape` row. They keep reading `bldgShape[lod]`/`treeShape[lod]` directly.
- [ ] Verify importer AABB is vertex-tight (recon flagged `ComputeBoundingBox` as node-center loose) so the routed bounds are correct; fix in `assimp_importer.cpp` if needed (carryover from original Task 8).
- [ ] Build engine. Commit.

---

## Slice 3 / 4 — validation (carried from `model-override-mvp-plan.md`, now dual-shape aware)

**Slice 3 — static-prop proof:** with proof manifest, replacement mesh visible; render bounds follow replacement (no pop); **collision/`cellsCovered` identical to stock** (unit routes the same cells — the load-bearing dual-shape check); fallback works when source missing; 0 GL errors; +0 destroys. No-mod path byte-identical. Smoke gates: no-mod identity, applied, missing-source fallback.

**Slice 4 — tree proof:** replacement tree visible; alpha-mask leaves (`alphaMode=MASK`, fixed 0.5); no culling pop; stock footprint preserved (markTerrain unchanged); double-sided documented; fallback works; 0 GL errors.

**Dual-shape-specific validation (both slices):** assert via logging or a debug probe that the collision path read the stock shape and the render path read the render shape — i.e. with an override whose footprint differs from stock, confirm passability matches STOCK (not the override) while the drawn mesh is the override.

---

## Risks
| Risk | Mitigation |
|---|---|
| A render reader missed → draws stock not override | Per-instance render inherits via CreateFrom source; only 6-ish route sites — audit table is the checklist |
| A collision reader accidentally routed → gameplay change | Collision rows explicitly "leave"; collision uses `bldgShape[lod]` directly, never the render accessor |
| LOD pop between override (LOD0) and stock (LOD1+) | Accessor LOD-collapses under override (returns override for all LODs) |
| Double override resolve (registry + LoadFromFile) | Task B note: resolve once; only the render shape ever imports override geometry |
| Importer AABB loose → wrong cull bounds | Task C: confirm/fix vertex-tight box |
| Shadow shape (`bldgShadowShape`) separate from render | Inherits if built from instance shape; verify in Task C, route if it CreateFroms the type shape |
