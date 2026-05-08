# PR2a — Detail M2c emit DELETE slice (design)

**Status:** spec, ready for executor.
**Date:** 2026-05-08
**Predecessor:** PR2 Stage 0 recon, six exploration docs
([`recon-1`](../explorations/2026-05-07-pr2-stage0-recon-1-perf-rebaseline-handoff.md)
through [`recon-6`](../explorations/2026-05-07-pr2-stage0-recon-6-render-order-hooks.md)).
Adversarial-plan-review pass clean on the load-bearing claim.
**Brainstorm:** [`2026-05-01-detail-overlay-consolidation-scope.md`](../brainstorms/2026-05-01-detail-overlay-consolidation-scope.md)
— Q4 detail-recipe-extension answer is moot; recon-6 reframed PR2a
from "indirect detail draw" to "delete the dead M2c emit."

> Discipline: every cited symbol grep-verified at write-time per worktree
> CLAUDE.md "Documentation Discipline." Verification appendix at end.

---

## Goal

Delete the per-frame CPU work in the M2c detail emit at
[`mclib/quad.cpp:1961-2024`](../../../mclib/quad.cpp). The block runs
every frame on water-interest tiles, builds a 4-corner `gos_VERTEX`
array, runs `clampUVs` per emitted tri, and pushes 1-2 tris to
`mcTextureManager` via `addVertices(terrainDetailHandle, ...,
MC2_ISTERRAIN | MC2_DRAWALPHA)`. The pushed vertices are
**unconditionally dropped** at `Render.Overlays`
([`mclib/txmmgr.cpp:1818-1822`](../../../mclib/txmmgr.cpp)) — non-water
alpha terrain nodes are reset-and-`continue`d, suppressing the legacy
flat-plane detail layer that conflicted with displaced tessellated
terrain (commit `521d83a` 2026-04-16).

**Net visual effect: zero pixels produced today.**
**Net CPU cost: per-frame work for nothing.**

PR2a deletes the dead emit. No draw bridge, no recipe extension, no
texture array, no parity test (visual diff is N/A — zero pixels before,
zero pixels after).

---

## Why no env gate

PR1 SOLID and the brainstorm assume each indirect-conversion slice
ships behind `MC2_TERRAIN_INDIRECT_<POP>` and bakes for one soak window
before flipping default-on. That discipline exists so a regression in
the new draw can be quickly rolled back at runtime.

PR2a has nothing to gate. There is no new draw, no new SSBO, no new
shader. The change is "delete code that produces no pixels." A
runtime opt-out would re-enable a path that is, by current
construction, dead. The smoke gate (zero-pixel-delta on tier1 and the
menu canary) is the only test that matters and it's already in
place.

If a regression somehow surfaces post-delete (e.g., a tier1 mission
turns out to depend on a side effect of the queueing — see Risks), a
git revert of one commit is the rollback path.

---

## Scope (in / out)

**In scope:**
- Delete the M2c detail emit block at `quad.cpp:1961-2024` (the
  `if (useWaterInterestTexture && terrainDetailHandle != 0xffffffff)`
  branch).
- Delete the surrounding `corner[]` build, `clampUVs` lambda, and
  per-tri `addVertices` calls.
- Add an env-gated counter `m2c_detail_emit_quads` (default off; on
  with `MC2_TERRAIN_COST_SPLIT=1`) that **asserts zero** post-delete.
  Confidence guard, not gate.

**Out of scope:**
- Shape-C fallback detail emits at [`quad.cpp:485-487, :501-503`](../../../mclib/quad.cpp)
  (legacy `addTriangleBulk` clusters). These fire ~0 times in tier1
  steady-state per `m2_thin_record_cpu_reduction_results.md`. Keeping
  them keeps the legacy path correctness for any non-fast-path
  regression. A future delete slice can retire them.
- Re-introducing detail rendering. Tessellation-aware detail layer is
  an independent feature, brainstormed separately.
- M2d overlay emit (PR2b).
- Mine emit (PR2c, blocked on visual audit).

---

## Stages

### Stage 0a — confidence counter (1 commit)

**Goal:** before deletion, ship a counter that confirms the M2c block
is the only `addVertices(terrainDetailHandle, ...)` site firing in
the fast path. Establishes the pre-delete baseline.

**Code change:**

- Add `Counters_AddM2cDetailEmitQuad()` to
  [`gos_terrain_indirect.h:106`](../../../GameOS/gameos/gos_terrain_indirect.h)
  / definition in [`gos_terrain_indirect.cpp`](../../../GameOS/gameos/gos_terrain_indirect.cpp)
  next to the existing `Counters_AddLegacyDetailOverlayQuad()`. Same
  N1-counter idiom.
