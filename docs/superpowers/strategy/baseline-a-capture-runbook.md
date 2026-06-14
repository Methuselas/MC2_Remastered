# Baseline-A Capture Runbook (S9 in-engine, warmup-stabilized)

**Status:** v1, 2026-06-13. Owns the *procedure* for producing and blessing a
Baseline-A golden-frame set. Schema/lifecycle owner remains
`visual-regression-lab-architecture.md` (§3 golden-sets, §8 Baseline-A); this
is the operational checklist + the warmup rule that keeps a bless honest.

## Why a warmup rule

The first cold run of a capture is NOT representative: shader compile, texture
residency, static-prop streaming, PBR cache, and first-use GL state all warm on
run 1. Blessing a cold run blesses first-use noise.

## Cursor MUST be parked (load-bearing)

The MC2 RTS camera **edge-scrolls whenever the OS cursor sits at a screen edge
— regardless of window focus or whether mc2.exe is minimized**
(memory: `smoke_autonomous_run_pattern` fact #2). The bookmark sweep re-applies
the pose each settle frame, but edge-scroll adds a per-frame camera delta on
top, so an un-parked cursor makes wide/high-altitude framings drift
nondeterministically (they "sometimes match" only when the cursor happens to
land off-edge). `run_visual_capture.py` parks the cursor to screen center
(`SetCursorPos`) before launch AND every poll iteration, so it stays pinned
through the entire sweep window.

**This was a real harness bug, not engine nondeterminism.** 2026-06-13 off
`mc2-win64-v0.4d-rc1`: with no cursor park, `highangle_wide` cycled
`2756b466`/`9d865796`/`d04198ad` across runs while the two tighter poses stayed
byte-identical (their narrow framing masked the edge-scroll delta). Adding the
cursor park made **all three** byte-stable (`highangle_wide` = `2756b466` every
run). Do not attribute capture drift to the engine before confirming the cursor
is parked.

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

## Current candidate: `baselineA-rc1` (FULL 3/3)

- Off `mc2-win64-v0.4d-rc1` (exe sha256 `6383bbf0…`; byte-identical to the
  deployed v0.4 game exe — same build `c5d255de`). Worktree HEAD `df7630bc`.
- **All three bookmarks byte-stable** (runs 2 & 3 identical, cursor parked):
  `overview_center` (`93c8ef99…`, terrain_splat + sky), `ridge_lowangle`
  (`11ac0201…`, terrain_lod_chunk_skirts + shadow_cascade), `highangle_wide`
  (`2756b466…`, terrain_splat + static_prop_pbr). No exclusions.

## Do NOT over-expand before bless

Held until Baseline-A v1 is blessed (governance §3.4; lab §8): Tube merge,
GlStateGuard, FX fixture, pixel-diff verdict hard-gates, more bookmark
expansion. The blessed baseline can be small. It just needs to be real.

## Artifact identity

All capture sidecars + `set.json` carry the unified `mc2-manifest/1` identity
block (`scripts/manifest_schema.py`, S12) so a golden set joins with smoke run
manifests, deploy manifests, and release reports on the same `exe.sha256` /
`git.commit` / `deploy_target` fields. Conformance:
`py -3 scripts/check-manifest-schema.py tests/visual`.
```
```
