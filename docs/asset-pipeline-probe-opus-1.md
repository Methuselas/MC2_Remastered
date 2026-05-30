# Asset-pipeline probe opus — summary

`TRACKG-ASSET-PIPELINE-PROBE-OPUS-1` (slice `ASSET-PIPELINE-PROBE-DOC-1`).

Reference doc. Branch: `claude/asset-pipeline-probe-opus-1`.

## 1. Purpose

This opus turned the asset-pipeline validation *scaffold* into a concrete,
validated, **offline** probe pipeline — with **zero runtime rendering changes**.
Everything here runs as a deterministic, machine-independent gate (Python +
repo-relative paths). No asset import, no renderer mutation, no broad CMake
wiring landed.

## 2. What shipped

| Slice | Commit | Summary |
|---|---|---|
| ASSET-MANIFEST-0-EXTEND | `f5e9e71a` | Optional manifest sections, backward compatible |
| MATERIAL-AUTHORING-VALIDATION-1 | `d6948f6b` | Material authoring invariants + colorSpace conventions |
| KTX2-BAKE-PROBE-1 | `3ef71573` | Deterministic offline KTX2 bake+inspect probe |

### ASSET-MANIFEST-0-EXTEND (`f5e9e71a`)

Added optional, shape-only manifest sections to `tools/validate_asset_manifest.py`.
A manifest without them stays valid (the minimal fixture still round-trips).

- `geometry` — the ASSIMP-IMPORTER-PHASE-0 output contract
  (`meshCount`/`vertexCount`/`indexCount`/`materialSlotCount` ints,
  `hasNormals`/`hasTangents` bools, `bounds{min[3],max[3],radius}`).
- `lods[]` items — LOD-stat shape `{level, vertexCount, triangleCount, error?}`
  (plain/opaque entries still allowed).
- `textureRefs[]` items — cooked fields `format`/`vkFormat`/`mips`/`dims`.
- `provenance` — `{tool, toolVersion, generatedAt, sourceHash}`.
- `generatedOutputs` — `[{path, kind}]` pointers to regeneratable artifacts.

Fixture: `tests/fixtures/assets/extended_asset_manifest.json`.

### MATERIAL-AUTHORING-VALIDATION-1 (`d6948f6b`)

Material/texture authoring invariants:

- `materials[].name` / `.shader` required non-empty strings.
- `alphaMode` ∈ {`opaque`, `alphaTest`, `blend`}; `alphaTestThreshold` only valid
  with `alphaMode='alphaTest'`, number in [0,1].
- `doubleSided` boolean.
- `pbr.baseColorFactor` / `metallicFactor` / `roughnessFactor` in [0,1].
- Slot → colorSpace convention: `albedo`/`emissive` = `srgb`;
  `normal`/`orm`/`mask` = `linear`.
- Cross-check: any `normal` slot ⇒ `capabilities.hasTangents = true`.

The validator gained `--expect-fail` (invert: exit 0 only if invalid) to drive
negative fixtures. Gate: `scripts/check-asset-manifests.py` (3 valid + 3 invalid
fixtures). Pass fixture `material_validation_pass.json`; negatives under
`tests/fixtures/assets/invalid/` (`material_fail_alphamode`,
`material_fail_colorspace`, `material_fail_normal_no_tangents`).

### KTX2-BAKE-PROBE-1 (`3ef71573`)

Deterministic offline bake+inspect probe `tools/asset_probe/ktx2_probe.py`:

1. Generates a fixed 8×8 RGBA source (no randomness) via Pillow.
2. Drives the existing `tools/mc2texcook/mc2texcook.py` → `.ktx2` for the
   `albedo` and `normal` presets (subprocess, `sys.executable`).
3. Parses the KTX2 header with a self-contained struct reader.
4. Asserts `vkFormat` / `mips` (levelCount) / `dims`; nonzero exit on mismatch.
5. Emits a manifest-ready `textureRef` provenance block to the summary.

CI wrapper `scripts/check-ktx2-probe.py` skips gracefully (exit 0) when Pillow is
unavailable. Artifacts land under gitignored `out/asset-pipeline-probe/` and are
**not** committed.

## 3. Toolchain truth table

