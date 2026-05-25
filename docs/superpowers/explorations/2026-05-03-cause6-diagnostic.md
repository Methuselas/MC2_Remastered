# Cause #6 Diagnostic — "CPU numLights>0 but GPU numLights=0"

Date: 2026-05-03
Branch: `claude/nifty-mendeleev`
HEAD: `d494638`
Scope: READ-ONLY diagnostic with env-gated trace instrumentation.
Mission: mc2_18, 15s smoke run.

---

## §1. Executive Summary

**Cause #6 as originally described (GPU numLights=0 for actors where CPU numLights>0) is NOT present at HEAD d494638.**

Three trace layers confirm:
1. `event=cacheGpuLightData` — every actor caches `gatheredNumLights >= 1` (all Bldg actors get 2, Tree actors get 1).
2. `event=submit_cause6` — every submitted typeId has `uboNumLights >= 1`. No `gatherFallback=1` events occurred.
3. Addition 3 GPU readback — typeId=474 shows `gpu_numLights=2`.

The white-out anomaly for typeId=474 that was catalogued as "Cause #6" in the Stage 2.D.3 inventory was **not caused by numLights=0** on the GPU. It was caused by the missing `kFlagIsLightsOut` gate in `static_prop.vert` (the N1.5 HOLD fix) — when base_light is seeded from `aRGBLight=0xFFFFFFE7` (non-magic RGB seed, ~0xFF value) and numLights=2, the lighting loop accumulates a non-zero final and it saturates to white. N1.5 (already in the working tree) correctly sets `base_light=vec3(0)` for lightsOut actors, fixing the white-out.

The **active mismatch pattern in mc2_18 at this HEAD** is 19 typeIds with Hot Pink `aRGBLight=0xFFFF00FF` vertices showing `cpu=0xFF2F2F2F, gpu=0xFF000000` — a separate bug class documented in §5.

---

## §2. Cause #6 Actor List

**Zero actors have GPU numLights=0 with CPU numLights>0.**

Complete inventory from `event=submit_cause6` trace (192 unique typeIds sampled):
- 173 Bldg typeIds: all `uboNumLights=2`
- 19 Tree typeIds: all `uboNumLights=1`
- 0 Generic typeIds (no Generic population registered in mc2_18 at this run)
- 0 actors with `uboNumLights=0`
- 0 fallback path taken (`gatherFallback=0` for all 192)

cacheGpuLightData trace (52 unique TG_Shape typeKeys sampled):
- All show `sNumLightsAtEntry=2` (Bldg/Tree actors call SetLightList before CacheGpuLightData)
- All show `gatheredNumLights >= 1`
- `preIndex=0xFFFFFFFF` for all (first-frame cache, sentinel as expected)

The N1.5 HOLD fix (`kFlagIsLightsOut` branch in `static_prop.vert`) is the correct fix for typeId=474 white-out.

---

## §3. Data-Flow Trace Row Table (representative 10 actors)

| typeId | pop  | lightsOut | cachedIdx | lightDataIndex | gatherFallback | uboNumLights | GPU numLights (Addition 3) | mismatching? |
|--------|------|-----------|-----------|----------------|----------------|--------------|---------------------------|--------------|
| 474    | Bldg | 1         | 0x00000002| 2              | 0              | 2            | 2 (confirmed)             | YES (N1.5 HOLD artifact) |
| 612    | Tree | 0         | 0x00000000| 0              | 0              | 1            | not sampled               | NO  |
| 161    | Bldg | 0         | 0x00000001| 1              | 0              | 2            | not sampled               | NO  |
| 306    | Bldg | 0         | 0x00000002| 2              | 0              | 2            | not sampled               | NO  |
| 182    | Tree | 0         | 0x00000004| 4              | 0              | 1            | not sampled               | NO  |
| 68     | Bldg | 0         | 0x00000002| 2              | 0              | 2            | not sampled               | NO  |
| 69     | Bldg | 0         | 2 (via LDI)| 2             | 0 (non-first child) | 2    | not sampled               | YES (separate bug) |
| 162    | Bldg | 0         | 1 (via LDI)| 1             | 0 (non-first child) | 2    | not sampled               | YES (separate bug) |
| 692    | Bldg | 0         | 0x00000000| 0              | 0              | 2            | not sampled               | NO  |
| 698    | Bldg | 0         | 0x00000057| 87             | 0              | 2            | not sampled               | NO  |

