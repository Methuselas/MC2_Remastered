# HZB Static-Prop Cull — Recon

`HZB-STATICPROP-CULL-RECON-1`. Wires **real static-prop bounds** into the
existing diagnostic HZB occlusion probe (`gos_postprocess::runHzbProbe`). Still
**no culling, no draw suppression** — counters only, `neverAppliedToDraws=1`.
This lane proves the would-cull mechanism works on real objects and surfaces the
false-occlusion risk *before* any cull consumer is contemplated.

## What was added

Per active static prop (capped, tombstone-aware iteration via
`GpuStaticPropRegistry::getActiveCount` + `staticPropGetModelMatrix` +
`staticPropGetExtentRadius`):

1. AABB = world center `(-mtx[3], mtx[11], mtx[7])` (MC2 east/north/elev) ±
   extent radius.
2. Project the 8 corners through `gos_postprocess::viewProj_` — the GL-NDC
   reverse-Z `world→clip` transform fed by `Camera::worldToClipGL()`, the SAME
   convention that produced the scene depth the HZB was built from (column-major,
   row-vector multiply, matching the sun-screen projection in `renderSkybox`).
3. Screen rect + `objClosest = max(clip.z/clip.w)` (reverse-Z = nearest point).
4. Pick HZB LOD from rect pixel size, **clamped coarser** to `Lmin` (dims ≤ 256;
   clamping coarser only ever keeps MORE — strictly conservative). Sample that
   level's covered texels, take MIN (farthest occluder).
5. Conservative cull comparison `objClosest < hzbMin` → `wouldCull` else
   `wouldKeep`. Logged as `[HZB_PROBE_OBJ v1]`. **Nothing acts on it.**

Cost-bounded: HZB readback restricted to levels ≥ `Lmin`; per-frame object cap
4096; inner texel loop capped at 4×4. Gate `MC2_HZB_PROBE` (default OFF, requires
`MC2_HZB_BUILD`). No new env gate, no shader, no contract change.

## Results (gate-ON, MC2_HZB_BUILD=1 MC2_HZB_PROBE=1)

| Mission | active | tested/frame | wouldKeep:wouldCull (normal) | self-test | GL err | FBO inc | Δdestroys |
|---------|--------|--------------|------------------------------|-----------|--------|---------|-----------|
| mc2_03  | 2552   | 42–161       | keep-dominant, cull 0–12      | clean     | 0      | 0       | +0        |
| mc2_17  | 1521   | 87–207       | keep-dominant; cull spikes on camera move | clean | 0 | 0 | +0 |
| mc2_24  | 2641   | 25–40        | keep-dominant, cull 0–4       | clean     | 0      | 0       | +0        |

- **`tested > 0` every gameplay frame → projection convention is CORRECT.** The
  `offscreen≈active, tested≈0` axis-swap failure mode did NOT occur. `offscreen`
  is large simply because the overhead RTS camera sees a fraction of the
  map-wide prop set.
- **`wouldKeep` dominates `wouldCull`** in steady frames; the HZB self-test stays
  `wouldCull=0 integrityMismatch=0` (the reduction is a faithful conservative
  MIN). The math is conservative-correct.

## KEY RISK (must resolve before any cull consumer)

- **Transient `wouldCull` spike during camera motion.** mc2_17 hit
  `tested=87 wouldKeep=22 wouldCull=65` (~75%) on a frame where the camera
  turned. Steady frames are keep-dominant; the spike correlates with motion.
  Leading hypothesis: **frame coherence** — `viewProj_` may be set at a slightly
  different point in the frame than the depth render that built the HZB, so under
  fast motion the projected screen rects lag the depth by ~1 frame and flag
  edge/grazing props as occluded. This is exactly a *false*-occlusion source.
  A cull consumer that acted on this would briefly pop visible props during
  camera moves.
- **High `nearClipped`** (any AABB corner with `clip.w ≤ 0`): currently the whole
  object is conservatively KEPT. Safe, but a real cull path wants per-plane near
  clipping rather than whole-object skip, or it leaves large props near the eye
  untestable.

## Verdict / next

The substrate + projection + conservative comparison are proven on real props
across three missions with zero rendering impact. **Do NOT build a cull consumer
yet.** Next investigation (`HZB-STATICPROP-CULL-FRAMECOHERENCE-1`, recon/diag):
- Confirm whether `viewProj_` and the HZB-source depth are the SAME frame; if
  not, snapshot the exact `world→clip` used for that frame's depth and project
  with it.
- Add a per-object visual cross-check (highlight `wouldCull` props in the debug
  preview) to separate TRUE occlusion from FALSE before trusting the rate.
- Only after the motion spike is explained and the false-occlusion budget is
  measured ~0 should a default-OFF, kill-switched cull consumer be considered.
Stop conditions remain as in `docs/hzb-visibility-mvp.md`.
