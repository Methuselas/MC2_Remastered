# ⚠️ SUPERSEDED — DO NOT USE AS A DESIGN CONTRACT

**Status (2026-05-02 close-of-session): SUPERSEDED.** This spec went
through three RED adversarial reviews. After the third, the advisor
called the slice over-large and the design "lost trust because it
repeatedly mis-modeled live shader behavior." Specifically: the
existing `gos_terrain.tese` samples the dirt normal map's alpha at a
tiled UV for **per-pixel displacement data**, not a per-class scalar
amplitude — REV2 and REV3 both got this wrong, invalidating the
two-classifier-unification core of the design.

**Replacement direction:** decompose into 5 slices with a recon-only
Slice 0 first. The big sidecar+UBO+new-slots+6-programs design is NOT
the next implementation step. See
[memory/material_palette_session_lessons.md](~/.claude/projects/A--Games-mc2-opengl-src/memory/material_palette_session_lessons.md)
for the full retrospective and the recommended slice ladder.

For the immediate product need (sand on mc2_24, moon on the moon
mission), the tactical path is **option C** from the session: widen
the existing 4-slot HSV classifier windows in `gos_terrain.frag` and
`terrain_common.hglsl` **in lockstep**, no UBO, no sidecar, no new
slots. ~1-2 day fix; structurally bit-for-bit on legacy; doesn't
deliver the per-mission portability goal but doesn't lock that out
either.

This document is preserved as-is for the historical record of what
was tried, why, and what the three reviews found. **Do not extract
implementation guidance from anything below this banner without
re-greping every claim.**

---

# Material palette sidecar — design spec [SUPERSEDED]

**Date:** 2026-05-02
**Status:** Design spec, **revision 3**. Output of brainstorm session
2026-05-02; revised twice in response to adversarial-plan-review:
- REV1 → REV2: first review RED (4 CRITICAL / 7 MAJOR / 4 MINOR);
  six D-decisions accepted by advisor.
- REV2 → REV3: second review RED (3 CRITICAL / 7 MAJOR + NEW-IN-REV2
  regressions); six R-decisions plus six extra requirements accepted
  by advisor with R1 narrowed and R5 rejected (preserves strict legacy
  rollback).