Note: typeId=69 and typeId=162 are NON-FIRST children of their multishape — they don't appear in `submit_cause6` (which traces the first SHAPE_NODE leaf of each multishape). Their `lightDataIndex` was retrieved from the `submit_header` trace. Their `uboNumLights=2` is inferred from the `lightDataIndex` → `submit_cause6` index mapping.

---

## §4. Pattern Identified

**Cause #6 (a)-(e) classification: NONE OF THE ABOVE applies.**

The active mismatch at HEAD d494638 is NOT a numLights=0 issue. The three candidate paths for
numLights=0:
- **(a) CacheGpuLightData() early-returns before gather**: Not observed. All 52 sampled typeKeys
  executed the full gather path. `sNumLightsAtEntry=2` for all Bldg/Tree actors confirms
  `SetLightList` was called with valid lights before `CacheGpuLightData()`.
- **(b) Gather runs but stays at sentinel**: Not observed. All `postIndex != 0xFFFFFFFF`.
- **(c) Cache valid but submit() falls back**: Not observed. `gatherFallback=0` for all 192 typeIds.
- **(d) SSBO upload writes wrong value**: Cannot be zero because `uboNumLights >= 1` for all actors.
- **(e) SSBO correct but shader reads stale UBO**: Cannot apply when `gpu_numLights=2` (Addition 3
  readback confirmed for typeId=474).

**Actual Cause #6 root cause clarification:** The "22 typeIds with GPU numLights=0" described
in the advisor dispatch was likely a mischaracterization of the white-out symptom. The white-out
for typeId=474 was produced by `base_light ≈ vec3(1)` (from non-magic RGB seed in `aRGBLight`)
combined with `numLights=2` saturating above 1.0. The root cause is the missing `kFlagIsLightsOut`
gate (N1.5), not numLights=0 on the GPU.

**The GenericAppearance SetLightList(NULL,0) concern** (architectural): At genactor.cpp:1201,
`genShape->SetLightList(NULL, 0)` is called before `genShape->CacheGpuLightData()` at genactor.cpp:1219.
This would give `gatheredNumLights=0` for Generic actors. However, **no Generic actors were registered
or submitted in mc2_18 at this run**. If mc2_18 had Generic actors, they WOULD get `uboNumLights=0`.
This is a latent architectural issue that remains untested in this run.

---

## §5. Active Mismatch: 19 Hot Pink typeIds (separate bug class)

Observed at parity compare frames 11 and 131: 19 typeIds show `cpu=0xFF2F2F2F, gpu=0xFF000000`.

**All have `aRGBLight=0xFFFF00FF` (Hot Pink magic tag) on all vertices.**

Pattern analysis:
- CPU: Hot Pink daytime → `base_light=0`, lighting loop (INFINITE + AMBIENT with ~0x1F AMBIENT)
  → final ≈ 0x1F-0x33 (ambient + some INFINITE if front-facing). Matches `cpu=0xFF2F2F2F`.
- GPU: `gpu=0xFF000000` (black, all channels 0). With `numLights=2` and calc_light running,
  this requires the lit output to be exactly `vec3(0)`.

**Hypothesis**: These typeIds have `isWindow=true` (TG_Shape::isWindow set by `LitWin_*` node name
detection at tgl.cpp:260), which in the GPU sets `lit = base_light = 0` via the
`kFlagIsWindow | kFlagIsSpotlight` gate (static_prop.vert:221). But CPU Hot Pink at daytime
produces non-zero output because CPU's isWindow gate only suppresses the lighting LOOP
(tgl.cpp:1936) — it doesn't suppress the ambient tail-add at tgl.cpp:2207-2209.

Wait — **CPU also skips the loop when isWindow=true**, and `redAmb` is populated inside the loop.
If the loop is skipped, `redAmb=0`, and the tail-add gives 0. CPU result would be 0 too.
But CPU shows 0x2F. Therefore: **these are NOT window nodes** (isWindow=false). Something else
causes GPU to produce 0.

