# TERRAIN-OVERLAY-DECAL-APPLYPIPELINE-ROUTING-1

**Status:** SHIPPED as **ROUTED_BY_APPLYPIPELINE** + `proofStatus:
pass_not_exercised_in_smoke` for both TerrainOverlay and TerrainDecal. Routes
their fixed-function state through `applyPipeline`. Provably byte-identical state;
no GL behavior change.

## What changed (`gameos_graphics.cpp`)
3 hand-set FF-state blocks replaced by `applyPipeline(getPipelineDesc(<row>))`:
- `drawTerrainOverlays` (`:9732`) → `applyPipeline(TerrainOverlay)`
- `drawDecalStaticBatch` (`:9850`, static cement bake) → `applyPipeline(TerrainOverlay)`
- `drawDecals` (`:9924`) → `applyPipeline(TerrainDecal)`

`glProgramName==0` → applyPipeline SKIPs program, so the manual `overlayProg_` /
`decalProg_` binds, VAO, uniforms, FBO, textures all stay. (Includes were already
present from the shadow-routing slice.)

## State-equivalence (provably no-op)
| Pass | old hand-set | applyPipeline(row) | diff |
|---|---|---|---|
| TerrainOverlay | depthTest on, depthMask TRUE, GEQUAL, BLEND off, CULL off | same + frontFace CCW (global already CCW), glBlendFunc(ONE,ZERO) (blend off → irrelevant), POLYGON_OFFSET off (already off) | no-op |
| TerrainDecal | depthTest on, depthMask FALSE, GEQUAL, BLEND on SRC_ALPHA/1-SRC_ALPHA, CULL off | same + frontFace/offset no-ops | no-op |

## Proof — why `pass_not_exercised_in_smoke`, not VISUAL_PROVEN
- ✅ build green; ✅ tier1 5/5 PASS; ✅ no GL errors; ✅ trace mechanism proven
  live (Shadow* `[PIPELINE_BIND]` rows fire); ✅ mc2_24 deterministic capture ==
  historical baseline `b11ff22a / e0fb9cc8 / c6df715e` (no regression on the
  common path); ✅ both rows confirmed as `applyPipeline` routed-evidence.
- ❌ **TerrainOverlay/TerrainDecal drew in ZERO tier1 missions** — they are
  content-dependent (cement-perimeter overlay tiles; bomb-crater / mech-footprint
  decals from battle damage) and tier1's idle maps contain neither. So the
  per-pass `[PIPELINE_BIND]` trace + an in-frame A/B could not be obtained here.

These passes ARE deterministic (byte-hashable once exercised), unlike VFX — so
the pending reason is `pass_not_exercised_in_smoke`, not `nondeterministic_*`.
Upgrade to VISUAL_PROVEN on a cement-overlay / battle-damage capture.

## Ledger / checker
- Ledger: TerrainOverlay + TerrainDecal `DESCRIPTIVE_REGISTERED →
  ROUTED_BY_APPLYPIPELINE` + `proofStatus: pass_not_exercised_in_smoke` + proofNote
  + per-pass visual-gate `next`.
- `check-pass-coverage.py`: added `pass_not_exercised_in_smoke` to the proof
  vocabulary; split proofStatus into LANDED vs PENDING sets and the
  "no VISUAL_PROVEN/SPIRV_ELIGIBLE while pending" guard now covers BOTH pending
  reasons.

## Verification
Build green; `pipeline_key`/`pipeline_desc`/`pass_coverage` PASS; adversarial
`VISUAL_PROVEN`-while-`pass_not_exercised_in_smoke` → FAIL (restored); no GL
errors; foreign WIP untouched.

## Exclusions held
Routed only the 3 overlay/decal FF-state blocks; no ownership moved for
program/VAO/FBO/uniforms/textures; no shader/blend change; no SPIR-V.

## Next
Land the content-mission A/B (cement overlay; battle-damage decals) +
`[PIPELINE_BIND]` trace → upgrade both to VISUAL_PROVEN. (Generalizes the
"deterministic-but-content-dependent pass" proof, complementing VFX's
"nondeterministic pass" pending case.)
