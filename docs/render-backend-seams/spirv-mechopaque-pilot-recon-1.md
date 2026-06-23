# SPIRV-MECHOPAQUE-PILOT-RECON-1

**Status:** RECON COMPLETE · read-only · doc-only commit to nifty (`43b579ed`).
**Question:** can MechOpaque be the next SPIR-V consumer pilot — the first
*registered/keyed* family with real variants, after the postprocess composite
(SPIRV-CONSUMER-PILOT-BUILD-1)?

## VERDICT: **GO — but gated on one prerequisite** (`SPIRV-KEYED-VARIANT-CONSUMER-1`)

MechOpaque is a **good** next pilot — crucially, `mech.vert`/`mech.frag` are **not
shared** with any other program, so the shared-vertex blast radius that bit the
postprocess pilot is **zero** here. BUT the existing consumer machinery is
hard-wired to *default-variant-only* and **refuses any prefix containing
`#define`** (`spirvCompositePilotProgram` + `trySpirvSpecialize`,
`shader_builder.cpp`). Mech's two variant macros are **both DEFAULT-ON**, so the
variant stock players actually run carries two `#define`s — the current pilot
would **silently GLSL-fallback and never fire** for mech. Therefore MechOpaque
cannot be piloted by adding a `pilots.json` row alone; it first needs the
consumer generalized from "default-variant-only" to **keyed-variant** lookup.

## Evidence (re-grepped @ 43b579ed)

### 1. Shader files + stages — 2-stage, single program
`gos_mech_batcher.cpp:552-553`: `makeProgram("mech", "shaders/mech.vert",
"shaders/mech.frag", mechPrefix)`; registered `bindProgram(PipelineId::MechOpaque,…)`
`:570`. No tess/geom. `makeProgram` → `makeProgram2` → the same SPIR-V seam the
postprocess pilot hooks.

### 2. Variant matrix — 2 macros, BOTH default-ON (the key finding)
Prefix builder `gos_mech_batcher.cpp:540-550`: injects `MC2_OBJECT_ID_BUFFER` (if
`RenderWorld::IsObjectIdBufferEnabled()`) + `MC2_USE_VIEW_UNIFORMS` (if
`s_mechViewUniforms`). **Both default-ON:**
- `MC2_OBJECT_ID_BUFFER` = `envFlagDefaultOn` (`RenderWorld.cpp:74-79`).
- `MC2_USE_VIEW_UNIFORMS`/`s_mechViewUniforms` = default-ON, kill-switch `=0`
  (`gos_mech_batcher.cpp:137-140`; the adjacent "DEFAULT OFF" comment is **stale** —
  `docs/tier1_env_vars.md:160` confirms ON).

No other injected macros (CSM/MRT/skinning are runtime `glUniform` toggles, not
`#define`s — they do NOT multiply the program count). `MC2_STATIC_PROP_LIGHTING`
at `mech.vert:15` is hard-coded source + classed DEAD_OR_STALE.

**Realized stock variant = `{MC2_OBJECT_ID_BUFFER, MC2_USE_VIEW_UNIFORMS}` both
defined.** Theoretical matrix = 4 combos (oid × viewuniforms); practically 1 built
per session (the both-on combo). Bake target = the both-on variant (ideally all 4
to cover kill-switch states).

### 3. Shared vertex shader — NO (clean)
Only one `makeProgram` references `mech.vert`/`mech.frag` (`gos_mech_batcher.cpp:552`).
The shadow pass is a **separate program** `shadow_mech` (`shaders/shadow_mech.vert`
+ `shaders/shadow_instanced.frag`, `gameos_graphics.cpp:4969-4973`). Blast radius
from the shared-vert lesson = nil.