**Alternative hypothesis**: The ambient contribution in `calc_light` is not reaching the Hot Pink
actors. This could be a light_type decode error at the GPU side for the AMBIENT light (type=0,
stored as `float(0)` in `light_dir[i].w`, decoded as `int(0.0f) = 0`). If `int(0.0f)` doesn't
equal `TG_LIGHT_AMBIENT = 0` on some code paths... or if the loop is not executing. With `numLights=2`
and both lights valid, the loop should run.

**This 19-typeId bug class is not Cause #6 and requires a separate investigation. It is likely
the "grayscale-additive" pattern from the Stage 2.D.3 inventory (`gpu-darker-than-cpu` with
uniform RGB delta).**

---

## §6. Fix Scope Estimate for Actual Root Cause

- **typeId=474 white-out**: The N1.5 HOLD fix in `shaders/static_prop.vert` is the correct fix.
  Scope: SMALL — one conditional gate already written, on HOLD pending commit authorization.

- **GenericAppearance numLights=0 latent bug**: The architectural fix is to call
  `CacheGpuLightData()` BEFORE `SetLightList(NULL,0)` at genactor.cpp — or, recognize that
  Generic actors with NULL lights should produce a specific lighting behavior. Scope: SMALL
  (one-line reorder or conditional), but requires verification that no Generic actors in
  any stock mission are expected to have lights.

- **19 Hot Pink GPU-black bug**: Unknown root cause. Requires further investigation with the
  `isWindow` flag trace and the `flags` value written to the instance SSBO for these typeIds.
  Scope: TBD (likely SMALL — shader flag or CPU flag population gap).

---

## §7. Open Questions

1. **GenericAppearance numLights=0 path**: Does any stock mission register Generic actors with
   the GPU batcher? If so, do they expect lights or not? The SetLightList(NULL,0) at genactor.cpp:1201
   precedes CacheGpuLightData() — this guarantees `gatheredNumLights=0` for all Generic actors.
   This must be verified on a mission that actually has Generic actors.

2. **Hot Pink GPU-black bug**: Why does GPU return `vec3(0)` for Hot Pink daytime actors with
   valid `numLights=2`? The `submit_cause6` trace doesn't capture the `flags` byte for these
   non-first children. An extension to the `submit()` per-leaf trace or an `ssbo_upload` event
   for typeId=69/162 would clarify whether `kFlagIsWindow` is being set incorrectly.

3. **typeId=474 frame=3 parity mismatch**: The `gpu=0x00000002` value is the Addition 3 debug
   write — not the rendered color. The actual visual output with N1.5 active should be ~0x9A9A9A
   (matches cpu). This mismatch event is a diagnostic artifact and should NOT be counted as a
   real substrate gap.

4. **22 typeIds described in dispatch vs observed**: The dispatch described "~22 typeIds in mc2_18
   with CPU numLights>0 but GPU numLights=0." This does NOT match the observed data. The 22 figure
   may have been derived from an earlier HEAD state or from a different mismatch inventory. The
   current state has ZERO actors with `uboNumLights=0` in mc2_18.

---

## §8. Trace Files

- Smoke artifact: `tests/smoke/artifacts/2026-05-03T18-54-28/mc2_18.log`
- Trace events: `[PARITY_DIAG v2] event=cacheGpuLightData`, `event=submit_cause6`
- GPU readback: `[PARITY_DIAG v2] event=shader_observed typeId=474 ... gpu_numLights=2`
- Parity mismatches: `[OBJECT_PARITY v1] event=lighting_mismatch`

---

## §9. Files Modified (trace only, no substrate changes)

| File | Lines added | Role |
|------|-------------|------|
| `mclib/txmmgr.h` | +4 | `getLightDataNumLights(uint32_t)` declaration |
| `mclib/txmmgr.cpp` | +7 | `getLightDataNumLights()` definition |
| `mclib/msl.cpp` | +3 includes, +27 trace | `CacheGpuLightData()` entry/exit trace |
| `GameOS/gameos/gos_static_prop_batcher.cpp` | +1 include, +28 trace | `submit_cause6` trace in `submitMultiShape()` |

No commits made. No lighting math edits. No `mclib/tgl.cpp` core edits.
`shaders/include/lighting.hglsl` and `shaders/static_prop.vert` HOLDS preserved.
