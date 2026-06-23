# VISUAL-DIFF-PERCEPTUAL-HARNESS-1

> General, policy-driven image comparison. **Byte-exact stays the default + correct
> bar for deterministic-math shaders.** A perceptual policy exists for procedural/
> noise shaders (FBM + smoothstep) where driver-GLSL vs glslang-SPIR-V ULP float
> drift flips smoothstep edges — real pixel deltas that don't mean the shader is
> wrong, only that byte-exact is the wrong gate for that class. CI/tooling only —
> no render/shader code, no relink.

## What shipped
- **`scripts/visual_compare.py`** — policy-driven comparator (numpy + Pillow, both present).
  - Metrics (always computed + reported): `byte_exact`, `max_abs_delta`,
    `mean_abs_delta`, `pct_changed(>changed_lsb)`, windowed-luma `ssim` (integral-image
    box filter, numpy-only).
  - Policies (gate selection): `byte_exact` (default — require byte-identical),
    `low_tolerance` (`max_abs_delta≤2` + `pct_changed(>2)≤0.05%`), `perceptual`
    (`mean_abs_delta≤cap` + `ssim≥floor` — tolerates noise drift, fails real change).
  - Emits an amplified diff PNG (`--out-diff`) on any failure.
  - `--self-test` (synthetic images; no game captures — no mouse grab).
- **`docs/render-backend-seams/visual-tolerance-policy.json`** — registry: default
  `byte_exact`; per-family allowlist (`cloud`, `shoreline` → `perceptual`) resolved via
  `--family`. **Allowlist is noise-shaders-only — do not widen to deterministic shaders.**
- Registered in `check-contracts.sh` as `visual_compare` (runs the self-test).

## Why mean_abs_delta drives perceptual (calibration)
On representative diffs, `mean_abs_delta` cleanly separates noise-drift from real change
(SSIM on flat synthetic gradients is fragile — used only as a lenient secondary floor):

| diff | ssim | mean_abs | verdict |
|---|---|---|---|
| salt-pepper 3% ±40 (ULP-like) | 0.92 | 0.57 | perceptual PASS |
| large 30% +150 | 0.015 | 41 | perceptual FAIL |
| uniform +10 tint (subtle regression) | 0.98 | 10 | perceptual FAIL |

The perceptual defaults (`mean_abs_delta≤8`, `ssim≥0.90`) are **starting points**; the
`cloud`/`shoreline` thresholds get tuned against **real captures** in the FBM pilot.

## Acceptance — all met (self-test, no game)
| Gate | Result |
|---|---|
| existing byte-exact goldens still byte-exact | ✅ default policy unchanged |
| planted one-pixel change fails byte-exact | ✅ + diff emitted |
| small ULP/noise change passes only under perceptual | ✅ byte_exact FAIL, low_tolerance FAIL, perceptual PASS |
| large visible change fails perceptual | ✅ |
| diff artifact emitted on failure | ✅ amplified PNG |
| (bonus) subtle uniform tint caught by perceptual | ✅ mean_abs gate |
| no render/shader change, no relink | ✅ pure Python tooling |
| seam-aggregator self-test wired | ✅ `visual_compare` PASS |

## Exclusions honored
No render code, no shader code, no relink (the comparator reads existing PNGs; the
screenshot/capture harness is untouched). General, not SPIR-V-specific.

## Next
`SPIRV-FBM-POSTPROCESS-PILOT-1` — bake + consume `cloud` + `shoreline` (and future
FBM/noise shaders), gated with `--family <name>` → perceptual policy (tuned vs real
captures), still requiring: no GL errors, no fallback, exact artifact selection,
selected-corruption fallback, non-selected ignored.
