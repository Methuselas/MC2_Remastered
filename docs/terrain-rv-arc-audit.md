# Terrain R→V Arc Audit

End-to-end audit of the terrain lane after the recent R→V slice train.
Authority-chain map + feature gate matrix + visual verdict + rollback
documentation. Updates the picture in
[docs/terrain-rv-arc-recon.md](terrain-rv-arc-recon.md) (slice 1 of
batch 1) with what actually shipped.

Audit HEAD: `8dddf6ae`. Validators at audit time:
- `tools/shader_reflect/reflect.py`: 77 checked, 0 drift, 0 new, 0 compile errors, 1 skipped.
- `scripts/check-env-registry.sh`: PASS (230 MC2_* vars, 56 registered, 210 allowlisted).

## Verdict

**YELLOW-GREEN**

- **GREEN** for geometry-fixed terrain visual work. The NFH + V1 + V2
  + transition-overlay stack is coherent, env-gated, ImGui-tunable,
  reviewer-passed, soaked across 3 missions, and default-OFF.
  Reusable as a template for future geometry-fixed terrain visual
  slices (lighting V3, sun-direction-aware sky tint, etc.).
- **YELLOW** as full R→V lane because terrain displacement remains
  explicitly deferred per the TERRAIN-GROUNDING-AUDIT-1 finding
  (tier1 p99 drift 6–15 wu, max 41–82 wu). The visible-displace
  path requires a near-unit fade or low-strength clamp mitigation
  slice that has not been designed.

Terrain is **not yet** a complete R→V analog of StaticPropOpaque
(which has packet/dispatch substrate end-to-end), but it IS a
coherent geometry-fixed visual lane with full Track-R debug substrate.

## 1. Authority chain (post-slice state)

### Data path (unchanged from slice-1 recon)

| Stage | Source | Notes |
|---|---|---|
| Mission `.pak` | PacketFile binary | Per-vertex `PostcompVertex` (32 B). |
| MapData blocks | `mclib/mapdata.cpp:205-211` `newInit()` | Loads at mission init. |
| Heightfield | `Terrain::realVerticesMapSide` ∈ {60,80,100,120} | Tier1: 100² and 120² grids. |
| World scale | `worldUnitsPerVertex = 128.0` | Map extent ≈ 15360 wu/side. |

### Render path (multiple — geometry unchanged across all slices)

| Path | Shader | Bind site | Status |
|---|---|---|---|
| Legacy tessellated terrain | `gos_terrain.{vert,tesc,tese,frag}` | `gosRenderer::terrainBindUniformsForPatchStream` + `terrainDrawIndexedPatches` (`gameos_graphics.cpp:5102+`, `5331+`) | Full terrain lighting stack |
| Thin/indirect tessellated terrain | same frag + `gos_terrain_thin.vert` | `terrainBindThinUniformsForPatchStream` (`:5204+`) | Full terrain lighting stack |
| Cement transition overlay | `terrain_overlay.{vert,frag}` | `gosRenderer::drawTerrainOverlays` + `uploadOverlayUniforms_` (`:7729+`) | Full terrain lighting stack (slice TERRAIN-DECAL-LIGHTING-1a) |
| Crater / footprint decal | `decal.frag` (+ shared overlay vert) | `gosRenderer::drawDecals` + same `uploadOverlayUniforms_` | **Deferred (classification B).** Locs cached but -1; helper skips. |
| Water fast path | `gos_terrain_water_fast.vert` | separate | **Out of scope.** No terrain lighting stack consumption. |
| Shadow terrain | `shadow_terrain.{vert,frag}` | shadow pre-pass | Untouched. Empty fragment body. |
| Overlay alpha clear | `overlay_alpha_clear.frag` | `gosPostProcess::clearOverlayAlpha` | Untouched. GBuffer1 cleanup only. |

### New shared shader infrastructure