- Call `Counters_AddM2cDetailEmitQuad()` once per quad that enters
  the M2c branch (gate on the same `if` predicate at line 1961). Place
  the call inside the `if`, after the body — once-per-quad, not
  once-per-tri. Env-gated identical to existing cost-split timers.
- 600-frame summary line in
  [`gameosmain.cpp`](../../../GameOS/gameos/gameosmain.cpp) extends to
  print `m2c_detail_emit_quads` alongside the existing
  `legacy_detail_overlay_quads`.

**Tier1 verification before Stage 1a:** run smoke gate; counter
output should show non-zero `m2c_detail_emit_quads` proportional to
water-interest quads in each mission, confirming the M2c branch is
firing as expected (i.e., we're about to delete real work, not
phantom work).

**Commit message:** `feat(pr2a): instrument m2c_detail_emit_quads
counter — pre-delete baseline`.

### Stage 1a — delete the M2c block (1 commit, ships gate-clean)

**Goal:** delete `quad.cpp:1961-2024`. Verify smoke gate clean
(zero pixel delta), counter goes to zero, perf delta is the
delete savings.

**Code change:**

- Delete `quad.cpp:1961-2024` inclusive (the entire `if
  (useWaterInterestTexture && terrainDetailHandle != 0xffffffff)`
  block, including the body's `corner[]` build, `clampUVs` lambda,
  and the 4 `addVertices` call sites).
- Delete the `Counters_AddM2cDetailEmitQuad()` call (added Stage 0a).
- **KEEP** the counter declaration in `gos_terrain_indirect.h/.cpp`
  with body short-circuited to no-op. Three reasons: keeps the
  600-frame summary line stable for tooling that scrapes it; provides
  a regression-proof "is the count truly zero" assertion if anyone
  reintroduces an emit through a different path; and matches the
  "demote, don't delete" debug-instrumentation rule per worktree
  CLAUDE.md.
- Delete any newly-orphaned local variables (`corner`, `clampUVs`,
  `oneOverTf`, `tilingFactor`, `whitenArgb` in this scope only).
  Verify no use outside the deleted block — see verification
  appendix.

**Tier1 verification:** smoke gate clean (5/5 PASS), menu canary
clean, counter reads zero, perf delta is whatever the parallel Tracy
session measures (target = positive; size TBD per
[recon-1](../explorations/2026-05-07-pr2-stage0-recon-1-perf-rebaseline-handoff.md)).

**Commit message:** `feat(pr2a): delete dead M2c detail emit
(quad.cpp:1961-2024) — zero-pixel since 521d83a`.

---

## Parity / verification gates

### Gate A (visual canary)

Tier1 + menu canary must be byte-equivalent pixels before vs after
Stage 1a. Per recon-6, the M2c emit produces no pixels today, so this
gate is automatically satisfied — but it's the load-bearing
correctness check.

### Gate B (counter sanity)

