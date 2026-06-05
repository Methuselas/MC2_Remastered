# TRACKG-OFFLINE-GLB-COOK-MANIFEST-1 — Plan

Date: 2026-06-04. Track G keystone, slice 1.
Scoping parent: `docs/superpowers/specs/2026-06-04-engine-convergence-and-fidelity-next-arc.md` (Arc G).
Discipline: recon-first (done), data-before-behavior, one asset end-to-end, no runtime behavior change, TDD per slice.

---

## Executive verdict

**Promote, do not rebuild.** ~60–70% of an offline glTF→cooked-static-prop pipeline
already exists as working, mostly GL-free pieces. This slice WIRES them into one
deterministic offline cook and defines the **`manifest.json` asset contract** — the
long-term G3 deliverable everything downstream (loader, capability, fallback, editor
inspector) keys on.

What exists (reuse): the override **registry** (load + validate + safe-path + LOD parse,
engine-clean, byte-identical on nifty), the **Assimp importer** (`ImportGeometryFromFile`
+ `DeriveMC2TextureName` + `a_` alpha convention), the **texture-handle resolver**
(`LoadOverrideRenderShapeTextures`), the five **render seams** the cooked asset must
satisfy, the workbench **GL-free loaders + BLOCK/WARN validators + bundle export with a
throwaway-registry round-trip gate**, the **`ktx.exe` BC7 cook** (`cook_tgl_tiers.py`),
the **MaterialGpu 32B schema** + **material-manifest v1 schema**.

What is genuinely new: (1) the unified **`manifest.json`** (geometry + materials +
capability + provenance) — superset of `models.json` (override) and
`material_manifest.schema.json` (textures); (2) an **offline cook driver** chaining
stage→geometry→texture→manifest→round-trip; (3) **frozen axis/UV convention** resolution
(the two-convention hazard); (4) **MeshCapability flags + `[RENDER_PATH v1]` log** —
DESCRIPTIVE only this slice.

**Runtime is untouched.** The cook produces files the *existing* override seams already
consume (`models.generated.json` + tier KTX2 + cooked glb). No engine code path changes
in this slice. Capability is logged, not yet path-authoritative (authority flip is a
later gated slice — "capability before fallback" means define the data first).

Estimated shape: 6 slices, bigbox.glb for plumbing, `2civliving` as the stock-parity
building. First real win = a hand-authored `manifest.json` that validates + round-trips
(data before behavior).

---

## Reuse map (from model-override branch + workbench + cook tooling)

Legend: **[asis]** reuse unchanged · **[wrap]** reuse behind cook driver · **[decide]**
reuse after freezing a convention · **[new]** build.

### Engine (branch `claude/model-override-system-recon-1`; registry byte-identical on nifty)
| Component | File:line | Role | Cook use |
|---|---|---|---|
| `model_override_registry.{h,cpp}` | `mclib/` | load-only JSON→struct; validate (`type==model`,`renderOnly`,`fallback==stock`,`scale==1.0`,safe rel `.glb/.gltf`,class∈{staticprop,tree}), LOD parse, `resolve()`, `instance()`→hardwired `data/model_overrides/models.json` | **[asis]** the authoritative round-trip oracle; cook never bypasses it |
| `ImportGeometryFromFile(path, TG_TypeMultiShape*)` | `mclib/assimp_importer.cpp:475` | Assimp glTF→`TG_TypeMultiShape`; triangulate+gensmoothnormals+joinidentical | **[decide]** runtime importer = authority; cook must target ITS convention |
| axis/UV transform | `assimp_importer.cpp:60-82,426` | `MC2_GLTF_AXIS/YOFF/GROUND` runtime env (default axis0 `(-x,-y,z)`); **`toMC2V = 1.0-v` flip applied** | **[decide]** FREEZE: bake to default axis0 + accept `1-v`; record in manifest |
| `DeriveMC2TextureName` + `BuildTextureList` | `assimp_importer.cpp:173,271` | base-color/diffuse → sanitized `.tga` name; glTF `alphaMode MASK/BLEND` (+leaf heuristic) → `a_` prefix | **[asis]** material-name + alpha discovery logic (port to offline) |
| `LoadOverrideRenderShapeTextures` | `bdactor.cpp:155` | type-shape handle resolver; path `tglPath/<ObjectTextureSize>/<name>`; `a_`→`gos_Texture_Alpha` | **[asis]** runtime seam; cook must land textures where this probes |
| **Render seams (integration contract)** | | | **[asis]** the cooked asset MUST satisfy these — they don't change |
| register-before-finalize | `bdactor.cpp:2934 registerStatic()` → `registerMultiShape(getBldgRenderShape(i), isOverride)` | mission-load pre-pass registers type into `s_typeIndex` before `finalizeGeometry` | bldg path IS wired (verify renders) |
| isStaticEligible bdAnim skip | `bdactor.cpp:2904` `overrideStatic = bldgRenderShape && !bldgTypeHasAnimations` | admits override recipe past the animating-guard | — |
| layerForPacket=-1 override route | `gos_static_prop_batcher.cpp:3043-3089` | `type.isOverride && !uniques.empty()` → route NULLTXM packet to layer 0 (visible) | `isOverride` set by `registerMultiShape(...,true)` |
| GPU-INSTANCE-SKIP-POOLS-1 | `bdactor.cpp:2501` `TransformMultiShape_HierarchyOnly` | zero-pool walk for registered types (kill-switch `MC2_LEGACY_INSTANCE_POOLS=1`) | — |