| File | Owner | Consumers |
|---|---|---|
| `shaders/include/terrain_height_normal.hglsl` | TERRAIN-DECAL-LIGHTING-1a | `gos_terrain.frag`, `terrain_overlay.frag` |
| `GameOS/gameos/gos_terrain_height_tex.{h,cpp}` | TERRAIN-NORMALS-FROM-HEIGHT-1 | All terrain bind sites via `bindTerrainHeightTexUniforms()` helper |

### Pass facts / inspector / capture

| Surface | Status | Notes |
|---|---|---|
| `TerrainPassFacts` (substrate snapshot record) | Unchanged from pre-slice | Single-bucket; water/solid/shadow not separated. **Recommended cleanup, not blocker.** |
| Object Inspector "Terrain Pass" panel | Trimmed to read-only diagnostic | All sliders moved to Graphics Options consolidation. |
| Graphics Options "Terrain Tuning" panel | Authoritative tuning surface | matNormalBoost, tintStrengthScale, NfH strength, Resample combo (1/2/4), V1 strength, V2 floor + status readouts. |
| Graphics Options "Static Prop Tuning" panel | New (49b54e3f) | IBL SH / PBR V1 / roughness override. **Not terrain, but lives in same window.** |
| `scripts/capture_baseline.py` `TRACKED_FLAGS` | Records all 4 new terrain envs | `MC2_TERRAIN_DEBUG_MODE`, `..._NORMALS_FROM_HEIGHT`, `..._HEIGHT_RESAMPLE_FACTOR`, `..._LIGHTING_V1`, `..._LIGHTING_V2`. |

## 2. Feature gate matrix

| Env var | Default | ImGui control | Runtime-safe? | Capture metadata? | Registry kind | Validation coverage |
|---|---|---|---|---|---|---|
| `MC2_TERRAIN_DEBUG_MODE` | unset = 0 (final) | Graphics Options Surface Debug Mode picker + Object Inspector mini-control display | YES — `gos_SetTerrainDebugMode()` per-frame | YES | Trace (kAuxEnvVars) | tier1 5/5; single-mission mode-2 + mode-10 + mode-11 captures |
| `MC2_TERRAIN_NORMALS_FROM_HEIGHT` | unset = OFF (force-zero) | Graphics Options NfH strength slider (effective only when env ON) | YES — env re-read per upload, slider lives in member | YES | Feature (kFeatureTable) | tier1 5/5 default OFF; mc2_01/03/17/24 NFH-only captures |
| `MC2_TERRAIN_HEIGHT_RESAMPLE_FACTOR` | unset = 1 (no resample) | Graphics Options Resample combo (1×/2×/4×) — sticky override on first ImGui change | YES — gos_setTerrainHeightResampleFactor() re-runs upload from cached source | YES | Trace (TUNE_*) | tier1 5/5; factor-4 + tdm10 captures |
| `MC2_TERRAIN_LIGHTING_V1` | unset = OFF (force-zero) | Graphics Options Ambient strength slider | YES — same env-re-read-per-upload pattern | YES | Feature | tier1 5/5 default OFF; mc2_01/03/17/24 V1 captures; soak-1 + soak-2 |
| `MC2_TERRAIN_LIGHTING_V2` | unset = OFF (force-floor-1.0 → V1 unmodulated) | Graphics Options Shadow fill floor slider (member default 0.3) | YES | YES | Feature | tier1 5/5 default OFF; mc2_03/17/24 V1+V2 captures + tdm11 captures |
| `matNormalBoost` (4 ch) | (0.9, 1.1, 1.1, 2.5) hardcoded shader defaults | Graphics Options sliders (per channel 0..5) + Reset | YES — `gos_Set/GetTerrainMatNormalBoost` C-API | NO (TRACKED_FLAGS records env gates only, not slider state) | n/a (always-on) | Smoke coverage indirect |
| `tintStrengthScale` | 1.0 hardcoded | Graphics Options slider 0..2 + Reset | YES | NO | n/a | Smoke coverage indirect |
| Transition overlay lighting (terrain_overlay.frag) | Inherits all 3 terrain env gates + slider values automatically | No independent controls (slice TERRAIN-DECAL-LIGHTING-1a deliberate — no new env vars per slice spec) | YES — same `bindTerrainHeightTexUniforms()` helper, locs cached on overlayProg_ | YES (via same env captures) | n/a (no new env) | mc2_03 default / V1 / V1+V2 captures (commit 8dddf6ae) |

