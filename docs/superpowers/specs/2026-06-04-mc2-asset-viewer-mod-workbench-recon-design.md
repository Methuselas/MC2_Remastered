# MC2-ASSET-VIEWER-MOD-WORKBENCH-RECON-1 — Design / Recon

**Date:** 2026-06-04
**Branch:** `claude/asset-viewer-mod-workbench-recon-1` (worktree `A:/Games/mc2-asset-viewer-mod-workbench`, off `claude/nifty-mendeleev` @ `377b4a91`)
**Status:** Recon + design contract. Terminal step → `writing-plans`.

---

## 1. Goal

Turn `mc2_asset_viewer` into a **mod workbench**: the single place a modder can
**inspect → validate → package** a model override.

Feature target (modder loop):

```
drop in a GLB + textures
  → viewer shows it replacing a stock prop
  → validates scale / pivot / bounds / materials / LOD / impostor / overdraw
  → emits a model_overrides bundle the engine can load
```

Why it matters: every future visual feature (overrides, LODs, impostors, PBR
materials) becomes a thing a modder can *use and validate*, instead of something
buried in hand-edited JSON and smoke logs.

---

## 2. Current-state recon

### 2.1 Asset viewer (`tools/asset_viewer/`, on nifty)

- Target `mc2_asset_viewer` — SDL2 + ImGui, GL 3.3. Entry `tools/asset_viewer/main.cpp`.
  Two-phase build: `mc2_tglloader` (game-free static lib) + the ImGui/SDL client.
- 3-panel shell: `AssetViewerApp::drawUi()` dispatches a left **sidebar**
  (`AssetTypeSidebar`), a center **browser** (`FileBrowser` / `ModelBrowser`), and
  a right **inspector** (`TexturePreview2D` / `MaterialPreviewPBR` / `MeshPreview3D`).
- Asset types shipped (stages 1–2.5): Textures, Materials, **StaticProps** (2,030
  `.tgl` from `tgl.fst`), Mechs, Vehicles.
- Model load is **headless `.tgl`** via `TglMeshLoader` (`ensureFastFile` →
  `LoadBinaryCopy` → per-texture `SubMesh`, Stuff→GL coord transform
  `x'=-x, y'=z, z'=y`, AABB). Upload via `MeshGpu`. Orbit/zoom FBO preview,
  resolution-tier hot-swap.
- **Preview is "Backend B"**: a viewer-local Cook-Torrance approximation
  (`LocalPbrMaterialBackend` / `MaterialPreviewPBR`). It is **not** the engine
  render path. `PreviewSurface` is the abstract seam; Backend A (real RenderCore)
  is a documented future swap-in (`tools/rendercore_standalone_spike` exists on
  nifty as proof-of-concept).
- **The viewer cannot load a GLB today** — only `.tgl`.

### 2.2 Model-override system

Two halves, **split across branches**:

- **On nifty** (infra only): static-prop GPU batcher/registry
  (`gos_static_prop_registry`, `gos_static_prop_batcher`),
  `RenderCore/StaticPropTypeDesc.h`, the per-asset manifest schema
  (`docs/asset-manifest-schema.md`) + shape-only validator
  (`tools/validate_asset_manifest.py`), and `mclib/assimp_importer.cpp`
  (GLB/FBX → `TG_TypeMultiShape`, geometry-only MVP, coord `x=-x,y=z,z=y` + UV
  V-flip, `ENABLE_ASSIMP_IMPORTER` gated).
- **On `claude/model-override-system-recon-1`** (the override *binding* runtime,
  **unmerged**): `mclib/model_override_registry.{h,cpp}` + the `bdactor.cpp`
  resolve sites + the tree-LOD line.

**The runtime override manifest exists and is real:** `model_override_registry`
loads `data/model_overrides/models.json`, keyed `"<class>:<appearanceName>"`.
Validated MVP invariants (bad entries dropped + logged, never fatal):

- `type == "model"`, `renderOnly == true`, `fallback == "stock"`, `scale == 1.0`
- `replaces: "<class>:<name>"`, class ∈ {`staticProp`, `tree`}
- `source`: safe relative `.glb`/`.gltf` (LOD0)
- `lods[]`: optional, ascending `lod` index, each a safe source + `distance` band
- duplicate key → first wins

Engine call sites: `bdactor.cpp:345` (`resolve("staticProp", …)`),
`bdactor.cpp:3863` (`resolve("tree", …)`); default load
`data/model_overrides/models.json`, manifest dir `data/model_overrides`
(`model_override_registry.cpp:162`).

**Critical for this design — `model_override_registry` is engine-independent.**
Its only includes are `nlohmann/json.hpp` (header-only) + stdlib
(`<cctype> <cstdio> <fstream> <mutex> <utility>`). No TG_/GameOS deps. 167 lines.
→ **It links cleanly into the standalone viewer.** The workbench can run the
*exact same* parse/validate the engine runs.

### 2.3 Validation, LOD, impostor today

