# PR2c mine zone visual-order audit — scope doc for a future session

**Date:** 2026-05-08
**Status:** Scope only. Self-contained brief for whoever picks up the
audit. **Blocks PR2c spec session** until the audit lands a finding.
**Prereq context:** Read first —
[`recon-6-render-order-hooks.md`](2026-05-07-pr2-stage0-recon-6-render-order-hooks.md)
"PR2c — mine" section. This doc is the audit recipe; recon-6 is the
"why."

---

## The question this audit answers

**Where in the render order should `Render.TerrainMines` (the indirect
mine draw shipped by PR2c) hook?**

Two candidates:

- **(A) Stay where mines are today** — non-terrain alpha drain at
  `mclib/txmmgr.cpp:2017-2023` (the legacy `!MC2_ISTERRAIN &
  !MC2_ISSHADOWS & !MC2_ISCOMPASS & !MC2_ISCRATERS & MC2_DRAWALPHA`
  loop). Mines compose against post-decal pixels, alongside other
  non-terrain alpha objects.

- **(B) Promote to a new `Render.TerrainMines` zone** between
  `Render.TerrainOverlays` (txmmgr.cpp:1777) and `Render.Decals`
  (:1782). Mines paint over cement transitions but craters paint
  over mines.

Cannot be answered by reading code. Mine emit's flag set
(`MC2_DRAWALPHA` only, no `MC2_ISTERRAIN`) is what routes them to
zone (A) today, but that routing predates `gos_DrawTerrainOverlays`
and `gos_DrawDecals` (the new zones #7 and #8). Compositing was never
re-evaluated under the new render order. The audit closes that.

---

## Why this blocks PR2c

PR2c spec needs to lock the indirect-draw hook location. If (A) wins,
PR2c's bridge function hooks the non-terrain alpha drain — and the
indirect draw must respect whatever blend/depth state the surrounding
non-terrain alpha loop sets. If (B) wins, PR2c gets a clean named
zone with terrain depth context, similar to PR1 SOLID's
`Render.TerrainSolid` hook.

Bridge function shape, depth state setup, and parity-test
expectations all differ between (A) and (B). Locking spec without
the audit means either:

- Spec picks (B) by default (cleaner, named zone) → if visual diff
  shows mines blend wrong, executor session debugs a bug that recon
  could have caught.
- Spec picks (A) by default → indirect path inherits whatever blend
  state leaks from prior non-terrain-alpha draws (mc2_argb_packing,
  blend state inheritance traps). Latent regression risk.

---

## Audit recipe (one operator, ~2 hours, no spec/code commit needed)

### Setup

1. **Mission:** mc2_24 (only mine-bearing tier1 mission per recon
   item 5 / brainstorm Q3).
2. **Camera angles:**
   - Active mines, normal zoom, mid-mission with mech adjacent to mines.
   - Active mines, full zoom out (per parallel-session telemetry
     2026-05-08, mine-cell counts scale with zoom; the answer might
     diverge by zoom).
   - Blown mines (set to value 2, post-explosion sprite). Same two
     zooms.
3. **Build:** `RelWithDebInfo` per worktree CLAUDE.md "Critical Rules."

### Two passes

**Pass 1 — baseline at current zone (A).** No code changes. Build,
run mc2_24, capture screenshots at the four camera setups.

**Pass 2 — prototype at zone (B).** Patch `mclib/txmmgr.cpp`'s
non-terrain alpha drain at `:2017-2023` to skip mine-textured nodes
(`mineTextureHandle` / `blownTextureHandle`), and add a temporary
draw block between `Render.TerrainOverlays` and `Render.Decals`
(after txmmgr.cpp:1780, before :1782) that walks `masterVertexNodes`
for `mineTextureHandle`/`blownTextureHandle` nodes and emits them
there. **Prototype only — DO NOT commit; discard after audit.** This
is purely to capture pixels at the candidate (B) location.

### Diff

Compare Pass 1 vs Pass 2 screenshots pixel-for-pixel at each of the
4 camera setups.

- **Match (or near-match, <0.5% pixel delta):** PR2c spec locks (B).
  Hook `Render.TerrainMines` between TerrainOverlays and Decals.
- **Diverge:** identify what's different. Likely candidates:
  - Decal craters overpaint mines correctly in (A) but mines overpaint
    decals in (B) → blending order requires (A).
  - Mines blend against terrain alpha differently → check if mine
    alpha multiplier reads from the correct backdrop pixel under
    each placement.
  - Cement-overlay-vs-mine compositing — mine on cement should still
    render as a visible mine, not be hidden by cement.

If diverge: PR2c spec keeps (A). The bridge hooks the non-terrain
alpha drain.

---

## Deliverable

Post-audit, the next session writes a small follow-up exploration
doc:

`docs/superpowers/explorations/YYYY-MM-DD-pr2c-mine-zone-audit-finding.md`

Contents:

- Decision: (A) or (B).
- Pixel-diff numbers per camera setup.
- Screenshots embedded as `.png` (commit alongside the doc — small,
  audit evidence is reusable).
- Spec-session input: bridge function shape per the chosen hook.

After that doc lands, PR2c spec session unblocks. Brainstorm Q4-mine
+ Q6-mine answers stand otherwise (parallel `MineCellRecipeSSBO`
keyed by `vertexNum × 16 + cellIdx`).

---

## Out of scope for this audit

- PR2a, PR2b. Both are independent slices.
- Tracy perf measurement of the prototype's (B) draw — perf gate
  sizing waits on the parallel Tracy-instrumentation session per
  [recon-1](2026-05-07-pr2-stage0-recon-1-perf-rebaseline-handoff.md).
- Texture-array build for mines. Audit uses legacy texture-bind path;
  texture-array shape is locked by recon-2.
- Non-mc2_24 missions. Mine-bearing missions outside tier1 may exist
  in stock content but tier1 is the validation surface per
  `feedback_offload_scope_stock_only.md`.

---

## Why audit-before-spec, not spec-then-prototype

The brainstorm + spec-session pattern in this project commits to a
plan structure before executor work begins. If spec locks (B) on
intuition and the executor finds the visual diff, the cost is a
mid-arc redesign (rare but expensive). The audit is cheap (~2h) and
the answer is binary, so audit-first is strictly safer. This mirrors
how PR1's adversarial-review-before-implementation discipline caught
fictional struct fields before executor session ran.

---

## Code-grounding verification appendix

| # | Symbol / claim | Citation | Status |
|---|---|---|---|
| 1 | Mine emit flags = `MC2_DRAWALPHA` only | [`mclib/quad.cpp:282`](../../../mclib/quad.cpp), `:290` (per recon-6 #13) | M |
| 2 | Mine emit drains via non-terrain alpha loop | [`mclib/txmmgr.cpp:2017-2023`](../../../mclib/txmmgr.cpp) (per recon-6 #19) | M |
| 3 | `Render.TerrainOverlays` zone first line | [`mclib/txmmgr.cpp:1777`](../../../mclib/txmmgr.cpp) (per recon-6 #4 verified 2026-05-07) | M |
| 4 | `Render.Decals` zone first line | [`mclib/txmmgr.cpp:1782`](../../../mclib/txmmgr.cpp) (per recon-6 #5) | M |
| 5 | `drawMine()` emit sites | [`mclib/quad.cpp:4240`](../../../mclib/quad.cpp) function def; emits at `:4373, :4374, :4378, :4379` (per recon-6 #14, re-grep'd 2026-05-08) | M |
| 6 | mc2_24 is the only mine-bearing tier1 mission | recon-5 finding + `m2_thin_record_cpu_reduction_results.md` campaign-wide ~97% mine-free claim | M (memory-trusted) |

**Status summary:** 6 entries, all M.