See [Adversarial review handoff](#adversarial-review-handoff) for the
full REV1→REV2→REV3 revision diff.
**Workstream:** Terrain rendering quality / per-mission portability.
**Predecessor:** brainstorm conversation 2026-05-02 (no separate
brainstorm doc — conversation is the brainstorm).
**Next step:** third adversarial-plan-review pass (re-run on REV3 to
catch any second-order regressions introduced by the REV2→REV3 fixes),
then writing-plans skill to produce the implementation plan.

> **Discipline:** every cited symbol grep-verified at write-time per
> worktree CLAUDE.md "Documentation Discipline." Verification appendix
> at end lists each load-bearing citation with `file:line` and status.

---

## TL;DR

Stock MC2 terrain uses a 4+1 hardcoded material classifier in
[gos_terrain.frag:151](../../../shaders/gos_terrain.frag) (rock / grass /
dirt / concrete + snow overlay). Materials are selected per-pixel from
colormap RGB via HSV-region thresholds. There are 4 numbered slots and
no slot for sand (or moon, or any non-Earth-temperate biome). On
mc2_24 (sand-dominant), sand pixels classify ≈ 91% as dirt + ≈ 9% as
rock — close enough hues to reach the dirt window — and render with
**dirt's normal-map / tiling / POM scale**. Dirt's brown-pebble normal
at tiling 1.0 doesn't visually read as sand-rippled detail; the texture
is technically applied but reads as "wrong material" rather than "no
material." Same shader, same engine; just a missing per-biome
specialization slot.

This spec adds a per-mission **material palette sidecar** — an optional FIT
file colocated with each mission's burnin colormap that overrides per-slot
material data (texture, tint, tiling, normal-boost, POM scale, classifier
HSV window, enable flag). Six classifier slots (up from 4) plus the
existing snow overlay weight. The killswitch (`MC2_MATERIAL_PALETTE=0`,
default) compiles a verbatim-legacy variant: structurally bit-for-bit
identical fallback when default-OFF. The palette-on path's engine
defaults aim to reproduce stock visuals to within a documented drift
envelope (Gate A target: ≤ 1% pixel-class drift on tier1), but that
gate's enforceability requires the Stage 0 drift-measurement tool —
**without that tooling, the gate is aspirational, not enforceable.**
Spec promotes the tooling to a Stage 0 deliverable.

The same architecture solves the moon-map case the user flagged ("reusing
rock currently, needs separate texture") — moon overrides slot 0 with
lunar regolith textures + a full-coverage HSV window.

| # | Decision | One-line why |
|---|---|---|
| D1 | 6 numbered classifier slots + 1 dedicated snow sampler (vs 4+1 today) | Headroom for sand+rock+grass on the same map; uniform numbering simplifies onboarding |
| D2 | Per-mission FIT sidecar `<colormapName>.matpalette.fit` | Mirrors existing `<colormapName>.normalmap.tga` / `.burnin.tga` chokepoint; modder-portable |
| D3 | UBO at binding=4 (vs plain uniform arrays) | 3 programs (frag + 2 tese) need same data; one bind beats three pushes; UBO-namespace binding 4 grep-confirmed free |
| D4 | Single classifier function in `terrain_common.hglsl` | Eliminates today's latent two-classifier divergence (frag vs tese) |
| D5 | Slot 0 = fallback (no positive window by default) | Makes "re-theme this map" = "override slot 0 texture" with no classifier-tuning required |
| D6 | Slot 3 = cement-bit reserved with **smooth-blend preservation** (REV2) | Per-vertex cement flag is load-bearing for road/building rendering; cement-bit is *fractional* in transition tiles per `pureConcrete = smoothstep(2.0, 3.0, TerrainType)`, must preserve `mix()` semantics via per-slot weight accumulator math, not a `step()` |
| D7 | **Palette-terrain-variant-only** sampler relocation (REV3 — narrowed from REV2's "both compile-variants in lockstep") | Legacy terrain variant keeps today's allocation exactly: matNormal0..4 at 5..9, shadow at 9 (overwriting matNormal4 — preserved quirk), dynShadow at 10. Palette terrain variant uses the new allocation: matNormal0..5 at 5..10, matNormalSnow at 11, shadow at 12, dynShadow at 13. Non-terrain shaders unchanged at 9/10. Preserves Gate C strict bit-for-bit on legacy variant |
| D8 | **Compile-time** `#define MC2_MATERIAL_PALETTE` per-program variant; **6 program matrix** (REV3 — bumped from REV2's 2-program claim) | `{legacy, palette} × {main_tess, main_thin, shadow}` = 6 shader programs compiled at engine init. Per-program shader-prefix `#define`. Bind site forks 2-axis (variant × program-role). No runtime branch in hot shader |
| D9 | tese + shadow_tese migrate with frag in v1 (REV2 — explicit) | Frag-only migration preserves the two-classifier bug; the slice exists to retire it. Geometric drift is gated explicitly by Gate A |
| D10 | **Role-based filenames** for new palette-variant files (REV3 — replaces mat5/mat6 off-by-one scheme) | Slots 0-3 reuse existing `mat0..3_normal.tga` (no churn). Slot 4 → NEW `mat_slot4_normal.tga`. Slot 5 → NEW `mat_slot5_normal.tga`. Snow → NEW `mat_snow_normal.tga`. Legacy variant continues to use `mat0..4_normal.tga` unchanged. No file-numbering note, no off-by-one, no slot↔file index ambiguity. Stage 3 deletes the legacy `mat4_normal.tga` |
| D11 | Drift gate is **measured**, not structural (REV2) | Stock-parity claim depends on Stage 0 drift-tooling deliverable existing. Without that tool, the 1% gate is unenforceable. Spec explicitly demands the tooling |
| D12 | C++ named constants `kTerrainMaterialSlotCount`, `kTerrainMaterialTextureUnitBase`, etc. (REV2 + REV3 narrowing — palette-variant only) | Constants used only in palette-variant code paths. Legacy variant continues using literals 5/9/10 unchanged. No drift across compile-variants because they're enforced separately |
| D13 | **Strict Gate C** — killswitch-OFF renders today's exact output including latent quirks (REV3 — replaces REV2 R5 softening) | Legacy variant preserves the matNormal4-overwritten-by-shadow-at-unit-9 collision *exactly*. Any "latent bug fix" (e.g., snow now rendering) is a palette-variant Gate-A concern, not a Gate-C concern. Gate C protects rollback confidence; the killswitch is meaningless if it accidentally fixes pre-existing accidental rendering |
| D14 | **Slot 0 leftover semantics preserved** + water carve-out preserved (REV3 — addresses NEW-5) | After `tc_classifyAll`, before normalization and before cement blending, apply: `weight[0] = max(weight[0], max(0, 1 - max(weight[1], weight[2], weight[4], weight[5])))`. Legacy `w.x = 1 - max(w.y, w.z)` rule generalized to N slots. Also preserve legacy water carve-out (`isWater = smoothstep(0.35, 0.45, h)`) in the new classifier so cyan-water hue regions move into rock as today rather than into nothing |

---

## Background — what's actually broken

### Symptom

mc2_24 mission, daytime, RTS zoom: trees, road, mechs, and rock outcrops
all render with detailed shading. The dominant sandy ground reads as a
uniform painted blur with no bump or relief shading. Other missions
(notably mc2_01) show clear material variation across all terrain types.

### Cause (grep-grounded)

Per-pixel material classification in
[gos_terrain.frag:151-175](../../../shaders/gos_terrain.frag):

```glsl
PREC vec4 getColorWeights(PREC vec3 color) {
    PREC vec3 hsv = rgb2hsv(color);
    PREC float h = hsv.x; PREC float s = hsv.y; PREC float v = hsv.z;
    PREC vec4 w = vec4(0.0);
    w.y = smoothstep(0.10, 0.20, h) * smoothstep(0.10, 0.32, s);   // grass
    w.z = smoothstep(0.17, 0.11, h) * smoothstep(0.10, 0.32, s);   // dirt
    w.x = 1.0 - max(w.y, w.z);                                     // rock = leftover
    w.w = 0.0;                                                     // concrete via cement-bit
    PREC float isWater = smoothstep(0.35, 0.45, h);
    w.x += isWater; w.y *= (1.0 - isWater); w.z *= (1.0 - isWater);
    PREC float total = w.x + w.y + w.z + w.w;
    w = (total < 0.01) ? vec4(1.0, 0.0, 0.0, 0.0) : w / total;
    return w;
}
```

Sand color (yellowish-orange, hue ≈ 0.06–0.10, sat ≈ 0.25–0.40, high value):
- Grass window: `smoothstep(0.10, 0.20, h)` ≈ 0 at h=0.08 → `w.y ≈ 0`.
- Dirt window: `smoothstep(0.17, 0.11, h)` is reverse-direction
  smoothstep, saturating to 1.0 for any h ≤ 0.11. At h=0.08 → 1.0.
  Saturation factor `smoothstep(0.10, 0.32, s)` at sand sat 0.30 ≈ 0.91.
  So `w.z ≈ 0.91`.
- Rock fallback: `w.x = 1 - max(w.y, w.z) ≈ 1 - 0.91 = 0.09`.
- After normalization: ≈ 91% dirt + 9% rock. Sand renders predominantly
  with `mat2_normal.tga` (dirt's normal at tiling=1.0, POMScale=2.5,
  normalBoost=1.1).
- Why this looks wrong: dirt's normal map is authored for brown earth
  with small pebbles and packed-clod relief. Stamped on a yellow-sand
  colormap surface, the wrong frequency of micro-relief reads as
  "blurry uniform," not "wind-rippled sand." The pixel-class
  classification is approximately right ("this is a soft material");
  the per-class material data is wrong ("dirt's normal map is not
  sand's normal map").

Same logic in the tessellation displacement pass via
`tc_getColorWeights` in
[shaders/include/terrain_common.hglsl:14](../../../shaders/include/terrain_common.hglsl) — but
with **different HSV thresholds** (`smoothstep(0.14, 0.17, h)` for grass
vs the frag's `0.10, 0.20`) and a **different fallback** (`vec4(0,0,1,0)`
= dirt vs frag's `vec4(1,0,0,0)` = rock). The tese and frag never agreed
about what sand should be; today's behavior is a render-time accident.

### Why now

Per [memory/visual_preference_knobs.md](memory file index) and the
worktree's stock-install-must-remain-playable rule, this is a long-tail
visual quality fix that doesn't break stock and doesn't depend on mod
content. The user has authored or will author 2K normal+displacement TGAs
for the missing biomes (sand, moon, possibly lava, etc.) and wants a
clean per-mission palette extension that doesn't bloat the shader's
hardcoded classifier.

---

## Architecture

### Slot model (the load-bearing structural change)

The shader exposes **6 generic classifier slots** plus **1 dedicated snow
sampler**. Each generic slot has:

- A texture (normal in RGB, displacement in alpha — same convention as
  current `mat0..3_normal.tga`)
- A tint (vec3) added to the lit material color
- A UV tiling rate (float; UV = base × tiling)
- A normal boost (float, scales the unpacked normal contribution)
- A POM scale (float, scales the parallax depth ray-march)
- An HSV window (lo, hi triplets) — when not the fallback
- An enable flag (boolean — disabled slot contributes 0 to weights)

Snow stays a separate-weight color overlay (today's pattern preserved):

- Texture (`mat_snow_normal.tga`, renamed from current
  [terrtxm2.cpp:2162](../../../mclib/terrtxm2.cpp) `mat4_normal`)
- Tint (vec3)
- Enable flag

### Compile-time variant — `MC2_MATERIAL_PALETTE` killswitch (REV3 — 6-program matrix)

The shader and bind path are split into compile variants via a
shader-prefix `#define MC2_MATERIAL_PALETTE`. **Three terrain program
roles** exist today, each becomes a per-variant pair → **6 programs
total compiled at engine init:**

| Program role | Shader pair | Legacy variant `MC2_MATERIAL_PALETTE=0` | Palette variant `MC2_MATERIAL_PALETTE=1` |
|---|---|---|---|
| Main terrain (tess) | `gos_terrain.{vert,tesc,tese,frag}` | `g_terrainMainLegacy` — verbatim today | `g_terrainMainPalette` — palette path |
| Main terrain (thin) | `gos_terrain_thin.vert` + `gos_terrain.frag` | `g_terrainThinLegacy` — verbatim today | `g_terrainThinPalette` — palette path (frag uses palette UBO + classifier) |
| Shadow terrain | `shadow_terrain.{vert,tesc,tese,frag}` | `g_shadowTerrainLegacy` — verbatim today | `g_shadowTerrainPalette` — palette tese uses `tc_classifyAll`/`tc_displacementAmplitude` |

**Shader prefix construction at engine init.** Each program is built
via the existing shader-prefix mechanism (e.g., `glsl_program::makeProgram`
or `gosRenderMaterial::load` per Stage 0 recon decision) with a prefix
string that injects `#define MC2_MATERIAL_PALETTE 0` or `1`. The
include `terrain_common.hglsl` is **fully `#if`-guarded** (per REV3
extra-1) — legacy variants never see the UBO declaration or any new
classifier symbols.

**Bind site forks on two axes:**

```cpp
// Pseudocode at each terrain bind site:
GLuint program;
if (g_useMaterialPalette) {
    if (renderingThinPath)        program = g_terrainThinPalette;
    else                          program = g_terrainMainPalette;
    bindPaletteSamplers();          // matNormal0..5 + matNormalSnow at units 5..11
    bindPaletteUBO();               // binding=4
    bindShadowSamplersPalette();    // shadowMap=12, dynamicShadowMap=13 (palette terrain only)
} else {
    if (renderingThinPath)        program = g_terrainThinLegacy;
    else                          program = g_terrainMainLegacy;
    bindLegacyMatNormals();         // matNormal0..4 at units 5..9 (today's behavior)
    bindShadowSamplersLegacy();     // shadowMap=9 (overwrites matNormal4 — preserved quirk),
                                    // dynamicShadowMap=10
}
glUseProgram(program);
```

Shadow program selection mirrors at the shadow bind site.

**Why 6 programs (not just 4 or 2):** RED #2 found `gos_terrain_thin.vert` +
`gos_terrain.frag` is a separate program at
[gameos_graphics.cpp:2615-2619](../../../GameOS/gameos/gameos_graphics.cpp);
omitting it leaves the thin path running palette-frag with a
legacy-shaped vertex stage. Shadow program (per D9) migrates classifier
in lockstep; otherwise displacement and shadow displacement diverge.

**Compile cost.** ~3× engine-init compile time vs today (6 programs
where today there are 3-ish). One-time cost; not in any hot loop.

**Rollback story.** The killswitch ON-state (legacy variant) is not
"skip the new code path" — it's **3 separately compiled programs with
no palette code linked in**, bound by separately compiled bind sites,
preserving today's exact unit allocations including the latent
matNormal4-overwritten-by-shadow quirk (per D13). Cannot drift.

### Two layers of palette state (palette-on variant only)

**Layer 1 — engine defaults.** Loaded once at engine init from
`data/textures/mat<N>_normal.tga` for N ∈ {0..5} plus
`data/textures/mat_snow_normal.tga`. Handles cached as static globals,
flagged `setTextureNeverFlush` (precedent
[terrtxm2.cpp:2057](../../../mclib/terrtxm2.cpp)). These represent the
shipping default for slots 0–3 (rock/grass/dirt/concrete), the snow
overlay, and disabled-by-default slots 4–5.

Note D10: during Stage 1 + Stage 2 soak, `mat4_normal.tga` is shipped
**twice** — once at the original name (containing snow content for the
killswitch-OFF path's legacy bind to read) and once as
`mat_snow_normal.tga` (the palette-on path's snow source). Identical
content. Stage 3 deletes the legacy `mat4_normal.tga` once the legacy
program is also deleted.

**Layer 2 — per-mission palette.** `TerrainColorMap` (existing class,
[terrtxm2.h:49](../../../mclib/terrtxm2.h)) gains a `MaterialSlot
matPalette[kTerrainMaterialSlotCount]` member plus snow override fields.
Initialized to engine defaults at `init`. If the per-mission sidecar
`<colormapName>.matpalette.fit` exists alongside the colormap, declared
fields override the per-slot defaults. Sidecar absence ⇒ defaults
remain ⇒ palette-on path renders within the Gate A drift envelope of
legacy behavior. Sidecar absence + `MC2_MATERIAL_PALETTE=0` ⇒
structurally identical to today.

### Data flow (palette-on variant) (REV3)

```
                            engine init:
                            6 shader programs compiled —
                              terrainMainLegacy   (MC2_MATERIAL_PALETTE=0)
                              terrainMainPalette  (MC2_MATERIAL_PALETTE=1)
                              terrainThinLegacy   (MC2_MATERIAL_PALETTE=0)
                              terrainThinPalette  (MC2_MATERIAL_PALETTE=1)
                              shadowTerrainLegacy (MC2_MATERIAL_PALETTE=0)
                              shadowTerrainPalette(MC2_MATERIAL_PALETTE=1)

data/textures/mc2_24.matpalette.fit    (optional sidecar)
        |
        v
TerrainColorMap::init(fileName)        (terrtxm2.cpp:1960+ extension)
        |
        v
matPalette[kTerrainMaterialSlotCount]  (defaults + sidecar overrides)
        |
        v
glBufferSubData(MaterialPaletteUBO)    (one upload per mission)
        |
        v
glBindBufferRange(GL_UNIFORM_BUFFER, 4, ...)
        |
        v
bind site (gameos_graphics.cpp), per-pass:
  axis 1: g_useMaterialPalette ? palette : legacy
  axis 2: tess vs thin (vs shadow at the shadow bind site)
        |
        v
selected program executes.
  - palette variant: terrain_common.hglsl (palette branch) supplies UBO,
    tc_classifyAll, tc_displacementAmplitude. Frag uses 6-slot mix +
    leftover-rule slot 0. Cement-bit weight accumulator preserves mix().
  - legacy variant: terrain_common.hglsl (legacy branch) supplies the
    pre-REV3 tc_getColorWeights. Frag uses today's getColorWeights.
    Verbatim today's rendering including the matNormal4-overwritten-
    by-shadow-at-unit-9 quirk.
```

When `MC2_MATERIAL_PALETTE=0` (legacy variant compile), the UBO
declaration, palette classifier, and palette samplers are
`#if`-eliminated from the program. UBO upload still happens engine-side
(harmless if no program reads it), or — cleaner — gated behind the
runtime flag so when killswitch is OFF the engine does not allocate
or upload the UBO at all. Stage 0 recon decides.

---

## File formats

### Sidecar — `<colormapName>.matpalette.fit`

Lives at `data/textures/<colormapName>.matpalette.fit` alongside the
existing `<colormapName>.burnin.tga` and `<colormapName>.normalmap.tga`.
Optional. Parsed via existing `FitIniFile` infrastructure (used by other
MC2 config files; no new parser needed).

Per-slot block, all fields optional. Missing field ⇒ engine default.

```ini
[Slot2]
TextureName = sand_normal               # loads data/textures/sand_normal.tga
Tint        = 0.78,0.69,0.50            # RGB triplet, 0..1
Tiling      = 2.0
NormalBoost = 1.4
POMScale    = 0.6
HueLo       = 0.06
SatLo       = 0.18
ValLo       = 0.40
HueHi       = 0.12
SatHi       = 0.55
ValHi       = 1.00
Enabled     = true

[Slot1]
Enabled     = false                     # disable grass entirely on this mission

[Snow]
Enabled     = false                     # no snow on a desert map
```

Field values above are **illustrative**, not normative. The HSV window
in particular is tuned per-mission against the actual colormap pixels —
plan/author work, not spec work. The example demonstrates the file
shape, not the exact m24 values.

If a mission needs to keep dirt AND introduce sand as **distinct**
materials (rather than re-skinning slot 2), use slot 4 or 5 (the
disabled-by-default generic slots) for sand and let slot 2 keep dirt's
defaults. The architecture supports both patterns; choice is per-mission
ergonomics.

Each block is independent. Mission with one biome change authors one
block; mission with no overrides authors no file at all.

### Engine default normal/displacement TGAs

**REV3:** Per D10 + D13, `mat4_normal.tga` is preserved unchanged on
disk (legacy variant continues binding it as snow at unit 9, exactly
as today). Slots 4 and 5 of the palette variant use **role-based
filenames** that don't conflict with the legacy mat<N>_normal.tga
sequence — eliminating the REV2 off-by-one trap entirely.

| File | Stage 1+2 state | Stage 3 final state | Used for (palette variant) | Used for (legacy variant) |
|---|---|---|---|---|
| `mat0_normal.tga` | unchanged | unchanged | Slot 0 (rock) | Slot 0 (rock) — unchanged |
| `mat1_normal.tga` | unchanged | unchanged | Slot 1 (grass) | Slot 1 (grass) — unchanged |
| `mat2_normal.tga` | unchanged | unchanged | Slot 2 (dirt) | Slot 2 (dirt) — unchanged |
| `mat3_normal.tga` | unchanged | unchanged | Slot 3 (concrete) | Slot 3 (concrete) — unchanged |
| `mat4_normal.tga` | **unchanged** (snow content for legacy variant's bind to unit 9) | **deleted** in same PR as legacy programs | not read | matNormal4 = snow (today's quirk: overwritten by shadow at unit 9 — preserved per D13) |
| `mat_slot4_normal.tga` | **NEW** (flat-blue placeholder OR usable generic) | unchanged from Stage 1 | Slot 4 (NEW generic, disabled by default) | not read |
| `mat_slot5_normal.tga` | **NEW** (flat-blue placeholder OR usable generic) | unchanged from Stage 1 | Slot 5 (NEW generic, disabled by default) | not read |
| `mat_snow_normal.tga` | **NEW** (copy of `mat4_normal.tga` content) | unchanged from Stage 1 | Snow overlay | not read |
| `sand_normal.tga` | NEW | unchanged | Sidecar-referenced (m24) — **author authors** as 2K TGA | not read |
| `moon_normal.tga` | NEW | unchanged | Sidecar-referenced (moon map) — **author authors** as 2K TGA | not read |
| (future biomes) | author-time | unchanged | Sidecar-referenced | not read |

**No file-numbering off-by-one.** Slot index in the palette variant
maps to a stable role name, not a sequential file number. If a future
slot 6 is added, it becomes `mat_slot6_normal.tga`. Authors and the
sidecar parser never need to track a "slot N → file N+1" remapping.

For slot 4 / slot 5 disabled-by-default state, no engine texture is
strictly required (the slot simply contributes 0 to weight). For
robustness, ship a flat-blue (0.5, 0.5, 1.0, 0.0) placeholder so a
sidecar that enables the slot without specifying TextureName still
gets a well-defined neutral texture rather than uninitialized GL state.

### TGA dimension policy (REV3 — per R4)

- **Engine default normal/displacement TGAs** (the `mat<N>_normal.tga`
  and `mat_slot<N>_normal.tga` files) follow the existing per-loader
  dimension constraint at
  [terrtxm2.cpp:2191-2196](../../../mclib/terrtxm2.cpp): all loaded
  defaults must share `arrayWidth`. Plan stage's loader extension
  preserves this constraint (existing convention; no asset churn).
- **Sidecar-referenced override TGAs** (`sand_normal.tga`,
  `moon_normal.tga`, etc.) load through `mcTextureManager->loadTexture()`
  which does NOT enforce shared dimensions. Authors ship 2K (or
  arbitrary square) per their authoring needs.
- The two paths are independent: the engine default array stays
  uniform-width; per-mission override textures live outside that
  constraint.

---

## Shader contract

### Common include — `terrain_common.hglsl` (REV3 — full `#if MC2_MATERIAL_PALETTE` guards)

Single source of truth for both compile variants. Both tese programs
already `#include` this file
([gos_terrain.tese:110](../../../shaders/gos_terrain.tese),
[shadow_terrain.tese:40](../../../shaders/shadow_terrain.tese) call
`tc_getColorWeights` from it). The frag does not currently include it
and must start to (palette-variant adds the include; legacy variant's
frag does not need to change its include list — see "Frag" below for
the variant-specific frag changes).

**REV3 contract:** every new symbol introduced for the palette path
(UBO declaration, `tc_classifySlot`, `tc_classifyAll`,
`tc_displacementAmplitude`, `MAT_SLOT_COUNT`) is wrapped in
`#if MC2_MATERIAL_PALETTE` ... `#endif`. The legacy `tc_getColorWeights`
function is wrapped in `#if !MC2_MATERIAL_PALETTE` ... `#endif`.
Result: each compile-variant program sees exactly one classifier
function; the other branch is preprocessor-eliminated.

```glsl
// terrain_common.hglsl (illustrative; final form pinned by plan)

vec3 tc_rgb2hsv(vec3 c) { /* unchanged from existing — used by both variants */ }

#if MC2_MATERIAL_PALETTE

// ============================================================
// PALETTE VARIANT (MC2_MATERIAL_PALETTE = 1) — REV3 design
// ============================================================

const int MAT_SLOT_COUNT = 6;

layout(std140, binding = 4) uniform MaterialPaletteUBO {
    vec4 tintTiling [MAT_SLOT_COUNT];   // .xyz=tint, .w=tiling
    vec4 shade      [MAT_SLOT_COUNT];   // .x=normalBoost, .y=pomScale, .z=enabled (0/1), .w=pad
    vec4 hsvLo      [MAT_SLOT_COUNT];   // .x=hueLo, .y=satLo, .z=valLo, .w=pad
    vec4 hsvHi      [MAT_SLOT_COUNT];   // .x=hueHi, .y=satHi, .z=valHi, .w=pad
    vec4 snowTint;                      // .xyz=tint, .w=enabled
} matPalette;

float tc_classifySlot(int i, vec3 hsv) {
    vec3 lo = matPalette.hsvLo[i].xyz;
    vec3 hi = matPalette.hsvHi[i].xyz;
    float wH = smoothstep(lo.x, lo.x + 0.02, hsv.x) *
               (1.0 - smoothstep(hi.x - 0.02, hi.x, hsv.x));
    float wS = smoothstep(lo.y, lo.y + 0.04, hsv.y) *
               (1.0 - smoothstep(hi.y - 0.04, hi.y, hsv.y));
    float wV = smoothstep(lo.z, lo.z + 0.04, hsv.z) *
               (1.0 - smoothstep(hi.z - 0.04, hi.z, hsv.z));
    return wH * wS * wV * matPalette.shade[i].z;  // *enabled
}

void tc_classifyAll(vec3 color, out float weight[MAT_SLOT_COUNT]) {
    vec3 hsv = tc_rgb2hsv(color);

    // 1. Per-slot classifier (slot 0 has empty window by default → 0 here)
    for (int i = 0; i < MAT_SLOT_COUNT; i++) {
        weight[i] = tc_classifySlot(i, hsv);
    }

    // 2. Water carve-out (REV3 D14, R-extra-3 — preserve legacy behavior).
    //    Legacy: cyan-water hue regions are added to rock and zeroed from
    //    grass/dirt. Preserve here so stock missions don't shift water-edge
    //    pixels into nothing.
    float isWater = smoothstep(0.35, 0.45, hsv.x);
    weight[0] += isWater;
    weight[1] *= (1.0 - isWater);
    weight[2] *= (1.0 - isWater);
    // (slots 4/5 by convention not in the water-vs-not classification —
    //  they're authored for non-water biomes; if a future biome is ocean-
    //  themed, the sidecar HSV window for that slot is the right place
    //  to handle it, not the water carve-out.)

    // 3. Slot 0 leftover rule (REV3 D14, R-extra-2).
    //    Legacy: w.x = 1 - max(w.y, w.z). Generalized to N slots:
    //    slot 0 always claims (1 - max of color-classifier slots).
    //    Slot 3 is excluded — it's the cement-bit target, not a color
    //    classifier. Slots 4/5 are included so they don't accidentally
    //    starve slot 0.
    float strongestColorSlot = max(max(weight[1], weight[2]),
                                    max(weight[4], weight[5]));
    weight[0] = max(weight[0], max(0.0, 1.0 - strongestColorSlot));

    // 4. Normalize. After leftover rule, total is guaranteed > 0 (slot 0
    //    contributes at least the leftover share); the total<0.01 fallback
    //    shouldn't fire, but kept for safety.
    float total = 0.0;
    for (int i = 0; i < MAT_SLOT_COUNT; i++) total += weight[i];
    if (total < 0.01) {
        for (int i = 0; i < MAT_SLOT_COUNT; i++)
            weight[i] = (i == 0) ? 1.0 : 0.0;
    } else {
        for (int i = 0; i < MAT_SLOT_COUNT; i++)
            weight[i] /= total;
    }
    // Cement-bit blend (D6) is applied IN THE FRAG (see "Frag" section)
    // AFTER tc_classifyAll returns — it's a per-fragment override that
    // depends on TerrainType (varying), not on color.
}

float tc_displacementAmplitude(float weight[MAT_SLOT_COUNT]) {
    float amp = 0.0;
    for (int i = 0; i < MAT_SLOT_COUNT; i++)
        amp += weight[i] * matPalette.shade[i].y;    // *pomScale
    return amp;
}

#else  // !MC2_MATERIAL_PALETTE

// ============================================================
// LEGACY VARIANT (MC2_MATERIAL_PALETTE = 0) — verbatim today
// ============================================================

vec4 tc_getColorWeights(vec3 color) {
    // ... existing pre-REV3 implementation (unchanged from today) ...
}

#endif // MC2_MATERIAL_PALETTE
```

Both legacy and palette variants of all 6 programs include this file
unconditionally; the `#if` guards ensure each variant sees only its
own classifier. UBO declaration is `#if`-eliminated from legacy
programs entirely — they neither declare nor read it.

### Frag — `gos_terrain.frag` (palette variant; legacy variant unchanged from today)

Today's per-pixel logic at
[gos_terrain.frag:371-525](../../../shaders/gos_terrain.frag) calls
`getColorWeights(colAvg)` and uses `matWeights.x..w` to index into the
hardcoded `matNormal0..3` samplers, fixed `matTiling`,
`pomScaleMat`, `normalBoost`, and per-channel tints. The legacy
compile-variant of `gos_terrain.frag` retains all of this unchanged
(under `#if !MC2_MATERIAL_PALETTE`). The palette compile-variant
under `#if MC2_MATERIAL_PALETTE`:

- Delete `getColorWeights` (palette variant only — legacy variant
  retains it).
- `#include` `terrain_common.hglsl` (already included by tese; frag
  starts including it now — both variants OK because the include is
  fully `#if`-guarded).
- Replace `vec4 matWeights` with `float matWeight[MAT_SLOT_COUNT]`,
  populated by `tc_classifyAll(colAvg, matWeight)`. Per the palette-
  variant `tc_classifyAll` body, this already applies:
  1. per-slot HSV-window classifier
  2. water carve-out (legacy isWater preserved)
  3. **slot-0 leftover rule** (REV3 D14): `weight[0] = max(weight[0],
     1 - max(weight[1], weight[2], weight[4], weight[5]))` BEFORE
     normalization
  4. normalization

- **Cement-bit override (REV2 D6) — applied IN THE FRAG, AFTER
  `tc_classifyAll` returns.** Today's
  [gos_terrain.frag:389](../../../shaders/gos_terrain.frag) does
  `matWeights = mix(matWeights, vec4(0,0,0,1), pureConcrete)` — a
  geometric blend, not a hard set. `pureConcrete` is *fractional* in
  (0, 1) at boundary tiles per
  [:384](../../../shaders/gos_terrain.frag)
  `pureConcrete = smoothstep(2.0, 3.0, TerrainType)`. The new design
  preserves the blend with a per-slot weight accumulator equivalent
  to `mix()`:

  ```glsl
  // In gos_terrain.frag main(), under #if MC2_MATERIAL_PALETTE:
  float matWeight[MAT_SLOT_COUNT];
  tc_classifyAll(colAvg, matWeight);   // already applies leftover + water carve-out + normalize

  // Cement-bit blend (preserves mix() semantics):
  for (int i = 0; i < MAT_SLOT_COUNT; i++) {
      matWeight[i] *= (1.0 - pureConcrete);
  }
  matWeight[3] += pureConcrete;        // slot 3 = concrete
  // Math: mix(w, vec_e3, p) = (1-p)*w + p*e3.
  // Accumulator: w[i] *= (1-p) for all, then w[3] += p.
  // Total: sum((1-p)*w_i) + p = (1-p)*1 + p = 1. Preserved.
  // Equivalent to mix() across all slots.
  ```

  `pureConcrete` ALSO continues to drive
  [gos_terrain.frag:391](../../../shaders/gos_terrain.frag)
  (snow suppression),
  [:501](../../../shaders/gos_terrain.frag) (detailN attenuation),
  [:573](../../../shaders/gos_terrain.frag) (normalLight blend) —
  those uses are unchanged in both variants.

- **Order of operations is load-bearing** (REV3 R-extra-2):
  classify → water carve-out → slot-0 leftover → normalize → cement
  blend. The cement blend MUST come after normalization because it
  preserves total=1; if applied earlier, normalization would re-shuffle
  it. The slot-0 leftover MUST come before normalization because the
  legacy `w.x = 1 - max(w.y, w.z)` is computed against unnormalized
  weights.
- Per-slot mix loops over 6 slots. GLSL 4.3 forbids dynamic sampler
  indexing — fan out via `switch (i)` to bound `matNormal0..matNormal5`
  samplers. Static unrolling at compile time.
- Tile, POM scale, normal-boost, tint per slot read from
  `matPalette.tintTiling[i]` / `matPalette.shade[i]`.
- **Grass NormalBoost preservation (REV2).** Today
  [gos_terrain.frag:466](../../../shaders/gos_terrain.frag) does
  `normalBoost.y *= fwGrass * grassNormalFade` — a per-fragment AA
  modulation specific to slot 1 (grass). Sidecar-overridable base
  value lives in `matPalette.shade[1].x`; the shader applies the
  per-fragment modulation **on top of** that base when reading slot 1's
  effective normalBoost. Pseudo-code:
  ```glsl
  float effectiveBoost = matPalette.shade[i].x;
  if (i == 1) {
      effectiveBoost *= fwGrass * grassNormalFade;
  }
  ```
  Slot 1 keeps its grass-specific AA forever; spec acknowledges the
  asymmetry. Other slots (rock, dirt, sand, etc.) do not get fragment-
  derivative AA modulation under this design. Future: a per-slot
  `appliesAAFade` UBO flag could generalize, but that's v2.
- Snow path unchanged in shape (still a separate weight derived from
  high-V/low-S color overlay rule); reads `matPalette.snowTint` for
  tint and enable flag.

### Sampler unit allocation (REV3 — per-variant + scope-narrowed shadow relocation)

**Today's terrain-bind allocation** (verified by re-grepping
[gameos_graphics.cpp:3573-3811](../../../GameOS/gameos/gameos_graphics.cpp)):

| Unit | Today's terrain bind | Source |
|---|---|---|
| 5 | matNormal0 (rock) | `glUniform1i(tl.matNormal[0], 5+0)` at :3575/:3674/:3781 |
| 6 | matNormal1 (grass) | same sites with i=1 |
| 7 | matNormal2 (dirt) | same with i=2 |
| 8 | matNormal3 (concrete) | same with i=3 |
| 9 | matNormal4 (snow) **THEN OVERWRITTEN** by shadowMap | matNormal bind at :3573-:3578; shadow bind at :3587 |
| 9 | shadowMap (static) — **overwrites matNormal4 every frame when shadows on** | :3587 |
| 10 | dynamicShadowMap | :3597 (or near) |

The `matNormal4 → unit 9` bind today is partially a no-op: when
shadows are enabled (the default), shadow overwrites it; when shadows
are disabled (RAlt+F3 toggle, MAJ-4), matNormal4 IS bound and DOES
contribute to snow rendering.

**Non-terrain shadow binds** (separate code path serving decal,
gos_grass, gos_tex_vertex_lighted, terrain_overlay programs that
include `shadow.hglsl`):

| Unit | Today | Source |
|---|---|---|
| 9 | shadowMap (static) | `gameos_graphics.cpp:5374` (and equivalent sites) |
| 10 | dynamicShadowMap | `gameos_graphics.cpp:5702` (and equivalent sites) |

These remain at units 9/10 in both variants per **R1 narrowed** —
they are NOT touched by this slice.

### REV3 allocation — per-variant terrain bindings

| Unit | Legacy terrain variant (`MC2_MATERIAL_PALETTE=0`) | Palette terrain variant (`MC2_MATERIAL_PALETTE=1`) |
|---|---|---|
| 5 | matNormal0 (rock) — unchanged | matNormal0 (rock) |
| 6 | matNormal1 (grass) — unchanged | matNormal1 (grass) |
| 7 | matNormal2 (dirt) — unchanged | matNormal2 (dirt) |
| 8 | matNormal3 (concrete) — unchanged | matNormal3 (concrete) |
| 9 | **matNormal4 (snow) THEN shadowMap** — preserved quirk per D13 | matNormal4 (slot 4 generic, disabled by default) |
| 10 | **dynamicShadowMap** — preserved at 10 today | matNormal5 (slot 5 generic, disabled by default) |
| 11 | (unused) | matNormalSnow (snow overlay) |
| 12 | (unused) | **shadowMap** (relocated for terrain palette program only) |
| 13 | (unused) | **dynamicShadowMap** (relocated) |

**Critical D13 invariant:** the legacy terrain variant's bind path is
**verbatim today's bind path** — same units, same overwrite quirk,
same draw output. The shadow-overwrites-matNormal4 collision is
preserved. Gate C verifies this strictly; any "latent bug fix" of the
collision is a palette-variant Gate-A concern, not a Gate-C concern.

**Why per-variant relocation (not lockstep):** REV2 proposed moving
shadow on both variants — REV3 narrows this. Reasons:
1. Lockstep relocation would change legacy-variant rendering
   (matNormal4 / snow would no longer be overwritten when shadows on,
   producing visually different output than today). Violates Gate C
   strict claim.
2. Bind-site fork is already required for the program selection
   (D8 6-program matrix); adding shadow-unit fork to the same fork is
   trivial extra cost.
3. Legacy variant is dead-code at Stage 3 anyway — there's no
   long-term cost to its slightly weirder bind allocation.

**Shadow program's matNormal2 bind site** at
[gameos_graphics.cpp:3338](../../../GameOS/gameos/gameos_graphics.cpp)
(REV2 RED MINOR-3, REV3 confirmed):

- Legacy shadow variant: unchanged. Continues binding matNormal2 at
  unit 7 (or wherever today's shadow program reads it from — Stage 0
  recon item to pin the exact unit and confirm).
- Palette shadow variant: replaces single-sampler bind with full
  6-slot UBO+sampler bind (palette tese uses `tc_classifyAll` which
  reads the UBO; sampler binds for matNormal0..5 at units 5..10 plus
  matNormalSnow at unit 11 follow the same allocation as palette
  main terrain).

**Sampler-unit headroom check.** Stage 0 verifies
`GL_MAX_TEXTURE_IMAGE_UNITS ≥ 14` (palette variant uses up to unit 13;
modern AMD/NVidia ≥ 32; sanity check, not blocker).

### Tese — `gos_terrain.tese`, `shadow_terrain.tese`

Today both tese programs read `matWeights = tc_getColorWeights(colSample)`
and use `matWeights.z` (dirt) as the displacement amplitude. After the
change:

- Both call `tc_classifyAll(colSample, weight)`.
- Both call `tc_displacementAmplitude(weight)` for the displacement
  scale.
- Identical helper used in both ⇒ shadow geometry tracks visible
  geometry by construction.
- Stock parity preserved: dirt's default POMScale (2.5 per
  [gos_terrain.frag:179](../../../shaders/gos_terrain.frag)) dominates
  the weighted sum on m01-class missions.

---

## Engine integration

### Default-texture loader extension (REV3 — two parallel loaders)

Today's engine default loader at
[terrtxm2.cpp:2161-2178](../../../mclib/terrtxm2.cpp) declares **two
parallel** 5-element name arrays AND a hardcoded "optional cutoff":

```cpp
const char* normalNames[5] = { "mat0_normal", ..., "mat4_normal" };
const char* dispNames[5]   = { "mat0_displacement", ..., "mat4_displacement" };
for (int mat = 0; mat < 5; mat++) {
    // load normalNames[mat] and dispNames[mat] together
    // mat >= 4 is treated as optional (skip-if-missing)
}
```

**REV3: Two parallel loaders.** Engine init runs both legacy and
palette default-texture loaders. Each populates its own handle array.
The bind site (per D8 fork) selects which array's handles to bind.

```cpp
// Legacy loader — unchanged from today's code
const char* legacyNormalNames[5] = { "mat0_normal", "mat1_normal",
                                       "mat2_normal", "mat3_normal",
                                       "mat4_normal" };
const char* legacyDispNames[5]   = { "mat0_displacement", "mat1_displacement",
                                       "mat2_displacement", "mat3_displacement",
                                       "mat4_displacement" };
for (int mat = 0; mat < 5; mat++) {
    // load into g_legacyMatNormalHandles[mat] / g_legacyMatDispHandles[mat]
    // mat >= 4 is optional (skip-if-missing) — preserved
}

#if MC2_MATERIAL_PALETTE_ENABLED   // C++ build flag, not shader flag
// Palette loader — runs only in builds that include the palette path
const char* paletteNormalNames[6] = { "mat0_normal", "mat1_normal",
                                        "mat2_normal", "mat3_normal",
                                        "mat_slot4_normal", "mat_slot5_normal" };
const char* paletteDispNames[6]   = { "mat0_displacement", "mat1_displacement",
                                        "mat2_displacement", "mat3_displacement",
                                        "mat_slot4_displacement", "mat_slot5_displacement" };
for (int slot = 0; slot < 6; slot++) {
    // load into g_paletteMatNormalHandles[slot] / g_paletteMatDispHandles[slot]
    // slot >= 4 is optional (snow / sand / etc. — these may legitimately be missing
    //   on minimal installs)
}
// Snow texture loaded explicitly (separate from the slot array):
//   load "mat_snow_normal.tga" → g_paletteMatNormalSnowHandle
//   load "mat_snow_displacement.tga" → g_paletteMatDispSnowHandle (optional)
#endif
```

Both loaders preserve existing conventions: same `setTextureNeverFlush`
flag (precedent [terrtxm2.cpp:2057](../../../mclib/terrtxm2.cpp)),
same all-same-width constraint per the existing
[terrtxm2.cpp:2191-2196](../../../mclib/terrtxm2.cpp) check (per R4 —
engine defaults stay uniform width; sidecar overrides are independent).

**Slots 0–3 of the palette loader REUSE the existing
`mat<N>_normal.tga` files** — no churn for slots that don't change.
Slot 4 / 5 use `mat_slot4_normal.tga` / `mat_slot5_normal.tga` which
are NEW files (per D10). Snow uses `mat_snow_normal.tga` which is
ALSO a new file (copy of `mat4_normal.tga`'s snow content per D10).

**Why two loaders rather than one with conditional logic.** A single
loader with conditional skip-or-load logic per variant would be
fragile (the slot index → file index relationship would need a remap
table, and the legacy variant's optional-cutoff at mat>=4 conflicts
semantically with the palette variant's optional-cutoff at slot>=4
since "slot 4" means different files in the two variants). Two
parallel loaders run on engine init, populate separate arrays, and
the bind site is the only fork point. No remap table needed.

**Plan stage decision** (Stage 0 recon item — see below): whether
displacement ships as separate `_displacement.tga` files OR collapses
into the alpha channel of `_normal.tga`. The frag comment at
[:179](../../../shaders/gos_terrain.frag) ("Alpha channel = displacement
map for per-material POM") suggests slots 0..3 already use alpha-channel
displacement. If so, palette variant slots 4/5 follow the same pattern
and `paletteDispNames[]` is unused.

### C++ named constants (REV3 — D12 narrowed: palette-variant scope only)

Stage 1 introduces in a shared header (e.g.,
`mclib/terrain_material_constants.h`):

```cpp
// All constants below describe the PALETTE variant's allocation only.
// Legacy variant continues using literal 5 / 9 / 10 unchanged in
// existing code paths — those paths are deleted at Stage 3.
constexpr int   kTerrainMaterialSlotCount         = 6;
constexpr int   kTerrainMaterialTextureUnitBase   = 5;
// Derived for palette variant:
constexpr int   kTerrainMaterialSnowUnit          = kTerrainMaterialTextureUnitBase + kTerrainMaterialSlotCount; // 11
constexpr int   kPaletteShadowMapUnit             = kTerrainMaterialSnowUnit + 1;  // 12
constexpr int   kPaletteDynamicShadowMapUnit      = kPaletteShadowMapUnit + 1;     // 13
constexpr int   kMaterialPaletteUBOBinding        = 4;
```

**Constants used in palette-variant C++ code paths only.** The
legacy-variant code paths in `gameos_graphics.cpp` keep their existing
literals (5/9/10) verbatim — D13 strict Gate C demands no change to
the legacy bind path. After Stage 3 (legacy delete), the literals are
removed along with the legacy code; only the constants remain.

**Plan-stage edit map (palette-variant scope):**

- `gameos_graphics.cpp` palette-variant matNormal bind loops use
  `kTerrainMaterialTextureUnitBase + i` for i ∈ 0..(slot count - 1)
- Palette-variant snow bind uses `kTerrainMaterialSnowUnit`
- Palette-variant shadow binds use `kPaletteShadowMapUnit` /
  `kPaletteDynamicShadowMapUnit`
- Palette-variant uniform-location arrays (`palette*` mirrors of
  `terrainLocs_.matNormal[]` etc.) sized via `kTerrainMaterialSlotCount`
- Palette-variant UBO bind uses `kMaterialPaletteUBOBinding`
- Shader prefix string for palette variants injects
  `#define MAT_SLOT_COUNT N` where N = `kTerrainMaterialSlotCount`,
  ensuring C++↔shader cross-language match per I7

**Legacy-variant edit map: NONE.** Legacy code stays exactly as today.
At Stage 3 the entire legacy code block is deleted in one PR — no
in-place migration of literal 5/9/10 to constants.

**Plan stage grep targets** (palette-variant code paths only):
literals `5`, `9`, `10` inside palette-variant code blocks (under
`#if MC2_MATERIAL_PALETTE_ENABLED` or in palette-only files).
Anywhere else, leave alone — it's either an unrelated 5/9/10 or
legacy code that will be deleted whole-cloth at Stage 3.

### Per-mission palette state

`TerrainColorMap` ([terrtxm2.h:49](../../../mclib/terrtxm2.h)) gains:

```cpp
struct MaterialSlot {
    DWORD            textureNodeIndex;   // 0xffffffff = use engine default for this slot
    Stuff::Vector3   tint;
    float            tiling;
    float            normalBoost;
    float            pomScale;
    Stuff::Vector3   hsvLo;
    Stuff::Vector3   hsvHi;
    bool             enabled;
};
class TerrainColorMap {
    // ... existing ...
    MaterialSlot     matPalette[6];
    Stuff::Vector3   snowTint;
    bool             snowEnabled;
    GLuint           paletteUBO;          // owned; lifecycle = mission
    bool             paletteDirty;        // re-upload on next bind if true
};
```

### `TerrainColorMap::init(fileName)` extension (REV2 — name-fallback chain + dual callsite scope)

**Both `terrainTextures2->init()` callsites are covered.** Per RED
MAJOR-2, the engine reaches `TerrainColorMap::init` from two sites:
[terrain.cpp:540-543](../../../mclib/terrain.cpp) (initial mission load)
and [terrain.cpp:820-823](../../../mclib/terrain.cpp) (mission reset/
reload). Both use the same effective name resolution. By placing the
matpalette load **inside** `TerrainColorMap::init` itself (single
chokepoint), both callers automatically pick up the sidecar — no
duplicated logic outside `init`.

**Filename derivation mirrors the normalmap pattern at
[terrtxm2.cpp:1963-1968](../../../mclib/terrtxm2.cpp)** (RED MAJOR-3).
Use the same three-tier fallback chain:

```cpp
const char* paletteName = Terrain::colorMapName  ? Terrain::colorMapName
                        : Terrain::terrainName   ? Terrain::terrainName
                                                 : fileName;
char paletteFileName[1024];
sprintf(paletteFileName, "%s.matpalette", paletteName);
FullPathFileName palettePath;
palettePath.init(texturePath, paletteFileName, ".fit");
```

This guarantees the sidecar lookup uses the same effective name as the
colormap and normalmap — missions with mission-specific colormap
overrides find their sidecar automatically.

**Loader sequence** — appends after `detail_normal.tga` load
(palette-on compile-variant only — under `#if MC2_MATERIAL_PALETTE`):

1. Initialize `matPalette[i]` for i ∈ 0..5 to engine defaults (the
   table in §"Engine defaults").
2. Initialize `snowTint` / `snowEnabled` to engine defaults.
3. Compute `paletteFileName` per the fallback chain above. If
   `fileExists(palettePath)` returns true:
   1. Open via `FitIniFile`.
   2. For each `[SlotN]` block (N ∈ 0..5):
      - Read declared fields (use the FitIniFile API surface — Stage 0
        recon item to confirm available `readId*` methods for vec3,
        float, bool).
      - Update `matPalette[N]` only for fields present (others keep
        defaults).
      - For `TextureName = X`: load `data/textures/X.tga` via
        `mcTextureManager->loadTexture(...)` with same flags as
        normalmap path; set `setTextureNeverFlush`. Store handle in
        `matPalette[N].textureNodeIndex`.
   3. For `[Snow]` block: read `Tint`, `Enabled`.
   4. Apply loader-enforced invariants (Section 6).
4. Allocate `paletteUBO` if not yet allocated; mark `paletteDirty = true`.

When `MC2_MATERIAL_PALETTE=0` (legacy compile-variant), this entire
block is `#if`-eliminated. `TerrainColorMap` doesn't carry the palette
struct, doesn't allocate the UBO, doesn't do the file lookup.

### `TerrainColorMap::destroy` extension

For each `matPalette[i]` where `textureNodeIndex != 0xffffffff` and
differs from the engine default for that slot: release the texture via
`mcTextureManager`. Free `paletteUBO`. Engine defaults outlive missions.

### Shader bind path (REV3 — 2-axis fork)

Bind sites today (verified):
- 3 terrain-bind sites in
  [gameos_graphics.cpp:3573-3811](../../../GameOS/gameos/gameos_graphics.cpp)
  (`:3574`, `:3673`, `:3780`) where `matNormal[0..4]` get bound at
  units 5..9. Each block has its own shadowMap/dynamicShadowMap bind
  at ~`:3587, :3597` (+2 parallel sites).
- The **thin** path is one of these 3 (per Stage 0 recon — pin
  exactly which `:3673` corresponds to thin vs main).
- Shadow program's matNormal2 bind at
  [gameos_graphics.cpp:3338](../../../GameOS/gameos/gameos_graphics.cpp).
- Non-terrain shadow binds at `:5374, :5702` — **NOT TOUCHED** per R1.

**Bind-site fork — 2-axis (variant × program-role):**

```cpp
// At each terrain bind site (main, thin, or shadow):
GLuint program;
if (g_useMaterialPalette) {
    // ===== Palette variant =====
    if (programRole == ROLE_MAIN_TESS)   program = g_terrainMainPalette;
    if (programRole == ROLE_MAIN_THIN)   program = g_terrainThinPalette;
    if (programRole == ROLE_SHADOW)      program = g_shadowTerrainPalette;
    glUseProgram(program);

    bindPaletteSamplers();   // matNormal0..5 at units 5..10
                             // matNormalSnow at unit 11
    bindPaletteUBO();        // UBO binding 4
    bindPaletteShadow();     // shadowMap=12, dynamicShadowMap=13 (terrain only)
} else {
    // ===== Legacy variant (verbatim today) =====
    if (programRole == ROLE_MAIN_TESS)   program = g_terrainMainLegacy;
    if (programRole == ROLE_MAIN_THIN)   program = g_terrainThinLegacy;
    if (programRole == ROLE_SHADOW)      program = g_shadowTerrainLegacy;
    glUseProgram(program);

    bindLegacyMatNormals();  // matNormal0..4 at units 5..9 — unchanged today
    bindLegacyShadow();      // shadowMap=9 (overwrites matNormal4 — preserved
                             //   quirk per D13), dynamicShadowMap=10
    // NO UBO bind on legacy path. The palette UBO is not declared by legacy
    // programs and not allocated when killswitch is OFF.
}
```

**`bindPaletteSamplers()` (palette variant only):**

1. Loop `i ∈ 0..(kTerrainMaterialSlotCount - 1)`:
   - Determine effective texture handle: if
     `terrainTextures2->matPalette[i].textureNodeIndex != 0xffffffff`,
     use it (after `tex_resolve` per
     [memory/mc2_texture_handle_is_live.md](memory file)); else use
     `g_paletteMatNormalHandles[i]` (engine default for that slot).
   - `glActiveTexture(GL_TEXTURE0 + kTerrainMaterialTextureUnitBase + i)`,
     `glBindTexture(GL_TEXTURE_2D, handle)`.
2. `glActiveTexture(GL_TEXTURE0 + kTerrainMaterialSnowUnit)` (= 11),
   bind `g_paletteMatNormalSnowHandle` (engine default for v1;
   spec-extensible to per-mission override later).

**`bindPaletteUBO()` (palette variant only):**

1. If `paletteDirty`,
   `glBufferSubData(paletteUBO, 0, sizeof(MaterialPaletteUBO), &data)`.
2. `glBindBufferRange(GL_UNIFORM_BUFFER, kMaterialPaletteUBOBinding,
   paletteUBO, 0, sizeof(MaterialPaletteUBO))`.
3. Set `paletteDirty = false`.

**`bindPaletteShadow()` (palette variant only — terrain programs only):**

1. `glActiveTexture(GL_TEXTURE0 + kPaletteShadowMapUnit)` (= 12),
   bind static shadow tex.
2. `glActiveTexture(GL_TEXTURE0 + kPaletteDynamicShadowMapUnit)` (= 13),
   bind dynamic shadow tex.
3. Update palette terrain programs' `shadowMap` / `dynamicShadowMap`
   uniform locations to point to units 12 / 13 respectively.
4. Non-terrain programs' shadow uniform locations remain pointing to
   units 9 / 10. Their bind sites at `:5374, :5702` are NOT changed.

**`bindLegacyMatNormals()` / `bindLegacyShadow()`:** verbatim today's
code — three bind sites (`:3573-:3578` matNormal loop, `:3587` shadow,
`:3597` dynShadow, plus parallel sites in the other terrain blocks).
No edit. Stage 3 deletes these alongside the legacy variant.

Per [memory/deferred_vs_direct_uniforms.md](memory file): UBO bind is
a direct GL call, not deferred. Mirrors the existing SSBO bind pattern
used by the indirect-terrain bridge.

### UBO binding choice — load-bearing (REV2 — namespace clarified)

**Binding = 4 in the UBO namespace.** OpenGL maintains separate
binding namespaces for UBOs and SSBOs (same integer, different
target).

**UBO bindings in use today** (grep `layout(.*std140.*binding\s*=`):

| UBO Binding | Used by | Source |
|---|---|---|
| 1 | `mesh_data` | shaders/gos_tex_vertex_lighted.vert:12 |
| (Stage 0 re-greps for any others — there are likely UBOs in include files like `lighting.hglsl` / `scene.hglsl` per RED-pass observation) | — | — |

**SSBO bindings in use today** (grep `layout(.*std430.*binding\s*=`):

| SSBO Binding | Used by | Source |
|---|---|---|
| 0 | `QuadRecordBuf`, `Instances` | gos_terrain.tesc:34, static_prop.vert:17 |
| 1 | `RecipeBuf`, `Colors` | gos_terrain_thin.vert:18, static_prop.vert:18 |
| 2 | `ThinRecordBuf*` | gos_terrain_thin.vert:9, gos_terrain.frag:90 |
| 5 | `WaterRecipeBuf` | gos_terrain_water_fast.vert:30 |
| 6 | `WaterThinBuf` | gos_terrain_water_fast.vert:48 |

**UBO binding 4 is unused** in the UBO namespace as far as currently
verified. Plan stage re-greps both namespaces at implementation time —
allocation can drift, and Stage 0 must enumerate any UBOs in shared
include files (lighting/scene) that the prior REV2 verification
table missed.

---

## Engine defaults (the stock-parity table)

Lifted verbatim from current shader constants. When no sidecar is
present, these reproduce today's render:

| # | Role | Texture | Tint (RGB) | Tiling | NormalBoost | POMScale | Enabled | HSV window |
|---|---|---|---|---|---|---|---|---|
| 0 | Rock (fallback) | mat0_normal.tga | (0.36, 0.37, 0.40) | 3.0 | 0.9 | 1.0 | true | empty (no positive window — fallback rule catches leftover) |
| 1 | Grass | mat1_normal.tga | (0.35, 0.42, 0.25) | 12.0 | 1.1 | 1.0 | true | hue [0.10, 0.30], sat [0.10, 1.0], val [0.0, 1.0] |
| 2 | Dirt | mat2_normal.tga | (0.48, 0.42, 0.33) | 1.0 | 1.1 | 2.5 | true | hue [0.0, 0.17], sat [0.10, 1.0], val [0.0, 1.0] |
| 3 | Concrete | mat3_normal.tga | (0.55, 0.53, 0.50) | 6.0 | 2.5 | 1.0 | true | bypassed (cement-bit forced) |
| 4 | (generic) | (none / flat-blue placeholder) | (0,0,0) | 1.0 | 1.0 | 1.0 | **false** | n/a |
| 5 | (generic) | (none / flat-blue placeholder) | (0,0,0) | 1.0 | 1.0 | 1.0 | **false** | n/a |

**Snow special path defaults:**

| Field | Default value |
|---|---|
| Texture | mat_snow_normal.tga (renamed from current `mat4_normal.tga`) |
| Tint | (0.75, 0.78, 0.84) |
| Enabled | true |

Sources:
- Tints — [gos_terrain.frag:516-520](../../../shaders/gos_terrain.frag)
- Tiling — [gos_terrain.frag:423](../../../shaders/gos_terrain.frag) (`matTiling = vec4(3.0, 12.0, 1.0, 6.0)`)
- POMScale — [gos_terrain.frag:179](../../../shaders/gos_terrain.frag) (`pomScaleMat = vec4(1.0, 1.0, 2.5, 1.0)`)
- NormalBoost — [gos_terrain.frag:455](../../../shaders/gos_terrain.frag) (`normalBoost = vec4(0.9, 1.1, 1.1, 2.5)`)
- HSV windows — derived from
  [gos_terrain.frag:162-164](../../../shaders/gos_terrain.frag) with
  bounded-window adjustment (drift surface documented below)

### Drift surface vs legacy classifier (REV3 — leftover rule preserved, two minor regions)

The killswitch-OFF (legacy compile-variant) path is structurally
bit-for-bit identical to today by construction (D8 + D13). The
killswitch-ON (palette-on) path with no sidecars deployed has these
known drift sources vs legacy:

1. **(Frag) High-hue greens (h > 0.30).** Legacy
   `smoothstep(0.10, 0.20, h)` for grass saturates to 1.0 above
   h=0.20 and is unbounded above (claims grass for everything
   bluer-cyan). New design clamps at h=0.30. Stock impact: limited
   to cyan-greens, which the existing `isWater` carve-out
   ([gos_terrain.frag:167](../../../shaders/gos_terrain.frag) — also
   preserved per D14) already redirects independently. Expected
   pixel-class drift: well under 1%.
2. **(Frag) Smoothstep edge shape.** Legacy edges are wider (~0.10
   hue). New uses 0.02 hue / 0.04 sat edge widths. Net effect:
   slightly crisper biome borders (visible at the few-pixel transition
   zones between grass and dirt). Should average into the per-mission
   visual canary noise floor.
3. **(Tese) Geometric displacement drift (REV2 — RED MAJOR-5).** Today
   `gos_terrain.tese` and `shadow_terrain.tese` use
   `tc_getColorWeights` from `terrain_common.hglsl` whose HSV
   thresholds **differ** from `gos_terrain.frag`'s. After migration
   to the unified palette classifier, both tese programs see the
   palette classifier's thresholds, and per-fragment `dirtWeight`
   changes. Since this drives vertex displacement via
   `tc_displacementAmplitude`, **stock terrain geometry shifts** on
   missions where the legacy two classifiers disagreed about a pixel's
   class. Shadow geometry moves with it. Visible artifact: subtle
   silhouette differences on tessellated displacement edges.

**(REV3 NEW-5 RESOLVED — slot 0 leftover rule preserved):** Earlier
spec drafts had slot 0 with empty HSV window claiming pixels only via
the `total < 0.01` fallback — a substantively different classifier
semantic from legacy's `w.x = 1 - max(w.y, w.z)`. **REV3 fixes this.**
The palette-variant `tc_classifyAll` applies
`weight[0] = max(weight[0], 1 - max(weight[1], weight[2], weight[4],
weight[5]))` BEFORE normalization (per D14 + R-extra-2). Stock pixel
classification is preserved near-exactly: any pixel where legacy gave
"rock = leftover" still gets the same numeric weight in palette-on,
modulo the smoothstep-edge-shape drift in #2.

**(REV3 R-extra-3 ALSO RESOLVED — water carve-out preserved):**
Legacy `isWater = smoothstep(0.35, 0.45, h)` adds cyan-water hue to
slot 0 (rock) and zeros it from slots 1/2 (grass/dirt). Palette
variant preserves this carve-out inside `tc_classifyAll` per D14.
No drift on water-hue regions.

**Why we accept tese drift in v1** (per D9): keeping tese on
`tc_getColorWeights` legacy preserves the very two-classifier bug
this slice exists to retire. Migrating tese in v2 would multiply the
parity-gate surface. Bundling in v1 means one parity-gate run, one
tier1 baseline update.

### Acceptance bar (REV2 — measured gate, not structural claim)

**Without the Stage 0 drift-tooling deliverable, the parity bar is
unenforceable.** This is honest: pixel-class drift cannot be
eyeballed from screenshots reliably. Spec promotes the tool to a
Stage 0 deliverable (per D11):

- **Stage 0 builds a drift-measurement tool** that:
  - Captures per-pixel matWeights debug-mode-5 frame from legacy
    compile-variant on each tier1 mission.
  - Captures the same from palette-on compile-variant with no sidecar
    deployed.
  - Computes per-pixel argmax(weight) class-mismatch fraction.
  - Reports per-mission percentage AND aggregate.
  - **Note:** Gate A0 requires a debug-mode-5-equivalent on the
    LEGACY variant too — i.e., the legacy variant ships a debug mode
    that outputs `matWeights.xyz` as RGB so the comparison is
    apples-to-apples. Today's debug-mode-4 (per
    [gos_terrain.frag:413](../../../shaders/gos_terrain.frag))
    already does this for the legacy classifier. Stage 0 verifies
    the legacy and palette debug-mode outputs use compatible formats.
- **Gate A target (Stage 2 promotion):** ≤ 1% pixel-class drift on
  each tier1 mission. With REV3's leftover-rule + water-carve-out
  preservation, this should be *much* easier to hit than REV2 set up
  (the previous "leftover semantics retired" drift category that
  worried the second adversarial review is removed).
- **Tese geometric drift target (Stage 2 promotion):** harder to
  measure pixel-for-pixel because vertex positions affect rasterized
  output non-linearly. Use **silhouette-edge difference** as a proxy:
  compute per-pixel depth-buffer diff between legacy and palette-on,
  count pixels with depth difference > some-threshold (e.g.,
  `TERRAIN_DEPTH_FUDGE * 2`). Acceptance: < 0.5% of pixels with
  significant depth shift. Stage 0 plan grades whether the metric is
  feasible or whether eyeball gate is the realistic best.

**If the tooling can't reach the per-pixel argmax drift number** (e.g.,
because debug-mode-5 capture infrastructure doesn't exist or can't be
plumbed cleanly), Gate A falls back to **eyeball** comparison of
visual canary screenshots — and the spec must downgrade the "1%"
language entirely. Stage 0 finds out which.

---

## Invariants

The loader enforces these. Plan stage exposes each as a unit-testable or
log-assertable check.

| # | Invariant | Enforcement |
|---|---|---|
| I1 | Slot 0 is always enabled | Loader silently coerces `[Slot0] Enabled = false` to `true` and warns |
| I2 | Slot 3 is always enabled | Same coercion (cement-bit target — see I4) |
| I3 | Slot 0 is the fallback target via leftover rule (REV3 — strengthened) | `tc_classifyAll` applies `weight[0] = max(weight[0], 1 - max(weight[1], weight[2], weight[4], weight[5]))` BEFORE normalization. Slot 0 always claims at least the unclaimed-by-color-classifiers share. The legacy `vec4(1,0,0,0)` fallback when total<0.01 is kept as a safety net but is unreachable after the leftover rule fires |
| I4 | Slot 3's cement-bit override preserves `mix()` semantics (REV2) | Shader applies the per-slot weight accumulator in §"Frag" — `weight[i] *= (1.0 - pureConcrete); weight[3] += pureConcrete;` — NOT a binary set. Sidecar may declare slot 3's HSV window for non-cement contribution; cement-bit drives the override at flagged tiles |
| I5 | POMScale ≥ 0.01 for any enabled slot | Loader clamps |
| I6 | HSV window: `lo < hi` per channel | Loader: if degenerate, disables slot and warns |
| I7 | `kTerrainMaterialSlotCount` matches C++ ↔ shader (REV2) | Single source of truth in `mclib/terrain_material_constants.h`; shader prefix string includes `#define MAT_SLOT_COUNT N` constructed from the C++ constant at engine init. Plan stage adds a build-time grep check that the `#define` and the C++ constant match |
| I8 | UBO binding=4 is unique in the UBO namespace (REV2) | Plan-stage grep `layout(.*std140.*binding\s*=` across `shaders/` AND across `shaders/include/`. SSBO binding 4 is independent and out of scope for this invariant |
| I9 | **Killswitch-OFF (legacy variant) renders STRICTLY bit-for-bit identical to today, including latent quirks (REV3 — strengthened from REV2)** | Structural: legacy compile-variant contains verbatim today's classifier, today's bind site, today's sampler allocation (matNormal4 at unit 9 overwritten by shadowMap at unit 9 — preserved per D13). Any "latent bug fix" that would change legacy output is a Gate-A-on-palette-variant concern, not a Gate-C concern. Gate C must verify: no visible difference between today's render and legacy-variant render |
| I10 | Killswitch-ON (palette compile-variant, no sidecar) renders within Gate A drift envelope of legacy (REV2) | **Measured** — depends on Stage 0 drift-tooling deliverable. ≤ 1% pixel-class drift on tier1 + < 0.5% silhouette-edge depth shift, OR eyeball acceptance if tooling proves infeasible. Not a structural guarantee. REV3 makes this *easier* to hit because slot-0 leftover and water carve-out are preserved |
| I11 | Palette variant sampler unit allocation contiguous and collision-free at compile time (REV3 — narrowed) | `kTerrainMaterialTextureUnitBase = 5`, slots 0..(N-1) at units 5..(5+N-1), snow at unit 5+N, palette-shadow at units 5+N+1, 5+N+2. Stage 0 grep verifies no other texture binds at those units WITHIN palette-variant programs. Legacy variant's allocation is NOT covered by I11 — it's protected by I9 instead |
| I12 | **Slot-0 leftover rule applied before normalization (REV3 — D14)** | `tc_classifyAll` body explicitly orders: classify → water carve-out → slot-0 leftover → normalize. Cement blend happens IN THE FRAG after `tc_classifyAll` returns. Order verified by reading the include source and frag main() |
| I13 | **Water carve-out preserved (REV3 — D14)** | `tc_classifyAll` includes `isWater = smoothstep(0.35, 0.45, hsv.x)` and applies `weight[0] += isWater; weight[1,2] *= (1 - isWater)` exactly as legacy. Stage 0 grep verifies the carve-out is in the palette branch |
| I14 | **`terrain_common.hglsl` is fully `#if MC2_MATERIAL_PALETTE` guarded (REV3 — extra-1)** | Every palette-only symbol (UBO declaration, `tc_classifySlot`, `tc_classifyAll`, `tc_displacementAmplitude`, `MAT_SLOT_COUNT`) under `#if MC2_MATERIAL_PALETTE` ... `#endif`. Legacy `tc_getColorWeights` under `#if !MC2_MATERIAL_PALETTE`. Plan-stage grep verifies no palette symbol leaks into legacy compile and vice versa |
| I15 | **Six terrain shader programs maintained (REV3 — D8)** | Engine init compiles `{legacy, palette} × {main_tess, main_thin, shadow}`. Bind site fork selects on g_useMaterialPalette × programRole. C++ unit-tests assert all 6 program handles are non-zero after init |

Violation of I1, I2, I3, I4, I7, I8, I11, I12, I13, I14, I15 =
engine-correctness failure.
Violation of I5, I6 = warn + repair (loader fixes silently with log).
Violation of **I9** (REV3 strict) = killswitch broken — must
re-bisect and re-spec before any merge. Rollback story is meaningless
if I9 fails.
Violation of I10 = Stage 2 promotion blocker; re-tune defaults or
accept eyeball gate after Stage 0 tooling infeasibility.

---

## Error handling

| Failure mode | Behavior |
|---|---|
| Sidecar file doesn't exist | Silent — use defaults (the parity invariant) |
| Sidecar parse error | Log `[MATERIAL_PALETTE v1] event=parse_error mission=<name>`; populate from defaults |
| `[SlotN]` block where N ∉ 0..5 | Log warning; ignore block |
| `TextureName = X` but `data/textures/X.tga` missing | Log warning; fall back to engine default for that slot |
| HSV window degenerate (lo ≥ hi) | Disable slot via I6; log warning |
| Texture wrong dimensions | `mcTextureManager` rejects → loader treats as missing-file |
| `[Snow] Enabled = false` | Honored — desert maps may legitimately disable snow |
| `[Slot0] Enabled = false` | I1 coerces to true; log warning |
| `[Slot3] Enabled = false` | I2 coerces to true; log warning |

---

## Debug instrumentation (per CLAUDE.md "Debug Instrumentation Rule for reworks")

This change touches render path + per-mission lifecycle. Instrumentation
lands in the same commit as the feature; gated off by default; demoted
not deleted after stabilization.

### CPU side — env-gated `[MATERIAL_PALETTE v1]` lifecycle prints

Env var: `MC2_MATERIAL_PALETTE_TRACE=1`

```
[MATERIAL_PALETTE v1] event=load mission=mc2_24 sidecar=present slots_overridden=1 textures_loaded=1
[MATERIAL_PALETTE v1] event=load mission=mc2_03 sidecar=absent slots_overridden=0 textures_loaded=0
[MATERIAL_PALETTE v1] event=missing_texture path=sand_normal slot=2 mission=mc2_24
[MATERIAL_PALETTE v1] event=invariant_violation slot=0 field=enabled coerced=true mission=<name>
[MATERIAL_PALETTE v1] event=destroy mission=mc2_24
```

**Always-on** (no env flag required):
- Startup banner addition: `[INSTR v1] enabled: ... material_palette=1 slots=6 ubo_binding=4`
- 600-frame summary line counting overrides applied this mission and
  missing-texture events
- First missing-texture per `(path, slot)` pair always logs once

### Shader side — extended debug visualization

`tessDebug.x` already drives debug modes (existing precedent — mode 4
outputs `matWeights.xyz` as RGB). Extend:

| Mode | Visualization |
|---|---|
| 0 | Off (production) |
| 4 (existing) | `matWeights.xyz` as RGB (legacy classifier output) |
| 5 (NEW) | `weight[0..2]` as RGB (rock=R, grass=G, dirt=B) |
| 6 (NEW) | `weight[3..5]` as RGB (concrete=R, slot4=G, slot5=B) |
| 7 (NEW) | argmax(weight) tinted by per-slot tint — "what material claimed this pixel, colored by its tint" |

Plan stage picks an unused `RAlt+<digit>` hotkey per CLAUDE.md "Debug
hotkeys" inventory.

---

## Parity gates (the ship gates) (REV2 — three gates including tooling)

### Gate A0 — drift-measurement tool exists and works (Stage 0 deliverable, REV2)

Before Gate A can run, build:

```
scripts/material_palette_drift.py
  --legacy-screenshots <dir>      # captured from MC2_MATERIAL_PALETTE=0 build
  --palette-screenshots <dir>     # captured from MC2_MATERIAL_PALETTE=1 build, no sidecars
  --debug-mode 5                  # matWeights output mode
  --output drift_report.json
```

Pass criteria for Gate A0:
- Tool runs end-to-end on captures from a tier1 mission.
- Reports per-pixel argmax(weight) class-mismatch fraction.
- Optionally reports silhouette-edge depth-shift fraction (for tese
  drift; harder, may be deferred).
- Output values are reproducible across re-runs of the same captures.

**If Gate A0 cannot ship** (e.g., debug-mode-5 capture infrastructure
turns out to require new shader plumbing not feasible in Stage 0),
spec downgrades to **eyeball-only Gate A** and explicitly removes the
"≤ 1%" language from I10 / TL;DR / drift surface. Stage 0 plan
records this decision.

### Gate A — stock parity (no sidecars deployed)

Per [memory/feedback_smoke_mission_filter.md](memory file) and
[memory/feedback_smoke_no_canary.md](memory file):

```bash
py -3 scripts/run_smoke.py --tier tier1 --duration 20 --kill-existing
```

Pass criteria:
- Exit 0 across all 5 tier1 missions (mc2_01, mc2_03, mc2_10, mc2_17,
  mc2_24).
- Per-mission visual canary screenshots vs pre-change baseline.
- **If Gate A0 shipped:** drift report shows ≤ 1% pixel-class drift on
  each (I10 measured-gate variant).
- **If Gate A0 downgraded:** human reviewer signs off that no visible
  regression exists across the 5 tier1 captures (I10 eyeball-gate
  variant).
- **Tese geometric drift:** if A0 reports it, < 0.5% silhouette-edge
  depth shift; if not, eyeball comparison of camera-pan-near-displacement
  in mc2_03 (most-tessellated stock mission).

If drift exceeds the bar on any tier1 mission, engine defaults need
re-tuning (probably widening grass/dirt windows). Recipe: read
colormap pixels, plot HSV histogram, set window edges at the natural
valley. Re-run Gate A.

### Gate B — override parity (`mc2_24.matpalette.fit` deployed with sand override)

Single-mission iteration loop:

```bash
py -3 scripts/run_smoke.py --tier tier1 --duration 20 --filter mc2_24 --kill-existing
```

Pass criteria:
- mc2_24 still passes smoke.
- Visual eyeball: sand tiles render with sand normal/tint/tiling, not
  the prior flat-dirt rendering.
- Visual eyeball: surrounding rock/grass tiles classified correctly
  (sand override didn't poison neighbors).
- Debug-mode 5 visualization confirms `weight[2]` non-zero on m24's
  sand pixels and on m01's dirt pixels (proves the slot-2 override on
  m24 is the texture/tint diff, not a classifier-poisoning event).

### Gate C — killswitch parity (REV3 — STRICT — D13)

After Stage 1 ships:

```bash
# Build with default-OFF; legacy variant runs
py -3 scripts/run_smoke.py --tier tier1 --duration 20 --kill-existing
```

Pass criteria:
- Tier1 exits 0.
- **Visual canary screenshots match pre-Stage-1 baseline EXACTLY** at
  the pixel-shift level. Includes shadow-on AND shadow-off renders
  (matNormal4-overwritten-by-shadowMap collision must be preserved
  per D13).
- D10 verification: `mat4_normal.tga` content unchanged on disk
  during Stages 1+2.
- Legacy variant produces today's render including the shadow/snow
  unit-9 collision. **Any visible difference is a Gate C failure.**
- Eyeball comparison of pre-Stage-1 vs post-Stage-1 captures across
  all 5 tier1 missions, both shadow-on and shadow-off.

**Gate C is strictly stricter than Gate A.** Gate A allows the
documented drift envelope on the palette variant. Gate C does NOT.
The killswitch is rollback insurance — if it visibly differs from
today's render, the rollback is meaningless and Stage 1 must be
re-bisected.

Any "latent bug fix" that the palette variant might introduce
(e.g., snow rendering correctly when shadows are off — REV2 considered
this a positive side-effect) is **explicitly NOT a Gate C concern**.
Such fixes appear only on the palette variant; the legacy variant
preserves today's quirks intact.

---

## Stage / ship ladder

Modeled on the renderwater fastpath / indirect-terrain slice cadence
([memory/renderwater_fastpath_stage2.md](memory file),
[memory/indirect_terrain_solid_endpoint.md](memory file)):

### Stage 0 — recon + tooling (during plan write) (REV2 — expanded)

**Code recon items:**

- Pin all three terrain-bind sites in
  [gameos_graphics.cpp:3574-3781](../../../GameOS/gameos/gameos_graphics.cpp)
- Pin shadow program's matNormal2 site at
  [gameos_graphics.cpp:3338](../../../GameOS/gameos/gameos_graphics.cpp)
- Pin shadow-unit literal sites (`shadowMap` and `dynamicShadowMap`
  binds at units 9, 10) — find every `glActiveTexture` / `glUniform1i`
  involving shadow textures
- Pin engine-default texture loader site at
  [terrtxm2.cpp:2161-2178](../../../mclib/terrtxm2.cpp); confirm
  parallel `dispNames[5]` array structure
- Pin BOTH `terrainTextures2->init()` callsites at
  [terrain.cpp:540-543](../../../mclib/terrain.cpp) and
  [terrain.cpp:820-823](../../../mclib/terrain.cpp)
- Re-grep UBO bindings (`layout(.*std140.*binding\s*=`) to confirm 4
  still free in UBO namespace
- Re-grep SSBO bindings (`layout(.*std430.*binding\s*=`) for the
  separate-namespace verification table
- Confirm `gos_terrain.frag` does not yet `#include
  "include/terrain_common.hglsl"` (grep `#include` lines)
- Determine `FitIniFile` API surface for reading vec3 / float / bool
  fields (grep `readId*` methods on the class hierarchy)
- Identify all callers of `getColorWeights` (frag) for deletion sweep
- Verify `GL_MAX_TEXTURE_IMAGE_UNITS ≥ 14` on AMD RX 7900 XTX
  (per D7 — sanity check)
- Verify `pureConcrete` is fractional in practice on stock missions
  with cement-bit boundaries — Stage 0 captures a debug-mode-4
  screenshot of mc2_03 (urban) with the cement-bit value visualized,
  confirms transition tiles exist with `pureConcrete ∈ (0.1, 0.9)`

**Tooling deliverable (D11 — new for REV2):**

- Build `scripts/material_palette_drift.py` per Gate A0 spec
  - Captures matWeights debug-mode-5 frames from both compile variants
  - Computes per-pixel argmax(weight) class-mismatch fraction
  - Output: per-mission percentage + aggregate
  - Demonstrate end-to-end on mc2_01 + mc2_24 capture pair
- If Gate A0 builds infeasible during Stage 0 (e.g., debug-mode capture
  needs deeper hooks), document why and downgrade Gate A target to
  eyeball-only **before Stage 1 opens**
- Decide and document: tese silhouette-edge depth-shift metric or
  eyeball fallback for that drift surface

**Spec-side deliverable (D12 — new for REV2):**

- Spec out `mclib/terrain_material_constants.h` content with the
  `kTerrainMaterialSlotCount = 6`, `kTerrainMaterialTextureUnitBase = 5`,
  derived snow/shadow unit constants
- Plan stage enumerates every literal `5`, `9`, `10` in
  `gameos_graphics.cpp` matching terrain-material context, and in
  `terrtxm2.cpp` matching default-texture context, that gets replaced
  by the named constant in Stage 1

### Stage 1 — implementation behind killswitch (default OFF) (REV3 — 6-program matrix)

`MC2_MATERIAL_PALETTE=0` (default at Stage 1) — bind site selects
legacy programs (3 of them: main_tess, main_thin, shadow). All
verbatim today.
`MC2_MATERIAL_PALETTE=1` — bind site selects palette programs (3 of
them with new path).

**All of these land in the same PR (no partial landing):**

Shader changes:
- `terrain_common.hglsl` rewrite with full `#if MC2_MATERIAL_PALETTE`
  guards (per I14):
  - Palette branch: UBO declaration at binding=4, `tc_classifySlot`,
    `tc_classifyAll` (with water carve-out + slot-0 leftover rule
    + normalize per I12+I13), `tc_displacementAmplitude`
  - Legacy branch: existing `tc_getColorWeights` unchanged
- Frag (`gos_terrain.frag`):
  - Palette branch: `#include` palette `terrain_common.hglsl`,
    use `tc_classifyAll` + per-slot mix loops with cement-bit
    weight accumulator math (D6) and grass NormalBoost preserved (M6)
  - Legacy branch: unchanged
- Tese (`gos_terrain.tese`, `shadow_terrain.tese`):
  - Palette branch: use `tc_classifyAll` + `tc_displacementAmplitude`
  - Legacy branch: continue calling `tc_getColorWeights` unchanged
- Shader prefix string injects `#define MC2_MATERIAL_PALETTE 0` or
  `1` per program-variant pair

Engine changes:
- `mclib/terrain_material_constants.h` (new file, D12) — palette-
  variant constants only
- `TerrainColorMap` palette state + load/destroy (palette-only via
  `#if MC2_MATERIAL_PALETTE_ENABLED` C++ guard, OR runtime-conditional
  with the UBO unallocated when killswitch is OFF — Stage 0 picks)
- Sidecar parser (in `TerrainColorMap::init` chokepoint; both callsites
  at terrain.cpp:540-543 + 820-823 covered automatically)
- Filename derivation uses the colormap-name fallback chain (M3)
- **Two parallel default loaders** at `terrtxm2.cpp:2161+`:
  - Legacy loader unchanged
  - Palette loader new (6 entries with `mat0..3_normal` reused +
    `mat_slot4_normal` + `mat_slot5_normal`; plus separate snow load
    `mat_snow_normal`)
- `gameos_graphics.cpp`:
  - **Legacy code paths unchanged** — preserves D13 strict Gate C
  - **NEW palette code paths** added in parallel:
    - `g_paletteMatNormalHandles[6]` storage
    - Palette uniform-location arrays for matNormal0..5 + matNormalSnow
      (mirrors of existing terrainLocs_/thinTerrainLocs_ patterns)
    - `bindPaletteSamplers()`, `bindPaletteUBO()`,
      `bindPaletteShadow()` helper functions
    - Bind-site fork on `g_useMaterialPalette` × `programRole` (per D8)
    - 6 program handles compiled at engine init from prefix-different
      sources
- Engine init compiles all 6 terrain programs (legacy ×3 + palette ×3)
- Debug instrumentation (`MC2_MATERIAL_PALETTE_TRACE` env var, debug
  visualization modes 5/6/7 — palette variant only; legacy variant
  retains existing modes including mode 4)

Asset changes (per D10):
- Ship NEW `mat_slot4_normal.tga` (slot 4 placeholder)
- Ship NEW `mat_slot5_normal.tga` (slot 5 placeholder)
- Ship NEW `mat_snow_normal.tga` (copy of `mat4_normal.tga`'s
  current snow content)
- `mat4_normal.tga` itself **unchanged on disk**
- Sample sidecar `mc2_24.matpalette.fit` (with sand override per Gate
  B example)
- New `_displacement.tga` companions if displacement-as-separate-files
  is the convention (alpha-channel-of-normal alternative per Stage 0
  recon)

Tooling:
- `scripts/material_palette_drift.py` if Gate A0 shipped from Stage 0

**Stage 1 verification:**
- Tier1 runs default-OFF — Gate C verifies legacy variant unchanged
  STRICTLY (per D13). Includes shadow-on AND shadow-off captures.
- Developer iterates with `=1` + Gate B mc2_24 single-mission canary
  (sand renders correctly with sidecar)
- Gate A (palette no-sidecar drift) measured (or eyeballed if A0
  infeasible) against the documented envelope

### Stage 2 — default flip (default ON)

After clean tier1+m24-override soak with `=1` AND clean Gate C with
`=0`. Default flips. Both compile variants retained.

Pre-conditions:
- Gate A passes (per A0-tooling-or-eyeball)
- Gate B passes (sand renders distinctly on m24)
- Gate C passes (legacy variant identical to today)
- ≥ 1 week of regular play across multiple stock missions

Mod content per [memory/feedback_offload_scope_stock_only.md](memory
file) is **out of parity scope** — stock parity is the merge gate;
mod fitness is diagnostic-only signal.

### Stage 3 — legacy delete

After additional ~1 week of clean Stage 2 soak. Same PR deletes:
- Legacy compile-variant of `gos_terrain.frag` (the `getColorWeights`
  function, the `matTiling` / `pomScaleMat` / `normalBoost` /
  per-channel tint constants, the `#if !MC2_MATERIAL_PALETTE` branch)
- `tc_getColorWeights` legacy function in `terrain_common.hglsl`
- Legacy compile-variant of tese + shadow_tese programs
- The `MC2_MATERIAL_PALETTE` flag itself (always palette-on)
- C++ legacy program allocations in `gameos_graphics.cpp`:
  - `g_terrainMainLegacy`, `g_terrainThinLegacy`, `g_shadowTerrainLegacy`
    program handles deleted
  - Legacy bind-site code (`bindLegacyMatNormals`, `bindLegacyShadow`)
    deleted
  - Legacy `terrain_mat_normal_[5]` + `setTerrainMaterialNormal` +
    legacy `terrainLocs_.matNormal[]` + legacy
    `thinTerrainLocs_.matNormal[]` storage all deleted
  - Bind-site fork collapses to single-program path
- Legacy default-texture loader at `terrtxm2.cpp:2161+` deleted (the
  palette loader becomes the only loader)
- `mat4_normal.tga` file deleted (snow content lives at
  `mat_snow_normal.tga` only — D10 final state). All 5 legacy
  `mat<N>_normal.tga` files for slots 0..3 are KEPT (palette variant
  uses them via the 6-element palette loader).

**Stage 1 and Stage 2 must NOT be split across separate PRs that
ship a default-flip without confirming Gate A + Gate B + Gate C all
pass.** That's the partial-landing hazard rule per plan-v2 brief
precedent.

**Stage 1 and Stage 0 dependencies:** Stage 0 tooling and recon
deliverables MUST land before Stage 1's first commit — without them
the spec's gates are unenforceable. If Stage 0 takes longer than
expected, Stage 1 waits.

---

## Open recon items (Stage 0 work) (REV2 — expanded)

The plan session opens with these grep-bounded recon items. Section
"Stage 0" above also lists code-recon and tooling deliverables —
those are duplicated here for grep-friendliness.

1. **Bind site inventory.** Confirm three terrain bind sites in
   [gameos_graphics.cpp:3574-3781](../../../GameOS/gameos/gameos_graphics.cpp);
   confirm shape of state save/restore each one needs (color mask,
   depth, blend, sampler) so new texture binds + UBO bind don't leak
   state across passes.
2. **Shadow program bind site.** Confirm the
   [gameos_graphics.cpp:3338](../../../GameOS/gameos/gameos_graphics.cpp)
   `matNormal2` site is the only place shadow_terrain.tese reads
   material normals; if more, list them.
3. **Shadow-unit literal sites.** Grep every `glActiveTexture` and
   `glUniform1i` involving shadow textures (today at units 9, 10).
   Plan stage moves them to units 12, 13 in lockstep on BOTH compile
   variants per D7.
4. **UBO binding 4 freshness in UBO namespace.** Re-grep
   `layout(.*std140.*binding\s*=` across `shaders/` immediately before
   implementation lands. Allocation may have drifted.
5. **SSBO binding 4 cross-check.** Re-grep
   `layout(.*std430.*binding\s*=` for the verification table; confirm
   binding 4 is also free in SSBO namespace (not strictly required —
   namespaces are independent — but conventional cleanliness).
6. **Frag include status.** Grep `gos_terrain.frag` for `#include`
   directives — if `terrain_common.hglsl` is not yet in the list, the
   plan must add it as a Stage 1 step (palette-variant only).
7. **`getColorWeights` callers.** Grep `getColorWeights` across
   `shaders/` to confirm only the frag references it (so deletion in
   Stage 3 is safe).
8. **`tc_getColorWeights` callers.** Confirm exactly two
   (gos_terrain.tese, shadow_terrain.tese) — any additional caller
   needs the migration too.
9. **FitIniFile API.** Confirm `readId*` methods needed for vec3 (or
   3 floats), float, bool, optional fields. Test the parser against a
   sample sidecar.
10. **`detail_normal.tga` lifecycle interaction.** The existing detail
    normal at [terrtxm2.cpp:2072](../../../mclib/terrtxm2.cpp) is
    engine-default, NOT mission-overridable in this spec's v1.
    Confirm no sidecar field conflicts with its loading.
11. **Cement-bit signal source.** Pin the per-vertex flag drives the
    shader's `pureConcrete` value at
    [gos_terrain.frag:384-389](../../../shaders/gos_terrain.frag);
    confirm the new weight-accumulator preserves all four uses
    (lines 389, 391, 501, 573).
12. **`pureConcrete` is fractional in stock content.** Stage 0 captures
    a debug-mode-4 screenshot of mc2_03 (urban, road-heavy) with the
    cement-bit value visualized; confirms transition tiles exist with
    `pureConcrete ∈ (0.1, 0.9)`. If transition tiles are vanishingly
    rare, the binary `step()` would have been acceptable; the smoothstep
    preservation logic is still cheap enough to ship regardless.
13. **Three smoothstep-edge constants** (0.02 hue, 0.04 sat-val) —
    if Gate A drift exceeds 1%, these become tunables. Plan stage may
    parameterize them as a 4th palette uniform up front, or defer.
14. **`mat4_normal.tga` content stability.** D10 demands the file
    stays unchanged on disk through Stages 1–2. Grep all callers of
    the literal string `mat4_normal` to ensure nothing in the
    palette-on path accidentally writes to or expects a renamed
    version.
15. **`dispNames[]` parallel array structure.** Confirm
    [terrtxm2.cpp:2161-2178](../../../mclib/terrtxm2.cpp) layout is as
    described in §"Default-texture loader extension"; flag any
    deviation from the parallel-arrays-with-mat>=4-cutoff pattern.
16. **`thinTerrainLocs_.matNormal[5]` parallel structure.** Per RED
    MINOR-1, confirm
    [gameos_graphics.cpp:1605, :1667-1671](../../../GameOS/gameos/gameos_graphics.cpp)
    has its own `matNormal[5]` array and lookup loop; both the
    `terrainLocs_` and `thinTerrainLocs_` migrations land in lockstep.
17. **GL_MAX_TEXTURE_IMAGE_UNITS sanity check.** Per D7, runtime
    query at engine init confirms ≥ 14 (modern AMD/NVidia ≥ 32; this
    is paranoia, not blocker).
18. **Drift-tooling feasibility.** Stage 0 builds the python tool per
    Gate A0 spec OR documents why it can't and downgrades Gate A
    target. Decision lands BEFORE Stage 1 opens.
19. **Tese silhouette-edge depth-shift metric feasibility.** Sub-item
    of #18 — separate decision because depth-buffer capture is a
    different mechanism than matWeights debug-mode capture.
20. (REV3 — REPLACED) Texture-naming convention is now D10 role-based
    (`mat_slot4_normal.tga`, `mat_slot5_normal.tga`); no off-by-one
    decision needed at Stage 3. Item retired.

### REV3-additional recon items

21. **Thin program inventory.** Per RED CRIT-3, `gos_terrain_thin.vert`
    + `gos_terrain.frag` is a separately compiled program at
    [gameos_graphics.cpp:2615-2619](../../../GameOS/gameos/gameos_graphics.cpp).
    Stage 0 confirms which of the three terrain bind sites
    (`:3574, :3673, :3780`) corresponds to the thin path; pins the
    shader pair construction call site; verifies the thin program
    needs both legacy and palette variants compiled at engine init.

22. **Non-terrain shadow bind sites stay at units 9/10.** Per R1
    narrowed, `:5374, :5702` (and other equivalents in
    `gos_grass.frag`, `decal.frag`,
    `gos_tex_vertex_lighted.frag`, `terrain_overlay.frag` non-terrain
    bind paths) are NOT touched. Stage 0 grep confirms there are no
    other shadow bind sites that would conflict.

23. **`gos_terrain` material-load mechanism.** Per RED MAJ-7,
    `gos_terrain` is loaded via `gosRenderMaterial::load(...)` at
    [gameos_graphics.cpp:2592](../../../GameOS/gameos/gameos_graphics.cpp);
    `gos_terrain_thin.vert` + `gos_terrain.frag` is loaded via
    `glsl_program::makeProgram(...)` at
    [gameos_graphics.cpp:2615-2619](../../../GameOS/gameos/gameos_graphics.cpp).
    Stage 0 traces how the palette variants are constructed for each
    program-role: same `gosRenderMaterial::load` with a different
    `mvar` (material variant) that injects the prefix `#define
    MC2_MATERIAL_PALETTE 1`, OR direct `glsl_program::makeProgram` for
    all 6 programs (uniformizing the construction path). Decision
    affects how shader prefix injection works.

24. **Per-mission UBO lifecycle.** Per RED MAJ-6, `paletteUBO`
    teardown timing must avoid post-context-destruction segfault.
    Stage 0 clarifies: UBO allocated lazily on first mission init,
    re-uploaded (not re-allocated) on subsequent mission inits,
    deallocated only at engine shutdown. Plan stage adds explicit
    lifecycle code path matching the existing `glDeleteBuffers`
    convention for other long-lived UBOs.

25. **Shader prefix-string mechanism availability.** Stage 0 confirms
    the engine's existing shader-prefix mechanism (used today for
    `#version 430` injection per CLAUDE.md "Shader #version" rule)
    can also inject `#define MC2_MATERIAL_PALETTE` per-program.
    Mechanism is already there per `makeProgram(..., kThinPrefix)`
    pattern at `:2615`; Stage 0 reads the prefix-construction code
    and confirms the per-program string can vary.

26. **Slot-3 cement-bit boundary classification with the leftover
    rule.** Per REV3 D14, slot 0 leftover excludes slot 3 (cement-bit
    target). Stage 0 verifies: at a non-cement-tile pixel that lands
    in slot 3's HSV window (concrete-colored pixels not flagged by
    cement-bit), slot 3 contributes positively to weights and the
    leftover rule still works correctly. Sanity check with a
    test palette scoring concrete-color pixels.

27. **GL_MAX_UNIFORM_BUFFER_BINDINGS sanity check.** Palette UBO
    binds at binding=4. AMD RX 7900 XTX limit is well above; sanity
    check, not blocker.

After items 1–27 close, plan writes per-stage stage structure +
perf-gate targets + parity-gate evidence. Spec decisions above stand
without further recon.

---

## Out of scope (explicit)

- Per-mission **snow texture** override (only tint/enable in v1; texture
  override is a v2 if a mission needs it).
- More than 6 generic slots. The shader ceiling at GLSL 4.3 core (no
  dynamic sampler indexing) makes 6→16 a separate slice if ever needed.
- POM-scale-as-classifier-tunable on the fragment side. Today's POM
  ray-march uses a single effective scale; spec preserves this. A future
  slice could vary POM steps per slot.
- Replacing the `isWater` carve-out at
  [gos_terrain.frag:167](../../../shaders/gos_terrain.frag) with
  per-slot windows. Water is its own pipeline; out of scope here.
- Asset authoring pipeline (the user authors 2K TGAs by hand for v1;
  any procedural generator is a separate workstream).
- Mod content parity. Stock-only is the parity gate; mod content is
  diagnostic per
  [memory/feedback_offload_scope_stock_only.md](memory file).
- Per-tile (not per-pixel) classification cache. The legacy classifier
  runs per-fragment; new design preserves this. A SSBO-cached
  per-tile-pre-classified shape is a separate offload candidate.
- The cement catalog atlas (separate slice, already shipped as
  indirect-terrain Stage 4).
- Bindless textures. Not needed at 6 slots.

---

## Code-grounding verification appendix (REV2 — entries fixed and added)

Every cited symbol grep-verified at write-time. Status: M = matches
claim, D = divergent, NF = not found, P = pending Stage 0 recon (claim
asserted but not re-greppable at this revision's write-time without
running the planned recon work).

| # | Symbol / claim | Citation | Status |
|---|---|---|---|
| 1 | 5 hardcoded `matNormal0..4` samplers in frag | [gos_terrain.frag:43-47](../../../shaders/gos_terrain.frag) | M |
| 2 | `getColorWeights` HSV thresholds | [gos_terrain.frag:151-175](../../../shaders/gos_terrain.frag) | M |
| 3 | `tc_getColorWeights` separate function with different thresholds | [terrain_common.hglsl:14](../../../shaders/include/terrain_common.hglsl) | M |
| 4 | `tc_getColorWeights` divergent fallback `vec4(0,0,1,0)` | [terrain_common.hglsl:34](../../../shaders/include/terrain_common.hglsl) | M |
| 5 | `getColorWeights` divergent fallback `vec4(1,0,0,0)` | [gos_terrain.frag:173](../../../shaders/gos_terrain.frag) | M |
| 6 | tese/shadow_tese both call `tc_getColorWeights` | [gos_terrain.tese:110](../../../shaders/gos_terrain.tese), [shadow_terrain.tese:40](../../../shaders/shadow_terrain.tese) | M |
| 7 | `matTiling = vec4(3.0, 12.0, 1.0, 6.0)` rock/grass/dirt/concrete | [gos_terrain.frag:423](../../../shaders/gos_terrain.frag) | M |
| 8 | `pomScaleMat = vec4(1.0, 1.0, 2.5, 1.0)` | [gos_terrain.frag:179](../../../shaders/gos_terrain.frag) | M |
| 9 | `normalBoost = vec4(0.9, 1.1, 1.1, 2.5)` BASE values | [gos_terrain.frag:455](../../../shaders/gos_terrain.frag) | M |
| 9b | Grass `normalBoost.y *= fwGrass * grassNormalFade` per-fragment AA modulation (REV2) | [gos_terrain.frag:466](../../../shaders/gos_terrain.frag) | M |
| 10 | `tintRock/Grass/Dirt/Concrete/Snow` constants | [gos_terrain.frag:516-520](../../../shaders/gos_terrain.frag) | M |
| 11 | Cement-bit `mix()` blend (NOT a hard set) (REV2 — corrected from REV1's "forces vec4(0,0,0,1)") | [gos_terrain.frag:389](../../../shaders/gos_terrain.frag) `matWeights = mix(matWeights, vec4(0,0,0,1), pureConcrete)` | M |
| 11b | `pureConcrete = smoothstep(2.0, 3.0, TerrainType)` — fractional in (0,1) at boundary tiles (REV2) | [gos_terrain.frag:384](../../../shaders/gos_terrain.frag) | M |
| 11c | `pureConcrete` also drives snow suppression, detailN attenuation, normalLight blend (REV2) | [gos_terrain.frag:391, :501, :573](../../../shaders/gos_terrain.frag) | M |
| 12 | Snow as separate weight, not classifier slot | [gos_terrain.frag:393, :498](../../../shaders/gos_terrain.frag) | M |
| 13 | `terrainTextures2->getDetailHandle()` per-mission singleton | [mclib/quad.cpp:428, :435](../../../mclib/quad.cpp) | M |
| 14 | `0xffffffff` sentinel for "no detail handle" | [mclib/mapdata.h:103](../../../mclib/mapdata.h), [mclib/quad.h:100](../../../mclib/quad.h) | M |
| 15 | `terrainTextures` per-type detail array (legacy MC_DetailType) | [mclib/terrtxm.h:76-85](../../../mclib/terrtxm.h) | M |
| 16 | `TerrainColorMap::init(fileName)` chokepoint | [mclib/terrtxm2.cpp:1960+](../../../mclib/terrtxm2.cpp) (normalmap load block) | M |
| 17 | `setTextureNeverFlush` flag pattern | [mclib/terrtxm2.cpp:2057](../../../mclib/terrtxm2.cpp) | M |
| 18 | `detail_normal.tga` engine-default precedent | [mclib/terrtxm2.cpp:2072](../../../mclib/terrtxm2.cpp) | M |
| 19 | Engine default loader has TWO parallel 5-element arrays plus mat>=4 optional cutoff (REV2 — corrected from REV1's single-array claim) | [mclib/terrtxm2.cpp:2161-2178](../../../mclib/terrtxm2.cpp): `normalNames[5]`, `dispNames[5]`, `for (mat = 0; mat < 5; mat++)`, `mat >= 4` skip-if-missing | M |
| 20 | `terrainLocs_.matNormal[5]` array of uniform locations | [GameOS/gameos/gameos_graphics.cpp:1592, :1638-1642](../../../GameOS/gameos/gameos_graphics.cpp) | M |
| 20b | `thinTerrainLocs_.matNormal[5]` PARALLEL location array (REV2 — RED MINOR-1) | [GameOS/gameos/gameos_graphics.cpp:1605, :1667-1671](../../../GameOS/gameos/gameos_graphics.cpp) | M |
| 20c | `terrain_mat_normal_[5]` runtime handle storage AND `setTerrainMaterialNormal idx<5` setter (REV2 — RED MAJOR-4) | [GameOS/gameos/gameos_graphics.cpp:1579, :1430](../../../GameOS/gameos/gameos_graphics.cpp) | M |
| 21 | Three terrain-bind sites at units 5..9 (REV2 — note: matNormal4→unit 9 today is OVERWRITTEN by shadow bind; effective only on stock-no-snow paths) | [GameOS/gameos/gameos_graphics.cpp:3574, :3673, :3780](../../../GameOS/gameos/gameos_graphics.cpp) | M |
| 21b | Shadow program's matNormal2 site (REV2 — RED MINOR-3) | [GameOS/gameos/gameos_graphics.cpp:3338](../../../GameOS/gameos/gameos_graphics.cpp) — `glGetUniformLocation(shp, "matNormal2")`, location stored at [`:1694`](../../../GameOS/gameos/gameos_graphics.cpp) | M |
| 21c | `shadowMap → unit 9`, `dynamicShadowMap → unit 10` binds — each terrain-bind block has parallel sites (REV2 — RED CRITICAL-1) | [GameOS/gameos/gameos_graphics.cpp:3587, :3597](../../../GameOS/gameos/gameos_graphics.cpp) and parallel sites in the other terrain-bind blocks | M |
| 22 | (merged into 20b) | — | — |
| 23 | UBO binding 4 free in UBO namespace (REV2 — namespace clarified) | grep `layout(.*std140.*binding\s*=` across `shaders/` | M (UBO-binding 4 unused; SSBO-binding 4 also unused but independent) |
| 24 | GL 4.3 context (no dynamic sampler indexing without 4.6) | worktree CLAUDE.md "Shader #version" rule | M |
| 25 | `colorMapName` drives normalmap filename | [mclib/terrtxm2.cpp:1963](../../../mclib/terrtxm2.cpp) | M |
| 25b | Three-tier name fallback chain `colorMapName ?: terrainName ?: fileName` (REV2 — RED MAJOR-3) | [mclib/terrtxm2.cpp:1963-1968](../../../mclib/terrtxm2.cpp) | M |
| 26 | `mc2_24.burnin.tga` exists in deploy | `data/textures/mc2_24.burnin.tga` (deployed) | M |
| 27 | `Terrain::terrainTextures2->init(colorMapName)` mission-init call (REV2 — RED MAJOR-2 — TWO callsites) | [mclib/terrain.cpp:540-543](../../../mclib/terrain.cpp) (initial mission load) AND [mclib/terrain.cpp:820-823](../../../mclib/terrain.cpp) (mission reset/reload) | M |
| 28 | TG / mcTextureManager handle indirection live per-frame | [memory/mc2_texture_handle_is_live.md](memory file) | M |
| 29 | Deferred uniform API discipline | [memory/deferred_vs_direct_uniforms.md](memory file) | M |
| 30 | Stock-install-must-remain-playable rule | [memory/stock_install_must_remain_playable.md](memory file) | M |
| 31 | Smoke discipline (single-mission filter, no menu canary) | [memory/feedback_smoke_mission_filter.md](memory file), [memory/feedback_smoke_no_canary.md](memory file) | M |
| 32 | Stock-only parity scope | [memory/feedback_offload_scope_stock_only.md](memory file) | M |
| 33 | Debug instrumentation rule for reworks | [memory/debug_instrumentation_rule.md](memory file) | M |
| 34 | (REV2) `pureConcrete` is fractional in stock content at cement boundaries | Stage 0 deliverable — capture mc2_03 debug-mode-4 with cement-bit visualized; confirm transition tiles exist | P |
| 35 | (REV2) `GL_MAX_TEXTURE_IMAGE_UNITS ≥ 14` on AMD RX 7900 XTX | Stage 0 deliverable — runtime GL query at engine init | P |
| 36 | (REV2) Drift-measurement tooling buildable end-to-end | Stage 0 deliverable — `scripts/material_palette_drift.py` runs on mc2_01+mc2_24 capture pair | P |
| 37 | (REV3) `gos_terrain_thin.vert` + `gos_terrain.frag` is a separately compiled program (the "thin" path) | [GameOS/gameos/gameos_graphics.cpp:2615-2619](../../../GameOS/gameos/gameos_graphics.cpp) `glsl_program::makeProgram(...)` call | M |
| 38 | (REV3) `gos_terrain` (main) is loaded via `gosRenderMaterial::load` | [GameOS/gameos/gameos_graphics.cpp:2592](../../../GameOS/gameos/gameos_graphics.cpp) | M |
| 39 | (REV3) `shadow.hglsl` is included by 5 shaders (decal, gos_grass, gos_terrain, gos_tex_vertex_lighted, terrain_overlay) | grep `#include.*shadow.hglsl` across `shaders/` | M |
| 40 | (REV3) Non-terrain shadow bind sites at units 9/10 separate from terrain bind sites | [gameos_graphics.cpp:5374, :5702](../../../GameOS/gameos/gameos_graphics.cpp) | M |
| 41 | (REV3) Today's `matNormal4 → unit 9` IS bound by terrain bind sites and overwritten by shadowMap → unit 9 only when shadows enabled | [gameos_graphics.cpp:3573-:3578](../../../GameOS/gameos/gameos_graphics.cpp) matNormal loop, [:3587](../../../GameOS/gameos/gameos_graphics.cpp) shadow conditional bind | M |
| 42 | (REV3) Legacy `tc_getColorWeights` uses different fallback than legacy frag `getColorWeights` (the two-classifier divergence this slice retires) | [terrain_common.hglsl:14, :34](../../../shaders/include/terrain_common.hglsl) (`vec4(0,0,1,0)` = dirt) vs [gos_terrain.frag:173](../../../shaders/gos_terrain.frag) (`vec4(1,0,0,0)` = rock) | M |
| 43 | (REV3) Legacy water carve-out `isWater = smoothstep(0.35, 0.45, h)` adds to slot 0, zeros from slots 1/2 | [gos_terrain.frag:167-170](../../../shaders/gos_terrain.frag) | M |
| 44 | (REV3) `kShadowMapUnit = 12` does not collide with any existing UBO/SSBO/sampler unit usage | Stage 0 deliverable — re-grep at impl time | P |

**Status summary:** 44 cited symbols, 38 M (match) + 4 P (pending
Stage 0 recon), 0 divergent, 0 not found.

**REV3 changes vs REV2:**
- Added entries #37-#43 for thin program, material loader, shadow
  include inventory, non-terrain shadow binds, today's matNormal4
  collision, legacy classifier divergence, legacy water carve-out
- Added P-pending #44 for shadow-unit-12 collision verification at
  implementation time
- Entry #21 (terrain bind sites) wording softened to acknowledge
  matNormal4 IS bound and DOES contribute when shadows off (REV2 was
  too strong)
- Entry #11 cement-bit semantics unchanged (REV2 already correct;
  REV3 just preserves)

---

## Adversarial review handoff (REV3 — two reviews done, third pending)

Per worktree CLAUDE.md "Review Discipline" — this is an architectural
endpoint slice (per-mission palette infrastructure that supersedes
hardcoded shader constants and unifies two divergent classifiers). It
qualifies for the **full adversarial-plan-review skill**, not the
prose-only review tier.

### First adversarial review (REV1 → RED)

A first adversarial-plan-review pass on REV1 returned **RED** with
4 CRITICAL, 7 MAJOR, 4 MINOR findings:

| ID | Issue | Status in REV2 |
|---|---|---|
| C1 | Sampler unit 9/10 collision with shadowMap/dynamicShadowMap | **Fixed** — D7 reallocates shadow units to 12/13 |
| C2 | Cement-bit semantic regression (`mix()` → binary `step()`) | **Fixed** — D6 weight-accumulator math preserves `mix()` semantics |
| C3 | Verification entry #19 missed parallel `dispNames` array | **Fixed** — appendix #19 expanded; Stage 1 deliverables include both arrays + cutoff |
| C4 | "≤ 1% pixel-class drift" unmeasurable with current tooling | **Fixed** — D11 promotes drift tooling to Stage 0 deliverable; Gate A0 added; I10 marked measured-not-structural |
| M1 | Killswitch gating mechanism unspecified | **Fixed** — D8 commits to compile-time `#define MC2_MATERIAL_PALETTE` with two-program variant |
| M2 | Two `terrainTextures2->init()` callsites missed | **Fixed** — appendix #27 now cites both; init() chokepoint covers both |
| M3 | Sidecar filename derivation contradicts existing pattern | **Fixed** — name-fallback chain mirrors [terrtxm2.cpp:1963-1968](../../../mclib/terrtxm2.cpp) |
| M4 | Stage 1 deliverables underspecified for `gameos_graphics.cpp` widening | **Fixed** — D12 introduces `kTerrainMaterialSlotCount` named constant + explicit edit list |
| M5 | Tese migration silently re-displaces stock geometry | **Fixed** — D9 explicit decision to migrate frag+tese together; drift surface Section 3 acknowledges geometric drift; Gate A target adds silhouette-edge metric |
| M6 | Grass NormalBoost is runtime-modulated | **Fixed** — Frag contract preserves grass-specific `fwGrass * grassNormalFade` modulation on top of UBO base value |
| M7 | mat4 rename breaks killswitch | **Fixed** — D10 copy-not-rename strategy preserves `mat4_normal.tga` content through Stages 1-2; rename happens in Stage 3 alongside legacy delete |
| MINOR-1 | thinTerrainLocs_.matNormal[5] mirror | **Fixed** — appendix #20b; Stage 1 deliverables explicit |
| MINOR-2 | UBO/SSBO binding namespace conflated | **Fixed** — UBO Binding Choice section + appendix #23 distinguish namespaces |
| MINOR-3 | Shadow program matNormal2 site missed | **Fixed** — appendix #21b; Stage 1 deliverables explicit |
| MINOR-4 | I7 static_assert is stylistic, not semantic | **Fixed** — I7 reformulated: shared C++ header generates shader `#define`; build-time grep check |

### Second adversarial review (REV2 → RED)

A second adversarial review on REV2 returned **RED** with 3 CRITICAL,
7 MAJOR, 5 MINOR + NEW-IN-REV2 findings. The pattern was the predicted
regression class: REV1→REV2 fixes introduced new CRITICAL findings
of their own. Six R-decisions and six extra requirements were signed
off by advisor with R1 narrowed and R5 rejected (preserves strict
legacy rollback).

| ID | Issue | Status in REV3 |
|---|---|---|
| CRIT-1 | D7 shadow-unit relocation breaks 4 non-terrain shaders that include `shadow.hglsl` | **Fixed via R1 narrowed** — palette-terrain-variant only relocates shadow units. Legacy terrain stays at 9/10. Non-terrain unchanged. |
| CRIT-2 | Default-texture loader inconsistent slot↔file mapping (mat5/6 off-by-one) | **Fixed via D10 + R2** — role-based filenames `mat_slot4_normal.tga` / `mat_slot5_normal.tga`. No off-by-one. Two parallel loaders (legacy + palette). |
| CRIT-3 | D8 compile-variant ignored `thin_terrain_prog_` | **Fixed via D8 bumped** — 6 program matrix `{legacy, palette} × {main_tess, main_thin, shadow}`. Thin path first-class. |
| MAJ-1 | Default-texture loader's all-same-width constraint blocks 2K author-supplied slot 4/5 textures | **Fixed via R4** — engine defaults stay uniform-width; sidecar textures load via `mcTextureManager->loadTexture()` independent of that constraint. |
| MAJ-2 | UBO declaration in `terrain_common.hglsl` not `#if`-guarded | **Fixed via I14** — full `#if MC2_MATERIAL_PALETTE` guards on the entire palette branch including UBO declaration. |
| MAJ-3 | REV2's "today's allocation" prose table got shadow units wrong | **Fixed** — corrected to shadowMap=9, dynamicShadowMap=10. |
| MAJ-4 | "matNormal4 → unit 9 is no-op today" claim too strong | **Fixed** — softened wording: matNormal4 IS bound and DOES contribute when shadows off; legacy variant preserves this exactly per D13. |
| MAJ-5 | Legacy variant's Gate C "bit-for-bit identical" likely false in shadow-on mode after relocation | **Fixed via R5 rejected (D13 strict)** — legacy variant does NOT relocate shadow; preserves today's exact allocation. Gate C remains strictly bit-for-bit. |
| MAJ-6 | Per-mission UBO lifecycle teardown unspecified | **Fixed via Stage 0 recon item #24** — explicit lifecycle: lazy alloc on first init, re-upload on subsequent inits, dealloc at engine shutdown. |
| MAJ-7 | `gos_terrain` material-load mechanism unspecified | **Fixed via Stage 0 recon item #23** — Stage 0 traces whether palette variant uses `gosRenderMaterial::load` with mvar OR direct `glsl_program::makeProgram` for all 6 programs. Decision sits in Stage 0. |
| MIN-1 | "Compile-time killswitch" terminology misleading | **Improved** — D8 wording now says "per-program shader-prefix variant; runtime selection." |
| MIN-2 | I4 said sidecar HSV window for slot 3 "effectively unused" — inaccurate | **Fixed** — I4 wording now says "sidecar may declare slot 3's HSV window for non-cement contribution; cement-bit drives the override at flagged tiles." |
| MIN-3 | Stage 0 sized as one stage, may need split | **Acknowledged** — Stage 0 has both recon items + tooling deliverable. Plan stage may split into 0a/0b. |
| MIN-4 | "Three-tier" name fallback wording cosmetically inaccurate | **Acknowledged** — claim is functionally correct; cosmetic only. |
| MIN-5 | Spec verification stub entry inflated count | **Cosmetic** — corrected. |
| NEW-1 | REV2 "today's allocation" prose table inconsistent with appendix #21c | **Fixed via MAJ-3** above. |
| NEW-2 | D10 file-numbering off-by-one creates loader ambiguity | **Fixed via D10 reformulated** to role-based names. |
| NEW-3 | shadow_terrain.tese needs UBO=4 binding too (new bind site) | **Fixed** — Shader bind path section now explicitly covers shadow program palette variant UBO bind at the shadow bind site. |
| NEW-4 | Gate A0 + debug-mode 5 chicken-and-egg | **Fixed** — drift surface section notes legacy variant uses existing debug-mode-4 (matWeights as RGB at frag:413) + palette variant's debug-mode-5; both produce comparable outputs. Stage 0 verifies the formats are compatible. |
| **NEW-5** | **Slot 0 fallback semantics — substantive classifier change vs legacy "leftover" rule** | **Fixed via D14 + R6 — adopted legacy leftover semantics.** `tc_classifyAll` applies `weight[0] = max(weight[0], 1 - max(weight[1], weight[2], weight[4], weight[5]))` BEFORE normalization. Stock pixel classification near-exactly preserved. |

### Third adversarial review (PENDING)

The next session should:

1. Read `.claude/skills/adversarial-plan-review.md`.
2. Apply it to **this revision** (REV3) of the spec doc.
3. Grep every cited symbol; cross-reference every load-bearing
   constraint; report findings as CRITICAL / MAJOR / MINOR per skill
   format.
4. Specifically check whether the second-pass fixes hold under
   third-pass scrutiny:
   - All 44 verification appendix entries re-greppable today (38
     should match; 6 are P-pending Stage 0).
   - **D14 leftover rule formula is mathematically correct** —
     `weight[0] = max(weight[0], 1 - max(weight[1], weight[2],
     weight[4], weight[5]))` accurately generalizes legacy
     `w.x = 1 - max(w.y, w.z)` to N slots while excluding slot 3
     (cement-bit target).
   - **D14 water carve-out is preserved** — `tc_classifyAll`'s
     palette branch contains the carve-out in the right order
     (after classify, before leftover rule, before normalize).
   - **I14 `#if MC2_MATERIAL_PALETTE` guards** — legacy and palette
     branches do not contain symbols visible to the other variant.
   - **I9 strict** — legacy variant code paths in `gameos_graphics.cpp`
     are documented as unchanged from today; D13 is enforced.
   - **6-program matrix is consistent** — every reference to "shader
     programs" is to 6, never 2 or 3.
   - **Two parallel loaders are consistent** — engine default loader
     section, asset table, and Stage 1 deliverables all reference
     legacy + palette loader pair, never single loader.
   - **Sampler unit allocation per-variant table** matches every
     other reference in the spec (verification appendix entries 21,
     21c, 41; bind path section; named constants section).
5. Surface any second-order issues introduced by the REV2→REV3 fixes
   themselves, especially:
   - The leftover rule's interaction with cement-bit blending order
     (cement happens AFTER normalize, but leftover happens BEFORE
     normalize — verify the math composes correctly across stock
     missions including transition tiles).
   - The 6-program matrix interaction with the existing shader-loading
     mechanism (`gosRenderMaterial::load` for main, `makeProgram` for
     thin) — Stage 0 has this as a recon item; spec doesn't fully
     resolve it but third review should flag if the resolution is
     load-bearing for Stage 1.
   - The two-parallel-loader interaction with the existing
     [terrtxm2.cpp:2191-2196](../../../mclib/terrtxm2.cpp)
     all-same-width constraint — both loaders must satisfy it
     independently.

Findings inform a spec REV4 if needed before plan-writing opens. If
REV3 passes third adversarial review with no CRITICAL or MAJOR
findings, plan-writing can begin.

---

## Self-review (skill checklist) (REV3 — re-done)

Per the brainstorming skill's spec self-review:

1. **Placeholder scan.** No `TBD`, `TODO`, or vague requirements
   outside Stage 0 recon items. Six appendix entries marked `P`
   (pending) with explicit Stage 0 deliverables to convert them to M.
   Done.

2. **Internal consistency.**
   - TL;DR D-table has 14 entries (D1-D14) matching every subsection's
     decision. Each D-decision appears in TL;DR and is fleshed out in
     a corresponding section.
   - REV3-specific D-decisions (D7 narrowed, D8 6-program, D10
     role-based, D13 strict Gate C, D14 leftover+water) are consistent
     across all referencing sections.
   - Section "Engine defaults" defaults match invariants I1-I15.
   - Section "Sampler unit allocation" per-variant table matches D7,
     D13, I9, I11, and the C++ constants in §"C++ named constants."
   - Section "Drift surface" matches Gate A target, I10, and the
     leftover-rule preservation in `tc_classifyAll`'s palette branch.
   - Stage ladder matches deliverables in §"Engine integration,"
     §"Sampler unit allocation," §"Default-texture loader extension,"
     §"Shader bind path."
   - All 14 D-decisions are referenced in TL;DR and body.
   - Six-program matrix is referenced consistently (Architecture,
     Data flow, Stage 1 deliverables, Stage 3 deletions, I15).

3. **Scope check.** Single coherent slice: per-mission classifier +
   binding override + two-classifier unification + sand/moon biome
   support. Adjacent work (snow texture override, > 6 slots, per-tile
   classification cache, water/cement systems, bindless textures)
   explicitly out-of-scope. Stage 0 tooling + recon adds 1-2 days.
   Sized for one implementation plan with a 4-stage ladder.

4. **Ambiguity check.** Resolved:
   - Slot 0 fallback semantics (REV3): D14 + I3 + I12 — leftover rule
     `weight[0] = max(weight[0], 1 - max(others))` applied BEFORE
     normalization. Stock pixel classification near-exactly preserved.
   - Snow as slot vs separate weight: separate weight; consistent.
   - Cement-bit slot 3 (REV2 D6): mix-equivalent weight accumulator
     applied AFTER `tc_classifyAll` returns. Order locked in I12.
   - mat4 lifecycle (REV3 D10): copy-not-rename; role-based new file
     names eliminate slot↔file index ambiguity entirely. No
     off-by-one.
   - Killswitch mechanism (REV3 D8): per-program shader-prefix variant
     with runtime selection. 6 programs compiled at engine init.
   - Sampler unit allocation (REV3 D7+D13): per-variant tables. Legacy
     unchanged from today; palette uses 5..13. Strict Gate C protects
     legacy.
   - Tese migration scope (D9): both tese programs migrate with frag
     in v1.
   - Drift gate (D11): Stage 0 tooling deliverable; eyeball fallback
     if tooling infeasible.
   - `terrain_common.hglsl` `#if` guards (I14): every palette symbol
     is `#if MC2_MATERIAL_PALETTE`-guarded. Legacy `tc_getColorWeights`
     under `#if !MC2_MATERIAL_PALETTE`.
   - Water carve-out preserved (D14): `tc_classifyAll`'s palette
     branch contains the carve-out in correct order (per I12+I13).

5. **REV3-specific consistency check.**
   - Per-variant sampler allocation table cross-checked: legacy
     terrain at units 5..10 (matNormal0..4 + shadowMap-overwriting-9
     + dynShadow-10), palette terrain at units 5..13 (matNormal0..5
     + matNormalSnow=11 + paletteShadow=12 + paletteDynShadow=13),
     non-terrain shadow at units 9..10 unchanged.
   - 6-program matrix consistent across Architecture, Data flow,
     Stage 1 deliverables, Stage 3 deletions, I15. Every "two
     programs" or "three programs" wording in REV2 has been bumped
     to "6 programs" or matches the new structure.
   - Role-based filenames consistent across §"Engine default
     normal/displacement TGAs" table, §"Default-texture loader
     extension" array contents, Stage 1 asset deliverables, Stage 3
     deletions. Old REV2 mat5_normal/mat6_normal naming is not
     present anywhere in REV3.
   - Slot-0 leftover rule consistent in:
     - `tc_classifyAll` palette-branch pseudocode (Common include)
     - Frag section's "order of operations is load-bearing"
     - I3 strengthened wording
     - I12 invariant
     - Drift surface section noting NEW-5 resolved
     - D14 in TL;DR

6. **Strict Gate C invariant check (REV3 — D13).**
   - Legacy variant code paths in `gameos_graphics.cpp` are
     described as unchanged from today (no in-place migration of
     literals 5/9/10).
   - Stage 3 deletes the legacy code as a unit (no incremental
     refactor).
   - Gate C verifies legacy variant produces today's render including
     latent quirks (matNormal4-overwritten-by-shadowMap-at-unit-9
     when shadows on). Eyeball verification across both shadow-on
     and shadow-off.
   - I9 wording strengthened: "STRICTLY bit-for-bit identical."
   - Adversarial review handoff records that R5 was rejected in favor
     of D13.

No revision required. Spec is ready for third adversarial review.

---

## Closing — ready-for-plan?

**Status (REV3): READY FOR THIRD ADVERSARIAL REVIEW.**

If third adversarial review passes (no CRITICAL or MAJOR findings),
the writing-plans skill takes this doc plus the expanded Stage 0
recon items + tooling deliverable and produces an implementation plan
broken into the 4-stage ladder above. Stage 0 work happens during
plan write; Stage 1 begins after Stage 0 closes (tooling shipped or
explicitly downgraded; recon items resolved).

If third review surfaces additional CRITICAL or MAJOR findings, the
spec goes to REV4 with another set of advisor decisions before
plan-writing. Two RED reviews already happened; pattern caution
applies (REV2's regressions show that fixes can introduce new bugs).
The specific surface area to scrutinize on REV3 is in the
"Third adversarial review (PENDING)" section above.