- **Validation:** `validate_asset_manifest.py` is **shape-only** (required fields,
  types, ranges) over the *per-asset descriptor*. No semantic checks
  (bounds coherence, scale, material completeness, pivot). The `model_override_registry`
  load is the only *override-binding* validation, and only the MVP invariants above.
- **LOD:** `models.json` `lods[]` parses (ascending index, safe source, distance).
  Engine discrete LOD ladder is `MAX_LODS=3` (`bdactor`/`apprtype`). Distance
  select on the override branch is plumbed but `activeLOD` pinned 0 in the
  unmerged MVP. No auto-decimation.
- **Impostor:** a **capability flag** in the per-asset schema; the actual 2-card
  bake lives on the foliage-impostor branch (`.claude/tree_export_impostor.py`),
  not in mainline. No impostor authoring in the viewer.
- **Overdraw:** never measured anywhere. (Foliage work established alpha-card
  *screen coverage / overdraw* is the dominant tree GPU cost — so even a heuristic
  flag here is high-value.)

---

## 3. Key architectural insight

**The inspect → validate → package loop ships without Backend A.**

Three things already exist and link into the standalone viewer:

1. **assimp** (vendored at `3rdparty/assimp`) → load the modder's GLB.
2. **`model_override_registry`** (engine-independent) → validate + emit `models.json`
   with engine-identical rules.
3. **`TglMeshLoader`** (already in the viewer) → load the bound **stock** prop.

"Shows it replacing a stock prop" is then: load the stock `.tgl` and the override
`.glb` **into the same Backend-B scene**, overlay / side-by-side, and compute
scale / pivot / bounds deltas geometrically. That comparison is fidelity-neutral —
both meshes render the same way, so the *geometric* replacement check is exact
even under Backend B.

Backend A upgrades only **pixel fidelity** — real in-game lighting, real material
response, and **measured** overdraw / impostor authoring. It is a v2 enhancement
behind the existing `PreviewSurface` seam, **not a gate** on the modder loop.

---

## 4. Locked decisions

| Decision | Choice |
|---|---|
| Preview spine | **Two-track** — data-driven validate+package in v1; Backend A behind `PreviewSurface` for v2 |
| GLB load | **Link assimp into the viewer** + `SDL_DROPFILE` drag-into-window |
| Package output | **Drop-in bundle folder** under `data/model_overrides/<id>/` + `models.json` record |
| Manifest format | `data/model_overrides/models.json` (the existing `model_override_registry` schema) is the **primary** output; `asset-manifest-schema.md` per-asset descriptor is an optional provenance sidecar |

---

## 5. Architecture

### 5.1 Placement

New **"Mod Workbench"** mode in the existing sidebar (`AssetTypeSidebar` already
reserves a disabled "Mod Package" slot). It reuses the 3-panel shell, the
`MeshPreview3D` FBO/orbit camera, `MaterialSlots`, and tier hot-swap. No new
windowing.

### 5.2 The three linkable seams

1. **GLB → MeshData adapter.** New viewer-side adapter that runs assimp and
   produces the viewer's `MeshData`, mirroring `assimp_importer`'s transforms
   (coord `x=-x, y=z, z=y`; UV `v = 1 - v`). This is a *parallel* path to the
   in-engine importer — it deliberately does **not** depend on `TG_TypeMultiShape`.
2. **`model_override_registry` (linked).** The workbench's validation + emit run
   `loadFromFile` on the candidate/emitted `models.json` and surface its
   drop-reasons verbatim as BLOCK-level warnings. Engine-identical.
3. **`TglMeshLoader` (existing).** Loads the bound stock prop for overlay compare.

### 5.3 Data flow

```
drag GLB ──► SDL_DROPFILE ──► extension sniff
                                  │ .glb/.gltf → assimp → MeshData → preview
                                  │ image      → texture slot
                                  │ .json      → load existing models.json
pick stock appearance ──► TglMeshLoader → stock MeshData → overlay
                                  │
both meshes ──► geometric deltas (bounds / pivot / scale)
                                  │
validation engine ──► [registry MVP invariants] + [semantic WARN checks]
                                  │
                              Warnings panel
                                  │
                 emit ──► data/model_overrides/<id>/ (GLB + textures)
                          + append/merge models.json record
                          + round-trip re-parse via registry (assert 0 drops)
```

---

## 6. v1 build sequence (data-track)

- **S0 — link spike (de-risk first).** Add `model_override_registry` + assimp to
  the `mc2_asset_viewer` target; confirm a clean compile/link (header-only
  nlohmann on the include path; viewer is a *separate* target, so the
  `check-json-isolation.sh` "one engine TU" rule does not apply — note it). Pull
  the 2 registry files in (cherry-pick from the override branch — they are
  self-contained) or set the merge-base. **Gate: builds + links, no engine symbols
  pulled.**
- **S1 — GLB load.** `SDL_DROPFILE` handler in `main.cpp` event loop → extension
  sniff → assimp → `MeshData` adapter → render in `MeshPreview3D`.
- **S2 — source-stock binding + overlay.** Pick a stock appearance from the
  existing roster → `TglMeshLoader` → overlay / side-by-side. Compute bounds /
  pivot / scale deltas here.