### Workbench (`A:/Games/mc2-asset-viewer-mod-workbench`, `tools/asset_viewer/`; all GL-free)
| Component | File | Role | Cook use |
|---|---|---|---|
| `MeshData` / `GlbMeshLoader` / `TglMeshLoader` | `TglMeshLoader.h:18`, `GlbMeshLoader.cpp`, `TglMeshLoader.cpp` | canonical in-mem geometry (`MeshVertex{px..nz,u,v}`, `SubMesh{verts,idx,textureName,isSpotlight}`, `MeshData{submeshes,bmin,bmax,ok,error}`); GLB=two-step srcToGl + Assimp auto-flip; TGL oracle=single Stuff→GL | **[wrap]** geometry parity oracle (GLB vs stock TGL bounds/footprint/pivot). **NOTE convention differs from runtime importer** |
| `ValidateRecordRules` (BLOCK) | `OverrideManifest.cpp:12` | hard blocks: renderOnly, fallback==stock, scale==1.0, class, appearance non-empty+verified, safe source, lod-order/source | **[asis]** |
| `ValidateSemantics` (WARN) | `WorkbenchValidation.cpp:3` | bounds-delta(>1.5/<0.67), pivot-xz(>0.25), pivot-y(>0.25), no-stock, texture-missing, overdraw(`a_`/`_a_` && no impostor) | **[asis]** |
| `ToModelsJson` / `ExportBundle` | `OverrideManifest.cpp:36`, `BundleExport.cpp` | emits `models.generated.json` + bundle dir; **round-trip gate**: throwaway `ModelOverrideRegistry`, `resolve()!=null` else "EXPORT BLOCKED BY REGISTRY" | **[asis]** the non-destructive write model |
| 6 smokes `--smoke-workbench-{link,glb,bind,validate,export,reload}` | `AssetViewerApp.cpp:1298-1414` | link/import/parity/validate/export/reload assertions | **[wrap]** cook-validation harness; add cook smokes alongside |