After Stage 0a, `m2c_detail_emit_quads` per mission must be non-zero
(otherwise we'd be deleting phantom work). After Stage 1a, the
counter must be zero (otherwise some path other than the M2c block is
emitting through it; investigate before merge).

### Gate C (perf)

Tracy `quadSetupTextures` zone delta on the smoke camera, post-delete
vs pre-delete. **Target value TBD** — sizing depends on the Tracy
sub-zone re-baseline session (parallel work). Spec executor: do NOT
hold up the delete on perf-gate sizing; the visual + counter gates
are the load-bearing ones. Perf is a measured outcome, not a
ship-blocker.

Per [recon-1's multi-zoom note](../explorations/2026-05-07-pr2-stage0-recon-1-perf-rebaseline-handoff.md):
`legacy_detail_overlay_quads` scales ~5× from normal zoom (~1,521/
frame on mc2_01) to full zoom out (~8,000/frame). PR2a's CPU
recovery is therefore zoom-sensitive — capture both zooms in the
post-delete baseline.

### No parity SSBO byte-compare

Unlike PR1 SOLID's `MC2_TERRAIN_INDIRECT_PARITY_CHECK`, PR2a has no
new data path to compare against legacy. Skip.

---

## Risks / hazards

### R1. Side-effect dependency

`addVertices(terrainDetailHandle, ...)` queues vertices into
`masterTextureNodes[].vertexData`. Deleting the call removes those
queued vertices. **Question:** does any code path read those queued
vertices for purposes OTHER than `Render.Overlays` drawing? E.g., a
counter, an asset-tracker, a test hook, an unrelated subsystem
walking masterVertexNodes for diagnostics?

**Mitigation:** grep verification appendix entry #6 confirms the
only consumer of the produced node is the legacy non-water alpha
suppress. Adversarial review on recon-6 (2026-05-08) re-grep'd
`addVertices` implementation in `txmmgr.h:915-960` and confirmed it
writes only to `masterTextureNodes[nodeId].vertexData` with no side
effects beyond the queue. Risk: **low**.

### R2. Performance counter visibility regression

If observability tools elsewhere read `legacy_detail_overlay_quads`
expecting M2c contributions, deleting the M2c emit drops the counter
contribution from that source. Not a correctness bug; could surprise
operators tracking that number.

**Mitigation:** the counter scope at the existing
`CostSplitDetailOverlayScope` brackets only the legacy Shape-C
fallback at `quad.cpp:485-487, :501-503` per
[recon-1](../explorations/2026-05-07-pr2-stage0-recon-1-perf-rebaseline-handoff.md);
M2c emits never contributed there to begin with. The new
`m2c_detail_emit_quads` counter (Stage 0a) shows the pre/post-delete
delta cleanly.

### R3. Tessellated detail re-introduction collision

If a parallel session re-introduces detail rendering (the
"tessellation-aware detail layer" future feature) by re-using the
M2c block as a starting point, deleting it forces them to start
fresh.

**Mitigation:** re-introduction is a future feature that should be
brainstormed separately per recon-6's "Open follow-ups" #4. Starting
fresh from a clean slate is preferable to inheriting a 3-week-dead
code path. No real conflict.

---

## Rollback

`git revert <stage-1a-commit>` is the entire rollback. Stage 0a's
counter is harmless to leave in place if rolled back to it.

---

## Code-grounding verification appendix

| # | Symbol / claim | Citation | Status |
|---|---|---|---|
| 1 | M2c detail emit block to delete | [`mclib/quad.cpp:1961-2024`](../../../mclib/quad.cpp) — `if (useWaterInterestTexture && terrainDetailHandle != 0xffffffff) { ... }` | M |
| 2 | `addVertices` flags emitted | [`mclib/quad.cpp:2001, :2007, :2015, :2021`](../../../mclib/quad.cpp) — all `MC2_ISTERRAIN \| MC2_DRAWALPHA` (no water flag) | M |
| 3 | Detail-suppress predicate at Render.Overlays | [`mclib/txmmgr.cpp:1818-1822`](../../../mclib/txmmgr.cpp) — non-water alpha+terrain nodes reset-and-continue | M |
| 4 | Suppress block introduced 2026-04-16 | `git log -S "dark striped under-pattern" -- mclib/txmmgr.cpp` returns single match `521d83a` | M |
| 5 | No path ORs `MC2_ISWATER` into a detail-textured node | grep `flags\|=.*MC2_ISWATER`/`MC2_ISWATER\\b` across worktree returns producers only on water path; verified by adversarial review subagent 2026-05-08 | M (negative claim defended per `feedback_data_flow_audit_asymmetry.md`) |
| 6 | `addVertices` writes only to `masterTextureNodes[nodeId].vertexData` | [`GameOS/gameos/txmmgr.h:915-960`](../../../GameOS/gameos/txmmgr.h) — implementation read by adversarial review 2026-05-08; no side effects beyond the queue | M |
| 7 | `Counters_AddLegacyDetailOverlayQuad()` declaration (idiom to mirror) | [`gos_terrain_indirect.h:106`](../../../GameOS/gameos/gos_terrain_indirect.h) | M |
| 8 | `MC2_TERRAIN_COST_SPLIT=1` env gate idiom | shipped in `bdb1628` per brainstorm appendix #25 | M (brainstorm-trusted; commit hash valid) |
| 9 | Shape-C fallback detail sites (NOT in PR2a scope) | [`mclib/quad.cpp:485-487, :501-503`](../../../mclib/quad.cpp) — `addTriangleBulk(r.terrainDetailHandle, MC2_ISTERRAIN \| MC2_DRAWALPHA, 2)` | M |
| 10 | Local-var scope `corner`, `clampUVs`, `oneOverTf`, `tilingFactor`, `whitenArgb` are M2c-block local | [`mclib/quad.cpp:1963-1980`](../../../mclib/quad.cpp) — all declared at function scope inside the deleted `if` block | M (no use outside; verify at delete-time by compile after delete) |

**Status summary:** 10 entries; 10 M, 0 D, 0 NF.

---

## Architectural decisions that need user/advisor sign-off before executor session

None — all major decisions resolved by recon. Spec is mechanically
executable.
