# TERRAIN-MACRO-VARIATION-1 — Recon (READ-ONLY)

Worktree: A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev
Date: 2026-06-25. All file:line grep-verified this session.

## 1. WHICH terrain frag is LIVE (mc2_01 / mc2_24)

**terrain_lod_chunk.frag IS the live frag.** `MC2_TERRAIN_LOD_CHUNK` is DEFAULT ON
(docs/tier1_env_vars.md:233). The chunk path links terrain_lod_chunk.vert +
terrain_lod_chunk.frag via makeProgram at gos_terrain_lod_chunk.cpp:366-370.

gos_terrain.frag is the LEGACY/thin path (runs only with the gate UNSET). The B4a
cliff/slope-bias uniforms exist ONLY in terrain_lod_chunk.frag + its driver — proof
the chunk frag is the production target.

**Edit terrain_lod_chunk.frag for the change to show in-game.** (Optionally mirror in
gos_terrain.frag for legacy-path parity, but not required for default config.)

## 2. FINAL albedo assembly line (terrain_lod_chunk.frag)

`baseColor` is the surface albedo. It is fully assembled by line 603 (snow dampen),
just before lighting at 605-638. Build sequence:
- :546 `baseColor = mix(base, materialTint, tintStrength);`  (material tint)
- :550 cement tone restore
- :556-563 cliff darken
- :595-601 break-up noise
- :603 `baseColor *= mix(1.0, snowBrightnessDampen, snowWeight);`
- :638 `vec3 lit = baseColor * normalLight * shadow;`  <-- lighting consumes baseColor

**INSERT POINT: after line 603, before line 605.** Wrap:
`baseColor = mix(baseColor, variedColor, u_macroStrength);`

## 3. Available data at insertion point

- World XY: `v_worldPos.xy` (varying `in vec3 v_worldPos;` decl :15; from
  terrain_lod_chunk.vert). Macro noise: `fbm(v_worldPos.xy * freq, oct)` — fbm already
  used at :597-598. noise.hglsl included :13.
- Slope / normal: macro (un-perturbed) slope is `macroNz` (float, :496/:507) and the full
  macro normal `smoothTerrainNormal(v_worldPos.xy)` (:103-111). Use `abs(macroNz)` for a
  rock-tint slope mask exactly like the cliff block (:557) — do NOT use perturbed `N`
  (fires on flat ground).
- matWeights (vec4 rock/grass/dirt/concrete, :463) and snowWeight available for masking.

## 4. Existing B4a code — build ALONGSIDE, do not conflict

- Slope-bias (MC2_TERRAIN_SLOPE_BIAS): frag :466-482 (mutates matWeights BEFORE detail
  normal / tint). Uniforms `useRockSlopeBias`/`rockSlopeBiasStrength` decl :147-148.
- Cliff darken (always-on): frag :556-563.
- Cliff triplanar (MC2_TERRAIN_CLIFF_TRIPLANAR): frag :565-594, uses
  MAT_LAYER_MARBLE_CLIFF, recomputes `smoothTerrainNormal`. Uniforms
  `useTriplanarCliff`/`cliffTriplanarStrength` decl :149-150.
NOTE: macro-variation at :603 runs AFTER all of these → composes cleanly. Keep it a pure
albedo mix; do NOT touch matWeights or N.

## 5. Uniform plumbing (terrain_lod_chunk path)

- Frag uniform decl: add `uniform float u_macroStrength;` near :144-150 (the tunables block).
  NO `#version` in frag — prefix injected at gos_terrain_lod_chunk.cpp:358 ("#version 430\n").
- C++ location static: add `static GLint s_locMacroStrength = -1;` near
  gos_terrain_lod_chunk.cpp:132-136.
- Lookup: add `s_locMacroStrength = glGetUniformLocation(s_terrainProgram, "u_macroStrength");`
  in the link block ~ :429-441 (alongside the B4a lookups :430-433).
- Upload: this path uses DIRECT `glUniform1f` (NOT the deferred setFloat/apply cache).
  Mirror the B4a slope-bias upload at gos_terrain_lod_chunk.cpp:843-858. Sample existing
  terrain float upload: :842 `glUniform1f(s_locNfhStrength, gos_GetTerrainNormalsFromHeightStrength());`
  Guard `if (s_locMacroStrength >= 0)`.
- GL rules: direct glUniform* after program bound (this path glUseProgram's s_terrainProgram
  before the uniform block). No setFloat/apply here. No matrix involved. No #version in frag.

## 6. Env gate -> uniform strength (existing pattern)

Mirror the cliff-triplanar gate block at gos_terrain_lod_chunk.cpp:859-874 (and slope-bias
:843-858): read `MC2_TERRAIN_CLIFF_TRIPLANAR` -> int on/off, and
`MC2_TERRAIN_CLIFF_TRIPLANAR_STRENGTH` -> atof default 1.0, then glUniform.

Recommended for macro-var: gate `MC2_TERRAIN_MACRO_VARIATION` (off => upload 0.0 =>
mix(orig, varied, 0) = byte-identical), strength `MC2_TERRAIN_MACRO_VARIATION_STRENGTH`
(atof, default e.g. 0.5). Add the getenv block right after the cliff block (:874).

## Recommended insertion (one-line core)

terrain_lod_chunk.frag, after :603:
```
vec3 variedColor = baseColor;  // macro fbm(v_worldPos.xy*lowFreq) tint + abs(macroNz) rock tint
baseColor = mix(baseColor, variedColor, u_macroStrength);  // default 0 = byte-identical
```
