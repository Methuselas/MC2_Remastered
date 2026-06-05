# Tree-Asset LOD Bake Pipeline — Design + Prototype

Date: 2026-06-04
Worktree: `A:/Games/mc2-model-override-recon`
Status: DESIGN + WORKING PROTOTYPE (one species baked end-to-end). Asset-pipeline
only — **no engine C++/shader changes.**

---

## 0. TL;DR

An offline, repeatable Blender-5.1-headless pipeline that turns a multi-tree OBJ
into a full per-species LOD chain matching the existing MC2 model-override
manifest:

```
LOD0  solid-canopy real geometry  (opaque diffuse)   — the near "pretty" tree
LOD1  Decimate collapse ~0.50                          — half tris
LOD2  Decimate collapse ~0.18                          — far solid geo
LOD3  2-card cross impostor       (alpha cutout card)  — distant
```

Each LOD → a GLB in `data/model_overrides/source/trees/`; the geo-LOD opaque
diffuse texture → `data/tgl/128/<stem>.tga`; the impostor card →
`data/tgl/128/a_<species>_imp.tga`; one manifest row per species (with `lods[]`).

Prototype proven on **Walnut_L** (see §5 numbers): LOD0 10,344 tris → LOD1 5,172
→ LOD2 1,861 → impostor 2; solid LOD0 has **49× fewer tris** and **3.7× lower
worst-case overdraw** (dc_max 67 vs 246) than the old lush alpha-card tree.

---

## 1. Asset reality (VERIFIED — overturns the briefing premise)

The briefing said trees9.obj is "grouped BY MATERIAL not by tree … each tree's
bark+leaves are in different material groups but co-located in space," and asked
for connected-component / X-band spatial clustering to recover 8 individual trees.

**That is not what the asset is.** Verified by loading `trees9.obj` in Blender 5.1
headless (`analyze_trees9*.py`, thumbnails in
`.claude/tree_assets_scratch/thumbs/`):

- The OBJ imports as **8 separate mesh objects**, one per material.
- Each material/object occupies a **distinct, essentially NON-overlapping X-band**
  — i.e. each material **is already one complete, spatially-isolated tree**
  (trunk + solid leaf geometry under a single material), packed side-by-side
  along X. They are NOT co-located.

Per-object bounding boxes (OBJ space, Z-up):

| object   | tris   | X range        | Z (height) | what it is (from thumbnail)          |
|----------|--------|----------------|------------|--------------------------------------|
| Bottom_T | 39,596 | -48.9 .. -30.5 | 45.4       | tall tree (sonneratia/mangrove)      |
| Bark___1 | 40,132 | -27.4 .. -13.5 | 35.0       | tall tree                            |
| Mossy_Tr | 32,408 | -10.2 ..   9.0 | 24.6       | big bushy solid-canopy tree          |
| Bark___0 | 94,544 |   6.9 ..  13.1 | 17.3       | very dense tree (highest tri count)  |
| Walnut_L | 10,344 |  14.4 ..  27.1 | 13.4       | compact dense deciduous (PROTOTYPE)  |
| Bark___S | 13,448 |  25.3 ..  34.2 | 12.5       | medium tree                          |
| Sonnerat | 10,408 |  34.5 ..  46.3 | 10.2       | mangrove, flat-top canopy, prop roots|
| Oak_Leav |  6,004 |  49.2 ..  50.8 |  2.7       | thin young oak / sapling (sparsest)  |

**Consequence for the pipeline:** "split into individual trees" reduces to
**"pick one material-object."** No union-find / connected-component clustering is
needed (and a naive X-band split would be wrong because the bands are uneven and a
couple of objects' X-extents touch — Mossy/Bark___0 overlap ~2u, Walnut/Bark___S
~1.8u — but the per-object split sidesteps that entirely). The materials are
diffuse JPG atlases mapping both bark and leaves of that one tree; **leaves are
real solid geometry, not alpha cards** → genuinely low overdraw, exactly the
"pretty solid-canopy LOD0" target.

**"Oak" caveat:** the literal `Oak_Leav` object is a thin sapling (6,004 tris,
2.7 tall, sparse) — the weakest demonstrator. The prototype therefore bakes
**Walnut_L** (compact, dense, self-contained, the cleanest solid-canopy tree) as
the representative deciduous tree, and the script is parameterized by `--species`
so `Oak_Leav` (or any of the 8) bakes identically.

---

## 2. Conventions reused (read, not reinvented)

From the existing scripts + importer — the pipeline matches all of these so the
output drops into the engine with no code change:

