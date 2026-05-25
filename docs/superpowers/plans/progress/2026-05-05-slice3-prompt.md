# Prompt — Slice 3: Static-object update bypass

**Date:** 2026-05-05
**Worktree:** `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`
**Authoritative design docs (read first):**
- Recon: [docs/superpowers/explorations/2026-05-04-slice3-static-update-bypass-recon.md](../../explorations/2026-05-04-slice3-static-update-bypass-recon.md) — ground truth on which populations to skip, disqualifier list, citations.
- Brainstorm: [docs/superpowers/brainstorms/2026-05-04-slice3-static-update-bypass-brainstorm.md](../../brainstorms/2026-05-04-slice3-static-update-bypass-brainstorm.md) — 9-question Q&A locking predicate placement (`IsStaticNow()` virtual on appearance), allowlist expansion order, counter design, 5-stage gate ladder.

**Both predate slice 2 default-on flip working correctly. Read the "What changed since recon" section below before acting on the design.**

---

## Current state (2026-05-05)

### Already landed
- **Stage 3.A counter scaffolding** (commits `7ec9f6d`, `fc53cf9`):
  - Counter struct + env flags `MC2_STATIC_UPDATE_TRACE`, `MC2_STATIC_UPDATE_SKIP` at [code/terrobj.cpp:87-101](../../../../code/terrobj.cpp).
  - External accessor functions at [code/terrobj.cpp:105-110](../../../../code/terrobj.cpp).
  - Summary emitter `g_staticUpdateEmitSummary()` at [code/terrobj.cpp:112-130](../../../../code/terrobj.cpp).
  - Header [code/static_update_counters.h](../../../../code/static_update_counters.h) declaring the contract.
  - Counter increments wired in `TerrainObject::update` (fc53cf9 commit).

### Not yet done in Stage 3.A
- ❌ **Per-frame summary hook in `objmgr.cpp`.** The brainstorm called for emitting `[STATIC_UPDATE v1]` every 600 frames at end of the TerrainObjects sweep. `objmgr.cpp` does not currently include `static_update_counters.h` (verify with `grep -n static_update code/objmgr.cpp` — should return nothing).
- ❌ **TracyPlot wiring.** Brainstorm Q6 specified two always-on plots (`TerrainObjects dynamic updates` and `TerrainObjects static skipped`). Not installed.

### Not started
- ❌ **Stage 3.B** — `IsStaticNow()` virtual + `TreeAppearance` override + skip gate in `TerrainObject::update`.
- ❌ **Stages 3.C, 3.D, 3.E** — see brainstorm Q7 for ladder.

---

## ⚠️ What changed since recon was written

Recon and brainstorm were authored at HEAD `61f6a66` (post-Stage-2.E pause, GPU-objects default-on flip just landed). At that point the GPU offload path was actually broken (axis-swap bug, no buildings rendering). The 2.0ms `GameLogic.Units.TerrainObjects` zone time in recon Q1 was measured with the GPU offload path **producing nothing visible** but still doing all its CPU work, AND the legacy CPU appearanceUpdate path also running in parallel for many actors.