From `docs/toolchain-bom.md` + `scripts/check-toolchain-bom.py`.

| Tool | Vendored | Build-integrated | Callable now | Status |
|---|---|---|---|---|
| Assimp importer | yes (`3rdparty/assimp/`) | yes — linked into mclib (`ENABLE_ASSIMP_IMPORTER=ON`) | no — see note | REQUIRED_NOW |
| shader_reflect | n/a (Python) | active CI gate + goldens | yes | REQUIRED_NOW |
| mc2texcook | n/a (Python) | KTX2 producer | yes | REQUIRED_NOW |
| KTX2 runtime loader | n/a | `KtxLoader.cpp` / sidecar loader | runtime (Phase-0 RGBA8) | REQUIRED_NOW |
| validate_asset_manifest | n/a (Python) | shape gate | yes | REQUIRED_NOW |
| meshoptimizer | yes (`3rdparty/meshoptimizer/`) | no — `ENABLE_CDAG_COOKER` declared, no `add_subdirectory` | no | FUTURE |
| gltfpack | no (dead code inside meshoptimizer) | no | no | FUTURE |

Key facts:

- **Assimp** is vendored and *linked* into mclib (`ENABLE_ASSIMP_IMPORTER=ON`,
  default ON), but `ImportGeometryFromFile()` is a `return -1;` **stub**. The real
  body lives on branch `claude/assimp-testing`; wiring it touches runtime
  `mech3d.cpp`.
- **mc2texcook.py** is a complete pure-Python KTX2 writer — REQUIRED_NOW,
  callable today.
- **KtxLoader.cpp** runtime loader exists (Phase-0 RGBA8).
- **meshoptimizer** is vendored but **unwired** (`ENABLE_CDAG_COOKER` option
  declared, no `add_subdirectory`).
- **gltfpack** is dead code inside vendored meshoptimizer; not wired.

## 4. Commands (offline gates)

```sh
python scripts/check-toolchain-bom.py
python tools/validate_asset_manifest.py tests/fixtures/assets/minimal_asset_manifest.json
python scripts/check-asset-manifests.py
python scripts/check-ktx2-probe.py
sh scripts/check-contracts.sh
python tools/asset_probe/ktx2_probe.py
```

## 5. Generated-output locations

- Generated artifacts (`.png`, `.ktx2`, JSON summaries) → `out/asset-pipeline-probe/`
  — **gitignored, regeneratable, never committed**.
- Committed = `tools/` + `scripts/` + `tests/fixtures/` + `docs/` only.
- Note: `tools/` and `tests/fixtures/` are gitignored at repo root, so committed
  files there are **force-added** (`git add -f`).

## 6. Deferred

Blocked by this opus's no-runtime / no-broad-CMake constraints.

| Deferred slice | Why blocked | Shipped instead | Concrete next step |
|---|---|---|---|
| ASSIMP-IMPORTER-PHASE-0 (running import) | importer is a `return -1;` stub; real body on `claude/assimp-testing`; wiring is runtime (`mech3d.cpp` `[Import] Source=` hook) | the geometry-summary manifest **contract** (`geometry` section) | ASSIMP-MECH-IMPORT-1: cherry-pick the 8-commit sequence + a tiny `.glb` fixture, runtime-gated |
| MESHOPT-LOD-PROBE-1 (running probe) | meshoptimizer unwired; needs CMake `add_subdirectory` + a new isolated `data_tools` host tool (never linked into mc2) + a C++ compile cycle | the `lods[]` LOD-stat manifest fields + `capabilities.hasLods` | MESHOPT-CLUSTERLOD-PROBE-1: wire the host tool (see `docs/superpowers/specs/2026-05-19-static-prop-cluster-lod-poc-design.md`) |

## 7. Next recommended slices (ordered)

1. **ASSIMP-MECH-IMPORT-1** — cherry-pick the importer body + `.glb` fixture,
   runtime-gated.
2. **MESHOPT-CLUSTERLOD-PROBE-1** — wire the isolated host tool for LOD stats.
3. **KTX2-RUNTIME-LOAD-1** — full mip-chain + Basis transcoding in `KtxLoader`.
4. Wire all four offline gates into CI alongside `check-toolchain-bom.py`.
