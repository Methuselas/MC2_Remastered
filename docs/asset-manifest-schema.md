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

## For the future asset-pipeline lane
Extend HERE rather than inventing a parallel format: add fields (LOD geometry
stats, KTX2 sidecar refs, meshopt flags) to the schema + validator + fixture,
and wire `validate_asset_manifest.py` into CI alongside `check-toolchain-bom.py`.
Keep file-existence / bake checks OUT of this validator — it is the shape gate.