Documentation status:
- `docs/tier1_env_vars.md` — entries for all 5 new MC2_TERRAIN_* env vars.
- `docs/terrain-decal-lighting-recon.md` — overlay-sharing design rationale.
- `docs/terrain-lighting-soak-{1,2}.md` — empirical soak results.
- `docs/terrain-rv-arc-recon.md` — slice-1 substrate map (now somewhat
  superseded by this audit on the shipped-state view).

## 3. Shader / pass matrix

| Path | Default-OFF | NFH | NFH+V1 | NFH+V1+V2 | dbg mode 10 | dbg mode 11 |
|---|---|---|---|---|---|---|
| `gos_terrain.frag` (legacy + thin) | byte-equivalent legacy | slope-tilted N | + hemisphere additive (over-bright in shadow) | + V2 shadow-aware floor | height normal as RGB | hemi additive ×4 RGB |
| `terrain_overlay.frag` (cement transitions) | byte-equivalent legacy | flat shadow N (deliberate) + hemi-N derived from height | + matching hemi additive | + matching V2 floor | n/a (overlay uses surfaceDebugMode int; mode 10/11 dispatched in terrain frag only) | n/a |
| `decal.frag` (craters/footprints) | unchanged | unchanged | unchanged | unchanged | unchanged | unchanged |
| `shadow_terrain.frag` | empty body | unchanged | unchanged | unchanged | n/a | n/a |
| `gos_terrain_water_fast.vert` | out of scope | out of scope | out of scope | out of scope | n/a | n/a |
| `overlay_alpha_clear.frag` | unchanged | unchanged | unchanged | unchanged | n/a | n/a |

Shader_reflect goldens current for all 77 shaders. No drift.

Notable shader interactions:
- **Shadow PCF**: both terrain and overlay deliberately pass flat-up
  `vec3(0,0,1)` to `calcShadow`/`calcDynamicShadow` — height-derived
  normal would cause sprinkle/inverted shadows at slope-scale-bias
  time (rationale at gos_terrain.frag:732).
- **Hemisphere placement**: post-shadow, pre-fog in both frags — sky
  bounce fills shadowed terrain, but atmospheric perspective
  desaturates the fill (correct).
- **Snow damping**: only in `gos_terrain.frag` (multiplied by
  `(1 - 0.5*snowWeight)`); cement overlay inlines `snowWeight=0`
  since cement doesn't snow.
- **Bilinear filter on height tex**: GL_LINEAR (set CPU-side in
  `gos_terrain_height_tex.cpp`). Switched from GL_NEAREST in commit
  `6e106e41` after per-tile facet regression. Critical — `texelFetch`
  + NEAREST would re-introduce the facets.

## 4. Height / grounding verdict

### Source heights (empirical, per TERRAIN-HEIGHT-AUDIT-SCRIPT-1)

| Mission | Grid | Elev min..max (wu) | Slope p99 | Blockiness (2nd-diff variance) |
|---|---|---|---|---|
| mc2_01 | 100² | 200..710 | 41.0° | 649 |
| mc2_03 | 100² | -300..800 | 45.3° | — |
| mc2_10 | 120² | 0..750 | 37.2° | — |
| mc2_17 | 120² | 0..486 | 25.5° | — |
| mc2_24 | 120² | -100..1400 | 51.1° | — |

### Memory (R32F height tex)

| Mission grid | 1× | 2× | 4× |
|---|---|---|---|
| 100² | 39 KB | 156 KB | 615 KB |
| 120² | 56 KB | 224 KB | 890 KB |

All combinations well under 1 MB per mission. **Memory is non-issue.**

### Grounding drift (per TERRAIN-GROUNDING-AUDIT-1, commit `3c02e4e4`)

