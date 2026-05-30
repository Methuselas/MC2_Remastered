# Asset manifest schema (scaffold)

`ASSET-MANIFEST-SCHEMA-SCAFFOLD-1` (VALIDATION-SCAFFOLD-PREFLIGHT-1).

A minimal manifest schema + validator FOUNDATION for the future asset-pipeline
probe lane (`TRACKG-ASSET-PIPELINE-PROBE-OPUS-1`). This is a guardrail, not the
pipeline: it validates manifest SHAPE only — no asset import, no KTX2 bake, no
file-existence checks (asset binaries are not committed).

## Validator
`tools/validate_asset_manifest.py <manifest.json>` — exit 0 valid / 1 invalid.
(`tools/` is gitignored at repo root; the file is force-added like the other
committed `tools/` scripts.)

## Fixture
`tests/fixtures/assets/minimal_asset_manifest.json` — the canonical minimal
valid manifest; round-trips through the validator (exit 0).

## Fields
| field | type | notes |
|---|---|---|
| `assetId` | string | stable identity, non-empty |
| `source` | string | source asset path (relative) |
| `kind` or `type` | string | e.g. `mesh` / `prop` / `mech` |
| `materials` | array | may be empty |
| `capabilities` | object | the six booleans below (all required) |
| `lods` | array | empty array = explicit "no LODs" |
| `textureRefs` | array | empty array = explicit "no textures" |

### Capabilities (vocabulary reconciled with the renderer)
Names match `RenderCore/RenderObjectDesc.h ArchetypeFlags` where they overlap so
the manifest vocabulary stays consistent with the engine:

| capability | engine mapping |
|---|---|
| `hasNormals` | vertex attribute — validator-only, no engine flag |
| `hasTangents` | vertex attribute — validator-only, no engine flag |
| `hasLods` | `ArchetypeFlags.hasClusterLod` |
| `hasImpostor` | `ArchetypeFlags.usesImpostor` |
| `castsShadow` | `ArchetypeFlags.castsShadow` |
| `supportsObjectId` | `RenderObjectDesc.gameObjectId` / object-ID buffer |

## Optional sections (`ASSET-MANIFEST-0-EXTEND`)
These are validated for SHAPE only when present; a manifest without them stays
valid (the minimal fixture above still round-trips). Worked example:
`tests/fixtures/assets/extended_asset_manifest.json`.

| field | type | notes |
|---|---|---|
| `geometry` | object | ASSIMP-IMPORTER-PHASE-0 output contract (see below) |
| `lods[]` items | object | may be `{level, vertexCount, triangleCount, error}` (MESHOPT LOD-stat contract); plain/opaque entries still allowed |
| `textureRefs[]` items | object | `slot` + `path` required; `slot` ∈ {`albedo`,`normal`,`orm`,`emissive`,`mask`}; optional cooked fields `format`/`vkFormat`/`mips`/`dims` |
| `provenance` | object | `{tool, toolVersion, generatedAt, sourceHash}` (all string) |
| `generatedOutputs` | array | `[{path, kind}]` — pointers to regeneratable artifacts (live under ignored `out/`) |

### `geometry` (ASSIMP-IMPORTER-PHASE-0 output contract)
The shape a future offline importer probe writes per imported source mesh:

| field | type |
|---|---|
| `meshCount` / `vertexCount` / `indexCount` / `materialSlotCount` | integer |
| `hasNormals` / `hasTangents` | boolean |
| `bounds` | `{min:[x,y,z], max:[x,y,z], radius}` |

## For the future asset-pipeline lane
Extend HERE rather than inventing a parallel format: add fields to the schema +
validator + fixture, and wire `validate_asset_manifest.py` into CI alongside
`check-toolchain-bom.py`. Keep file-existence / bake checks OUT of this
validator — it is the shape gate. Material authoring invariants (colorSpace
conventions, alphaMode, PBR ranges, normal→tangent) are added by
`MATERIAL-AUTHORING-VALIDATION-1`; cooked-texture provenance by
`KTX2-BAKE-PROBE-1`.
