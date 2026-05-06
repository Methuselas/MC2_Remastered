# Track A3 — Terrain Admission Predicate Replacement (sketch)

> **Status:** Sketch only. CONDITIONAL on data from Track A1 + A2 ship.
> Plan-writing happens AFTER A1+A2 capture data is reviewed.

## Why this is a sketch and not a full plan

The project's own capture-replay data (`docs/superpowers/specs/projectz-capture-report.md` §5) verdicts terrain admission with **High** confidence as the *worst* candidate among the three wedge-class wrappers for predicate replacement:

- Modern predicates over-cull terrain ~36% across missions (vs. ~5-10% for object/effect admission).
- `rectGuard` permissive admissions concentrate at 74% on terrain — terrain is THE wedge-risk vector.

This means the same predicate-replacement pattern that works cleanly for object admission (A1, 1 site, lifecycle gate) and effects admission (A2, 7 sites, billboard rendering) may NOT work cleanly for terrain (6 sites, dense vertex admission, ~40K calls/frame).

A1+A2 are the proving ground. If they ship clean, we re-run the capture-replay corpus with the new predicate active on those wrappers and look at:

1. Did A1+A2's residual disagreement counts stay inside their authored envelopes? If yes, predicate logic is sound at scale.
2. Does the residual visual-quality gap (wolfman zoom over-cull, etc.) attributable to terrain admission justify the wedge-risk?
3. Do A1+A2 reveal any new failure modes that don't apply to terrain? (Or vice versa — the project still uses the rect predicate on terrain regardless.)

## Sites (verified at session 2026-05-06)

Six callsites in `mclib/terrain.cpp`:
- `terrain.cpp:1438` — clone added since 2026-04-25 inventory.
- `terrain.cpp:1597` — original.
- Plus 4 others — full enumeration via `grep -n "projectForTerrainAdmission" mclib/terrain.cpp` at plan-write time.

## What the slice would look like (if green-lit)

Same dual-output wrapper pattern as A1/A2:
- `Camera::projectForTerrainAdmission` body changes to declare `LegacyProjectionResult result;`, call `projectZ(point, screen, &result)`, return either legacy bool (default) or `clipSpaceFrustumAdmit(result.rawClip)` (modern) per env flag `MC2_TERRAIN_ADMISSION_PREDICATE=modern|legacy`.
- All 6 callsites unchanged at the call site itself.
- Substrate (predicate function, env probe, trace `homogClipFull` candidate) already shipped from A1.

## Gates / risks specific to terrain

- **Volume.** Terrain admission fires per-vertex × per-quad × per-frame. ~40K calls/frame at typical zoom. Even small per-call divergence accumulates. Heatmap envelope authoring will need much wider tolerance bands than A1's single-site capture.
- **Wolfman zoom.** Terrain rect-finite predicate's known false-negative rate at extreme zoom-out (~87% per `memory/wolfman_is_max_zoom.md` and `memory/cull_gates_are_load_bearing.md` background) IS the visual symptom we'd hope to fix. But terrain is also where the rect-permissive admissions concentrate. Net: hard to predict whether modern predicate is net-positive without measurement.
- **`UndisplacedDepth` interaction.** Track terrain shaders use `UndisplacedDepth` for over-displacement compensation (`gos_terrain.frag:712` `gl_FragDepth = clamp(max(UndisplacedDepth, gl_FragCoord.z) + 0.0005, 0.0, 1.0)`). Now hardware-native after clip-control. A predicate change on terrain admission shouldn't perturb this further, but the interaction requires fresh visual canary.
- **Performance.** A predicate body that's even slightly more expensive than the rect-finite test multiplies by 40K calls/frame. Need to measure both correctness AND throughput on terrain.

## Decision criteria for promoting from sketch to plan

After A1+A2 ship default-on and soak ≥1 week jointly, run:

1. **Heatmap re-capture** with all three wedge-class wrappers active (A1+A2 modern + A3 still legacy). Compare A3's `homogClipFull_disagreements` count vs. legacy admission count. Build a candidate envelope.
2. **Visual quality audit** at wolfman zoom on tier1 stock + cinematic camera moves. Look for: terrain pop-in/out at frustum edges, terrain gaps where rect predicate previously over-rejected.
3. **Tracy profile** of `Terrain::geometry()` per-quad cost. Modern predicate's per-call cost vs. legacy.

**Promote to plan if:** envelope is small enough to authoring-confidence (per-mission disagreement <5% of legacy admit count), wolfman canary shows fixed cases, perf delta within ±5%.

**Stay as sketch (i.e., DON'T ship A3) if:** envelope is too noisy to author cleanly, OR wolfman fixes are absent, OR perf delta is >10% adverse, OR A1/A2 surface unexpected interaction issues with the modern predicate at scale.

## When to revisit

- After A2 ships default-on (so we have both A1 and A2 capture data).
- After ≥1 week joint A1+A2-modern soak.
- Before any HZB/Track G work that would change the visual-quality calculus on terrain.

If A1+A2 close cleanly and the data argues for A3, plan-writing follows the writing-plans skill cadence (recon-zero → brainstorm → spec → plan → adversarial review → execute) using A1's plan as the reference template.

If A1+A2 close cleanly but data argues against A3, this sketch stays as the closing document and the wrapper stays on legacy permanently. That's a defensible end-state — the project's own data justifies it.
