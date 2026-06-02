# MC2 Asset Viewer — Resolution-Tier Switcher + Constant Display Size

**Date:** 2026-06-02
**Slice:** `MC2-ASSET-VIEWER-RESTIERS-0`
**Base:** `claude/asset-viewer-res-tiers` off `claude/nifty-mendeleev` (has the KTX2 decoder + `data/tgl/128` default folder).
**Status:** SPEC — design approved 2026-06-02. Next: writing-plans → adversarial review → execute.

## Goal

Two related improvements to `mc2_asset_viewer`'s texture browser:

- **Part A — constant display size.** The preview must render every texture at the
  same on-screen size regardless of the texture's native resolution. Today a 256×256
  texture draws twice as large as a 128×128 (size = `nativePixels × zoom`), so opening
  a bigger texture or switching resolution tiers "zooms in." Fix: size the image by
  **fit-to-region × zoom** so source resolution no longer changes the on-screen size.
- **Part B — resolution-tier switcher.** A `64/128/256/512` selector that detects the
  **numeric sibling folders** of the current browse folder and repoints the browser to
  the chosen tier, keeping the same selected asset (same filename) so you see the same
  texture at a different resolution. Generic: works for `tgl/{128,256,512}` and
  terrain's `textures/{64,128,256}` alike; tiers that don't exist on disk are simply
  not offered.

## Context / findings (2026-06-02)

