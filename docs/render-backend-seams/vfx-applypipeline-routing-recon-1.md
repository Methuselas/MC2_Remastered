# VFX-APPLYPIPELINE-ROUTING-RECON-1

**Type:** RECON (no build, no GL change). Scopes routing the VFX/particle
fixed-function state through `pipeline_binder::applyPipeline`, driven by the 6
descriptive VFX rows (now that BLENDMODE-ADDITIVE-VOCABULARY-1 made the additive
funcs precise). Authority = live draw-block calls + their lifetime.

**VERDICT: GO (all 3 live programs) — mechanical, provably behavior-neutral.**
The one real consideration is the *proof mechanism*: VFX spawn is nondeterministic,
so the byte-hash capture used for terrain/shadow **will not work** — the routing
build slice must gate on an oracle/perceptual A/B, not a golden hash.

Scratch evidence: `.claude/VFXROUTE-EDITMAP.md`.

## 1. Blend-selector lifetime (the routing shape)
Blend func is **re-set per-item** (blendMode varies per draw-group / record /
instance); depth + cull are set **once per flush, outside the loop**. So routing
= call `applyPipeline(getPipelineDesc(<item-blend-pipeline>))` **per item**,
replacing the per-item `glBlendFunc` 2-liner — NOT once per flush.

| Program | per-item blend site | live? | route id (blendMode 0 / 1) |
|---|---|---|---|
| particle_billboard | `gos_particle_bridge.cpp:1127-1131` | LIVE | VfxBillboardAlpha / VfxBillboardAdditive |
| tube_ribbon (deferred) | `gos_particle_bridge.cpp:767-768` | LIVE | VfxTubeAlpha / VfxTubeAdditive |
| tube_ribbon (immediate) | `gos_particle_bridge.cpp:527-528` | DEAD | (low priority) |
| vfx_mesh | `gos_vfx_mesh_bridge.cpp:311-312` | LIVE | VfxMeshAlpha / VfxMeshAdditive |

## 2. State lifetime / leak
All 4 paths already have a full save block + restore epilogue +
`InvalidateRenderStateCache`. `applyPipeline` touches the same state classes and
runs INSIDE that existing window → **no new wrapper, no new leak**. `polygonOffset`
isn't saved but is already-off (applyPipeline disabling it is benign).

## 3. Stays manual (NOT applyPipeline's job — keep verbatim)
Program bind (VFX `glProgramName==0` → applyPipeline SKIPs program); `glDrawBuffers(1,COLOR0)`
deferred-tube MRT override (`gos_particle_bridge.cpp:678-681` / restore `:797`);
sampler binds; VAO/SSBO/texture binds; ALL uniform uploads (`u_vfxIsAdditive`,
soft-particle depth, lit-particle, additive brightness, MVP). The flush-level
depth/cull set also stays (applyPipeline re-asserts identical values per item).

## 4. Diffs vs the descriptive rows
**NONE.** The bridges never set `depthMask TRUE`, never enable cull, always use
`GEQUAL`, never call `glFrontFace` (cull off → frontFace moot), and never enable
`GL_POLYGON_OFFSET_FILL` during VFX. The blend rows match the hand-set funcs
byte-for-byte. ⇒ `applyPipeline(VfxX)` produces exactly the hand-set FF state →
**provably no-op.** (Per-item applyPipeline re-issues depth/cull redundantly —
identical values, per-group not per-particle, negligible.)

## 5. Routing shape (build slice)
4 REPLACE sites (3 live + 1 dead): swap the per-item `glBlendFunc` 2-liner for
`applyPipeline(getPipelineDesc(blendMode==1 ? VfxXAdditive : VfxXAlpha), "VfxX...")`.
KEEP flush depth/cull, program bind, uniforms, drawBuffers, samplers. Mechanical,
behavior-neutral. (The `[PIPELINE_BIND]` trace + `MC2_PIPELINE_BIND_TRACE` gate
already exist — routing will emit the per-group VfxX bind lines.)

## 6. ★ Proof mechanism (the real caveat for the build slice)
mc2_24 is VFX-dense (combat), BUT particle **spawn is nondeterministic** (random
spin/emit) → a before/after **byte-hash capture FAILS even with no change**. The
terrain/shadow byte-identical gate does NOT transfer here. Options for the build
slice's visual gate:
- **MC2_VFX_ORACLE_TUBE_COVERAGE** occlusion-query A/B (existing oracle) + an
  A/B screenshot pair eyeballed for parity — the recon's recommendation.
- or a fixed-seed/fixed-clock particle path if one exists (investigate), to make
  a deterministic capture possible.
- The `[PIPELINE_BIND]` trace (correct VfxX selected per group) + zero GL errors +
  smoke PASS are necessary but not sufficient alone.

Because §4 proves the GL state is byte-identical to today, visual *risk* is low;
the work is in the *proof*, not the change.

## Verdict (per program)
- particle_billboard — **GO**
- tube_ribbon (deferred, LIVE) — **GO**
- vfx_mesh — **GO**
- tube_ribbon (immediate) — GO but DEAD path, low priority (route for completeness
  or skip + document).
None DEFER, none DO_NOT_MODEL (registration already settled that).

## Next slice (named — shape is stable)
**VFX-APPLYPIPELINE-ROUTING-1** — behavior-bearing. Route the 3 live VFX
per-item blend sites through `applyPipeline`; gate on `MC2_PIPELINE_BIND_TRACE`
(correct per-group pipeline) + no GL errors + smoke PASS + a VFX oracle-coverage /
A-B screenshot parity check (NOT byte-hash). On success, move VFX
DESCRIPTIVE_REGISTERED → VISUAL_PROVEN. (TerrainOverlay/Decal + WaterArmed remain
the lower-risk routing candidates and CAN byte-hash, unlike VFX.)
