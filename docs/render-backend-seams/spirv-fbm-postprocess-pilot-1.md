# SPIRV-FBM-POSTPROCESS-PILOT-1

> Brings `cloud` + `shoreline` back as SPIR-V pilots under the **perceptual** gate
> (VISUAL-DIFF-PERCEPTUAL-HARNESS-1) — the FBM/noise shaders that byte-exact
> correctly rejected. Default-OFF (`MC2_SHADER_SPIRV=1`), program-atomic,
> table-driven allowlist. **No shader math changed to chase byte-exact.**

## What shipped
- `cloud` + `shoreline` re-added to the consumer allowlist (`kSpirvPilotStages` /
  `kSpirvPilotPrograms`) + `pilots.json` (frag-only; program = `postprocess.vert` +
  `<name>.frag`; vert reuses the baked `postprocess|vert|` artifact).
- Per-family **tuned** perceptual policies in `visual-tolerance-policy.json`
  (allowlist `cloud → perceptual_cloud`, `shoreline → perceptual_shoreline`).

## Thresholds — tuned from REAL captures (tight, not hand-waved)
Measured OFF (GLSL) vs ON (SPIR-V) per family, isolated, across **3 mc2_24 cameras**
(overview / highangle / ridge):

| family | worst mean_abs | worst ssim | max | → policy |
|---|---|---|---|---|
| cloud | 3.24 | 0.9957 | 18 | `mean≤5.0, ssim≥0.99, max≤120` |
| shoreline | 0.62 | 0.9895 | 73* | `mean≤2.0, ssim≥0.985, max≤120` |

\* shoreline's high `max` is foam-edge highlight flips — why the gate is `mean`+`ssim`,
not `max` (max is a generous backstop only). Thresholds sit just above the real worst
(≈1.5× mean headroom for cloud; tight absolute for shoreline) so a real regression
can't hide: a planted large regression (`mean≈45`) and a uniform +10 tint (`mean=10`)
both FAIL. **The gate is as tight as the real noise drift allows.**

## Acceptance — all met (AMD 7900 XTX)
| Gate | Result |
|---|---|
| 9 seam checks | ✅ |
| visual_compare self-test | ✅ |
| package metadata deterministic | ✅ (rebaked, --check clean) |
| OFF / ON smoke (mc2_10) | ✅ PASS / PASS |
| no GL errors | ✅ 0 GL_INVALID_* |
| no fallback | ✅ 0 rebuild / 0 no-artifact under ON |
| exact artifact selection | ✅ corrupt `cloud.frag` → cloud atomic GLSL rebuild |
| selected corruption → fallback | ✅ |
| non-selected corruption → ignored | ✅ corrupt `mech.frag.objectid` → no rebuild |
| cloud diff PASS under cloud / FAIL byte-exact | ✅ |
| shoreline diff PASS under shoreline / FAIL byte-exact | ✅ |
| large planted regression → FAIL perceptual (+diff) | ✅ |
| composite / MechOpaque / 4 PP pilots still pass | ✅ 0 rebuild under ON |

## SPIR-V coverage now
composite · MechOpaque(×4) · ssao · fog_oob · edge_fog · hzb_reduce (byte-exact) ·
**cloud · shoreline (perceptual)** — all default-OFF, program-atomic, keyed; governed
by artifacts/reflection/package checkers + the perceptual harness.

## Exclusions honored
No shader math change, no weakening of global/default gates (byte_exact stays default;
perceptual is allowlisted to cloud/shoreline only), no Vulkan, no relink beyond this
pilot. Foreign WIP untouched.

## Next candidates
- A non-mc2_24 mission camera for cloud/shoreline (broaden the perceptual sample).
- `ssao_apply` (deterministic 7th PP member).
- `SPIRV-MECHOPAQUE-PIPELINEKEY-INTEGRATION-1`, or StaticProp family once a depth-parity gate exists.