- **Axis / orientation** (`tree_export_lush.py`, `assimp_importer.cpp` `axisMap`):
  export with `export_yup=True` **and** a 180° rotation about X on the mesh data
  (`mesh.transform(Matrix.Rotation(...))` — `rotation_euler +=` silently fails).
  This matches the engine's default `MC2_GLTF_AXIS=0` mapping `(-x,-y,z)` so the
  tree is upright. Grounding handled by the engine's `MC2_GLTF_GROUND=2` default
  (base at most-positive stuff.y → translated to 0).
- **Texture naming** (`assimp_importer.cpp::DeriveMC2TextureName`): the engine
  derives the texture name from the **embedded glTF image stem**, lowercased +
  sanitized, `.tga` appended. An `a_` prefix is **auto-added only** when the glTF
  `alphaMode` is MASK/BLEND. Therefore:
  - geo LODs use an **OPAQUE** material (`blend_method='OPAQUE'`) referencing an
    image named `<stem>` → engine loads `<stem>.tga` as opaque diffuse (no prefix).
  - the impostor card material is alpha CLIP → engine adds `a_` → it must be
    deployed as `a_<species>_imp.tga`.
- **Impostor render** (`tree_export_impostor.py`, `cook_stock_impostor.py`):
  EEVEE, `view_transform='Standard'` (AgX washes bakes), `film_transparent=True`,
  ortho front view, emission-fed albedo so the card holds flat color; build a
  2-card cross (X-Z and Y-Z planes) sized to the bbox, full-texture UVs on each.
- **Overdraw metric** (`foliage_cook_metrics.py`): depth-complexity raster
  (mean/max layers-per-pixel), front + top projections, report the worse.

---

## 3. Pipeline stages

```
trees9.obj ──(Blender 5.1 headless, tree_lod_bake.py --species X)──┐
                                                                   │
  isolate object X  → recenter XY, base→Z0, scale, opaque diffuse  │
  ├─ LOD0  export GLB (yup + 180°X flip)                           │
  ├─ LOD1  Decimate(collapse 0.50) → export GLB                    │
  ├─ LOD2  Decimate(collapse 0.18) → export GLB                    │
  └─ LOD3  ortho front render → a_X_imp.tga ; 2-card cross → GLB   │
                                                                   ▼
  TEXTURES:  walnut_l.tga (opaque diffuse, via PIL JPG→TGA)
             a_walnut_l_imp.tga  (alpha card, rendered directly)   → data/tgl/128/
  GLBS:      <stem>_lod0/1/2.glb + <stem>_imp.glb                  → source/trees/
  MANIFEST:  one row printed for models.json (with lods[])
```

### Why the diffuse TGA is deployed via system PIL, not Blender

Blender 5.1 lazy-loads JPGs; the pixel buffer is empty until a channel is touched.
The first prototype run hit two symptoms of this:
1. saving the diffuse TGA from Blender produced "Image does not have any image
   data";
2. the **impostor card rendered MAGENTA** (missing-texture fallback) because the
   emission texture had no decoded pixels.

Fix (in the script): force a decode with `_ = img.pixels[0]` immediately after
load (`has_data` then True), which fixes the impostor albedo and the GLB embed.
The deployed **opaque diffuse TGA is still produced by system PIL** (Blender lacks
PIL; PIL JPG→TGA is one reliable line) — the script prints the exact
`DEPLOY_DIFFUSE_CMD::` for the wrapper to run. This two-tool split (Blender for
geometry/render, PIL for the diffuse texture file) is deliberate and matches the
existing `foliage_crop.py` PIL-stage precedent.

### Distance ladder (manifest `lods[]`)

Default emitted: LOD1 @ 60, LOD2 @ 120, impostor @ 200 (world units). These are
starting values to tune in-engine with `MC2_FORCE_LOD` / `MC2_IMPOSTOR_DIST`
(the engine already does `selectActiveLOD` distance selection per the foliage
impostor handoff). The existing lush manifest uses a single impostor @ 120; the
geo-LOD ladder slots two solid decimations before the impostor so the silhouette
holds longer than the alpha-card path did.

---

## 4. Files

| file | role |
|------|------|
| `.claude/tree_lod_bake.py` | **the prototype bake script** (`--species`, `--scale`, `--ratios`, `--res`) |
| `.claude/lod_bake_metrics.py` | depth-complexity driver (reuses `foliage_cook_metrics.py` helpers) over the new LODs + the old lush tree |
| `.claude/analyze_trees9{,b,c}.py` | asset-reality probes (bbox, X-histogram, per-object identity, trunk/canopy pairing) |
| `.claude/thumb_trees9.py` | per-object thumbnail renders (identify each of the 8 trees) |
| outputs: `data/model_overrides/source/trees/walnut_l_lod{0,1,2}.glb`, `walnut_l_imp.glb` | LOD GLBs |
| outputs: `data/tgl/128/walnut_l.tga`, `a_walnut_l_imp.tga` | deployed textures |

