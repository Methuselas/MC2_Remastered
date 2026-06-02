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

## Slice 3 — STATICPROP-MODEL-OVERRIDE-PROOF — PROVEN (runtime)

Runtime validated on the canonical runtime **`A:/Games/mc2-opengl/mc2-win64-v0.4`**, mission **`mc2_01`** (the validate-skill default `mis0101` does not exist in any deploy dir; the project smoke harness `scripts/run_smoke.py` uses `mc2_01` on v0.4 — that is the real target). Built full `mc2` (0 errors) and deployed exe+pdb+dlls+63 shaders + `data/model_overrides/`.

Asset: reused `tests/fixtures/assets/cube.gltf` (self-contained base64 GLB-equivalent) as `data/model_overrides/source/props/box.gltf` (unit cube).

Discovery: added env-gated `MC2_MODOVERRIDE_TRACE` log of every staticProp/tree appearance name at the resolve site (bdactor.cpp, matches `MC2_ASSIMP_TRACE` convention). One trace run on `mc2_01` listed 56 static props + 11 trees actually loaded.

Proof run (manifest overriding 14 real props → box, `MC2_MODOVERRIDE_TRACE` off):
- **All 14 `[MODOVERRIDE] staticProp 'X': render override applied`** (crateswithtarp, dumpster, geodesicdome, hangar, hqtent, junkpile, lookouttower, Quonset, quonset2, sandbagbunker, sandbagwall, tent, wirefence, woodencrates).
- `--validate --frames 30 -mission mc2_01` → **exit 0, gl_errors=[], shader_errors=[]**, avg 11.4ms. No destroys, no crash.
- **Visual (render-completeness CONFIRMED):** stock screenshot `.claude/slice3_stock.png` shows the large hangar/quonset complex right of the runway; override screenshot `.claude/slice3_override.png` shows it GONE (replaced by the unit-cube box, invisible at world scale). Terrain, trees, runway, non-overridden center vehicle props identical. → **the per-instance override render shape DOES reach the GPU batcher** — resolves the Slice-2 quality-review render-completeness flag (batcher draws the per-instance shape, not the stock type shape).
- **Fallback:** manifest pointing `hangar` at a missing `.glb` → `[MODOVERRIDE] staticProp 'hangar': import failed … using stock`, exit 0, gl_errors=[]. Stock renders, no crash.
- **No-mod identity:** discovery run with empty manifest = stock baseline (`registry loaded: 0 override(s)`, byte-equivalent to v0.4 stock validate `vcheck.json`).
- **Collision:** code-guaranteed (dual-shape; collision reads stock type array, diff-proven Slice 2) + stable run (+0 destroys). Visual gameplay-collision probe is out of `--validate` scope.

Deploy manifest restored to `{"overrides":[]}` (install not left modded). Trace code is permanent (env-gated, zero-cost when unset).

Limitation: unit-cube box is tiny at MC2 world scale, so the override reads as "prop disappeared" rather than "prop is a visible box". The render-replacement is nonetheless proven by the hangar's disappearance + the applied logs. A larger box asset would make it visually obvious (deferred; not needed for proof).

## Slice 4 — TREE-MODEL-OVERRIDE-PROOF — PENDING
Same path applies (tree class already routes; discovery saw palms/oak/maple/tc1_* in mc2_01). Needs a leaf asset with `alphaMode=MASK` @ 0.5 to exercise the alpha-cutout leaf rule (the box has no alpha). Optional next step.
