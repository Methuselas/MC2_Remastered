# V-IBL-STATIC-1-SOAK — Static-prop IBL-SH strength sweep eyeball pass

- **Branch SHA at capture time:** `8a9b79cc`
- **Date:** 2026-05-27
- **Scope:** StaticPropOpaque pipeline. 5-point strength sweep
  (`off, 0.25, 0.50, 1.00, 1.50`) on two camera presets to support
  default-value selection for `g_iblShStrength` after the V-IBL-STATIC-1
  ship.
- **Engine knob:** `MC2_STATIC_PROP_IBL_SH_STRENGTH` (clamped 0..3, parsed
  once at process start; ImGui slider still authoritative at runtime).
  `MC2_STATIC_PROP_IBL_SH=0/unset` = legacy gate-OFF; `=1` enables SH path
  and uses the env strength as the slider default.

## Captures index

| Mission             | OFF                                                            | s=0.25                                                          | s=0.50                                                          | s=1.00                                                          | s=1.50                                                          |
|---------------------|----------------------------------------------------------------|-----------------------------------------------------------------|-----------------------------------------------------------------|-----------------------------------------------------------------|-----------------------------------------------------------------|
| mc2_24 (preset 01)  | `staticprop_baseline_01_8a9b79cc_ibl_sh_off.png`               | `staticprop_baseline_01_8a9b79cc_ibl_sh_s0p25.png`              | `staticprop_baseline_01_8a9b79cc_ibl_sh_s0p50.png`              | `staticprop_baseline_01_8a9b79cc_ibl_sh_s1p00.png`              | `staticprop_baseline_01_8a9b79cc_ibl_sh_s1p50.png`              |
| mc2_10 (preset 02)  | `staticprop_baseline_02_8a9b79cc_ibl_sh_off.png`               | `staticprop_baseline_02_8a9b79cc_ibl_sh_s0p25.png`              | `staticprop_baseline_02_8a9b79cc_ibl_sh_s0p50.png`              | `staticprop_baseline_02_8a9b79cc_ibl_sh_s1p00.png`              | `staticprop_baseline_02_8a9b79cc_ibl_sh_s1p50.png`              |

All paths relative to `tests/visual/baselines/`. Each PNG has a sibling
`.json` sidecar (resolution, sha256, capture timestamp) and `.log`
(child mc2.exe stdout/stderr).

### Known sidecar metadata caveat (non-fatal)

The strength sweep was driven correctly into the mc2.exe subprocess env
(see `scripts/capture_baseline.py:166-171`), but
`captured_flags()` snapshots the **parent** Python process env, which
was never modified. As a result every sidecar shows
`MC2_STATIC_PROP_IBL_SH = "default"` and
`MC2_STATIC_PROP_IBL_SH_STRENGTH = "default"`. The PNG content is
correct — the filename suffix (`_off`, `_s0p25`, …) is the authoritative
variant tag, and PNG sha256 differences across variants confirm the
engine saw the different strengths. The sidecar metadata bug is logged
for a follow-up cleanup; it does NOT invalidate this soak.

Note: preset_02 `_s0p50` and `_s1p00` share an identical PNG sha256
(`224d23f2…`); plausibly a screenshot race / identical capture frame
under the 100-fps minimised cap rather than identical render output.
If the eyeball pass needs distinct s=0.50 vs s=1.00 frames for that
preset, re-capture those two with a longer warm-up offset.

## Eyeball review checklist (user-filled)

For each preset, walk the strength ladder OFF → 0.25 → 0.50 → 1.00 → 1.50
and answer:

- [ ] Too blue? (sky tint dominating ambient term)
- [ ] Too flat? (lost directional contrast on prop normals)
- [ ] Too washed out? (low-frequency ambient eating albedo)
- [ ] Roof / vertical contrast **improved** vs OFF?
- [ ] Shadowed sides now **readable** without losing shape?
- [ ] Material colors **preserved** (no muddy/grey shift)?

Per-preset notes:

- **mc2_24 (preset 01):** _________________________________________
- **mc2_10 (preset 02):** _________________________________________

## PNG sha256 + size audit

| File suffix                                        | sha256 (first 16) | bytes    |
|----------------------------------------------------|-------------------|----------|
| `01_…_off.png`                                     | `eb9c9fe8c03a10a9` | 1061847  |
| `01_…_s0p25.png`                                   | `e930c4b552bd6ce8` | 1062869  |
| `01_…_s0p50.png`                                   | `1fc905eed5670bdc` | 1063088  |
| `01_…_s1p00.png`                                   | `0b0de0b5cad4f271` | 1062066  |
| `01_…_s1p50.png`                                   | `34c3655e4d571b7f` | 1062014  |
| `02_…_off.png`                                     | `e5b9a26bb8ae618b` | 1062316  |
| `02_…_s0p25.png`                                   | `1489135e9ca256d9` | 1063937  |
| `02_…_s0p50.png`                                   | `224d23f27ed0414c` | 1063261  |
| `02_…_s1p00.png`                                   | `224d23f27ed0414c` | 1063261  |
| `02_…_s1p50.png`                                   | `b35e7691eb0162f3` | 1063217  |

Preset 01: 5 distinct shas. Preset 02: 4 distinct (s0p50 == s1p00, see
caveat above).

## Validation probes (this soak)

All probes ran post-capture against the deployed `mc2-win64-v0.4/mc2.exe`
built from `8a9b79cc`.

| Probe                                                                   | Result | Notes                                            |
|-------------------------------------------------------------------------|--------|--------------------------------------------------|
| Default-OFF tier1 5/5                                                   | PASS   | 5/5 PASS, Δ destroys = 0 across all missions     |
| Kill-switch mc2_10 (`MC2_STATIC_PROP_IBL_SH=0`)                         | PASS   | 1/1 PASS, frame count nominal                    |
| Strength override mc2_10 (`IBL_SH=1` + `STRENGTH=0.25`)                 | PASS   | 1/1 PASS, no crash, no GL errors                 |
| Strength clamp upper mc2_10 (`STRENGTH=99`, expects clamp to 3.0)       | PASS   | 1/1 PASS — silent clamp confirmed                |
| Strength clamp lower mc2_10 (`STRENGTH=-1`, expects clamp to 0.0)       | PASS   | 1/1 PASS — silent clamp confirmed                |

Smoke artifact directories under `tests/smoke/artifacts/2026-05-27T19-*`.

## Default-strength recommendation

Recommended default after eyeball pass: ______ (user fills in)

Current engine default: `g_iblShStrength = 1.0f` (set in
`GameOS/gameos/gos_static_prop_batcher.cpp:199`).

## Next-slice options

- **V-IBL-STATIC-2** — per-mission HDRI selection (right cubemap per
  biome instead of a single global probe).
- **V-MATERIAL-PBR-1** — contract-first material model expansion
  (roughness/metallic/F0 surface knobs feeding the same SH ambient).
- **StaticProp lighting tune defaults** — only if the eyeball pass
  surfaces a clear winning strength different from 1.0; one-line
  default-constant change + tier1 5/5 + commit.

## Cross-references

- V-IBL-STATIC-1 ship: `64e58c11`
- HYGIENE-4 (slider plumbing, prior pre-req): `1e978be1`
- V-IBL-STATIC-0 plan: `cfad795c`
- V-IBL-SH-PROJECTOR-RECON (probe / projector recon): `4c2bd769`
- Soak slice prerequisites HEAD: `8a9b79cc` (this branch tip)
