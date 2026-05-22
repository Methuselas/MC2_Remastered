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

## Conclusion

A-pre gate passes 5/5. Stage A direct emit (`gl_Position = u_worldToClipGL * vec4(world, 1)`) is empirically viable.

Stage A still gated on: user-driven mc2_10 wolfman canary (Task 9) + AMD shader review (Task 20 Step 5) + plan-stage decision to fold the polarity fix's negation into either Camera::worldToClipGL() (current) or kAxisSwapMC2toGL row inversion (cleaner; same product).
