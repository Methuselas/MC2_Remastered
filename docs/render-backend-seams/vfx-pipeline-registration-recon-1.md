# VFX-PIPELINE-REGISTRATION-RECON-1

**Type:** RECON (no build, no registration, no GL). Maps the VFX/particle finite
family from the **live draw blocks** (not comments/prior prose). Classifies and
recommends GO/DEFER/DO_NOT_MODEL.

**VERDICT: GO for DESCRIPTIVE registration (6 rows, no new field needed) —
ROUTING DEFERRED (needs an extended blend vocabulary first).** The family is
finite, stable, and enumerable; nothing is DO_NOT_MODEL.

Scratch evidence: `.claude/VFXRECON-particles.md`, `.claude/VFXRECON-mesh-and-sweep.md`.

## Surface (verified against actual gl* calls)

**3 GL programs**, each with its own draw block; no other VFX draw path exists
(`gos_cardcloud_sim.cpp` is compute-only and feeds tubes; debug_renderer is out
of scope; no separate muzzle/beam/contrail renderer):

| Program | File / draw | shaders | vertex source |
|---|---|---|---|
| particle_billboard | `gos_particle_bridge.cpp` use:933 draw:1061/1141 | particle_billboard.{vert,frag} | gl_VertexID (SSBO slot 14) |
| tube_ribbon | `gos_particle_bridge.cpp` use:504/645 draw:533/771 | tube_ribbon.{vert,frag} | SSBO 14/15/16 + IBO |
| vfx_mesh | `gos_vfx_mesh_bridge.cpp` use:262 draw:315 | vfx_mesh.{vert,frag} | fixed VAO (pos3+uv2, u16 EBO) |

## Invariant state (CONSTANT across every VFX draw — verified)
`depthTest ON · depthFunc GEQUAL (reverse-Z) · depthWrite OFF (glDepthMask FALSE)
· cull None (GL_CULL_FACE disabled — double-sided) · frontFace Ccw · no polygon
offset · blendEquation FUNC_ADD (never set)`. Depth/cull never branch.

## The only variable: blend (finite 2-way selector)
- **Selector source:** per-record/per-instance `int blendMode` ∈ {0,1} — a
  hardcoded 2-way if/else, NOT an open enum. (`gos_particle_bridge.cpp` per-group;
  `gos_vfx_mesh_bridge.cpp:311-312` per-instance `inst.blendMode`.) FINITE+ENUMERABLE.
- **★ The trap — additive factors DIVERGE across programs:**

| Program | alpha | additive |
|---|---|---|
| particle_billboard | `SRC_ALPHA / ONE_MINUS_SRC_ALPHA` (:1130) | **`SRC_ALPHA / ONE`** (:1128) |
| tube_ribbon | `SRC_ALPHA / ONE_MINUS_SRC_ALPHA` (:768) | **`ONE / ONE`** (:767/527) |
| vfx_mesh | `SRC_ALPHA / ONE_MINUS_SRC_ALPHA` (:276) | **`SRC_ALPHA / ONE`** (:311) |

The `BlendMode` enum's single `Additive` cannot distinguish `ONE/ONE` (tube) from
`SRC_ALPHA/ONE` (billboard/mesh). **`applyPipeline` currently maps
`Additive → glBlendFunc(GL_ONE, GL_ONE)`** — which matches *only* tube. Routing
billboard/mesh through it would change `SRC_ALPHA/ONE → ONE/ONE` = a **real
behavior change, not a no-op.**

## Color attachments (also varies — pass-level, not PSO)
billboard + immediate-tube write the scene MRT (color0/1/2); `vfx_mesh` and the
**deferred-tube** path force `glDrawBuffers(1, COLOR0)` (`gos_particle_bridge.cpp:680`,
AMD R32UI+blend bug) → color0 only. This is a pass-level MRT override (the
prior recon classed MRT as NEEDS_PASS_REG_FIRST), not a per-PSO blend axis.

## Candidate rows (6 — all FINITE_STABLE, none DATA_DRIVEN)

| # | id | blend (BlendMode / actual factors) | depthTest | depthWrite | depthFunc | cull | color |
|---|---|---|---|---|---|---|---|
| 1 | VfxBillboardAlpha | AlphaBlend / SRC_ALPHA,1-SRC_ALPHA | on | off | GEQUAL | None | 0/1/2 |
| 2 | VfxBillboardAdditive | Additive / **SRC_ALPHA,ONE** | on | off | GEQUAL | None | 0/1/2 |
| 3 | VfxTubeAlpha | AlphaBlend / SRC_ALPHA,1-SRC_ALPHA | on | off | GEQUAL | None | 0/1/2 |
| 4 | VfxTubeAdditive | Additive / **ONE,ONE** | on | off | GEQUAL | None | 0/1/2 |
| 5 | VfxMeshAlpha | AlphaBlend / SRC_ALPHA,1-SRC_ALPHA | on | off | GEQUAL | None | 0 |
| 6 | VfxMeshAdditive | Additive / **SRC_ALPHA,ONE** | on | off | GEQUAL | None | 0 |

## Classification
- **GO — DESCRIPTIVE registration (6 rows), no new field.** Same pattern as
  TerrainDecal/WaterArmed: the schema `blendState` carries the exact
  `srcFactor/dstFactor/equation`, so the additive divergence is captured
  losslessly even though `PipelineDesc.blend` is the coarse `Additive`. Rows are
  stable + verified → registration is safe and useful now.
- **DEFER — applyPipeline routing.** NOT a provably-no-op candidate: routing
  billboard/mesh additive through the current `Additive→ONE/ONE` would corrupt
  their `SRC_ALPHA/ONE`. Routing needs an extended blend vocabulary first.
- **DO_NOT_MODEL — none.** The whole VFX surface is finite/stable.

## Prerequisite for routing (separate, later)
**BLENDMODE-ADDITIVE-VOCABULARY-1** — distinguish `SRC_ALPHA/ONE` from `ONE/ONE`
before any VFX routing. Options: a new `BlendMode` value (e.g. `AdditivePremul`
for ONE/ONE vs `Additive` for SRC_ALPHA/ONE), or explicit src/dst factor fields.
This is the same shape as the colorWrite/raster promotions: extend vocabulary →
checker → then route with a pixel gate. (Until then, descriptive rows are exact
because the schema carries the factors.)

## Next slice (rows are stable → named)
**VFX-PIPELINE-REGISTRATION-1** — register the 6 descriptive rows above
(`PipelineId` VfxBillboardAlpha/Additive, VfxTubeAlpha/Additive, VfxMeshAlpha/Additive;
schema entries carrying exact src/dst/equation; ledger VFX → DESCRIPTIVE_REGISTERED).
No new field, no routing, no smoke (descriptive-only). Routing waits on
BLENDMODE-ADDITIVE-VOCABULARY-1.

## Exclusions held
No VFX registration (this is recon) · no particle rewrite · no shader/blend
changes · no applyPipeline routing · no SPIR-V · no UI/HUD.