- The cooking pipeline already exists (`upscale_gpu.py` Real-ESRGAN 4× → `tools/mc2texcook/batch_cook.py` → BC7 KTX2). **But `tgl` only has the `128` tier cooked today** (256/512 not generated yet — that's a separate cooking run, owned by another session). So the switcher MUST gracefully offer only the tiers that exist and light up new ones automatically when they appear.
- `data/tgl/128/*.orm.ktx2` are the **textures**; the static-prop **geometry** is separate (`.ase`/`.tgl` in `tgl.fst`) and out of scope here.
- The viewer already defaults its browse folder to `data/tgl/128` (commit `fae84a73`).

## Decisions locked in design review

| Question | Decision |
|---|---|
| Display sizing | Fit-to-region (aspect-preserving) × zoom. `zoom_` persists across loads so tier switches keep size constant. |
| What "tier" means | Numeric-named sibling folders of the current browse folder (e.g. `…/tgl/{64,128,256,512}`). No `tgl` hardcoding. |
| Missing tiers | Only existing sibling tiers are offered; switching to a tier where the selected file is absent switches the folder without a selection (no crash). |
| Same-asset continuity | On tier switch, if the previously-selected filename exists in the new tier, auto-select+load it (same asset, new res). |
| Scope | Viewer-only. No cooking, no engine changes, no model/geometry preview. |

## Part A — constant display size

### Current behavior (to change)
`TexturePreview2D::draw()` computes `imageSize = { meta_.width * zoom_, meta_.height * zoom_ }` and shows it in a scrollable child. On-screen size is therefore proportional to native resolution.

### New behavior
Extract a pure helper (headless-testable, no GL/ImGui):

```cpp
// Returns a plain POD (NOT ImVec2 — keeps this header ImGui-free + headless-testable).
struct FitSize { float w; float h; };
// Largest aspect-preserving size that fits (texW x texH) into (availW x availH),
// then scaled by zoom. Guards against zero/negative inputs.
FitSize FitTextureDisplaySize(int texW, int texH, float availW, float availH, float zoom);
```

Semantics:
- `zoom == 1.0` → the texture exactly fits the available area (letterboxed by aspect).
- Two textures with the **same aspect ratio** but different native resolutions (128 vs 256 vs 512) produce the **same** display size at the same zoom — satisfying the caveat.
- `zoom > 1` enlarges beyond fit (pan via the existing horizontal/vertical scroll child).
- Degenerate inputs never produce NaN/Inf/divide-by-zero: a zero-area texture (texW/texH ≤ 0) returns `{0,0}` (nothing to show); non-positive avail/zoom are clamped so the result stays finite and positive.

`draw()` uses `FitTextureDisplaySize(meta_.width, meta_.height, avail.x, avail.y - <sliderRowHeight>, zoom_)` instead of the native-pixel formula. `zoom_` is NOT reset in `setSource` (already the case), so flipping tiers preserves the on-screen size.

Note: the zoom slider semantics shift from "× native pixels" to "× fit". Update its label/range accordingly (e.g. `0.25×…8×`, default `1×` = fit).

## Part B — resolution-tier switcher

### Detection (FileBrowser)
Add to `FileBrowser`:

```cpp
// Numeric-named sibling directories of the current folder, ascending (e.g. {"64","128","256"}).
// Empty if the current folder's siblings aren't a numeric tier set.
std::vector<std::string> SiblingTiers() const;

// The current folder's leaf name if it is numeric (the active tier), else "".
std::string CurrentTier() const;

// Repoint to <parent>/<tier>. If a file was selected and a file of the SAME name exists
// in the new tier, select+load it (same asset, new res); otherwise switch folder with no
// selection. No-op if <parent>/<tier> doesn't exist.
void SwitchTier(const std::string& tier);
```

- `SiblingTiers()` scans the parent of `folderPath_` for subdirectories whose names are all digits; returns them sorted numerically. Only directories that exist are returned (so today `tgl` yields `{"128"}`; once 256/512 are cooked they appear automatically).
- `SwitchTier()` preserves the current selection's *filename* across the switch via the existing `selectFile()` path, so `AssetViewerApp`'s existing `if (browser_.hasSelection()) surface_.setSource(...)` reloads the same asset at the new tier with no new wiring.

### UI
In `FileBrowser::draw()`, above the folder input, render a tier row **only when `SiblingTiers().size() > 1`**: one selectable per tier, current tier highlighted, click → `SwitchTier(tier)`. When ≤1 tier exists (today's `tgl` with only `128`), the row is hidden (no clutter, no dead buttons).

### Data flow
```
user clicks tier "256"
  → FileBrowser::SwitchTier("256")
      → folderPath_ = parent(folderPath_)/"256"; refresh()
      → if prev selected filename exists here: selectFile(it)  → hasSelection_=true
  → AssetViewerApp::drawUi: browser_.hasSelection() → surface_.setSource(sameFile@256)
  → TexturePreview2D shows the 256 asset at the SAME on-screen size (Part A)
```

## Architecture (files)

Modified (all under `tools/asset_viewer/`):
- `TexturePreview2D.{h,cpp}` — replace native-pixel sizing with `FitTextureDisplaySize`; adjust zoom slider.
- A small home for `FitTextureDisplaySize` — put it in `TextureMetadata.{h,cpp}` (already a formatting/util TU) or a new `PreviewLayout.{h,cpp}`. (Plan picks one; prefer reusing `TextureMetadata` to avoid a new file.)
- `FileBrowser.{h,cpp}` — `SiblingTiers()`, `CurrentTier()`, `SwitchTier()`, tier row in `draw()`.
- `AssetViewerApp.{h,cpp}` + `main.cpp` — `--smoke-fit` and `--smoke-tiers` entrypoints.
- `tests/fixtures/asset_viewer/` — a tier fixture tree (e.g. `tiers/128/sample.ktx2` + `tiers/256/sample.ktx2`, both copies of `tex_rgba8.ktx2`) so `SiblingTiers`/`SwitchTier` are testable.

## Testing (existing `--smoke*` harness; no new framework)

- **`--smoke-fit`** (no GL): `FitTextureDisplaySize` —
  - 128×128 and 256×256 into the same avail at zoom 1 → **equal** display size (the caveat, asserted directly);
  - aspect preserved for a non-square (e.g. 256×128);
  - `zoom 2` → exactly 2× the zoom-1 size;
  - degenerate inputs (0 dims, 0 avail) → finite, non-zero, no divide-by-zero.
- **`--smoke-tiers <dir>`** (no GL): point at the tier fixture tree —
  - from `tiers/128`, `SiblingTiers()` == `{"128","256"}`, `CurrentTier()` == `"128"`;
  - `SwitchTier("256")` repoints folder to `tiers/256` and, because `sample.ktx2` exists in both, leaves a live selection whose path is under `tiers/256`;
  - `SwitchTier(<missing tier>)` is a no-op (folder unchanged);
  - switching to a tier lacking the selected file switches the folder with `hasSelection()==false`.
- Existing smokes (`--smoke`, `--smoke-decoder`, `--smoke-ktx*`, `--smoke-preview`) must stay green.

GUI-only aspects (the tier row rendering, actual on-screen pixels) are verified by a manual launch, not asserted by goldens.

## Deferred / out of scope

- Generating the 256/512 tiers (cooking pipeline — separate session).
- Mip-level scrubber; cubemap/array KTX2; model/geometry preview.
- Remembering per-asset zoom; thumbnail grid.

## Risks

1. **Sizing regression** for the existing texture view — mitigated by `--smoke-fit` asserting the math and a manual check.
2. **Sibling-tier false positives** (a numeric-named folder that isn't a real tier) — acceptable; the switch just repoints and the browser lists whatever's there. Hidden when ≤1 tier.
3. **Selection-continuity edge cases** (filename case, extension differences across tiers) — match on exact filename; if absent, fall back to no-selection (covered by smoke).