---

## 5. Prototype results (Walnut_L)

Depth-complexity (`.claude/lod_bake_metrics.py`, 256² raster, worse of front/top):

```
asset               tris   cards   dc_mean    dc_max
------------------------------------------------------
OLD lush LOD0     508734    6663     25.46       246    (alpha-card reference)
OLD lush imp           4       2      1.00         2
walnut LOD0        10344     412     16.42        67    (solid-canopy NEW LOD0)
walnut LOD1         5172     473     16.29        62
walnut LOD2         1861     535     15.92        58
walnut imp             4       2      1.00         2
```

GLB / texture sizes:

```
LOD0     10344 tris   ~413 KB   walnut_l_lod0.glb
LOD1      5172 tris   ~258 KB   walnut_l_lod1.glb
LOD2      1861 tris   ~121 KB   walnut_l_lod2.glb
impostor     2 tris   ~141 KB   walnut_l_imp.glb
diffuse  walnut_l.tga       ~120 KB (152×263 opaque)
impostor a_walnut_l_imp.tga ~410 KB (512×512 RGBA, 37.9% alpha coverage)
```

Interpretation:
- **49× fewer tris** at LOD0 vs the old lush alpha-card tree (10.3K vs 508.7K).
- **dc_max 67 vs 246 = 3.7× less worst-case fragment stacking.** Combined with
  **opaque depth-test** (early-Z rejects occluded fragments — unlike the alpha
  path where every layer must be shaded for the alpha test), the real AlphaOn-pass
  fill cost of the solid canopy is far below what dc_mean alone suggests.
- dc_mean stays ~16 across LOD0→LOD2: decimating solid geometry does **not** add
  overdraw, it only cuts tris — the desired LOD behavior. (The residual ~16 is the
  canopy's genuine top-down leaf-clump overlap; it is opaque, so it is cheap.)
- Both impostors collapse to dc_mean 1.00 (2 non-overlapping cross-cards).
- Impostor albedo verified natural (mean opaque RGB ≈ [253,251,248], the pale
  walnut leaf color) after the pixel-decode fix — no magenta.

---

## 6. Batching the other species — what remains

The script is already species-parameterized, so batching is a loop. Remaining work:

1. **Per-species scale + distance tuning.** `--scale 1.6` and the 60/120/200
   ladder were set for Walnut. The 8 trees range 2.7→45.4 tall in OBJ units; each
   needs a scale chosen so its in-game footprint matches the stock tree it
   `replaces`, and distances tuned with `MC2_IMPOSTOR_DIST`/`MC2_FORCE_LOD`.
2. **`replaces:` mapping.** Decide which stock tree id each species overrides
   (the manifest currently maps `oak1`, `tc1_1..4`, `palm1`, `palms`). Map
   walnut/oak/sonnerat/mossy/etc to appropriate stock ids; emit one manifest row
   each (the script prints a ready row per run).
3. **Diffuse TGA deploy.** Run the printed `DEPLOY_DIFFUSE_CMD::` (PIL JPG→TGA)
   per species — trivially scriptable as a batch wrapper.
4. **Decimate-ratio per species.** 0.50/0.18 suits a 10K-tri tree; the dense
   Bark___0 (94K) and Bottom_T/Bark___1 (~40K) want lower ratios (e.g.
   0.25/0.08) to land at comparable LOD1/LOD2 budgets. Expose already via
   `--ratios`.
5. **Sapling exception.** `Oak_Leav` (6K, sparse) barely needs geo LODs; it can
   ship LOD0 + impostor only, or be dropped in favor of a denser species labeled
   "oak."
6. **In-engine visual validation.** Deploy the manifest + assets to the running
   build and confirm: upright orientation (axis default), grounding, opaque
   diffuse binds (no magenta), distance-select transitions, impostor shadow-skip
   (`RecipeRange.noShadow` per the foliage handoff — flat cards cast blob shadows).

### Blockers / risks

- **None intractable.** The feared "spatial clustering is hard" blocker does NOT
  apply — the asset is pre-split per object (§1). The fallback the briefing asked
  for ("treat each material-group as-is") is in fact the *primary* path here.
- **Low-res diffuse JPGs** (~150×250) are the visual quality ceiling for geo LODs;
  acceptable for distant/mid trees, and orthogonal to the LOD pipeline. Upscaling
  the diffuse atlas (the repo's ESRGAN scripts) is an optional later polish.
- **glTF embed of JPG**: must force `img.pixels[0]` decode before export/render
  (done) or both the embed and the impostor render fail silently (magenta).
```