### 4. Bindings / samplers vs manifests
Explicit binding-base resources (offline bake already avoids `--auto-map-bindings`,
so preserved): SSBO **0** InstanceBuffer (`mech.vert:60`), SSBO **1** BoneBuffer
(`mech.vert:66`), SSBO **2** MechMaterial (`gos_mech_batcher.cpp:1941`), SSBO **20**
LightsData (`lighting.hglsl:53`, shared named pair), UBO **3** ViewUniformsBlock
(`view_uniforms.hglsl:23`, gated by `MC2_USE_VIEW_UNIFORMS`). All present in
`binding-slot-occupancy.json`. Samplers `u_tex`/`u_pbrNormalTex`/`u_pbrOrmTex`/
`u_pbrPaintNormalTex`/`u_pbrPaintOrmTex` (units 0-4, `mech.frag:42-49`) are set by
**literal `glUniform1i(loc,N)`** (loc-cached, by name) — `sampler-unit-occupancy.json`
flags them "UNKNOWN binder".

### 5. Material path — mixed, leans CPU/by-name
Bulk data = descriptor-like SSBOs (instance/bone/material/lights via
`glBindBufferBase`). But textures + ~25 scalar material params reach the shader via
cached-location `glUniform1i/1f` **by name** (`gos_mech_batcher.cpp:1970-2130, 2220-2283`).
SPIR-V GL default-uniform-by-name works, but this is the fragile axis from the
postprocess lessons, and mech's surface is ~an order of magnitude larger (~25
uniforms + 5 samplers vs postprocess's ~near-zero).

### 6. Shadow/mech variant — separate program, can stay GLSL
`shadow_mech` is its own single-variant (`#version 430` only) program
(`gameos_graphics.cpp:4969-4973`, used `gos_mech_batcher.cpp:879-882`). Program-atomic
isolation → it stays GLSL independently; NOT required in the mech pilot (a clean
separate future target if wanted).

### 7. Visual gate surface
mc2_24 is the canonical mech mission with established goldens (`baselineA-head-mc2_24`,
`ub201-pre2-mc2_24`) + bookmark `tests/visual/bookmarks/mc2_24.json`; UB2-02 (mech.frag)
was previously byte-diff-gated via `ub201-pre2` — direct precedent. mc2_01 has
`mc2_01_combat`/`mc2_01_werewolf` bookmarks. **Caveat:** mech is animated (gait/pose
per frame), unlike the static postprocess composite — a byte-exact OFF/ON diff needs a
frozen/deterministic frame (fixed bookmark camera + seeded/paused anim). Achievable
(UB2-02 did it) but more delicate than the post-fx capture.

## Risk read (per postprocess lessons)
- **(a) shared vert?** No — zero blast radius. ✅ Better than postprocess.
- **(b) variants to bake?** The both-on combo (default-ON dual macro) — NOT the empty
  default. This **breaks the pilot's current "no-#define default-variant-only"
  contract** → the prerequisite.
- **(c) binding/material fragility?** Bindings round-trip cleanly (no auto-map). The
  exposure is the **wide by-name uniform/sampler surface** (~25 uniforms + 5 samplers)
  that must survive `glSpecializeShader` reflection with matching names/locations — the
  main correctness risk.

## Prerequisite: `SPIRV-KEYED-VARIANT-CONSUMER-1`
Generalize the consumer + bake from "default only" to **keyed by define-set**:
1. `build_variants.py`: add the mech pilot with its variant list (`[{OBJECT_ID,VIEW_UNIFORMS}]`,
   ideally all 4 combos). artifact naming already = `shaderVariantId = hash{base + sorted
   defines}` — works unchanged.
2. Consumer: replace the `prefix-contains-#define → reject` guard with a **define-set →
   variant-id** resolver (parse the realized prefix's `#define`s, hash, look up the matching
   sidecar). Keep program-atomic (all stages of the program+variant SPIR-V or all GLSL).
3. Then `SPIRV-MECHOPAQUE-PILOT-BUILD-1`: wire mech as a pilot program, bake the both-on
   (+ combos) variants, gate `MC2_SHADER_SPIRV=1`, deterministic mc2_24 mech-frame visual
   OFF/ON byte-diff, tier1/single-mission smoke OFF/ON, atomic GLSL fallback per variant.

## Exclusions honored
Recon only — no code, no shader edits, no bake, no consumer change, no Vulkan. Foreign WIP
(`mech3d.cpp`, `txmmgr.h`, `golden-sets.json`) untouched.