| Mission | p50 | p99 | max | verdict |
|---|---|---|---|---|
| mc2_01 | 0.00 | 13.92 | 80.17 wu | ⚠ |
| mc2_03 | 0.00 | 9.83 | 41.61 wu | ⚠ |
| mc2_10 | 0.00 | 13.66 | 59.97 wu | ⚠ |
| mc2_17 | 0.00 | 6.48 | 41.31 wu | ⚠ |
| mc2_24 | 0.00 | 15.44 | 82.15 wu | ⚠ |

### Why normal / resample / lighting is safe

- Normal-from-height is read-only on the height texture, no geometry
  moved. Lights respond to slope; nothing else changes.
- Resample factor 1/2/4 affects only what the shader samples; the
  source heightfield is preserved at corner positions (factor-multiple
  rows/cols match exact source values).
- V1 hemisphere ambient is purely additive lighting; geometry
  unchanged.
- V2 floor modulates the V1 additive only; geometry unchanged.

### Why displacement is deferred

- Median drift is 0 (most cells flat-on-grid) but p99 6–15 wu and
  max 41–82 wu. Comparable to or exceeding small-unit foot height
  (~2–5 wu).
- Strength=1 visual displacement without near-unit fade would
  float/sink units on every tier1 mission; worst on mc2_24.
- TERRAIN-RESAMPLE-PLAN-0 explicitly identified the mitigation
  options (reduced strength ≤ 0.25, OR near-unit fade ~2–3 cells,
  OR retain micro-detail-only via the existing `tc_sampleDisplacement`
  alpha path). None of those mitigations has been designed/shipped.

## 5. Visual verdict (per soak-1 + soak-2)

| Effect | Verdict | Notes |
|---|---|---|
| NFH normals-from-height | **clean** | Visible slope shading on steeper missions; subtle on flat grass. No artifacts at any resample factor. Per-tile facet regression fixed by `6e106e41`. |
| V1 hemisphere ambient | **works, over-bright shadows in V1-only** | Soak-1 finding. V2 addresses it cleanly. |
| V2 shadow-aware floor | **clean across mc2_03/17/24** | Floor 0.3 = cross-mission sweet spot per soak-2. Most valuable on mc2_24 (steepest). |
| Transition overlay (slice 1a) | **closes the seam** | Cement transitions now share hemisphere fill + V2 modulation with the surrounding terrain. Captures at commit `8dddf6ae` show distinct shas per env config — overlay path proven live. Visual seam check intentionally deferred to user-driven review. |
| Crater/footprint decals | **unchanged** | classification B per recon; intentionally unaffected. |

Remaining artifacts: none surfaced in soak captures.

Recommended tunables when opted in (per soak-2):
- `MC2_TERRAIN_NORMALS_FROM_HEIGHT=1`
- `MC2_TERRAIN_LIGHTING_V1=1`
- `MC2_TERRAIN_LIGHTING_V2=1`
- `terrainNormalsFromHeightStrength=1.0`
- `terrainLightingV1Strength=1.0`
- `terrainLightingV2ShadowFillFloor=0.3`
- `MC2_TERRAIN_HEIGHT_RESAMPLE_FACTOR=1` (4 for debug clarity only)

**Default ship posture: all terrain visual gates OFF.** No default
flip authorized. User opts in via env vars or Graphics Options
sliders.

## 6. Rollback / failure modes

