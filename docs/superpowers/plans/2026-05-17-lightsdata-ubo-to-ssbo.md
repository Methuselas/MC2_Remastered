# PLAN: LightsData UBO → SSBO (remove the 64-slot light-data ceiling)

> Branch `claude/gpu-driven-rendering`, worktree `…/gpu-driven-rendering`.
> Anchors grep-verified @ `5bffaf3` (3 water-only commits atop D2 `2dca942`;
> light path untouched). RE-GREP at edit-time.
> Recon: `docs/superpowers/plans/progress/2026-05-17-lightsdata-ubo-to-ssbo-recon.md`
> + the two fork resolutions (this session). Supersedes Option A
> (STOP THE LINE: 64-window had no headroom — 57/64 today).

## Goal & honest framing

Convert the `LightsData` light-data buffer from `std140 uniform {
ObjectLights light[64]; }` to `std430 buffer { ObjectLights light[]; }`
(SSBO). Removes the 64-slot window entirely.

**Substitutive honesty (`feedback_offload_must_be_substitutive_not_additive.md`):**
this slice by itself **retires no CPU zone** — it is *enabling
infrastructure*. Its deliverables are (1) the wall removed, (2)
bit-parity-identical lighting (mech + static + legacy-lit), (3) a
pre-existing latent ceiling fixed (mc2_17 is 57/64 today —
`lighting.hglsl:41-43`; any denser mission silently corrupts lighting
NOW, independent of this work). The static-lighting mission-load bake
(the actual per-frame CPU-zone-death slice) becomes trivial *on top of
this* and is the explicit follow-on. Do not sell this as a perf win;
it is a correctness/ceiling fix + enabler. Commit message says exactly
that, code dimensions only.

## Three CRITICAL atomic-commit invariants (review must enforce)

1. **FORK-2 fix in the SAME commit as the qualifier flip.** The legacy
   `gos_tex_vertex_lighted`/`gos_vertex_lighted` material path is LIVE
   (`txmmgr.cpp:1646` every frame via `ShapeRenderer::render` else-branch
   `b_old_way=false` `:1622`; also `gameos_graphics.cpp:5104`
   `drawIndexedTris`). It binds `LightsData` via UBO reflection
   (`gos_SetRenderMaterialUniformBlockBindingPoint(mat,"LightsData",…)`
   `txmmgr.cpp:1364` → `glUniformBlockBinding`, `gameos_graphics.cpp:384-392`,
   reflection enumerates `GL_ACTIVE_UNIFORM_BLOCKS` only,
   `shader_builder.cpp:563-599`). An SSBO block is NOT UBO-enumerated →
   `setUniformBlock("LightsData")` silently returns false → **legacy lit
   meshes render garbage lighting from frame 1.** The
   `glShaderStorageBlockBinding` fix (S4) MUST be in the same atomic
   commit or the default path breaks instantly.
2. **Binding 20 threaded GLSL + C++ in one commit; no UBO-slot-0 reuse.**
   SSBO bindings 0–19 are fully allocated (0=instance, …19=mask_solid
   recipe — full map in the fork-1 resolution). Recycling
   `LIGHT_DATA_ATTACHMENT_SLOT=0` aliases LightsData over the instance
   SSBO in `static_prop.vert`/`mech.vert` → catastrophic silent
   corruption of all static+mech rendering.
3. **Zero `TG_HWLightsData`/`ObjectLights` field edits in this commit.**
   The std140≡std430 bit-identity verdict (all members vec4/mat4 arrays
   + ivec4 tail; `MAX_HW_LIGHTS_IN_WORLD=16`; 1808 B, /16 both layouts)
   holds ONLY for a pure qualifier change. Any field edit re-arms the
   `cpp_glsl_ubo_struct_lockstep.md` regression. Struct stays byte-frozen.

## Files / symbols (re-grep at edit-time)

- `shaders/include/lighting.hglsl`: `:11` add `#define
  LIGHT_DATA_SSBO_BINDING 20`; `:39` block qualifier; `:54`
  `light[64]`→`light[]`. (Shared include → `static_prop.vert`,
  `mech.vert`, `gos_tex_vertex_lighted.{vert,frag}` all inherit it.)
- C++ binding constant: grep `LIGHT_DATA_ATTACHMENT_SLOT` definition
  (`GameOS/include/gameos.hpp` / `mclib/txmmgr.h` region) — add sibling
  `LIGHT_DATA_SSBO_BINDING = 20` (do NOT rename the UBO constant; keeps
  UBO/SSBO namespaces unambiguous, matches `*_SSBO_BINDING` convention).
- `mclib/txmmgr.cpp`: create/bind `:321-322`, recreate/upload/rebind
  `:1563-1569`, destroy `:386-388` → raw GL SSBO. FORK-2 bind site
  `:1364-1365` (drop the LightsData UBO bind, keep SceneData UBO bind).
