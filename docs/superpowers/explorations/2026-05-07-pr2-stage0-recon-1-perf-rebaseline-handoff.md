# PR2 Stage 0 recon — item 1: perf re-baseline handoff

**Date:** 2026-05-07
**Status:** Recon — **handoff doc only.** Tracy sub-zone instrumentation
lands in a different session per user direction (2026-05-07). This doc
specifies what that session must add, what data to capture, and why the
existing `MC2_TERRAIN_COST_SPLIT=1` numbers cannot size PR2's perf
gates by themselves.
**Closes:** brainstorm open recon item #1
([`brainstorms/2026-05-01-detail-overlay-consolidation-scope.md:813-817`](../brainstorms/2026-05-01-detail-overlay-consolidation-scope.md))
— specification only; numbers TBD.

---

## TL;DR

- The existing `CostSplitDetailOverlayScope` RAII timer brackets ONLY
  the legacy Shape-C fallback at
  [`mclib/quad.cpp:485-487, :501-503`](../../../mclib/quad.cpp). It does
  NOT bracket the M2c detail emit ([`:1961-2024`](../../../mclib/quad.cpp))
  or the M2d overlay emit ([`:2035-2083`](../../../mclib/quad.cpp)) that
  fire on the fast path.
- Per `memory/m2_thin_record_cpu_reduction_results.md` line 26 ("Legacy
  quads/frame: 0"), the M2 fast path covers ~100% of frames in tier1
  steady-state. **The cost-split timer is therefore reading roughly
  zero µs while ~917 µs of CPU spend lives in the unbracketed fast-path
  branches.**
- A perf-gate target for PR2 sized off the existing cost-split number
  would target ~30 µs and miss the actual ~917 µs — meaning PR2 could
  ship "passing the gate" while saving negligible CPU.
- This session does NOT add the missing Tracy zones (per user). The
  follow-up session adds them and runs the re-baseline.

---

## What's measured today (post-PR1 SOLID, default-on)

`CostSplitSolidScope` and `CostSplitDetailOverlayScope` shipped in
Stage 1 commit `bdb1628`. Both are RAII scopes with explicit
constructor/destructor brackets emitting micro-second deltas to the
indirect-terrain TU's Tracy counter.

### Locations

| Scope | Site | Population covered |
|---|---|---|
| `CostSplitSolidScope` | [`mclib/quad.cpp:480-482`](../../../mclib/quad.cpp), `:495-497`, `:510-512` (within `addTerrainTriangles`) | Legacy SOLID `addTriangleBulk` clusters in Shape-C fallback only — but PR1 SOLID gate-off at `:479,494,509` (`BeginLegacySolidCluster()`) bypasses these when armed, so this scope reads ~0 µs in PR1-armed frames |
| `CostSplitDetailOverlayScope` | [`mclib/quad.cpp:485-487, :501-503`](../../../mclib/quad.cpp) | Legacy detail `addTriangleBulk(MC2_ISTERRAIN \| MC2_DRAWALPHA, 2)` Shape-C fallback only |

### What the existing scopes DO NOT cover

`addTerrainTriangles` is the **Shape-C fallback** — it only fires when
`fastPathEligible` is false. Per `m2_thin_record_cpu_reduction_results.md`
line 26, in tier1 steady-state `Legacy quads/frame: 0`. The actual CPU
spend lives in the M2c (detail) and M2d (overlay) inline emits inside
the `vertexProjectLoop` fast path:

| Branch | Site | Why it's ungated |
|---|---|---|
| **M2c detail emit** | [`mclib/quad.cpp:1961-2024`](../../../mclib/quad.cpp) — gated by `useWaterInterestTexture && terrainDetailHandle != 0xffffffff` | Inside the M2 fast-path `if (M2 eligible)` block, AFTER the SOLID thin-record build. No `CostSplitScope` here. |
| **M2d overlay emit** | [`mclib/quad.cpp:2035-2083`](../../../mclib/quad.cpp) — gated by `useOverlayTexture && overlayHandle != 0xffffffff` | Same. Calls `gos_PushTerrainOverlay` per emitted tri. |

These two branches collectively contain:

- 4 `addVertices(terrainDetailHandle, ...)` sites (M2c, BOTTOMLEFT/RIGHT × tri1/tri2)
- 4 `gos_PushTerrainOverlay(w, overlayTexId)` sites (M2d, BOTTOMLEFT/RIGHT × tri1/tri2)
- Per-corner `gos_VERTEX corner[4]` build (4 multiply-adds, 4 ARGB
  fetches, 4 fogRGB fetches per quad)
- Per-corner `WorldOverlayVert wov_corner[4]` build (similar cost)
- `clampUVs` lambda overhead per emitted detail tri
- `tex_resolve(overlayHandle)` at line 2037 — per-quad-with-overlay

This is the work PR2a (detail) and PR2b (overlay) replace with
indirect draws. **It is not in any Tracy sub-zone today**, so frame-time
shows up at the `quadSetupTextures` outer zone level only — the
~947 µs the user cited.

---

## What the follow-up Tracy session must add

> User directive 2026-05-07: do not add Tracy zones in this session;
> they're being added in a parallel session.

For when that session runs, the recipe is:

### Sub-zones to add inside `vertexProjectLoop` / setupTextures

```
Stage 0 — within mclib/quad.cpp setupTextures() M2 fast-path block:

ZoneScopedN("CostSplit.M2c.Detail")   -- bracket :1961 to :2024 inclusive
ZoneScopedN("CostSplit.M2d.Overlay")  -- bracket :2035 to :2083 inclusive
ZoneScopedN("CostSplit.M2.PerQuad")   -- bracket the entire M2 fast-path
                                          early-return block (the "if
                                          (M2 eligible)" branch, including
                                          the corner[] / wov_corner[] builds
                                          before the per-tri emits)
```

The `CostSplit.M2.PerQuad` zone catches per-quad scaffolding that's not
in either inline emit (e.g., uvMode, pzTri1/pzTri2 derivation, the
common-case fast-path setup). Subtraction yields fast-path scaffolding
cost: `M2.PerQuad - (M2c.Detail + M2d.Overlay)`.

### Counter additions (mirror existing N1)

- `m2c_detail_emit_quads` — count quads that fired M2c emit
- `m2d_overlay_emit_quads` — count quads that fired M2d emit
- (Optional) per-tri counts if PR2's parity gates need finer
  resolution

These extend the existing `Counters_Add*` pattern at
[`gos_terrain_indirect.cpp:86`](../../../GameOS/gameos/gos_terrain_indirect.cpp).

### Run shape

```
py -3 .claude/worktrees/nifty-mendeleev/scripts/run_smoke.py \
    --tier tier1 --kill-existing --duration 30
```

with env: `MC2_TERRAIN_INDIRECT=1 MC2_TERRAIN_COST_SPLIT=1`
(both already on by default; explicit for clarity).

Capture per mission (mc2_01, mc2_03, mc2_10, mc2_17, mc2_24):

- `Terrain::geometry quadSetupTextures` outer zone — current ms
- `CostSplit.M2c.Detail` — new sub-zone µs (per-frame mean, p50, p95)
- `CostSplit.M2d.Overlay` — new sub-zone µs
- `CostSplit.M2.PerQuad` — new sub-zone µs
- `CostSplitDetailOverlayScope` — existing legacy scope (expected ≈ 0)
- `legacy_detail_overlay_quads` total
- `m2c_detail_emit_quads` total
- `m2d_overlay_emit_quads` total

mc2_24 is the mine canary. Hand-off note: mine cost is currently NOT
inside any cost-split scope at all. Per recon item 5,
`enqueueTerrainMineState` runs in `addTriangleBulk` clusters at
[`mclib/quad.cpp:282,290`](../../../mclib/quad.cpp), and `drawMine()` at
[`:4246+`](../../../mclib/quad.cpp). PR2c's perf gate baseline needs a
similar sub-zone bracketing. Adding `CostSplit.MineEnqueue` and
`CostSplit.MineDraw` zones is part of the same instrumentation pass.

---

## What the perf-gate target should look like

Once the re-baseline completes, the spec session can size:

| Slice | Target |
|---|---|
| PR2a (detail) | If retirement-only per recon item 6: target `m2c_detail_emit_quads`/frame ≈ 0 with `MC2_TERRAIN_INDIRECT_DETAIL=1`; perf delta = full reduction in `CostSplit.M2c.Detail` (TBD µs). |
| PR2b (overlay) | Target `m2d_overlay_emit_quads`/frame ≈ 0 with `MC2_TERRAIN_INDIRECT_OVERLAY=1`; perf delta = full reduction in `CostSplit.M2d.Overlay`, **PLUS** any overhead transferred to the new `Render.TerrainOverlays` indirect-draw path (target net negative). |
| PR2c (mine) | Target `enqueue_mine_quads`/frame ≈ 0 on mc2_24 with `MC2_TERRAIN_INDIRECT_MINE=1`; perf delta vs `CostSplit.MineEnqueue + CostSplit.MineDraw` (TBD µs, mc2_24 only). |

Brainstorm-cited overall PR2 target (`quadSetupTextures` ≤ 200 µs at
mc2_01 normal zoom) is reasonable IF the M2c+M2d aggregate is the
~917 µs unbracketed delta — but **must be confirmed against the
re-baseline numbers before locking in.** Don't take the 947 / 200
arithmetic at face value; the floor depends on what's left after
M2c+M2d retirement (e.g., per-quad scaffolding is independent of which
emits fire).

---

## What this session can ground without running the harness

Without Tracy zones (deferred), this session can confirm the structural
claim from grep:

1. **`CostSplitDetailOverlayScope` is in `addTerrainTriangles` only.**
   Verified at [`mclib/quad.cpp:485-487, :501-503`](../../../mclib/quad.cpp);
   no other instances of the constructor in the worktree (grep on
   `CostSplitDetailOverlayScope` returns 2 matches in `quad.cpp` plus
   the type definition in `gos_terrain_indirect.cpp`).
2. **`addTerrainTriangles` only fires on the Shape-C fallback path.**
   Per Stage 1/M2 history docs and the surrounding code comments,
   the M2 fast-path bypasses this function entirely when eligible.
3. **M2c/M2d emits are inside `vertexProjectLoop` / `setupTextures`'s
   M2 fast-path branch.** Verified at quad.cpp:1961-2024 (M2c) and
   :2035-2083 (M2d). No `ZoneScopedN` markers inside these blocks.
4. **Per `m2_thin_record_cpu_reduction_results.md`,** legacy
   addTriangleBulk count is ~0 in tier1 steady-state, confirming the
   cost-split coverage gap is real.

---

## Code-grounding verification appendix

| # | Symbol / claim | Citation | Status |
|---|---|---|---|
| 1 | `CostSplitDetailOverlayScope` brackets only Shape-C fallback | [`mclib/quad.cpp:485-487`](../../../mclib/quad.cpp), `:501-503` (only 2 instantiations in quad.cpp) | M |
| 2 | `CostSplitSolidScope` brackets only Shape-C fallback | [`mclib/quad.cpp:480-482, :495-497, :510-512`](../../../mclib/quad.cpp) (3 instantiations) | M |
| 3 | M2c detail emit unbracketed | [`mclib/quad.cpp:1961-2024`](../../../mclib/quad.cpp) — no `ZoneScopedN` or `CostSplit*Scope` declarations within the block | M |
| 4 | M2d overlay emit unbracketed | [`mclib/quad.cpp:2035-2083`](../../../mclib/quad.cpp) — same; verified by grep `ZoneScopedN\|CostSplit` against the line range | M |
| 5 | `Counters_Add*` API at indirect TU | [`GameOS/gameos/gos_terrain_indirect.cpp:86-90`](../../../GameOS/gameos/gos_terrain_indirect.cpp) | M |
| 6 | `legacy_detail_overlay_quads` counter exists | brainstorm appendix #26 + [`gos_terrain_indirect.cpp`](../../../GameOS/gameos/gos_terrain_indirect.cpp) (cite per brainstorm; not re-grep'd here) | M (brainstorm-trusted) |
| 7 | M2 fast path covers ~100% in tier1 steady state | [`memory/m2_thin_record_cpu_reduction_results.md`](C:/Users/Joe/.claude/projects/A--Games-mc2-opengl-src/memory/m2_thin_record_cpu_reduction_results.md) line 26: "Legacy quads/frame: 0" | M |
| 8 | Mine setup/draw emit unbracketed | recon item 5: `enqueueTerrainMineState` ([`mclib/quad.cpp:282,290`](../../../mclib/quad.cpp)), `drawMine()` ([`:4246+`](../../../mclib/quad.cpp)) — neither has `CostSplit*Scope` | M |

---

## Decision input for spec session

- **Block PR2a/b/c perf-gate sizing on the follow-up Tracy run.** Spec
  session should write Stage 4 perf gates as "target ≈ TBD until
  follow-up Tracy session lands."
- The follow-up session must add the listed sub-zones BEFORE running
  the re-baseline; otherwise the recipe stays incomplete.
- **The brainstorm's quoted ~3.01 ms `quadSetupTextures` and ~947 µs
  numbers predate PR1 SOLID's default-on flip.** Today's number will
  differ; treat all old µs/ms figures in the brainstorm as
  unverified until re-measured.