- **S3 — validation engine + Warnings panel.** (Severity matrix §8.)
- **S4 — bundle emit + round-trip.** Write `data/model_overrides/<id>/`
  (normalized GLB + textures), append/merge the `models.json` record, re-parse via
  the registry, assert the new record survives (0 drops). **Gate: round-trip clean.**
- **S5 — panel polish.** LOD-chain panel (load + preview each LOD source, show
  tri-count reduction + distance bands), texture-set panel, impostor-presence panel.

---

## 7. Panel mapping (user's list → behavior)

| Panel | v1 behavior |
|---|---|
| Tree/model override | The dropped GLB: mesh stats, preview |
| Texture set | Slot pickers (reuse `MaterialSlots`), per-slot sRGB/linear, missing-texture flags |
| LOD chain | Load + preview each `lods[]` source; tri-count reduction; ascending/distance validate |
| Impostor | **Inspect/validate presence** (declared LOD/flag); WARN if high-overdraw alpha asset lacks one. **Authoring = v2** |
| Material slots | Reuse `MaterialSlots`; completeness check |
| Collision / source stock object | Bind `replaces:` to a stock appearance; overlay vs stock. **Collision stays stock** (renderOnly MVP, CASE A) |
| Lighting preview | Backend-B controllable directional light in v1; engine lighting via Backend A in v2 |
| Warnings | Aggregator of BLOCK + WARN from the validation engine |

---

## 8. Validation matrix

**BLOCK** (emit refused / record would be dropped — sourced from
`model_override_registry` rules, engine-identical):

- `type != "model"`, `renderOnly != true`, `fallback != "stock"`
- `scale != 1.0`
- `class` not in {`staticProp`, `tree`}, or `class` disagrees with `replaces`
- unsafe / non-relative / non-`.glb`/`.gltf` source
- `lods[]` not ascending, or a LOD source unsafe/missing
- duplicate key

**WARN** (workbench-added semantics; emit allowed, modder informed):

- bounds delta vs stock prop beyond a threshold (footprint X× stock)
- scale ≠ 1.0 *implied by baked GLB transform* → "bake scale into the GLB; runtime
  forces scale 1.0"
- pivot/origin mismatch vs stock convention
- material slot incomplete / texture file missing
- overdraw **heuristic** (alpha-coverage / leaf-card area ratio) — *flag, not
  measured* in v1
- high-overdraw alpha asset with no impostor LOD declared

---

## 9. Manifest & bundle format

- **Primary output:** a `models.json` record (existing schema, §2.2). Source paths
  relative to `data/model_overrides`, so an override's GLB at
  `data/model_overrides/<id>/model.glb` → `source: "<id>/model.glb"`.
- **Bundle:** `data/model_overrides/<id>/` holds the normalized GLB + its textures;
  the central `data/model_overrides/models.json` gets the appended/merged record.
  "Drop-in" = ship the `<id>/` folder + the record.
- **Round-trip invariant:** after emit, re-parse `models.json` through the linked
  registry and assert the new record is **not** dropped.
- The per-asset `asset-manifest-schema.md` descriptor is **optional** provenance
  written alongside — do **not** conflate it with `models.json`.

---

## 10. Backend A (v2) seam

Keep `PreviewSurface` the swap point. v2 drops `rendercore_standalone_spike`
behind the same interface and unlocks, with no workbench rework:

- engine-faithful lighting + material response in the preview
- **measured** overdraw (stencil-increment or fragment counter) replacing the
  v1 heuristic
- impostor authoring: render-to-card 2-card bake from the real engine path

---

## 11. Dependencies & risks

- **DEP-1 — in-game effect.** The *runtime* override (the prop actually swapping
  in-game) needs `claude/model-override-system-recon-1` merged to nifty. **Out of
  workbench scope.** The workbench delivers authoring/validation/packaging value
  standalone; the recon flags the merge as the downstream consumer.
- **DEP-2 — registry source location.** `model_override_registry.{h,cpp}` is on the
  override branch, not nifty. S0 cherry-picks the 2 self-contained files (or sets
  the merge-base).
- **RISK — assimp viewer link.** Adds assimp to the SDL/ImGui viewer target
  (build size/time). Already vendored and built by mclib; the viewer is a separate
  link. Manageable.
- **RISK — overdraw is heuristic-only in v1.** Set modder expectations; real number
  is a Backend-A (v2) deliverable.
- **RISK — manifest conflation.** Keep `models.json` (primary) and
  `asset-manifest-schema.md` (optional sidecar) distinct.

---

## 12. Out of scope (v1)

Backend A · impostor authoring · auto-LOD decimation · non-unit scale · collision
override · mech / animation overrides.

---

## 13. Open follow-ups

- Confirm the stock-appearance picker source for the `replaces:` key (reuse the
  existing 2,030-prop `.tgl` roster vs an appearance-name list).
- Decide whether S5's LOD-chain preview also offers a "normalize scale to stock on
  export" bake (stretch — turns the scale WARN into a one-click fix).
- Sequence the `claude/model-override-system-recon-1` merge so the in-game half
  lands behind the authoring half.