**As of 2026-05-05, the slice 2 axis-swap fix landed** (this session — see [docs/superpowers/plans/progress/2026-05-04-slice2-gpu-draw-broken-handoff.md](2026-05-04-slice2-gpu-draw-broken-handoff.md) for the prior chapter, this prompt's predecessor). Buildings/trees now render through the GPU path correctly. `BldgAppearance::update` at `bdactor.cpp:2069` short-circuits on `inView || g_useGpuStaticProps`, so for offloaded actors much of the 1.55ms appearanceUpdate hot path is already gone.

**Implication for slice 3 perf gate (Q8 of brainstorm):**
- The 2.0ms baseline is stale. Re-measure on current `main` HEAD before assuming slice 3 has 30% headroom to reduce.
- If most appearance updates are already short-circuited via GPU offload, slice 3's tree-only Stage 3.B may produce negligible savings.
- If TreeAppearance is not yet GPU-offloaded the same way as BldgAppearance (verify via grep), then slice 3 still has tree-population value — but the brainstorm's "1.55ms in appearanceUpdate" justification specifically pointed at BldgAppearance, which is no longer the bottleneck.

**Recommended first step before any code:** Re-run Tracy with current HEAD on `mc2_01` (forest, the recon's reference). Confirm `GameLogic.Units.TerrainObjects` zone time, then identify which appearance class is the new top consumer. If it's `TreeAppearance`, slice 3 is still worth doing. If it's something else (gate, animated, destructible — all on the disqualifier list), slice 3's design needs revisiting.

---

## ⚠️ Coordination with other in-flight work

Two follow-up items from this session are queued and may touch shared code:

1. **[Static-prop alpha-test prompt](2026-05-05-static-prop-alpha-test-prompt.md)** — fence transparency. Touches `gos_static_prop_batcher.cpp` (per-packet materialFlags resolution) and `static_prop.frag`. **No collision** with slice 3 (slice 3 doesn't touch the GPU path or shaders).

2. **[LOD-handoff prompt](2026-05-05-static-prop-lod-handoff-prompt.md)** — buildings using stock textures instead of upscaled. **Potential collision.** One of its three fix paths (Path 1) reintroduces a stripped `TransformMultiShape` call for offloaded actors, purely for the texture-handle update side effect. If Path 1 ships, it puts CPU work back into the per-frame loop that slice 3 is trying to skip. Coordinate: either ship slice 3 first and force LOD-handoff to use Path 2 or 3 (which keep the offload pure), OR explicitly mark TMS-handle-update as a non-skippable side effect in the slice 3 disqualifier list.

   Brainstorm Q9 already calls out `mc2_texture_handle_is_live.md` as a known constraint. Make sure the slice 3 implementation does NOT skip whatever path resolves texture handles for offloaded actors — and document it.

---

## Plan-stage work needed before code

Per worktree CLAUDE.md "Review Discipline" + the brainstorm's closing line ("Suggested next step: plan-format design doc covering Stage 3.A through 3.B at minimum… Run the plan through adversarial review"), this slice's design has been brainstormed but not formalized as a plan.

**Required deliverables before code:**

1. **Re-measurement note.** Append a short addendum to the recon dating today's Tracy capture, with current zone-time numbers and a top-3 list of which appearance classes are now the hot consumers. This unblocks the perf-gate question.

2. **Slice 3 plan document** (`docs/superpowers/plans/2026-05-05-slice3-plan.md`):
   - Stages 3.A-tail, 3.B at minimum (3.C/D/E can be planned later per brainstorm Q7).
   - Cite every symbol grep-verified at write time (CLAUDE.md "Documentation Discipline").
   - Must include a verification appendix per `.claude/skills/adversarial-plan-review.md` recipe.
   - **Must explicitly address** the slice 2 / LOD-handoff coordination above.

3. **Adversarial review** of the plan via `.claude/skills/adversarial-plan-review.md` — mandatory per worktree CLAUDE.md for any architectural-endpoint-class slice. Slice 3 qualifies (touches lifecycle, has perf-gate-default-on flip, intersects with shipped-and-load-bearing slice 2).

---

## Implementation outline (post-plan, post-review)

This is **NOT a substitute for the plan** — it's a quick orientation for the eventual implementer.

### Stage 3.A-tail (finish what 7ec9f6d / fc53cf9 started)
- Wire `g_staticUpdateEmitSummary()` call from `code/objmgr.cpp` at end of TerrainObjects sweep ([objmgr.cpp:1756-1783](../../../../code/objmgr.cpp)). Match the cadence pattern of `MC2_TGL_POOL_TRACE` summary (every 600 frames + on shutdown).
- Install the two TracyPlot calls per brainstorm Q6 — always-on, regardless of trace env.
- Smoke gate: tier1 clean, summary lines emit when `MC2_STATIC_UPDATE_TRACE=1`, plots visible in Tracy.

### Stage 3.B
- Find the right base class for `IsStaticNow()` virtual. Brainstorm punted this; grep for the actual inheritance. Likely candidates: `MC_TextureNode`, `TG_Appearance`, or a new mixin. **Don't skip this step** — putting the virtual at the wrong inheritance level wastes a stage.
- Override on `TreeAppearance` per brainstorm Q2. Predicate: `owner->fallRate == 0 && !owner->isFalling`. Verify the field accessors against current code at impl time.
- Wire the skip gate in `TerrainObject::update` at [code/terrobj.cpp:603-609](../../../../code/terrobj.cpp). Behind `s_staticUpdateSkip` env flag (already declared at line 88).
- Counter increments: when skip fires, bump `updates_skipped` not `updates_run`.
- Smoke + perf gate per brainstorm Q7 Stage 3.B definition.

### Stages 3.C-3.E
Plan and review separately once 3.A-tail + 3.B land cleanly. The brainstorm already scoped these.

---

## Out of scope

Per brainstorm Q9 + worktree CLAUDE.md "feedback_offload_scope_stock_only":

- **Mod content** — Carver5O, Magic, MCO, Wolfman, MC2X all out. Stock missions only.
- **Slice 2 GPU offload changes** — slice 3 is CPU-side. No shader work, no SSBO changes, no GPU draw-path edits.
- **Visual-diff harness** — Stage 2.E was paused on determinism floor (mech idle anims). Slice 3's gate is not visual; it's "+0 destroys, frame count parity, zone time delta." Don't try to revive visual-diff for slice 3.
- **Texture-handle architecture rework** — see brainstorm Q5. The 25µs in `get_gosTextureHandle` is unavoidable in slice 3's scope.
- **ABL-scripted detection beyond a class flag** — brainstorm Q2 punted this to Stage 3.D. Don't try to solve it in 3.B.

---

## Files to read first (in order)

1. **[Worktree CLAUDE.md](../../../../CLAUDE.md)** — load-bearing project rules, Review Discipline section especially.
2. **Recon** ([docs/superpowers/explorations/2026-05-04-slice3-static-update-bypass-recon.md](../../explorations/2026-05-04-slice3-static-update-bypass-recon.md)) — every symbol citation is the source of truth; verify at impl time per CLAUDE.md "Documentation Discipline".
3. **Brainstorm** ([docs/superpowers/brainstorms/2026-05-04-slice3-static-update-bypass-brainstorm.md](../../brainstorms/2026-05-04-slice3-static-update-bypass-brainstorm.md)) — design decisions Q1-Q9.
4. **Memory notes called out by recon/brainstorm:**
   - `memory/cull_gates_are_load_bearing.md` — recalcBounds must NOT be skipped.
   - `memory/tgl_pool_exhaustion_is_silent.md` — pool-budget side effects.
   - `memory/mc2_texture_handle_is_live.md` — handle resolution rule.
   - `memory/bldg_animation_lod_swap_unsafe.md` — animated-building LOD trap (matters for Stage 3.D).
   - `memory/feedback_offload_scope_stock_only.md` — validation scope.
5. **Stage 3.A landed code:**
   - [code/static_update_counters.h](../../../../code/static_update_counters.h)
   - [code/terrobj.cpp:65-130](../../../../code/terrobj.cpp) (counter scaffolding)
6. **Slice 2 axis-swap predecessor handoff** ([2026-05-04-slice2-gpu-draw-broken-handoff.md](2026-05-04-slice2-gpu-draw-broken-handoff.md)) and the resolution this session (commit on `claude/nifty-mendeleev` HEAD with `static_prop.vert` axis swap).

---

## Verification recipe (per stage)

**Stage 3.A-tail:**
```bash
# Build/deploy clean (no global-init flips, incremental link OK).
"C:/.../cmake.exe" --build build64 --config RelWithDebInfo --target mc2
cp -f build64/RelWithDebInfo/mc2.exe "A:/Games/mc2-opengl/mc2-win64-v0.3/mc2.exe"

# Trace summary on:
cd "A:/Games/mc2-opengl/mc2-win64-v0.3"
MC2_STATIC_UPDATE_TRACE=1 MC2_SMOKE_MODE=1 ./mc2.exe --mission mc2_01 --duration 12 2>&1 | grep "STATIC_UPDATE v1"
# Expected: at least one summary line every 600 frames; updates_skipped=0 (no gate yet).

# Smoke gate:
py -3 scripts/run_smoke.py --tier tier1 --kill-existing
# Expected: exit 0.
```

**Stage 3.B:**
```bash
# Baseline (skip OFF):
MC2_STATIC_UPDATE_TRACE=1 MC2_SMOKE_MODE=1 ./mc2.exe --mission mc2_01 --duration 12 2>&1 | grep "STATIC_UPDATE v1" > /tmp/baseline.txt

# Feature ON:
MC2_STATIC_UPDATE_TRACE=1 MC2_STATIC_UPDATE_SKIP=1 MC2_SMOKE_MODE=1 ./mc2.exe --mission mc2_01 --duration 12 2>&1 | grep "STATIC_UPDATE v1" > /tmp/feature.txt

# Expected: feature shows non-zero updates_skipped (likely thousands per frame on a forest mission); baseline shows zero.

# Smoke gate (with skip on):
MC2_STATIC_UPDATE_SKIP=1 py -3 scripts/run_smoke.py --tier tier1 --kill-existing
# Expected: exit 0, +0 destroys vs baseline.

# Visual canary: launch mc2_01 normally with skip=1, pan camera through forest. Trees render, none flicker, none disappear, lighting unchanged.
```

**Stage 3.E (perf gate):**
- Tracy capture before/after on mc2_01, mc2_03, mc2_24.
- Required: ≥30% reduction in `GameLogic.Units.TerrainObjects` zone time on the highest-population mission (likely mc2_01 forest).
- If <30%, the slice ships env-gated, NOT default-on. Surface explicitly per brainstorm Q8.

---

## Communication style

User wants: direct, evidence-before-assertions, one fix at a time. Slice 3 is a multi-stage arc — work the stages in order, smoke after each, don't compress them.

Don't take the brainstorm's perf claim as gospel — the recon was written before slice 2 worked correctly, and the bottleneck has likely shifted. **Re-measure first**, plan second, code third.

Adversarial review is mandatory before code. The slice intersects with load-bearing infrastructure (cull gates, lifecycle returns, GPU offload coordination) and a wrong design here can silently regress in subtle ways (a popup turret stuck at rest, a gate that never opens, a tree that doesn't fall when shot). Worth the extra scrutiny.