| Feature | Disable | Gate-off behavior | Fallback if height tex missing | Evidence |
|---|---|---|---|---|
| Terrain debug mode | unset `MC2_TERRAIN_DEBUG_MODE` or set to 0 via inspector/slider | Final render path | Mode 10/11 return black if tex not uploaded (handled inside shader) | tier1 5/5 default; mode-flip soak captures |
| Normals-from-height | unset `MC2_TERRAIN_NORMALS_FROM_HEIGHT` (default) | CPU force-zeroes `useTerrainNormalsFromHeight`; shader branch skipped | Same — `useTerrainNormalsFromHeight=0` if `htex==0` regardless of env | Reviewer-verified initial-state safety (slice 4 review); tier1 5/5 default OFF byte-equivalent |
| Resample factor | unset env or select 1× in combo | factor=1 path; no resample buffer alloc | n/a (only re-runs upload if cached source present) | Reviewer-verified byte-equivalence at factor=1; tier1 5/5 default |
| Lighting V1 | unset `MC2_TERRAIN_LIGHTING_V1` (default) | CPU force-zeroes strength; shader `if (strength > 0.0)` short-circuits | Same | tier1 5/5 default OFF byte-equivalent |
| Lighting V2 | unset `MC2_TERRAIN_LIGHTING_V2` (default) | CPU force-uploads floor=1.0; `mix(1.0,1.0,shadow)=1.0` → V1 unmodulated | Same | tier1 5/5 default OFF byte-equivalent |
| Transition overlay lighting (1a) | unset all 3 terrain env gates | Helper called with -1 strength → shader branches skipped | Same as terrain frag | Captures at `8dddf6ae` show distinct shas per env config; default capture byte-equivalent to pre-slice |

**Kill-switch escalation:** if ALL terrain gates need to be forcibly
OFF at process start without unsetting env vars, the user can also
disable via Graphics Options sliders (strength → 0). Floor of trust
is the CPU helper's force-zero/force-one on env gate state.

No gate or fallback bug found during this audit.

## 7. Recommendations

### Must fix before more terrain visual work

**None.** Every gate behaves correctly; default-OFF byte-equivalence
is solid; reviewer-PASS on every code slice; cross-mission soak clean.

### Should fix before any default-on flip (not authorized)

- Soak-3 across more presets (e.g. mc2_05, mc2_12, mc2_20) to widen
  the witness set beyond the current 5 missions (mc2_01/03/10/17/24).
- Per-mission tuning store (some missions might want V2 floor != 0.3)
  — currently floor is global. Not a regression, just a future ergonomics
  improvement.
- `TerrainPassFacts` bucket separation (water-fast / solid / shadow /
  detail). Slice-1 recon called this out; still unaddressed. Inspector
  diagnostic only; no runtime impact.

### Optional polish

- TERRAIN-LIGHTING-3 — replace hardcoded sky/ground tints with
  IBL-SH-derived values for skybox-anchored sky color per mission.
- Sun-direction-coupled hemisphere — currently the sky term is
  isotropic; rotating with `terrainLightDir` would tie the bounce to
  time of day.
- Move `matNormalBoost` / `tintStrengthScale` / NfH strength /
  Lighting V1 strength / V2 floor into capture sidecar metadata
  (currently only env gate state is recorded; slider state is not).
- ShaderReflect golden auto-comment about the `_NNNN` spirv-cross
  anonymous-struct IDs that renumber on member-count changes
  (reviewer MEDIUM finding from slice 1a).

### Deferred displacement prerequisites (none authorized)

Before any TERRAIN-DISPLACE-VISUAL-* slice:

1. **Near-unit fade design** OR **default strength ≤ 0.25** OR
   **retain micro-detail-only path** (per TERRAIN-RESAMPLE-PLAN-0).
2. **TERRAIN-GROUNDING-AUDIT-2** at the chosen mitigation strength
   showing post-mitigation drift < ~2 wu (small-unit foot height).
3. **Shadow desync audit** — visible terrain Z moving without
   shadow projection moving could cause peter-panning/floating
   shadows. Not analyzed in any current slice.
4. **`Terrain::getTerrainElevation` snap-at-contact verification** —
   units already read CPU gameplay height; document the visual-vs-
   gameplay seam at unit feet under displacement.
5. **Inspector strength-clamp UI** — if displacement ships with a
   hard low ceiling, the slider should reflect that clamp.

## 8. Verdict (restated)

**YELLOW-GREEN.** Geometry-fixed terrain visual work is GREEN:
template is coherent, reviewer-passed, soaked, default-OFF, fully
env-gated, fully ImGui-tunable, transition overlay seam closed.

Terrain as full R→V lane is YELLOW because displacement remains
deferred without authorized mitigation design.

No must-fix items surfaced. Audit complete.
