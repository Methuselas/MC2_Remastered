# Next-session prompt: paint-scheme color picker swapped

Paste everything below this header into a fresh session.

---

## Task

A v0.1.x playtester reports: in Options → Gameplay, the **player paint
scheme** colors don't match what's selected in the picker. Their guess is
that "the actual colors might be flipped left-to-right in the picker."
Reporter is on a 4K display; the same session also reported AAR text
overflow at high resolution, so resolution-dependent layout is on the
suspect list. Fix it.

## What I already know (do not re-derive)

- File: `code/optionsarea.cpp`, class `OptionsGamePlay`.
- Geometry:
  - `rects[4..35]` — 32 color swatches the user clicks.
  - `rects[36]` — base-color preview box.
  - `rects[37]` — highlight-color preview box.
  - `rects[i].getColor()` returns the swatch's packed uint32 ARGB.
- Click handler: `update()` lines ~906-932 — picks `pRect` (= rects[36]
  or rects[37] depending on which radio button is pressed), iterates
  swatches 4..35, and on hit: `pRect->setColor(rects[i].getColor())`,
  then calls `camera.setMech("Bushwacker", rects[36].getColor(),
  rects[37].getColor(), rects[37].getColor())` to update the 3D mech
  preview. The same uint32 flows to picker preview AND to the mech
  shader path.
- Init: `rects[]` are loaded by `LogisticsScreen` base class from a
  `.fit` layout file (likely `mcl_optionsgameplay.fit` under
  `data/...`). Swatch colors are baked into that file, not assigned
  in code.
- Persisted in `prefs.baseColor` / `prefs.highlightColor`
  (`code/prefs.cpp:72-73, 230-235, 364-365`).
- Mech-paint pipeline: dominant-channel paint classifier was ported to
  `GVAppearance` in commit `e0670e6` (memory:
  `mech_paint_and_mipmap_system.md`). MC2 packs ARGB as BGRA-in-memory
  in some paths (memory: `mc2_argb_packing.md`).

## Hypotheses, ranked

1. **Layout-file mirroring at high resolution.** The 32-swatch grid is
   defined in the `.fit` layout file by absolute pixel rects. If the
   layout file or its scaling logic is mirroring x-coordinates at 4K
   (or any non-stock resolution), the user clicks the swatch they SEE
   but `rects[i]` is the swatch at the mirrored index, so the assigned
   color is "right" per the data array but "wrong" per the user's
   visual. **Test**: drop to 1024×768 (stock) and see if the bug
   disappears.
2. **ARGB ↔ BGRA byte-order mismatch between picker swatch render and
   mech shader.** The picker draws via the 2D `aRect`/HUD path; the
   mech preview uses the 3D mech shader path which reads the same uint32
   but might decode it `.bgra` instead of `.argb` (or vice versa).
   **Test**: read `prefs.baseColor` after a click, compare to the bytes
   the mech shader actually samples (capture via RenderDoc / RGP, or
   add a printf in the mech shader's color uniform binding site).
3. **Dominant-channel paint classifier (`e0670e6`) misclassifying which
   atlas region is "base" vs "highlight"**, causing the two colors to
   swap on the rendered mech. **Test**: pick base = bright red,
   highlight = bright blue (or vice versa) and inspect which mech parts
   end up red vs blue. If the *parts* swap (legs vs torso), this is the
   classifier; if the *colors themselves* swap (you set red, mech is
   cyan), this is hypothesis 2.

## What to do

1. **Get a screenshot from the user.** Specifically: the picker open at
   4K, with one swatch clicked, the preview box showing its color, and
   the 3D mech rendered alongside. Without this, you're guessing.
2. **Repro at stock resolution first.** Build + deploy
   (`/mc2-build-deploy`), launch with the lowest available resolution,
   reproduce the user's exact click sequence, see if the bug is still
   present. This kills hypothesis 1 instantly if so.
3. **Instrument**: add an env-gated trace in `OptionsGamePlay::update()`
   that logs, on each swatch click, `(mouseX, mouseY, hit_index_i,
   rects[i].globalX, rects[i].getColor)`. Compare to what the user
   thinks they clicked.
4. **Bisect** the three hypotheses by the tests above. Fix the actual
   cause, not whichever one feels narrative.
5. **Verify**: pick a distinctive color (saturated red), confirm picker
   preview matches, confirm mech preview matches, exit Options, re-enter,
   confirm persisted. Repeat at 1024×768 and 4K.

## Constraints

- **Do not** edit `.fit` layout files unless hypothesis 1 is confirmed
  and you understand the layout-file scaling system end-to-end.
- **Do not** change the dominant-channel paint classifier without
  reading `memory/mech_paint_and_mipmap_system.md` and
  `memory/mc2_argb_packing.md` first.
- Per worktree CLAUDE.md: build `--config RelWithDebInfo`; deploy
  via `cp -f` + `diff -q` (use `/mc2-deploy`).

## Out of scope

- Other v0.1.x bug reports (rebind crash, cursor jumps, HUD edge line,
  AAR text overflow, encyclopedia preview).
- AAR text overflow — same root class as hypothesis 1 (resolution-
  dependent layout) but different widget; file separately.

## Done = ready to roll into v0.1.3

User-reported swap is gone at 4K AND stock; instrumentation left
gated; commit on `claude/nifty-mendeleev`.
