# Slice 3 brainstorm — Static-object update bypass

**Date:** 2026-05-04
**Worktree HEAD:** `61f6a66`
**Recon:** [docs/superpowers/explorations/2026-05-04-slice3-static-update-bypass-recon.md](../explorations/2026-05-04-slice3-static-update-bypass-recon.md)
**Format:** Q&A, code-grounded. Every cited symbol grep-verified at write time per `memory/brainstorm_code_grounding_lesson.md`.

---

## Q1. Where does the dynamism predicate live — type-time field, runtime virtual, or both?

**Decision: virtual `bool IsStaticNow() const` on the appearance class, default `false`.** Per-class overrides supply the actual logic.

Reasoning:
- Type-time-only would miss runtime-induced dynamism: a tree that's static becomes dynamic when knocked down (`terrobj.cpp:572-590` `fallRate != 0`); an undamaged building becomes dynamic when its damage state changes (`bdactor.cpp:1992-2003` weapon-node recycling).
- Runtime-virtual lets each class self-evaluate per-frame against its own state. Cheap (one virtual + a few field reads) compared to the 1.55ms `appearanceUpdate` it gates.
- Existing precedent: `BldgAppearance::update` at `bdactor.cpp:2069` already short-circuits on `inView || g_useGpuStaticProps`. A `IsStaticNow()` early-out is the same shape.

Type-time *augments*: an appearance class can cache invariant facts at type-load (e.g., `m_bdFrameRate == 0 && m_bdAnimData == NULL && m_destructible == false`) so `IsStaticNow()` is just `cachedTypeIsStatic && !instanceIsCurrentlyTransient`.

---

## Q2. What's the initial allowlist, and what's the disqualifier list?

**Allowlist (Stage 3.B opening move): trees only.** `TreeAppearance` (`bdactor.cpp:4295-4436`) overrides `IsStaticNow()` to return `true` when:
- The owning `TerrainObject` is not falling — recon citation `terrobj.cpp:572-590` `fallRate != 0` is the dynamism trigger.
- (Trees have no other dynamism sources — no animation, no destructibility in stock content, no scripted hooks.)

**Disqualifier list (must always return `false`):**

| Class | Reason | Citation |
|---|---|---|
| `GateAppearance` | gesture machine MUST tick | `gate.cpp:348` "MUST update appearance every frame" |
| Animated buildings (`bdFrameRate != 0` OR `bdAnimData[i] != NULL`) | frame increment in animation tick | `bdactor.cpp:2161-2210` |
| Destructible buildings (`damageLevel > 0`) | weapon-node recycling timers decay per frame | `terrobj.cpp:658`, `bdactor.cpp:1992-2003` |
| Power-supply-dependent buildings (`powerSupply != 0xffffffff`) | parent-destruction check | `terrobj.cpp:563-568` |
| GOSFX-effect-bearing | per-frame `Execute()` | `terrobj.cpp:611-635` |
| Falling trees (`fallRate != 0`) | per-frame `pitchAngle` advance | `terrobj.cpp:572-590` |
| Anything we can't classify | conservative default | `IsStaticNow()` returns `false` |

ABL-scripted objects: leave dynamic. Per recon's punt, we can't statically detect "scripted" without runtime info; safer to require a class flag `m_isScripted` set by ABL hooks, and treat as dynamic when set. Defer to Stage 3.D when we touch buildings.

**Stages 3.C/3.D expand the allowlist:**
- 3.C — walls (`TERROBJ_WALL_*`), fences (`TERROBJ_NONE`), bridges (`TERROBJ_BRIDGE`) — recon flagged these as provably static.
- 3.D — non-animated, non-destructible, non-power-dependent civilian buildings — needs all 5 disqualifier checks to pass.

---

## Q3. How do we handle "was static, now dynamic" transitions?

**Decision: predicate is evaluated every frame.** The cost of the virtual call + field reads is negligible (~tens of ns) vs the saved work (microseconds per object).

Transitions to handle:
- Tree gets knocked down: `fallRate` becomes non-zero → `TreeAppearance::IsStaticNow()` returns `false` → next frame's update runs. No state catch-up needed because the tree was static in prior frames (no animation to "miss").
- Building takes damage: `damageLevel` increments → `BldgAppearance::IsStaticNow()` returns `false` next frame. Damage-state transitions are visible-effect-bearing (smoke, debris) so they typically also light up GOSFX, which is its own disqualifier.
- Power-supply parent destroyed: `terrobj.cpp:563-568` runs `appearance->setLightsOut(true)`. This currently runs UNCONDITIONALLY at `terrobj.cpp:563` regardless of dynamism — need to keep it OUTSIDE the appearanceUpdate skip path. Easy: it's already at line 563 above the gate at 603-609.

