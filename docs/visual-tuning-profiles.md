# Visual Tuning Profiles — MISSION-VISUAL-TUNING-1

Per-mission renderer tuning profiles. Lets specific missions set their own
default values for exposure, shadow softness, terrain lighting, and other
renderer tunables without changing global engine defaults.

## File location

`data/visual_tuning.json` relative to the game working directory.

Override with env var `MC2_VISUAL_TUNING_FILE=path/to/file.json`.

Missing file: silent no-op (current behavior unchanged).

## Precedence

```
1. Engine hardcoded defaults
2. Profile "defaults" block         ← lowest profile level
3. Profile "missions".<name> block  ← mission-specific overrides
4. Env vars (MC2_*)                 ← always override profile
5. ImGui live sliders               ← highest priority
```

Env vars that cover a specific key are checked at load time; if set, the
profile value is skipped and the env var value remains authoritative.

## Schema

```json
{
  "defaults": {
    "exposure":                   1.0,
    "shadowSoftness":             0.9,
    "terrainLightingV1Strength":  1.0,
    "terrainLightingV2Floor":     0.3,
    "staticPropIblStrength":      0.5,
    "waterSkyTintStrength":       0.0,
    "vfxBrightness":              1.0,
    "vfxAdditiveBrightness":      1.0
  },
  "missions": {
    "mc2_24": {
      "exposure": 1.1
    }
  }
}
```

Unknown keys: one-time warning to stderr, no crash.
Invalid values: clamped to safe range where applicable.

## Supported keys (v1)

| Key | Type | Range | Env var (overrides) | Notes |
|-----|------|-------|---------------------|-------|
| `exposure` | float | ≥0 | — | HDR composite exposure multiplier |
| `shadowSoftness` | float | 0–1 | — | Shadow PCF softness |
| `terrainLightingV1Strength` | float | 0–2 | — | Hemi ambient lighting strength |
| `terrainLightingV2Floor` | float | 0–1 | — | Shadow-aware fill floor level |
| `staticPropIblStrength` | float | 0–3 | `MC2_STATIC_PROP_IBL_SH_STRENGTH` | IBL SH contribution |
| `waterSkyTintStrength` | float | ≥0 | `MC2_WATER_SKYTINT` | Sky horizon tint on water |
| `vfxBrightness` | float | 0–8 | `MC2_TUNE_VFX_BRIGHTNESS` | Particle overall brightness |
| `vfxAdditiveBrightness` | float | 0–8 | `MC2_TUNE_VFX_ADDITIVE_BRIGHTNESS` | Additive blend brightness |
| `bloomThreshold` | float | ≥0 | — | Bloom bright-pass cutoff (visible only with `MC2_HDR_POST`+`MC2_BLOOM`) |
| `bloomIntensity` | float | ≥0 | — | Bloom composite weight (same gating) |
| `aoRadius` | float | ≥0 | — | SSAO sample radius (visible only with `MC2_SSAO`) |
| `aoStrength` | float | ≥0 | — | SSAO darkening strength (same gating) |
| `aoBias` | float | ≥0 | — | SSAO depth bias (same gating) |
| `mechAmbientStrength` | float | 0–2 | `MC2_MECH_AMBIENT_V1_STRENGTH` | Mech hemisphere ambient fill (gate `MC2_MECH_AMBIENT_V1`, default-ON) |
| `mechSpecularStrength` | float | 0–4 | `MC2_MECH_SPECULAR_STRENGTH` | Mech Blinn specular strength (gate `MC2_MECH_SPECULAR_V1`, default-ON) |

Keys not in this list are silently skipped (warn once). The shipped
`data/visual_tuning.json` `defaults` block intentionally lists only a subset;
any key absent from the file falls back to the engine default (current behavior).
Both the reader (`applyKey`) and the writer (`visualTuning_saveCurrentToMission`,
the "Set as Mission Defaults" button) cover this full list, so live edits to any
of these round-trip.

## Load timing

Profile is applied once at mission load after `missionScriptName` is set
(`mission.cpp: Mission::init`).  On mission reload, profile is re-applied
(ImGui edits from previous session reset to profile values for that mission).

## ImGui panel

Graphics Options window → **Visual Tuning Profile** collapsing section shows:
- Current profile file path
- Load status (green = loaded, orange = missing)
- Active mission name and key count
- **Reset to Profile** button — re-applies current profile mid-session

## Adding a new mission entry

Open `data/visual_tuning.json`, add an entry under `"missions"` with the
scenario script name (lowercase, e.g. `"mc2_05"`):

```json
"missions": {
    "mc2_05": {
        "exposure": 0.9,
        "shadowSoftness": 0.7
    }
}
```

Restart mission or press **Reset to Profile** in Graphics Options.

## Adding a new tunable key (future)

1. Add a `strcmp` branch in `applyKey()` in `visual_tuning_profile.cpp`.
2. Call the existing `gos_Set*` function or write directly to the extern float.
3. Add env var check via `envIsSet()` if a corresponding env var exists.
4. Add a row to the table above.
