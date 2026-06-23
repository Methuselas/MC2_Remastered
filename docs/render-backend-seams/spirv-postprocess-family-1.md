# SPIRV-POSTPROCESS-FAMILY-1

> Widens the SPIR-V pilot set over small color-output postprocess programs that
> share the already-proven `postprocess.vert`. Default-OFF (`MC2_SHADER_SPIRV=1`),
> program-atomic, keyed by define-set. **No material/static-prop code touched.**

## Shipped: 4 of 6 candidates
**SHIPPED** (byte-exact OFF/ON parity, no FBM): `ssao`, `fog_oob`, `edge_fog`, `hzb_reduce`.
**EXCLUDED**: `cloud`, `shoreline` — see finding below.

Each shipped member is **frag-only**: the program is `postprocess.vert` + `<name>.frag`
(`gos_postprocess.cpp:548/572/591/607`, `kShaderPrefix` = no macros). The vert reuses
the already-baked `postprocess|vert|` artifact; only the unique frag is baked + keyed
(`<name>|frag|`). All have a single `TexCoord` varying (links to postprocess.vert),
a single color output, **no SSBO/UBO/material bindings**, by-name samplers only.

Consumer allowlist is now table-driven (`kSpirvPilotStages` + `kSpirvPilotPrograms` in
`shader_builder.cpp`).

## Key finding — cloud + shoreline EXCLUDED (FBM float-codegen divergence)
Bisection (mc2_24 overview, OFF baseline `b11ff22a`):
```
cloud only ON      -> 998aac6d  *** DIFFERS ***
shoreline only ON  -> e4202ce5  *** DIFFERS ***
ssao/fog_oob/edge_fog/hzb_reduce only ON -> b11ff22a  parity-OK
```
`cloud.frag` and `shoreline.frag` use **FBM procedural noise** (`snoise`/`fbm`) + `smoothstep`
thresholds. The driver's GLSL compiler and glslang's SPIR-V codegen produce ULP-level
floating-point differences; in chaotic noise feeding a `smoothstep` edge, a ULP flip changes
visible pixels → the OFF/ON capture hash differs. **This is fundamental compiler divergence,
not a consumer bug** (no GL errors, no fallback; the 4 non-FBM members are byte-identical).
Byte-exact golden parity is therefore **not achievable** for noise-bearing postprocess shaders
with the current two-compiler model.

**Lesson (banked):** SPIR-V↔GLSL byte-exact output parity holds for numerically-stable shaders
(composite, mech, these 4) but NOT for numerically-chaotic ones (FBM + smoothstep). Shipping
cloud/shoreline as SPIR-V needs a **perceptual-tolerance visual gate** (not byte-exact) — future
work, out of scope here.

## Acceptance — met (AMD 7900 XTX)
| Gate | Result |
|---|---|
| 9 seam checks pass before/after | ✅ |
| package metadata updates deterministically | ✅ (`build_variants --check` rc=0) |
| reflection checker covers new artifacts | ✅ ssao/fog_oob/edge_fog/hzb_reduce |
| package checker rejects undeclared/stale | ✅ (no stray cloud/shoreline; planted tests earlier) |
| SPIR-V OFF smoke | ✅ mc2_10 PASS |
| SPIR-V ON smoke | ✅ mc2_10 PASS, 0 rebuild, 0 fallback |
| ≥2 deterministic golden comparisons | ✅ mc2_24 3/3 bookmarks byte-identical OFF vs ON (4-member set) |
| selected artifact corruption → atomic fallback | ✅ corrupt `ssao.frag` → ssao atomic GLSL rebuild |
| non-selected artifact corruption → no fallback | ✅ corrupt `mech.frag.objectid` (non-default) → no rebuild |
| postprocess + MechOpaque still pass | ✅ 0 rebuild under ON; goldens byte-identical |
| no material/static-prop code touched | ✅ |
| no static-prop depth build | ✅ |

## StaticPropDepth revisit condition (unchanged)
Revisit only after: a real depth-parity capture/checker exists, OR StaticPropOpaque is
ready (prove both as a family), OR the binding-5 material path is validated under SPIR-V
elsewhere.

## Exclusions honored
No material/static-prop code, no StaticPropDepth build, no Vulkan, no GLSL removal.
Foreign WIP untouched.

## Next candidates
- `cloud`/`shoreline` via a perceptual-tolerance gate (FBM noise).
- `ssao_apply` (the 7th postprocess member, same shape) if desired.
- A perceptual visual-diff harness (SSIM/threshold) to unblock noise shaders broadly.
