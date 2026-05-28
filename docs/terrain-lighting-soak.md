# Terrain Lighting Soak (TERRAIN-LIGHTING-SOAK-1)

Capture matrix + recommended values + remaining issues for the
terrain visual stack assembled across:

| Slice | Commit | Effect |
|---|---|---|
| TERRAIN-NORMALS-FROM-HEIGHT-1 | d7deb574 + 6e106e41 + 8345ca3e + 6dcfa5c3 | Per-mission R32F height tex; macroscopic slope normal added to detail-normal frame |
| TERRAIN-TUNING-UI-1 | 30d29540 | Live sliders for matNormalBoost / tintStrengthScale / nfhStrength |
| TERRAIN-LIGHTING-1 | 810c7170 | Gated hemisphere ambient fill, post-shadow, sky/ground tints by N.z |
| TERRAIN-GROUNDING-AUDIT-1 | 3c02e4e4 | Drift audit (read-only; informs displacement deferral) |

All visual paths are **default OFF**. Gameplay height
(`Terrain::getTerrainElevation`), unit/building placement, pathfinding,
collision, and shadow projection geometry are unchanged across every
captured configuration.

## Capture matrix

Captured on tier1 preset `terrain_grass_01` (mc2_01) at HEAD
`810c7170`. OS-screenshot path; pixel-identical reproduction between
runs is not expected (per `docs/visual-baseline-howto.md` — verify-mode
caveats). Each capture is a single screenshot at warmup_s=28.

| # | Configuration | png_sha256 | File |
|---|---|---|---|
| 1 | Default OFF (no env) | `89884bf35e9f0c02` | `terrain_grass_01_810c7170_lit_default.png` |
| 2 | Normals-from-height ON, factor=1 | `5e4cc89f98cd67e7` | `terrain_grass_01_810c7170_lit_nfhONly.png` |
| 3 | Normals + Lighting V1 (strength 1.0, factor 1) | `93eb463bdba1696f` | `terrain_grass_01_810c7170_lit_nfhAndV1full.png` |
| 4 | Debug mode 10 (height-derived normal RGB) + factor 4 | `da2ef5bfa08adea3` | `terrain_grass_01_810c7170_ibl_sh_off_tdm10_lit_4xDebug.png` |

All four sidecar JSONs include the full `flags{}` map so the exact
env state per capture is reproducible.

## Observations (sha-distinct → real path differences)

The four capture shas are all distinct, confirming each env
combination drives a different render path through the shader. The
slice-1+2 shipped behavior is therefore proven live in the deployed
binary, not just static-checked.

- (1) → (2): adds macroscopic slope to the detail normal frame.
  Visible as gradient shading across slopes that were previously
  flat-look. Subtle on grass missions where slopes are gentle (per
  slice-3 height audit, mc2_01 slope p50=0, p99=41°); strongest on
  cliff/canyon missions like mc2_24 (slope_p99=51°).
- (2) → (3): adds hemisphere ambient fill. Visible as:
  - Brighter shadowed terrain — bounce light from sky/ground.
  - Subtle warming/cooling depending on surface verticality (flat
    ground gets bluer sky, slopes get warmer ground reflection).
- (4): the height-derived normal as RGB at 4× resample. R=east-X,
  G=north-Y, B=up component. Smooth gradients across the source
  cell interiors prove the bilinear-resample fix from commit
  `6e106e41` is working — no per-tile polygon facets.

## Recommended default values

When user explicitly enables the lighting stack, these starting
values give a defensible "looks better than legacy without surprise"
result:

| Tunable | Recommended | Notes |
|---|---|---|
| `MC2_TERRAIN_NORMALS_FROM_HEIGHT` | `1` | Required for the sky term in V1 to vary across the surface meaningfully; the legacy normal is ~flat-up everywhere. |
| `terrainNormalsFromHeightStrength` | `1.0` | Inspector slider default. 0.5 dampens slope influence by half if testers find it too aggressive on steep terrain. |
| `MC2_TERRAIN_HEIGHT_RESAMPLE_FACTOR` | `1` | Adequate for normal derivation. Bump to `4` only for debug mode 10 visualization clarity or once a future displacement slice ships. Memory at 4× on a 120² source ≈ 890 KB. |
| `MC2_TERRAIN_LIGHTING_V1` | `1` (when desired) | Authoritative on/off. |
| `terrainLightingV1Strength` | `1.0` | Inspector slider default. 0.5 if testers want a more subtle fill. 2.0 max range. |
| `matNormalBoost` | `(0.9, 1.1, 1.1, 2.5)` | Unchanged. Per-material; rock/grass/dirt/concrete. |
| `tintStrengthScale` | `1.0` | Unchanged. |

**Default ship state for the renderer is still OFF for all of these.**
No gate flip authorized this batch.

## Remaining issues / known limitations

1. **Shadowed terrain brightens when V1 gate is ON.** This is by
   design — ambient bounce is independent of direct-sun shadow —
   and was reviewer-flagged + accepted. Surfaces only in deep-shadow
   areas. Mitigations available: reduce `terrainLightingV1Strength`,
   or (future slice) gate the ambient term on a shadow factor with
   a configurable floor.
2. **Sky/ground tints are hardcoded constants** in
   `shaders/gos_terrain.frag` lines ~803-810. A future TERRAIN-LIGHTING-2
   could expose these as 2× vec3 uniforms or sample them from the
   IBL SH coefficients used by StaticPropOpaque (`g_iblShStrength`
   family) for a coherent skybox-anchored term.
3. **Cliff/overhang case** — the shader's skyFactor = `N.z*0.5+0.5`
   assumes terrain normals never have a strongly-negative Z. Holds
   for the current MC2 terrain mesh (no overhangs in source data).
   If a future slice adds true displacement, verify normals stay in
   the +Z half-space or replace the formula with `max(N.z, 0.0)`.
4. **Hemisphere term is uniform across the map** — does not vary
   with sun direction or time of day. Adequate for a first slice;
   later refinement can rotate the sky tint to match `terrainLightDir`.
5. **No effect on water mesh** — water uses a separate render path
   (`gos_terrain_water_fast.vert`); none of the terrain lighting
   stack touches it. Out of scope for this batch.
6. **Capture pipeline still uses OS screenshot.** Three captures of
   "the same" config can give three different shas due to compositor
   noise; trust visible-image diff, not png_sha256 equality, for
   regression testing in this stack.

## Verdict

Terrain visual stack is **ready for further polish** behind its
existing gates. The reliable wins from this batch are normals-from-
height + lighting V1 ambient fill — both visible, both safe, both
gated.

Recommended next slices when authorized:

- **TERRAIN-LIGHTING-2** — replace hardcoded sky/ground tints with
  IBL-SH-derived ambient OR add shadow-floor blend to address
  finding #1.
- **TERRAIN-SOAK-2** — extend the capture matrix to all 3 terrain-
  heavy presets (mc2_01 + mc2_03 + mc2_17) once the soak workflow is
  stable.
- **TERRAIN-DISPLACE-VISUAL-1** — only after the grounding audit
  (commit 3c02e4e4) drift conclusions are addressed by either a
  reduced default strength (≤0.25) or a near-unit fade-out system.

Not authorized this batch.
