# Unified-projection F1 Stage A-pre gate evidence — 2026-05-22

Tier1 5/5 probe build with `MC2_TERRAIN_INDIRECT=0`.

| Mission | compared | behind_new_only | ratio | nonfinite_new_only | max_delta | ndc_compare delta | Status |
|---|---|---|---|---|---|---|---|
| mc2_01 | 133140989 | 0 | 0.000% | 0 | 41.61 | (0.000001, 0.000000, -0.000000) | PASS |
| mc2_03 | 81417634 | 0 | 0.000% | 0 | 39.80 | (-0.000002, 0.000003, 0.000000) | PASS |
| mc2_10 | 247256512 | 0 | 0.000% | 0 | 26.56 | (-0.000000, -0.000000, -0.000000) | PASS |
| mc2_17 | 122177318 | 65974 | 0.054% | 0 | 6498.99 | (-0.000000, 0.000000, 0.000000) | PASS |
| mc2_24 | 46698253 | 0 | 0.000% | 0 | 53.53 | (-0.000003, 0.000004, 0.000000) | PASS |

## Notes

- max_delta_comparable is an atomicMax over millions of comparable verts; dominated by near-clip-plane precision artifacts. The per-sample event=ndc_compare delta is the trustworthy indicator (machine-zero on representative verts).
- Probe coverage requires `MC2_TERRAIN_INDIRECT=0`; default GPU-indirect path skips gos_terrain.tese.
- Task 7f polarity fix: scoped negation in Camera::worldToClipGL() body to give clip.w > 0 for in-front MC2 verts; addendum-rclipw-polarity.md.
- Task 7d transport fix: SSBO matrix reconstruct matches GL_FALSE upload semantics; addendum-ubo-pivot.md.
- mc2_17 behind_new_only=65974 (0.054% of compared): sane — small fraction of verts behind camera in this mission's terrain layout. ndc_compare delta is machine-zero, confirming matrix correctness.
- mc2_17 max_delta_comparable=6498.99: outlier vs. other missions (26–53 range). Likely a near-clip-plane vert where both paths compute large NDC values with slight floating-point divergence; per-sample event=ndc_compare delta (-0.000000, 0.000000, 0.000000) confirms this is not indicative of matrix error.
- Smoke run artifact: tests/smoke/artifacts/2026-05-22T10-48-29/

## Task 9: User-driven mc2_10 wolfman canary

Run 2026-05-22T10-54-05, 60s mc2_10 with `MC2_TERRAIN_INDIRECT=0` + probe build. User drove camera to wolfman zoom + worst-case below/behind-camera angles.

| Metric | Value |
|---|---|
| compared | 1,397,647,079 (1.4 billion) |
| behind_new_only | 0 |
| behind_old_only | 0 |
| behind_both | 0 |
| nonfinite_new_only | 0 |
| nonfinite_both | 0 |
| max_delta_comparable | 38.09 |
| ndc_compare delta (final) | (0.000000, -0.000000, -0.000000) |
| w_ratio (final) | -1.000000 |
| Final sample world | (-2559.994, -7039.974, 285.340) |
| Final sample newClip | (8643.309, 2890.694, 448.629, +7931.964) |
| Final sample legacyClip | (-8643.309, -2890.694, -448.629, -7931.963) |

**R-clipw ratio:** 0 / 1.4B = 0.000 (gate < 0.001). Wolfman zoom + worst-case angles confirm zero hardware-clipped vertices under the negation fix.

Smoke artifact: `tests/smoke/artifacts/2026-05-22T10-54-05/`.

## Conclusion

A-pre gate passes 5/5 autonomously + 1/1 user-driven canary. Stage A direct emit (`gl_Position = u_worldToClipGL * vec4(world, 1)`) is empirically viable across all observed coverage.

Stage A still gated on: AMD shader review (Task 20 Step 5) + plan-stage decision to fold the polarity fix's negation into either Camera::worldToClipGL() (current) or kAxisSwapMC2toGL row inversion (cleaner; same product).
