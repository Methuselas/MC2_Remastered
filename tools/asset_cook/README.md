# tools/asset_cook — Track G offline static-prop cook

The cooked-asset contract + validator. Slice `TRACKG-OFFLINE-GLB-COOK-MANIFEST-1`.
Plan: `docs/superpowers/plans/2026-06-04-trackg-offline-glb-cook-manifest-plan.md`.

**G3a (this slice — DONE): the manifest contract, data before behavior.** No cook yet.

## Files
- `asset_manifest.schema.json` — `mc2-asset-manifest-v1` (draft-07). Superset of the
  override `models.json` (identity/geometry) and `material_manifest` (textures). Geometry +
  materials + capability + provenance for one cooked static-prop/tree override asset.
- `validate_asset_manifest.py` — schema + cross-field coherence the schema can't express:
  `slot==index`, `replaces == '<class>:<appearanceName>'`, **lowercase `class`** (registry
  canonical), `alphaClass ↔ flags.alpha_test ↔ 'a_' prefix` agreement, `capabilities.alphaTest
  == any alpha material`, `capabilities.hasLodChain == lods non-empty`, bounds ordering, LOD
  ascension, `deps.stockFallback == appearanceName`. `--check-files` verifies referenced
  glb/ktx2 exist (off by default; cooked artifacts don't exist until G1/G2).
- `tests/golden/bigbox/manifest.json` — golden (bigbox→`staticprop:hangar`, the R0-proven map).
- `tests/broken/*.json` — 5 fixtures, one load-bearing invariant each.
- `tests/run_tests.py` — golden must PASS, all 5 broken must FAIL **for cause**.

## Validate / test
```
py -3 tools/asset_cook/validate_asset_manifest.py <manifest.json>
py -3 tools/asset_cook/tests/run_tests.py
```

## Contract invariants (load-bearing)
- **Class spelling = lowercase `staticprop`/`tree`** (registry `normalizeKey` lowercases;
  compare `model_override_registry.cpp:77`). Input case-insensitive at runtime; manifest emits
  lowercase. (Schema `enum` + coherence both enforce.)
- **`scale == 1.0`** (bake scale into the glb).
- **Convention = audit-only**: cooked glb baked to the runtime importer's default env
  (`assimp_importer.cpp` axis0 + `1-v`); no `MC2_GLTF_*` in the contract.
- **`albedo_ktx2` = tier map** → MaterialGpu `albedoTex` (texture-array layer at runtime).
- **`a_` prefix iff alpha** — the resolver naming convention; mismatch leaves the runtime
  texture handle unresolved.

## Next (not yet built)
G1 stage (geometry section + cooked glb) · G2 KTX2 cook (materials section) · G3b assemble +
`models.generated.json` projection + registry round-trip · G5 capability + `[RENDER_PATH v1]`.

## Follow-up
Registry-side C++ unit test asserting mixed-case class input resolves (the case-insensitive
accept) — the manifest side is enforced here; the runtime accept is proven by R0's registry
grep but not yet a doctest.