- `GameOS/gameos/gameos_graphics.cpp`: `s_perCmdSsbo` raw-GL pattern
  (`:2100-2101` create, `:2267-2269` update) is the precedent;
  `getGLBufferType` `:6303` (asserts non-UNIFORM → gos API can't make an
  SSBO, raw GL is in-scope/correct); `drawIndexedTris` `:5093/5104`
  (same legacy material program — block binding set once/program).

## Design

### S1. New binding constant (lockstep)
`#define LIGHT_DATA_SSBO_BINDING 20` in `lighting.hglsl` + a C++ sibling
constant beside `LIGHT_DATA_ATTACHMENT_SLOT`. Binding 20 verified free
(0–19 fully enumerated; grep `GL_SHADER_STORAGE_BUFFER, 2[0-9]` /
`*_SSBO_BINDING` ≥20 = zero hits). `LIGHT_DATA_ATTACHMENT_SLOT` is left
defined (audit it has no remaining LightsData consumer post-conversion;
note if it becomes dead).

### S2. Shader block flip (lighting.hglsl, shared include)
`layout(binding=LIGHT_DATA_ATTACHMENT_SLOT, std140) uniform LightsData {
ObjectLights light[64]; }` → `layout(binding=LIGHT_DATA_SSBO_BINDING,
std430) buffer LightsData { ObjectLights light[]; }`. Index path
`light[int(inst.lightDataIndex)]` (`static_prop.vert:306`,
`mech.vert:162`) unchanged on an unbounded array. `#version 430`
confirmed on all 3 lighted programs (SSBO needs 4.3); AMD RX 7900 XTX
vertex-SSBO read already in production via these same shaders.

### S3. txmmgr raw-GL SSBO (Vulkan-prep: one named helper, not scattered)
Replace the `gos_CreateBuffer(UNIFORM)`/`gos_BindBufferBase`/
`gos_UpdateBuffer` trio with a small `LightDataSsbo` helper:
- create: `glGenBuffers`+`glBindBuffer(GL_SHADER_STORAGE_BUFFER,id)`+
  `glBufferData(GL_SHADER_STORAGE_BUFFER, sz, NULL, GL_DYNAMIC_DRAW)`.
- bind once: `glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 20, id)`.
- upload: `glBufferSubData`/`glBufferData` (whole buffer, same cadence as
  today — sub-range deferred, NOT required for this slice).
- grow (`txmmgr.cpp:1563-1565` path): delete+regen+rebind at 20. **This
  path now genuinely fires** (no 64 cap) — must be correct, not the
  previously-near-dead branch. Capacity grows via the existing +128
  chunk logic, now unbounded by the shader.
- destroy `:386`: `glDeleteBuffers`.
Explicit device-mediated GL, no assumed cross-call state (vulkan-prep).

### S4. FORK-2 legacy-lit bind fix (CRITICAL, same commit)
In `ShapeRenderer::render` (`txmmgr.cpp:~1364`): delete
`gos_SetRenderMaterialUniformBlockBindingPoint(mat,"LightsData",
LIGHT_DATA_ATTACHMENT_SLOT)` (now a silent no-op). Keep the `SceneData`
UBO bind `:1365` (still a UBO). After the program is active
(`gos_ApplyRenderMaterial` `:1367`), once per program (cache the
resource index): `glShaderStorageBlockBinding(prog,
glGetProgramResourceIndex(prog, GL_SHADER_STORAGE_BLOCK, "LightsData"),
20)`. The single `glBindBufferBase(…,20,id)` in S3 then feeds it.
`drawIndexedTris` (`gameos_graphics.cpp:5104`) uses the same material
program → storage-block binding is per-program, not per-call-site
(set-once). No `parse_storage_blocks` reflection addition needed
(direct `glGetProgramResourceIndex`).

### S5. Reversibility model (no runtime kill-switch — justified)
A UBO↔SSBO toggle needs two shader variants + two C++ paths = heavy,
disproportionate. Per `feedback_soak_waiver_with_probes_and_reviews_validated`,
substitute: (a) atomic self-contained commit (git-revertible in one
step), (b) an env-gated **parity probe** (`MC2_LIGHTSSBO_PROBE`):
sample 1/N frames, assert the SSBO-resident `light[k]` bytes == the CPU
`lightData_[k]` for a sampled `k` (catches binding-aliasing + any
layout drift), (c) mandatory pre-merge visual canary. State explicitly:
this slice's safety is the parity gate + atomic revert, not a flag.

## Verification (soak waived; probes + canary substitute)
- Adversarial-plan-review ≠ STOP THE LINE; the 3 atomic invariants +
  the binding-20 map + the FORK-2 same-commit requirement + std430
  bit-identity (no field edits) all accepted.
- Parity probe (S5b) green over a tier1 run.
- tier1 5/5 `GL_INVALID_*`=0 `+0` destroys (default — no env).
- **Visual canary vs parent `5bffaf3`, side-by-side, three surfaces:**
  (1) mech lighting, (2) static-prop lighting, (3) **legacy-lit / HUD
  meshes** (the FORK-2 surface — if S4 is wrong this is where garbage
  appears first), heaviest mission + mc2_03. Bit-visually identical.