**One-frame stale-state risk:** if a tree's `fallRate` becomes non-zero between frame N's `IsStaticNow()` check and frame N's update, we'd miss one frame of fall animation. But `fallRate` is set ONLY by `terrobj.cpp:572-590` itself, which only runs WHEN `update()` runs. So the transition is self-consistent — `update()` ran the frame the tree got hit, set `fallRate`, next frame `IsStaticNow()` sees it and returns `false`. No stale-state.

---

## Q4. Do we still call `recalcBounds` on objects we skip?

**Yes. Always.** Per recon, `recalcBounds` at `terrobj.cpp:599-601` is the load-bearing cull gate cited in `memory/cull_gates_are_load_bearing.md`. It returns `inView`. Skipping cascades into:
- Stale `inView` flag → object continues to be rendered after going off-screen, OR object disappears while still on-screen
- Stale `objBlockInfo.active` → update loop iteration count diverges from "what's visible"
- Pool exhaustion (mechs canary) per `memory/tgl_pool_exhaustion_is_silent.md`

The skip target is **only** the `appearanceUpdate()` call at `terrobj.cpp:603-609`. The full `update()` body still runs, including `recalcBounds`, lifecycle return, GOSFX tick, power-supply check, tree-falling animation. That's the 132µs `recalcBounds` + 53µs `update` self-time + 22µs `appearanceSetup` from the Tracy capture — ~210µs unaffected. The 1.55ms `appearanceUpdate` is the win.

Net expected: ~210µs floor, ~210µs + (animated_objects × per-object-cost) when feature is on. Today's 2ms zone time → projected ~210µs + small dynamic remnant.

---

## Q5. Can we also skip `get_gosTextureHandle` for static objects?

**No.** Per `memory/mc2_texture_handle_is_live.md`: "MC2 texture handles MUTATE per-frame; store slot index, resolve at draw time, never cache handle." The 25µs spent in `get_gosTextureHandle (×1894)` is unavoidable today.

But: `get_gosTextureHandle` is called from the *render* path (texture lookup at draw time), not from `appearanceUpdate`. Skipping appearanceUpdate doesn't affect the texture-handle resolution count. The 25µs is constant regardless of slice 3.

If we ever want to eliminate it, that's a SEPARATE arc (texture-handle architecture rework), not slice 3.

---

## Q6. What's the counter/instrumentation surface?

Matches the `MC2_TGL_POOL_TRACE` / `MC2_DESTROY_TRACE` pattern from worktree CLAUDE.md "Tier-1 Instrumentation Env Vars":

```
MC2_STATIC_UPDATE_TRACE=1   → enables per-frame counters; summary line every 600 frames
MC2_STATIC_UPDATE_SKIP=1    → enables the actual skip behavior (default-off in Stage 3.B)
```

Counter struct (file-private static in `code/objmgr.cpp` or a new small TU):
```cpp
struct StaticUpdateCounters {
    uint32_t objects_seen;
    uint32_t updates_run;
    uint32_t updates_skipped;
    uint32_t dyn_gates;
    uint32_t dyn_animated;
    uint32_t dyn_destructible;
    uint32_t dyn_power;
    uint32_t dyn_effects;
    uint32_t dyn_falling;
    uint32_t dyn_scripted;
    uint64_t zone_time_ns;  // optional, if we wrap the zone in our own timer
};
```

Summary line:
```
[STATIC_UPDATE v1] frame=N seen=N run=N skip=N gates=N anim=N destruct=N power=N fx=N fall=N scripted=N
```

Tracy plots (always-on):
- `TracyPlot("TerrainObjects dynamic updates", int64_t(updates_run))`
- `TracyPlot("TerrainObjects static skipped", int64_t(updates_skipped))`

These two plots together graph the slice's value at runtime. If `updates_skipped` is consistently small, the feature isn't earning its keep on that mission.

---

## Q7. Staging — how many commits, what's the gate at each?

**5 commits total**, matching prior arc cadence:

**Stage 3.A — instrumentation only.** Add counter struct + env-gated trace + Tracy plots. NO `IsStaticNow()` predicate yet, NO behavior change. Default-off and on are equivalent. **Gate:** build clean, default-env smoke +0 destroys, counter increments observable when trace env set.

**Stage 3.B — `IsStaticNow()` predicate + tree-only override.** Virtual on the relevant base class (likely `MC_TextureNode` or a new mixin — needs grep at impl time to find the right inheritance level). `TreeAppearance` overrides; all other appearances default-false. Gated behind `MC2_STATIC_UPDATE_SKIP=1`, default-off. **Gate:** with skip on/off, tier1 forest mission (mc2_01) shows non-zero `updates_skipped`, +0 destroys delta, frame count parity within 1%.

