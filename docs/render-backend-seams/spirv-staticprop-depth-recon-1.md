# SPIRV-STATICPROP-DEPTH-RECON-1

**Status:** RECON COMPLETE · read-only · doc-only commit to nifty (`9076a721`).
**Question:** is `StaticPropDepth` (the camera depth-prepass pipeline) a safe next
SPIR-V consumer pilot, or does it expose material/binding weirdness that should
divert us to the postprocess-family?

## VERDICT: **DIVERT → `SPIRV-POSTPROCESS-FAMILY-1`** (defer StaticPropDepth)

StaticPropDepth's fragment *logic* is trivial (alpha-discard, depth-only), but its
**binding/material/sampler surface is the SAME as StaticPropOpaque** — not a
simplification — and its OFF-vs-ON **parity is only observable through a fragile,
capture-less indirect early-Z effect**. Per the slice's own divert condition
("if it exposes material/binding weirdness, divert to postprocess-family"), the
evidence triggers the divert. Postprocess-family widens SPIR-V coverage with a
direct color golden-frame gate and zero material-system risk.

## Evidence (re-grepped @ 9076a721)

1. **Shader pair:** `makeProgram("static_prop_depth", "shaders/static_prop.vert",
   "shaders/static_prop_depth.frag", depthPrefix)` (`gos_static_prop_batcher.cpp:1390-1394`);
   `bindProgram(PipelineId::StaticPropDepth,…)` `:1397`. vert+frag, no tess/geom.
   Goes through `makeProgram`→`makeProgram2` → inherits program-atomic fallback for free.
2. **Shared vert:** `static_prop.vert` is shared by 3 runtime programs (static_prop,
   static_prop_coalesce, static_prop_depth). **The depth frag `static_prop_depth.frag`
   is UNIQUE** → program-atomic matching by both filenames isolates it (the
   postprocess-proven pattern; shared-vert-SPIR-V-in-one-program is safe). Blast radius LOW.
3. **Define set:** depth copies the COLOR prefix verbatim (`:1387-1389`) — same
   4-define family: `MC2_COALESCE` (normal), `MC2_USE_VIEW_UNIFORMS` (**default-ON**),
   `MC2_OBJECT_ID_BUFFER`, `MC2_STATICPROP_PBR_SLOTS`. **Same variant fan-out as Opaque.**
4. **Bindings:** SSBO 0/1/2 (instances/colors/perType), SSBO 3 ParityOut **multiplexed**
   with UBO 3 ViewUniformsBlock (`kViewUniformsBinding`), SSBO 4 PerDrawData, **SSBO 5
   MaterialTable (MaterialGpu)**. (`static_prop.vert:56-68`, `static_prop_depth.frag:51,60`;
   binding-slot-occupancy confirms.)
5. **Samplers:** YES — `sampler2DArray u_texArr` (coalesce) / `sampler2D u_tex` (legacy),
   sampled for alpha (`static_prop_depth.frag:54,73,102,105`). Bound **by-name**
   (`glUniform1i`, `:1419-1423`) → needs `--auto-map-locations` (as pilots do).
6. **Binding 5 MaterialGpu:** YES — `layout(std430, binding=5) MaterialTable`
   (`static_prop_depth.frag:60`), `glBindBufferBase(...,5,...)` (`:5149`). **Same full
   material-table surface as Opaque** — not simplified.
7. **Texture-array layer path:** YES — replicates Opaque's `effectiveLayer` selection
   verbatim (`:94-102`), with the **load-bearing invariant** that the discard be
   BYTE-IDENTICAL to `static_prop.frag` (`:12-19`). Needs the full alpha-selection chain.
8. **Depth-only FF state:** ad-hoc `glColorMask(FALSE)` saved/restored around the draw
   (`:5122-5130,5197`); DepthFunc=GreaterEqual from PipelineDesc. **No** polygonOffset/
   depth-bias/depthFunc-override. SPIR-V changes none of this.
9. **Output:** **no `out` vars, no `gl_FragDepth`** — depth-only by design
   (`static_prop_depth.frag:80,115`), matching `colorAttachments {false,false,false}`.
10. **Parity gate — THE blocker:** depth writes zero color; its only effect is indirect
    (lays nearest reverse-Z so the color pass runs `GL_EQUAL` early-Z, `:6413-6419`).
    No depth-buffer capture exists in `scripts/`. The prepass is gated
    `MC2_STATIC_PROP_DEPTH_PREPASS` **default-OFF** (`:5081`) and no-ops under
    `MC2_VIEW_UNIFORMS=0` (`:5110`). The only observable OFF/ON signal is "props VANISH
    under GL_EQUAL if depth mismatches" — fragile, indirect, no byte-exact golden frame.
11. **Fallback:** program-atomic GLSL rebuild applies automatically once allowlisted
    (`makeProgram`→`makeProgram2`, `:964`/`:657`). Confirmed not hand-rolled.
12. **Safer than Opaque?** NO. Same vert, same 4-define family, same binding-5 MaterialGpu,
    same texture-array selection, same by-name sampler. Simpler **only in the fragment
    body** (no lighting/PBR/SH/obj-id). Binding/material fragility ≈ Opaque.

## Risk read
- **(a) shared-vert:** LOW (unique depth frag; program-atomic isolates).
- **(b) variants:** MODERATE — same 4-define family as Opaque, not fewer.
- **(c) material/binding weirdness:** **ELEVATED** — full binding-5 MaterialGpu SSBO +
  `material_gpu.hglsl` + texture-array `effectiveLayer` selection + by-name `sampler2DArray`
  + slot-3 SSBO/UBO multiplex. Any SPIR-V reflection/auto-map mismatch breaks the
  byte-identical-discard invariant → props vanish under GL_EQUAL.
- **(d) parity gate:** **BIGGEST BLOCKER** — depth-only pass, no color output, no depth
  capture tooling, prepass default-OFF; no direct golden-frame verification like postprocess had.

## Why divert (not just defer)
StaticPropDepth gives little that's "simpler" (only the frag body) while carrying
Opaque's full material/binding surface AND a hard-to-verify depth-parity gate.
`SPIRV-POSTPROCESS-FAMILY-1` expands SPIR-V coverage over several small,
color-output, golden-frame-verifiable programs (cloud/shoreline/ssao/fog/edge_fog/
hzb_reduce — all using the already-baked-safe `postprocess.vert`) with **no
material-system risk** and a **direct visual gate**. It hardens the artifact
indexing/fallback over many programs — higher confidence per unit risk.

## When to revisit StaticPropDepth
Pick it up **with StaticPropOpaque** (shared vert + identical material/binding
surface — bake the family together), and only after a **depth-parity gate** exists
(a depth-buffer capture, or a forced-`MC2_STATIC_PROP_DEPTH_PREPASS=1` +
prop-vanish coverage check on a static, prop-dense, mech-light bookmark).

## Exclusions honored
Recon only — no build, no StaticPropOpaque, no material refactor, no binding
2/5/7 unification, no render-graph/pipeline-abstraction work, no Vulkan. Foreign
WIP untouched.