### Texture cook (`A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`)
| Component | File | Role | Cook use |
|---|---|---|---|
| `ktx.exe` 2-step | `A:/Games/mc2-tools/ktx/ktx.exe` | `create --encode uastc --format <FMT> --assign-tf <TF> [--generate-mipmap]` → `transcode --target bc7`; output **BC7 stored** (vk 145 UNORM / 146 SRGB) | **[asis]** loader rejects Basis/supercompressed |
| `_ktx_cook()` | `tools/mc2texcook/cook_tgl_tiers.py:23` | static-prop tier cook → `data/tgl/{128,256,512,1024}/<name>.ktx2`, mips ON, Lanczos down, never upscale | **[wrap]** copy verbatim |
| MaterialGpu 32B | `RenderCore/MaterialGpu.h:88` + `material_gpu.hglsl:46` | `albedoTex`(off0)=tex-array LAYER, normal/MR/emissive, flags, 3 factors; `kMaterialTexAbsent=0xFFFFFFFF`; mirror gate `scripts/check-material-gpu-mirror.sh` | **[asis]** manifest references this index model |
| material-manifest v1 | `tools/material_cook/material_manifest.schema.json` + `validate_manifest.py` | `{version:1, materials[]{slot==idx, albedo_ktx2 req, normal/mr/emissive opt, factors, flags}}` | **[wrap]** EXTEND, don't fork |
| `StaticPropTypeDesc` 16B | `RenderCore/StaticPropTypeDesc.h:15` | typeId/firstPacket/packetCount/**alphaClass**; populated `finalizeGeometry` `gos_static_prop_batcher.cpp:2216` | manifest grouping must be alphaClass-aware |

**Promote-vs-build summary:** promote registry + importer-material-logic + resolver + 5
seams + workbench loaders/validators/export + ktx cook + material schema. Build: the
manifest schema (superset), the offline cook driver, the convention freeze, capability +
RENDER_PATH log. **Do NOT build a second importer, a second material schema, a gltfpack
route (source-only/unbuilt), or any central-manifest writer.**

---

## Offline staging format

Data lives DEPLOY-side (`A:/Games/mc2-opengl/mc2-win64-v0.4/`); cook tooling lives in the
worktree (tracked). Layout:

```
data/model_overrides/
  models.json                       # CENTRAL authoritative override manifest — NEVER auto-written (reviewed merge only)
  source/
    props/<id>.glb                  # AUTHORED input (e.g. bigbox.glb [exists], 2civliving.glb [to author])
  cooked/<id>/                      # NEW per-asset cook output (bundle-local DRAFT)
    manifest.json                   # the G3 contract (geometry + materials + capability + provenance)
    <id>.glb                        # cooked geometry — frozen axis/uv, optional meshopt-optimized
    models.generated.json           # runtime override projection (registry-consumable subset)
    cook.log                        # tool versions + decisions (provenance)
data/tgl/{128,256,512,1024}/<texname>.ktx2   # SHARED tier dirs — textures land HERE (where LoadOverrideRenderShapeTextures probes), NOT bundle-local
```

Rules:
- Geometry glb + manifest are **bundle-local** under `cooked/<id>/` (DRAFT, non-destructive).
- Textures cook to the **shared `data/tgl/<tier>/` dirs** because the runtime resolver
  builds `tglPath/<ObjectTextureSize>/<name>` — bundle-local textures would be invisible to it.
  The manifest's `deps.textures[]` records exactly which tier files were produced.
- Promotion = a separate reviewed step appending `cooked/<id>/models.generated.json`'s entry
  into central `models.json`. The cook NEVER does this (forbidden — stop condition).
- Source `.glb` axis/orientation must be authored in MC2 world units, scale==1.0, origin =
  stock prop pivot (so derived bounds match). The cook FREEZES the import convention and
  validates parity; it does not silently re-scale.

Convention freeze (the load-bearing decision): the **runtime importer
(`assimp_importer.cpp`) is authority** — default `MC2_GLTF_AXIS=0` `(-x,-y,z)` + `1-v`
V-flip. The cook bakes orientation so the default-env runtime import is correct, and
records `geometry.convention = {axis:0, vflip:true, importer:"assimp_importer.v1"}` in the
manifest. The workbench `GlbMeshLoader` preview uses a *different* chain (two-step, no
flip) → its preview orientation may differ from runtime; reconcile or label (task R0).

---

## manifest.json schema (the asset contract)

Superset of override-`models.json` + material-manifest-v1. Draft-07, `$id:
mc2-asset-manifest-v1`. Hand-authored golden first (data before behavior), cook-emitted after.

```json
{
  "schema": "mc2-asset-manifest-v1",
  "cookVersion": 1,
  "asset": {
    "id": "2civliving",
    "class": "staticprop",                 // staticprop | tree
    "appearanceName": "2civliving",        // stock .ase/.ini base name → override key
    "replaces": "staticprop:2civliving"    // composite key the registry consumes
  },
  "geometry": {
    "source": "source/props/2civliving.glb",
    "cooked": "2civliving.glb",            // bundle-local cooked glb
    "convention": { "axis": 0, "vflip": true, "importer": "assimp_importer.v1" },
    "scale": 1.0,                          // MVP invariant == 1.0
    "bounds": { "min": [x,y,z], "max": [x,y,z], "radius": r },
    "pivot": [x, y, z],
    "counts": { "verts": N, "tris": M, "submeshes": S },
    "lods": [                              // LOD0 == cooked; optional chain (mirrors override lods)
      { "lod": 1, "cooked": "2civliving_l1.glb", "distance": 120.0 }
    ]
  },
  "materials": [                           // EXTENDS material_manifest v1; index == submesh material slot
    {
      "slot": 0,
      "textureName": "2civliving",         // base name; a_ prefix iff alphaTest
      "alphaClass": 0,                     // 0=opaque 1=alpha (OR-reduce source for StaticPropTypeDesc)
      "albedo_ktx2": { "128":"data/tgl/128/2civliving.ktx2", "256":"...", "512":"...", "1024":"..." },
      "normal_ktx2": null,                 // null → kMaterialTexAbsent + flag unset (in-game normal/ORM SHELVED per asset-pipeline §6)
      "metallic_roughness_ktx2": null,
      "emissive_ktx2": null,
      "base_color_factor": 1.0,
      "metallic_factor": 0.0,
      "roughness_factor": 1.0,
      "flags": { "alpha_test": false, "double_sided": false, "window": false }
    }
  ],
  "capabilities": {                        // MeshCapability (descriptive this slice — see below)
    "hasLegacyMesh": true,                 // stock .tgl fallback exists
    "hasCookedGlb": true,
    "hasLodChain": false,
    "hasMeshlets": false,                  // .cdag — future, always false now
    "hasImpostor": false,                  // far-LOD card — future for buildings
    "alphaTest": false,
    "castsShadow": true,
    "supportsObjectId": true
  },
  "deps": {
    "stockFallback": "2civliving",         // appearanceName the runtime falls back to
    "textures": ["data/tgl/128/2civliving.ktx2", "..."],
    "sourceGlb": "source/props/2civliving.glb"
  },
  "provenance": {
    "sourceSha256": "…",
    "cookTools": { "ktx": "<ktx --version>", "assimp": "<ver>", "driver": "trackg_cook.v1" },
    "cookedUtc": "<stamped post-cook, NOT in script>"
  }
}
```

Notes:
- **`models.generated.json` is the runtime PROJECTION** of this manifest: only
  `{type:"model", class, replaces, source, renderOnly:true, scale:1.0, fallback:"stock",
  lods[]}`. The registry consumes that subset; the rich manifest is authoring/asset-db truth.
- `albedo_ktx2` carries the TIER SET (the runtime picks tier by `ObjectTextureSize`); the
  material-manifest v1 single-path field is extended to a tier map. Validation keeps slot==index.
- Absent normal/MR/emissive = `null` → `kMaterialTexAbsent` + flag NOT set, per MaterialGpu rule.
- `convention` + `provenance.sourceSha256` make the cook reproducible + drift-detectable.

---

## Material / texture cook plan

1. **Discover** materials from the source glb: port `DeriveMC2TextureName` logic offline
   (Assimp base-color/diffuse slot → sanitized name; glTF `alphaMode MASK/BLEND` or
   `leaf/foliage` heuristic → `a_` prefix → `alphaTest=true`, `alphaClass=1`). One material
   per submesh material slot (matches importer's 1:1 `localTextureHandle = mMaterialIndex`).
2. **Resolve source pixels**: embedded glTF textures → extract to PNG; external refs →
   locate. (MVP: opaque building = single albedo; no normal/ORM — in-game normal/ORM is
   SHELVED per `asset-pipeline.md` §6, deployed props are albedo-only by design.)
3. **Cook to KTX2 BC7** reusing `_ktx_cook()` (`cook_tgl_tiers.py:23`) verbatim:
   `ktx create --encode uastc --format R8G8B8A8_SRGB --assign-tf srgb --generate-mipmap` →
   `ktx transcode --target bc7`. Albedo = SRGB(146); (future normal/ORM = UNORM(145)).
   Lanczos downscale to tiers `{128,256,512,1024}`, **never upscale**. Output to shared
   `data/tgl/<tier>/<name>.ktx2`.
4. **Verify** each output with `ktx2check` (stored BC7, no supercompression — loader rejects
   otherwise → stop condition).
5. **Emit** `materials[]` into the manifest with tier paths + factors + flags + alphaClass.
6. **Name parity**: cooked texture base name MUST match what `LoadOverrideRenderShapeTextures`
   builds (`a_` prefix iff alpha) — else the runtime resolver leaves the handle
   `0xFFFFFFFF` and the override renders on the wrong layer.

Do NOT route through gltfpack (vendored source only, unbuilt). Do NOT author normal/ORM
into the in-game cook this slice (shelved).

---

## MeshCapability flags

Bitfield (`RenderCore/MeshCapability.h` — **[new]**, P2-3 `RenderableCapability` realized):

| bit | flag | source | this-slice value |
|---|---|---|---|
| 0 | `HasLegacyMesh` | stock `.tgl`/appearance exists | true (fallback present) |
| 1 | `HasCookedGlb` | cook produced glb | true |
| 2 | `HasLodChain` | manifest `lods[]` non-empty | false (single LOD MVP) |
| 3 | `HasMeshlets` | `.cdag` sidecar | false (future) |
| 4 | `HasImpostor` | far-LOD card baked | false (future for buildings) |
| 5 | `AlphaTest` | any material `alphaClass==1` | per asset |
| 6 | `CastsShadow` | shadow policy | true (opaque building) |
| 7 | `SupportsObjectId` | static-prop ID writer | true |

Bits 8-31 reserved=0. Lives in the manifest `capabilities{}` (named bools) + a packed
`uint32` the runtime can read later. **This slice: DESCRIPTIVE ONLY.** Capability is
authored/logged; it does NOT yet drive path selection (the renderer still uses
`isStaticEligible`/`isOverride`/`layerForPacket` as today). Authority flip = a later gated
slice. "Capability before fallback" = define + validate the data now, wire the decision later.

---

## Renderer handoff contract

The cooked asset plugs into the **unchanged** runtime via the five seams. The cook's job is
to produce artifacts that satisfy them; no engine code changes this slice.

Contract the cooked asset MUST satisfy:
1. **Geometry**: cooked glb imports cleanly via `ImportGeometryFromFile` under default env
   (axis0 + `1-v`), producing a `TG_TypeMultiShape` with bounds/pivot matching the stock prop
   (parity within WARN thresholds: footprint ratio ∈ [0.67,1.5], pivot XZ ≤0.25, Y ≤0.25).
2. **Materials/textures**: base names match `LoadOverrideRenderShapeTextures` probe
   (`tglPath/<size>/<name>`, `a_` iff alpha); KTX2 BC7 stored at the tier paths.
3. **Override entry**: `models.generated.json` resolves through the authoritative
   `ModelOverrideRegistry` (`type==model`, `renderOnly`, `fallback==stock`, `scale==1.0`,
   class `staticprop`, safe source) → registers via `registerMultiShape(shape, isOverride=true)`
   pre-finalize → admitted past `isStaticEligible` bdAnim guard → routed to layer 0 → drawn.
4. **No-animation**: building override carries no bdAnimData (`bldgTypeHasAnimations==false`)
   so the `overrideStatic` skip applies.

`[RENDER_PATH v1]` log (**[new]**, descriptive): per registered override, emit one line —
`[RENDER_PATH v1] key=staticprop:<name> path=override_multidraw layer=0 caps=0x<hex>
pools=skipped`. Closes the observability half of P2-3/P2-10; answers "why did this asset
take this path?" from a log line. No behavior change.

---

## Validation and round-trip tests

Reuse the workbench harness; add cook-specific gates. Each is a TDD anchor for its slice.

1. **Manifest schema** (`validate_manifest.py` extended): draft-07 validates the golden +
   cook-emitted manifest; slot==index, required albedo tier set, class∈{staticprop,tree},
   scale==1.0, safe relative paths. RED first on a hand-broken manifest.
2. **BLOCK rules** (`ValidateRecordRules`): reuse — scale!=1, unsafe source, unverified
   appearance, bad LOD order all refuse.
3. **Semantic WARN** (`ValidateSemantics` + `TglMeshLoader` oracle): cook bigbox/2civliving,
   compute footprint ratio + pivot delta vs stock TGL; assert within thresholds (gross
   axis/scale errors trip bounds-delta → catches the convention hazard).
4. **KTX2 verify** (`ktx2check`): every emitted tier file is stored BC7 (145/146), mips
   present, no supercompression.
5. **Registry round-trip** (`ExportBundle` gate / throwaway `ModelOverrideRegistry`):
   `models.generated.json` resolves back; appearanceName escaping survives; bundleId traversal
   refused.
6. **Render parity smoke** (extend `--smoke-workbench-bind` + a deploy run): cooked override
   (a) registers, (b) renders NON-invisible (not dropped at layer=-1), (c) correct orientation
   vs stock (no mirror/upside-down — the convention check), (d) tier1 smoke 0 GL errors / 0
   destroys. bigbox proves the staticprop-class path; 2civliving proves stock-parity.
7. **Round-trip determinism**: cook twice → byte-identical glb + manifest (modulo stamped
   `cookedUtc`/provenance) — reproducibility gate.

New cook smokes alongside the 6 workbench smokes: `--smoke-trackg-{cook,manifest,render}`.

---

## Staged task breakdown

Data before behavior. Each slice: RED test → impl → GREEN → tier1 5/5 + 0 GL errors gate.

- **R0 — convention reconciliation (recon-finish, no code):** confirm runtime importer
  (`assimp_importer.cpp` axis0 + `1-v`) is the authority; document the workbench
  `GlbMeshLoader` divergence; decide bake-into-glb vs record-in-manifest (recommend: bake
  orientation, record convention). Output: a 1-page convention note + the `convention{}`
  manifest field locked. **Gate: STOP if the staticprop-class render seam isn't actually
  wired for buildings** (verify a trivial bigbox override renders where trees do — agent
  recon says bldg path IS wired at `bdactor.cpp:344/2934`, but prove it before building cook).

- **G3a — manifest schema FIRST (data):** `tools/asset_cook/manifest.schema.json`
  (mc2-asset-manifest-v1, superset) + `validate_manifest.py` (extend the material-cook
  validator). Hand-author a GOLDEN `cooked/bigbox/manifest.json`. Test: golden validates;
  3 hand-broken variants fail. NO cook yet. **This is the contract — the real first win.**

- **G1 — offline GLB staging:** `tools/asset_cook/trackg_cook.py stage` — source glb →
  cooked glb (freeze convention; optional meshopt via vendored meshoptimizer lib). Emit
  `geometry{}` (bounds/pivot/counts via reusing `GlbMeshLoader`; parity vs `TglMeshLoader`
  oracle). Test: bigbox stages; bounds match a known fixture; convention recorded.

- **G2 — KTX2 material cook:** `trackg_cook.py textures` — discover materials (offline
  `DeriveMC2TextureName` port), cook albedo → BC7 tiers via `_ktx_cook()`, land in
  `data/tgl/<tier>/`. Emit `materials[]`. Test: tier files exist, `ktx2check` BC7-stored,
  name parity with resolver probe.

- **G3b — manifest assembly + projection:** `trackg_cook.py manifest` — combine
  geometry+materials+capability+provenance → `manifest.json`; project → `models.generated.json`;
  run `ExportBundle` round-trip gate. Test: registry resolves; schema validates; never writes
  central `models.json`.

- **G5 — capability + RENDER_PATH log (descriptive):** populate `capabilities{}` (packed
  uint32) in cook; add `[RENDER_PATH v1]` engine log line per registered override (read-only,
  no path change). Test: log emits expected `caps=0x..` for bigbox/2civliving; schema-grep
  `\[RENDER_PATH v[0-9]+\]` passes; tier1 unchanged.

- **G-E2E — first assets end-to-end:** Slice A `bigbox.glb` (plumbing, staticprop-class
  proof). Slice B `2civliving` (author the .glb from the stock .ase, real building,
  stock-parity render). Test: render parity smoke (visible, correct orientation, 0 GL errors).

Sequence: R0 → G3a → (G1 ∥ G2) → G3b → G5 → G-E2E. G3a is the gating deliverable; G1/G2 can
overlap; G5 + E2E last.

---

## Stop conditions

Hard stops — do not paper over, surface to the user:

1. **Staticprop-class render seam NOT wired** — if a trivial bigbox override does not render
   where a tree override does, the building injection path is broken. STOP; fix the seam as a
   separate slice before building any cook. (R0 verifies this first.)
2. **Convention mismatch** — cooked geometry renders mirrored / rotated / upside-down / wrong
   scale vs stock. The axis/UV convention is wrong. STOP; reconcile the runtime-importer vs
   workbench-loader divergence before generalizing past one asset.
3. **KTX2 not stored-BC7** — any tier file is Basis/UASTC-supercompressed; the runtime
   `KtxLoader` rejects it (silent missing texture). STOP; fix the cook to `transcode --target
   bc7` stored.
4. **Round-trip failure** — `models.generated.json` does not resolve through the authoritative
   `ModelOverrideRegistry`. The export gate already refuses; STOP, do not hand-edit around it.
5. **Central-manifest write** — the cook attempts to write/append `data/model_overrides/models.json`.
   FORBIDDEN. Cook emits bundle-local DRAFT only; promotion is a separate reviewed merge. STOP.
6. **Capability becomes path-authoritative this slice** — if MeshCapability starts driving
   renderer path selection (vs the existing `isOverride`/`isStaticEligible` gates), that is the
   later gated slice, not this one. STOP; keep capability descriptive (logged) this arc.
7. **Scope creep** — mech assets, LOD chains, impostor bake, in-game normal/ORM (shelved per
   asset-pipeline §6), meshlets/.cdag, a second importer/material-schema, gltfpack route, or a
   full editor. All out of scope. STOP/defer to a named follow-up.
8. **Runtime behavior change** — any engine render-path edit beyond the additive
   `[RENDER_PATH v1]` log. This slice is cook + contract only. STOP.

---

## Answers to the 10 framing questions (index)

1. Promote: `model_override_registry` (asis), `assimp_importer` material/`a_` logic + import
   (convention-frozen), `LoadOverrideRenderShapeTextures`, the 5 render seams, workbench
   loaders/validators/`ExportBundle`. → Reuse map.
2. Staging: bundle-local `data/model_overrides/cooked/<id>/{manifest.json, <id>.glb,
   models.generated.json}`; textures → shared `data/tgl/<tier>/`. → Offline staging format.
3. Canonical geometry = `MeshData`/`TG_TypeMultiShape` (expanded verts, GL space) under the
   FROZEN runtime-importer convention (axis0 + `1-v`). → Staging + R0.
4. Materials: offline `DeriveMC2TextureName` (diffuse slot → name; alphaMode/leaf → `a_` →
   alphaClass). → Material cook plan.
5. KTX2: `ktx.exe` `create --encode uastc` → `transcode --target bc7`, BC7 stored, tiers
   `{128..1024}`, mips on, never upscale; reuse `_ktx_cook()`. → Material cook plan.
6. `manifest.json` = mc2-asset-manifest-v1 superset (geometry+materials+capability+deps+provenance)
   projecting to `models.generated.json`. → manifest.json schema.
7. MeshCapability: HasLegacyMesh/HasCookedGlb/HasLodChain/HasMeshlets/HasImpostor/AlphaTest/
   CastsShadow/SupportsObjectId — descriptive this slice. → MeshCapability flags.
8. Renderer path: UNCHANGED runtime (`isOverride`/`isStaticEligible`/`layerForPacket` seams);
   capability LOGGED via `[RENDER_PATH v1]`, authority flip deferred. → Renderer handoff contract.
9. `models.json`: central authoritative, load-only, NEVER auto-written; cook emits bundle-local
   `models.generated.json` projection; promotion is a separate reviewed merge. → Staging + stop #5.
10. First asset: `bigbox.glb` (plumbing, staticprop-class proof) then `2civliving` (stock-parity
    building; author .glb from the .ase). → Staged task breakdown G-E2E.
```