**Stage 3.C — walls / fences / bridges allowlist expansion.** Override `IsStaticNow()` for the appearance classes that handle `TERROBJ_WALL_*`, `TERROBJ_NONE`, `TERROBJ_BRIDGE`. **Gate:** tier1+tier2 stock smoke +0 destroys, no visual canaries flag (walls don't disappear, bridges still passable).

**Stage 3.D — non-animated civilian buildings.** `BldgAppearance::IsStaticNow()` with the 5-way disqualifier check (frameRate==0 && bdAnimData all null && damage==0 && powerSupply==invalid && no active effect). Subtle; needs careful per-instance verification. **Gate:** missions with civilian buildings (mc2_03, mc2_24) +0 destroys, manual visual check on mission with destructible buildings.

**Stage 3.E — perf measurement + default-on flip.** Need ≥30% reduction in `GameLogic.Units.TerrainObjects` Tracy zone time on a forest-heavy tier1 mission to justify default-on. If we don't hit 30%, surface — slice's value is empirical question, not assumption. **Gate:** measured perf delta + tier1+tier2 smoke clean default-on.

---

## Q8. Perf gate before default-on?

Tracy zone time `GameLogic.Units.TerrainObjects` on mc2_01 (forest) baseline ≈ 2.0ms.

Acceptance for default-on flip:
- ≤ 1.4ms (≥30% reduction) on mc2_01 baseline-vs-feature comparison
- No regression (measured delta ≥ 0) on any tier1 mission
- p1% FPS unchanged or improved
- Δ destroys = +0 for tier1+tier2

If reduction is below 30%, slice 3 isn't worth default-on. Either expand the allowlist (Stage 3.D needs careful work) or accept the env-gated state as the deliverable.

---

## Q9. What known traps from prior arcs apply?

From `memory/cull_gates_are_load_bearing.md`: don't skip recalcBounds. **Already addressed in Q4** — only `appearanceUpdate` is the skip target.

From `memory/bldg_animation_lod_swap_unsafe.md`: BldgAppearance LOD swap caches LOD-0 node->index in shared per-type state; LOD swap drives wrong node for animated types. **Implication for slice 3:** don't try to be clever about LOD-skipping for static buildings — leave LOD swap alone, only skip the per-frame animation tick. Stage 3.D's BldgAppearance work needs to NOT touch the LOD path.

From `memory/tgl_pool_exhaustion_is_silent.md`: pool budget is finite; if cull bypass marks more shapes "must allocate," pool exhausts silently. **Slice 3 doesn't touch allocation paths**, but Stage 3.B verification should confirm pool stats unchanged.

From `memory/mc2_texture_handle_is_live.md`: handles mutate per frame; can't cache. **Already addressed in Q5** — get_gosTextureHandle stays.

From `memory/feedback_offload_scope_stock_only.md`: validation gates are tier1 stock only. **Slice 3 follows this** — no Carver5O, Magic, MCO, Wolfman, MC2X testing in scope.

From the Stage 2 arc (just-closed): wall-clock-driven shader uniforms (`SDL_GetTicks()` in three places at `gos_postprocess.cpp:639,814,882`) and mech idle animations are nondeterminism sources. **Implication for slice 3:** parity check and visual-diff tests are NOT applicable to slice 3 — there's no GPU/CPU output to compare. The slice 3 gate is "+0 destroys, frame count parity, zone time delta." Simpler than slice 2's gate ladder.

---

## Bottom line

**Slice 3 is bounded and well-scoped.** The recon answered every architectural question; this brainstorm locks the predicate placement (`IsStaticNow()` virtual on appearance), the allowlist expansion order (trees → walls/fences/bridges → buildings), the counter design (matches existing tier-1 instrumentation pattern), and the gate ladder (5 stages, perf gate before default-on flip).

**Open punts** (deferrable to plan/implementation, not blocking):
- Exact base class for the `IsStaticNow()` virtual — could be `MC_TextureNode` or a new `TG_Appearance` mixin. Grep at plan time.
- ABL-scripted detection mechanism — likely a flag set by ABL hooks. Resolve at Stage 3.D.
- The exact set of GOSFX effect-pointer fields that mark "active effect" — enumerate at Stage 3.D.

**Suggested next step:** plan-format design doc covering Stage 3.A through 3.B (instrumentation + tree allowlist) at minimum. Stages 3.C-3.E can be planned later once 3.A/B land. Run the plan through adversarial review per `.claude/skills/adversarial-plan-review.md` discipline before code starts.

The slice should land in 5 commits, ~1-2 days of work, with a measured perf gate at the end. No GPU surgery, no shader work, no schema changes.