- Memory: append outcome to
  `lighting_is_mission_load_static_no_dynamic_emitters.md` (ceiling
  removed; bake now unblocked) + a new
  `lightsdata_ssbo_binding_20_and_legacy_lit_bind.md` (the binding map +
  the FORK-2 `glShaderStorageBlockBinding` gotcha — load-bearing for any
  future LightsData touch). Commit: enabler framing, no wall-clock,
  records the 3 invariants honored.

## Adversarial review: VERDICT PROCEED — required fixes folded (grep-verified @ 5bffaf3)

Core verdicts grep-confirmed sound (bit-identity TRUE: `MAX_HW_LIGHTS_IN_WORLD=16`, all mat4/vec4 arrays + ivec4 tail, 1808B /16 both layouts; binding 20 free, 0–19 fully allocated; FORK-2 break mechanism real; negative-claim complete = 14 files, no missed UBO reader). Required-before-code, now binding:

- **RF1 (S4 handle + cadence).** Program id = `mat->getShader()->shp_`
  (pattern `gameos_graphics.cpp:1841/4393/4482`), with NULL-guard
  (`if(!mat||!mat->getShader()||!mat->getShader()->shp_) return;`).
  **Cadence DECIDED: unconditional per-draw** `glShaderStorageBlockBinding(
  shp, glGetProgramResourceIndex(shp,GL_SHADER_STORAGE_BLOCK,"LightsData"),
  LIGHT_DATA_SSBO_BINDING)` via ONE shared helper
  `bindLightDataStorageBlock(mat)` called from BOTH `ShapeRenderer::render`
  (`txmmgr.cpp:~1367`) AND `drawIndexedTris` (`gameos_graphics.cpp:5104`).
  Per-draw chosen over per-program-cache: idempotent, ~free, and immune
  to the CLAUDE.md shader-hot-reload relink (which resets program block
  bindings to 0 — a cached guard would silently revert). `glGetProgram
  ResourceIndex` result may be cached per `shp_` (resource index is
  stable until relink; the *binding call* is what must be unconditional).
- **RF2 (S3 lifetime asymmetry).** On buffer grow/recreate
  (`txmmgr.cpp:1563-1565`): re-issue `glBindBufferBase(
  GL_SHADER_STORAGE_BUFFER, LIGHT_DATA_SSBO_BINDING, newId)` ONLY (context
  state, must follow the new id). Do NOT re-call
  `glShaderStorageBlockBinding` there (program state, unaffected by buffer
  recreate; RF1's per-draw call already covers it).
- **RF3 (S3 mandated instrumentation).** Add env-gated `[LIGHTSSBO v1]
  event=buffer_grow old=<n> new=<n>` + `event=enabled binding=20` at
  create — resource-lifetime rework requires same-commit lifecycle prints
  (worktree debug-instrumentation rule, non-optional).
- **RF4 (scope — FUNCTIONAL, not cosmetic).** `gos_mech_batcher.cpp:897`
  `kUboLightSlotCap=64u`; `:902` `if (ps.desc.lightDataIndex >=
  kUboLightSlotCap) overCap=true` **actively drops mech lights ≥64** —
  post-SSBO this clamp defeats the unbounded buffer (the exact thing
  this slice enables). Must remove/raise the cap and repurpose the
  `[MECHLIGHT v1] event=cache_full` (`:907`) / `ubo_cap_check` (`:227-233`)
  to the new buffer-capacity bound, same commit. In Files & Design.
- **RF5 (S5 probe scope).** State explicitly: the byte-probe `light[k]==
  lightData_[k]` does NOT detect binding-20 aliasing (bytes upload fine;
  the *bind* is the failure). The 3-surface visual canary (mech / static
  / legacy-lit) is the LOAD-BEARING detector for binding/FORK-2 failure;
  augment the probe with the existing downstream `parityOut_` /
  `u_parityNumLightsDebugMode` lit-ARGB harness (`static_prop.vert:67/
  304-308`) which catches a wrong bind.
- **RF6 (liveness).** CONFIRMED at edit-time: `ShapeRenderer::render`
  fires every frame at `txmmgr.cpp:1646`; `b_old_way=false` (`:1622`) →
  the lit-material else-branch (LightsData bind `:1364`) is the LIVE
  default path. S4/FORK-2 fix is load-bearing, not belt-and-suspenders.

## Out of scope
The static-lighting mission-load bake (the follow-on substitutive
slice this unblocks — now trivial: persistent per-recipe light table,
no partition/window gymnastics). `glBufferSubData` sub-range upload
(deferred optimization). `gosBUFFER_TYPE::STORAGE` GameOS-API addition
(raw-GL-in-txmmgr is correct/in-scope; API expansion unjustified for
one consumer). D2 stays committed (mech per-frame floor, now into an
unbounded SSBO).
