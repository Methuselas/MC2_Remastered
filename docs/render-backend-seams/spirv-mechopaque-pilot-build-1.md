# SPIRV-MECHOPAQUE-PILOT-BUILD-1

> Second SPIR-V consumer pilot and the first **registered/keyed** family
> (PipelineId::MechOpaque), built on SPIRV-KEYED-VARIANT-CONSUMER-1. Default-OFF
> (`MC2_SHADER_SPIRV=1`), program-atomic. Default runtime selects the both-on
> variant (both macros are default-ON).

## What shipped
- **pilots.json:** `mech` program (`mech.vert`+`mech.frag`) with all 4 define
  combos: `{}`, `{MC2_OBJECT_ID_BUFFER=1}`, `{MC2_USE_VIEW_UNIFORMS=1}`,
  `{both}` → 8 baked `.spv` + sidecars + index entries.
- **Consumer allowlist:** `spirvPilotStage` + `spirvCompositePilotProgram` accept
  `mech.{vert,frag}`. Suffix hardened to `/mech.vert` / `/mech.frag` so
  `shadow_mech` (`shadow_mech.vert` + `shadow_instanced.frag`) NEVER matches —
  it stays GLSL (both the suffix and the program-pair gate exclude it).
- Per-stage variant keyed by the complete define-set (the keyed consumer); stock
  runtime (both macros ON) resolves the `mech|stage|MC2_OBJECT_ID_BUFFER=1;MC2_USE_VIEW_UNIFORMS=1` key.

## Acceptance — all met (AMD Radeon RX 7900 XTX, mc2_24)
| Gate | Result |
|---|---|
| seam checks (7) | ✅ PASS |
| from-scratch build | ✅ green |
| SPIR-V OFF smoke | ✅ PASS |
| SPIR-V ON smoke | ✅ PASS, 0 mech rebuild, 0 pp fail |
| **exact artifact selection** | ✅ corrupt the both-on artifact → mech atomic rebuild; corrupt a NON-selected combo (objectid-only) → **no** rebuild |
| missing one mech stage | ✅ delete both-on vert → atomic GLSL rebuild, mech not broken |
| bad one mech stage | ✅ corrupt → atomic rebuild (fatal gate available) |
| incomplete/extra define key | ✅ tampered index key (`+MC2_EXTRA=1`) → key miss → atomic rebuild |
| postprocess pilot still works | ✅ 0 pp fail under ON |
| by-name uniform/sampler watchpoint | ✅ no `GL_INVALID*`, `draw_packet gl_errors=0` (~25 uniforms + 5 samplers resolve under `glSpecializeShader`) |
| MechOpaque visual frozen frame | ✅ mc2_24 3/3 bookmarks OFF-vs-OFF STABLE **and** OFF-vs-ON **byte-identical** (OFF=GLSL mech, ON=SPIR-V mech → parity) |

The corrupt-selected-vs-corrupt-non-selected control is conclusive proof the
runtime loads exactly the both-on keyed artifact. Visual: OFF (GLSL mech) vs ON
(SPIR-V mech) byte-identical across all 3 deterministic bookmarks, with an
OFF-vs-OFF determinism control passing first (per the shadow-nondeterminism
lesson).

## Watchpoints honored
- **By-name uniforms/samplers:** clean (no GL errors) — confirmed.
- **Variant completeness:** key miss (extra/missing define) → fallback — proven.
- **Program atomicity:** no SPIR-V vert + GLSL frag — preserved; a stage miss
  rebuilds the whole program GLSL.
- **Shadow stays GLSL:** `shadow_mech` excluded by `/mech.frag` suffix + pair gate.
- **Imported BT mech:** out of scope — stock MechOpaque only.

## Exclusions honored
No shadow_mech, no spec constants, no shader-system rewrite, no GLSL removal, no
Vulkan, no PSO cache, no imported-mech lane. Foreign WIP untouched.

## Next (not started — gate MechOpaque first: DONE)
`SPIRV-STATICPROP-DEPTH-RECON-1` · `SPIRV-POSTPROCESS-FAMILY-1` ·
`SPIRV-MECHOPAQUE-PIPELINEKEY-INTEGRATION-1`.
