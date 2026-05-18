# Water Enabling-Infrastructure Map (Greybeard Survey)

**Date:** 2026-05-18
**Branch:** `claude/water-material-v1` (isolated)
**Status:** DIRECTION-SETTING (no implementation). Code-grounded, advisor-first
(render / terrain-indirect / cpu-gpu-offload / shader), Rule-0 verified.
**Why this doc exists:** consolidate the foundational water-enabling analysis
so it is findable across sessions (user asked it be persisted). The detailed
reverse-Z item has its own scoping doc:
`2026-05-18-SCOPING-reversed-z-float-depth-modernization.md`.

## User directives captured (2026-05-18)

- **#1 (continuous water surface) is a COMMITTED goal** - user: "I
  definitely want (1) done". Not optional; it is the keystone the rest is
  sequenced toward.
- Plan: **(a) scope reverse-Z into a real design -> (b) take the
  depth-vs-geometry ruling decision -> new session** (execution / #1).
- BAR-shoreline polish PARKED (option C); reverted to validated `be5fb0d`
  at `d9516f3`. Resume context in the water living-record memory.

## Headline (reorders the mental model)

- **A sampled scene-depth texture ALREADY EXISTS and is bound** -
  `sceneDepthTex_` (`gos_postprocess.cpp:~292-300`, `GL_DEPTH24_STENCIL8`),
  consumed by shadow/ssao/godray and a `shoreline.frag` pass that *declares*
  `uniform sampler2D sceneDepthTex` but never samples it. It is NOT wired
  into the water material. => depth-driven water (soft-contact shoreline,
  refraction, true underwater fog) is "one bind + one linearize" away, not a
  new GBuffer/pass.
- **The fast-path geometry is already Vulkan-prep clean** - std430 SSBO +
  ring + `glBindBufferRange` (`gos_terrain_water_stream.cpp:~411-628`). No
  buffer-type debt there. Only *style params* are compile-time `const` in
  the frag (-> the demand-gated params-UBO, #4).

## Ranked enablers

| # | Enabler | Current state (grep-verified) | Unlocks | Effort / blast | Order |
|---|---------|-------------------------------|---------|----------------|-------|
| 1 | **Continuous water surface** | water = per-terrain-quad flagged + binary `discard` (`gos_terrain_water_mdi.frag:~109-110`; one `WaterRecipe`/quad `gos_terrain_water_stream.cpp:~280`). NOT a surface. | smooth shores, contact foam, refraction, reflection geometry, shoreline SDF - structurally everything | LARGE, net-new. Keystone. Shape (geometry mesh vs screen-space) decided by fork B. | after fork B |
| 2 | **Wire scene-depth into water FS** | resource exists+bound for post only; water FS has no depth sampler (`WaterThickness` is CPU `waterElevation-velev`, vert `~:205`) | depth soft-contact shoreline (parked Option A), refraction, true underwater fog | SMALL-MED infra; **gated by fork B (camera-indep ruling)**; quality bounded until #3 | after fork B + #3 |
| 3 | **Reverse-Z + float depth** | ALREADY SCOPED. Prereq `glClipControl ZERO_TO_ONE` shipped (`gameosmain.cpp:~930`). Reverse-Z = the delta (~20+ C++, ~10 shaders, sign-flipped `terrain_depth_bias` lockstep). | removes the zoom precision-tax warping every depth-based water trick; deletes per-consumer fudge family; obviates Fix A (NOT Fix B) | LARGE-WIDE, mostly mechanical; 2 gating design Qs | FIRST (fork-free, greenlightable) |
| 4 | Water-params UBO | style consts compile-time in frag (`gos_terrain_water_mdi.frag:~40-55`); per-frame scalars immediate `glUniform*` | per-biome / runtime tuning without recompile | SMALL, Vulkan-prep-aligned | demand-gated (YAGNI) |
| 5 | **Retire legacy CPU-raster water** | dual path: fast `renderWaterFastPath` (`terrain.cpp:~1341`) vs legacy `renderWater`->`quad.cpp:~3310` (Stuff `projectZ`, `camera.cpp:~1941`). Un-armed CPU-water `dz` sign-flip divergence. | one projection authority -> features built once, kills correctness hazard | MED-LARGE; MUST be substitutive-not-additive; NOT a perf slice | before/with #1 |

## Two user-decision forks (named, not pre-resolved)

- **Fork B (PIVOTAL - gates the cheapest path & #1's shape):** the
  load-bearing water ruling *"ZERO perceptible camera-dependence"* blocks
  depth-driven water (#2) because depth-buffer reads make contact-band
  extent mildly zoom-dependent. **Does smooth-shoreline/refraction come from
  DEPTH (cheap, infra ~75% exists, needs the ruling relaxed/qualified) or
  from a continuous-surface GEOMETRY (#1, ruling-safe, the large keystone)?**
  This decision cascades through #1 (its shape) and #2 (its legality). Only
  the user can decide. **To be taken THIS session (step b).**
- **Fork P/F (shared #5 <-> #3-Q2):** port the legacy scalar
  `inverseProjectZ`/`projectZ` to reverse-Z/[0,1], vs **fence** the dying
  CPU-water/TacMap path. Water-going-full-GPU favours fencing. Methodology
  call under substitutive-not-additive.

## Greybeard sequence

1. **Greenlight #3 reverse-Z** - already scoped, Fix B unblocked it,
   highest-leverage, fork-free. (Session step a = scope it into a real
   design; answer its 2 gating Qs: gos_terrain.frag POM `gl_FragDepth`
   re-derivation; legacy `projectZ` port-vs-fence = Fork P/F.)
2. **Resolve Fork B** (session step b) - depth vs geometry; cascades to #1.
3. **#5 retire/fence legacy water** - one projection authority; shares Fork
   P/F with #3.
4. **#1 continuous water surface** - the COMMITTED keystone; shape chosen by
   Fork B outcome.
5. **Wire #2 + #4 on demand** - depth into water FS; params UBO when a
   feature needs runtime/per-biome config.

## Enablers vs nice-to-haves

- True structural enablers (features impossible without): #1, #2, #5.
- Root-class precision (bounds all depth-based water quality): #3 (scoped).
- Convenience, correctly demand-gated: #4.
