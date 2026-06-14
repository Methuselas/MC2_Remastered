# Baseline-A Capture Runbook (S9 in-engine, warmup-stabilized)

**Status:** v1, 2026-06-13. Owns the *procedure* for producing and blessing a
Baseline-A golden-frame set. Schema/lifecycle owner remains
`visual-regression-lab-architecture.md` (§3 golden-sets, §8 Baseline-A); this
is the operational checklist + the warmup rule that keeps a bless honest.

## Why a warmup rule

The first cold run of a capture is NOT representative: shader compile, texture
residency, static-prop streaming, PBR cache, and first-use GL state all warm on
run 1. Blessing a cold run blesses first-use noise. Measured on `mc2_01`
2026-06-13 off `mc2-win64-v0.4d-rc1`: `highangle_wide` produced a different
sha on the cold run than on warm runs — and, worse, kept drifting run-to-run
even after warmup (see Known-unstable). The other two bookmarks were byte-
identical on **every** run.

## The rule

```
run N captures (N>=3 recommended)
discard the first K as warmup (K=1)
require runs[K:] byte-identical per bookmark
bless the LAST run
```

Implemented by `scripts/run_visual_capture.py`:

```
py -3 scripts/run_visual_capture.py --mission mc2_01 \
    --exe "A:/Games/mc2-opengl/releases/mc2-win64-v0.4d-rc1/mc2.exe" \
    --runs 3 --warmup 1 --candidate-set baselineA-rc1 [--allow-partial]
```

- `--runs 3 --warmup 1`: 3 launches, run 1 discarded, runs 2 & 3 must match.
- `--candidate-set <id>`: on stability PASS, copy the last run's PNGs+sidecars
  into `tests/visual/baselines/<id>/`, write `<id>/set.json`, and register the
  set in `tests/visual/golden-sets.json` as **status=candidate**.
- `--allow-partial`: materialize the byte-stable subset even if some bookmarks
  drift; drifters are recorded in `set.json.excluded_unstable`. A small-but-
  real baseline beats a fake-complete one.
- Determinism requires `MC2_SMOKE_FIXED_TIMESTEP=1` (default ON in the runner;
  the engine stamps each sidecar's `deterministic` honestly).
- Uses the engine glReadPixels FBO readback (NOT pyautogui) — no screen grab,
  runs minimized. The OS-screenshot `capture_baseline.py` is for eyeballing
  only and the lab never grades it (lab §10 anti-goal).

## Bless criteria (all must hold)

```
[ ] RC exe sha256 recorded            -> set.json identity.exe.sha256
[ ] git commit recorded               -> set.json identity.git.commit
[ ] env recorded                      -> set.json identity.env_delta
[ ] mission recorded                  -> set.json mission
[ ] bookmarks recorded                -> set.json bookmarks[]
[ ] engine deterministic = true       -> each bookmark.engine_deterministic
[ ] run K+1 == ... == run N (per bm)  -> stabilization + per-bookmark sha match
[ ] golden-sets.json written          -> status=candidate
[ ] sidecar manifests S12-conformant  -> scripts/check-manifest-schema.py
```

**Bless = a human commit** flipping `golden-sets.json` `status: candidate ->
blessed` and setting `blessed_commit` (lab §3). The runner never blesses; it
only produces the candidate + the evidence. Re-bless on any *intentional*
visual change, naming the change in the commit (lab §3, same discipline as
`tests/smoke/baselines.json`).

## Current candidate: `baselineA-rc1`

- Off `mc2-win64-v0.4d-rc1` (exe sha256 `6383bbf0…`; byte-identical to the
  deployed v0.4 game exe — same build `c5d255de`). Worktree HEAD `df7630bc`.
- **Stable (blessable):** `overview_center` (`93c8ef99…`, covers terrain_splat
  + sky), `ridge_lowangle` (`11ac0201…`, covers terrain_lod_chunk_skirts +
  shadow_cascade). Byte-identical across every run today.
- **Excluded — known unstable (v2):** `highangle_wide` (covers
  terrain_splat + static_prop_pbr). Cycles across runs (`2756b466` /
  `9d865796` / `d04198ad`) with fixed timestep + sim-freeze engaged → the
  drift source is async render-side (static-prop streaming / chunk-LOD
  admission / PBR first-use), NOT the sim clock and NOT pure cold-start. The
  sweep's `settle` flushes but does not *deterministically order* that
  admission. Fix is engine-side (freeze/serialize the streaming source before
  capture) and is **out of scope** until after Baseline-A v1 is blessed.

## Do NOT over-expand before bless

Held until Baseline-A v1 is blessed (governance §3.4; lab §8): Tube merge,
GlStateGuard, FX fixture, pixel-diff verdict hard-gates, more bookmark
expansion, the `highangle_wide` streaming-freeze fix. The first blessed
baseline can be small (2 bookmarks). It just needs to be real.

## Artifact identity

All capture sidecars + `set.json` carry the unified `mc2-manifest/1` identity
block (`scripts/manifest_schema.py`, S12) so a golden set joins with smoke run
manifests, deploy manifests, and release reports on the same `exe.sha256` /
`git.commit` / `deploy_target` fields. Conformance:
`py -3 scripts/check-manifest-schema.py tests/visual`.
```
```
