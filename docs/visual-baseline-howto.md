# Visual Baseline How-To (V-BASELINE-0)

This doc explains how to capture and compare visual baselines for the
StaticPropOpaque rendering lane. The goal is **objective** before/after
judgement — no eyeballing across days.

## Concepts

- **Camera preset** — a named tuple `(mission, warmup_s, smoke_seed)` that
  yields a deterministic frame when launched via `mc2.exe --profile stock
  --mission <m> --duration <d>` with `MC2_SMOKE_MODE=1` +
  `MC2_SMOKE_SEED=0xC0FFEE`. No in-engine camera-preset hook exists yet; the
  current preset identity *is* the deterministic state mc2 lands in. The
  registry lives in `tests/visual/baselines/presets.json`.
- **Baseline** — a `(PNG, JSON)` pair captured at a specific commit. PNG is
  the rendered (well, OS-screenshot of the) frame. JSON is the sidecar
  metadata: commit SHA, mission, preset name, resolution, env flags,
  PNG sha256.
- **Reference set** — the committed baselines under
  `tests/visual/baselines/` representing the visual state at the commit
  named in each sidecar's `commit` field.

## Capturing baselines

```
cd A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev

# Capture every preset
py -3 scripts/capture_baseline.py

# Capture a single preset
py -3 scripts/capture_baseline.py --preset staticprop_baseline_01

# Capture-and-self-verify: re-runs each preset twice and reports whether
# the two PNGs are byte-identical (see "Reproducibility" below).
py -3 scripts/capture_baseline.py --verify
```

Outputs land in `tests/visual/baselines/<preset>_<commit-short>.{png,json,log}`.

## Comparing before/after

Workflow for evaluating a candidate change:

1. **Capture reference** at the baseline commit (or use the already-committed
   reference set).
2. Apply the candidate change. Build + deploy as usual (see
   `.claude/skills/mc2-build.md` + `.claude/skills/mc2-deploy.md`).
3. **Capture candidate** at the new HEAD:
   ```
   py -3 scripts/capture_baseline.py
   ```
4. **Diff**. For each preset, you now have:
   - `<preset>_<ref-sha>.png`  (reference)
   - `<preset>_<new-sha>.png`  (candidate)

   Three diff techniques in order of rigor:

   - **Side-by-side eyeball** (fast, low rigor). Open both PNGs.
   - **ImageMagick pixel diff** (medium rigor):
     ```
     magick compare -metric AE \
       tests/visual/baselines/staticprop_baseline_01_<ref>.png \
       tests/visual/baselines/staticprop_baseline_01_<new>.png \
       diff.png
     ```
     Prints the count of differing pixels and writes a red-on-grey diff
     image. Threshold to taste; "zero" is achievable only in the
     deterministic-engine case below.
   - **Sidecar JSON diff** to see which env flags changed between the two
     captures:
     ```
     diff <(jq .flags tests/visual/baselines/<preset>_<ref>.json) \
          <(jq .flags tests/visual/baselines/<preset>_<new>.json)
     ```

## Reproducibility

Same commit + same preset + same env should produce a frame that, *as
rendered by the engine*, is byte-identical thanks to `MC2_SMOKE_SEED` (see
`GameOS/gameos/gos_smoke.cpp`).

But the captured PNG goes through an **OS-level screenshot path** (pyautogui
→ Win32 desktop capture), which introduces non-determinism that is
*outside* the engine:

- Cursor position (we park to screen center, but the cursor pixel is still
  in the image)
- Taskbar clock and any other desktop chrome behind the mc2 window
- Window-manager async paint timing on the first frame after foreground
- Multi-monitor / DPI scaling deltas across machines

Therefore: **engine-output equivalence is provable; PNG byte-identical
equivalence is not, on this capture path.** This is acceptable for the
V-BASELINE-0 charter: humans compare via PNG diff, and the sidecar's
`png_sha256` + flags catalog are the audit trail.

If true byte-identical capture is needed later (e.g. for CI), a future slice
should add an in-engine `glReadPixels` capture hook gated by an env var like
`MC2_FRAMEBUFFER_CAPTURE_PATH`. That is **out of scope for V-BASELINE-0**
per the brief's hard constraints (no GL state changes, no draw-path edits).

The `--verify` flag on `capture_baseline.py` exists to make this
non-determinism *visible* — if you see "identical=NO" with the two sha256s
differing, that is the OS-screenshot delta, not an engine regression.

## What the harness deliberately does NOT do

These are out of scope for V-BASELINE-0 and would belong to a follow-up
slice if needed:

- **In-engine framebuffer capture.** Would require a new env-gated
  `glReadPixels` path in the renderer.
- **In-engine camera preset positioning.** Would require a new env-gated
  camera init hook (`MC2_CAMERA_PRESET=...`). The current "preset" is
  defined entirely by mission + seed + warmup; if those produce a useful
  framing, that becomes the preset.
- **Automated pass/fail thresholding.** We emit data; humans (or a future
  reviewer slice) judge.
- **Cross-machine reproducibility.** GPU driver/version, monitor DPI, and
  desktop chrome vary; baselines are valid on the capture machine.

## Adding a new preset

Edit `tests/visual/baselines/presets.json` and add a new entry under
`presets`. Then capture and commit the resulting PNG+JSON. See
`tests/visual/baselines/README.md` for the registry schema.

## Files

- `scripts/capture_baseline.py` — capture harness
- `tests/visual/baselines/presets.json` — preset registry
- `tests/visual/baselines/README.md` — corpus naming + schema reference
- `tests/visual/baselines/*.png` + `*.json` — the reference set
