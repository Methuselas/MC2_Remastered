# XFORM-CONVENTION-HARNESS-1 — recon + build plan

**Arc:** VULKAN-CONTRACT-MANIFEST-ARC option D · 2026-06-22 · verdict **CLEAN-SEAM-EXISTS**
Implementation in flight on branch `claude/xform-convention-harness-1` (worktree `A:/Games/mc2-xform-harness`).

## Why
Slice 5 (xform ledger) documented the conventions; this locks them with a GL-free host test so a future change — notably BT2018 GPU mech placement (1B → per-instance/GPU bone-index) — cannot silently regress matrix space, handedness, or clip conventions.

## The seam (testable leaves, NOT the Camera class)
The Camera-member projection methods (`Camera::projectZ`, `worldToClipGL`) remain DEFER for host testing — `camera.h` is consumed by ~30 TUs (class-layout risk), re-confirmed by `camera-harness-recon-1.md`. The win is the layer below:
- `mclib/camera.cpp` file-scope free functions `makeAxisSwapMC2toGL()` (~:78) and `makePixelHomogToGLNDC()` (~:143) — pure, depend only on `Stuff::Matrix4D`, no GL. These carry the axis-swap (det = +1, even parity / winding-PRESERVED — y↔z transposition + x negation; the recon's earlier "det=−1 handedness flip" was WRONG, corrected to match the verified test and the MC2_GLTF_AXIS=2 even-parity note), Y-flip, and w-sign. Lift to a leaf `mclib/xform_conventions.{h,cpp}` so a host test links them without GL-coupled camera.cpp.
- The `gos_SetWorldToClipGL` chokepoint (`gameos_graphics.cpp:8956`) is the single producer of the canonical `u_worldToClipGL`.
- `code/gamecam.cpp:333` F1-3C per-frame byte-compare proves UBO payload == `gos_GetTerrainMVPMat4()` (maxDiff ≤ 1e-5) — today print-not-fail; promote to fatal under a gate.

## Build plan (three parts — being implemented)
1. **Lift** the two pure functions to `mclib/xform_conventions.{h,cpp}` (behavior-preserving; camera.cpp delegates). Add to main build + `mc2_tests` sources.
2. **Host test** `tests/unit/test_xform_convention.cpp` (doctest, GL-free): axis-swap parity (det=+1 even/winding-preserved, basis→(−x,elev,north)); reverse-Z range [0,1] near→1/far→0; row-major repack round-trip (lock `M[i*4+j]=col[j*4+i]` vs `stuffToRowMajor`); clip.w>0 in-front; static-prop v*M det>0 + col-3=origin.
3. **Promote F1-3C** to a hard invariant under new default-OFF gate `MC2_XFORM_PARITY_FATAL` (abort on maxDiff fail when set; byte-identical when unset). Register the env var.

## How it guards BT2018 GPU placement
(a) mech bake already routes `mc2skel::EvaluateClipGpuBones` whose JSON is the banked oracle — a GPU-placement port must keep engine-checksum == harness-checksum (1A acceptance, now mechanizable). (b) If BT2018 moves to a per-instance model matrix like static props, the v*M order + det>0 + axis-swap asserts catch a column-vs-row or handedness flip at unit-test time, before any frame renders. (c) The promoted F1-3C fatal gate catches divergence between the world→clip the new mech path consumes (UBO binding=3) and the canonical terrain MVP.

## Status
Implementation dispatched (subagent). Host-test (mc2_tests) is the cheap validation; the camera.cpp lift + gamecam.cpp gate need a full mc2 relink to validate the ~30 camera consumers — that build is the long pole and may land as WIP. This doc banks the recon; the commit on the harness branch will carry the code + test results.
