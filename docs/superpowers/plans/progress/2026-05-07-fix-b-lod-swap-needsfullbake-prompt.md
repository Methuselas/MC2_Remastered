# Execution prompt — Fix B: LOD-swap `needsFullBakeNextFrame` (black trees correctness)

**Paste this into a fresh session to execute.** Self-contained briefing.

---

You are executing the second half of the H4 fix from the `MC2_STATIC_UPDATE_SKIP=1` residual investigation. Yesterday's H4 fix (commit `396effa`) covered the mission-load `registerStatic()` path. **This fix covers the per-frame re-registration path**, which is triggered by LOD swap on trees and damage-state swap on buildings.

## The bug in one sentence

After per-frame LOD swap on a tree, the new `treeShape` (fresh `TG_MultiShape` from `CreateFrom()`) has default-zero `lightData_`. Under `MC2_STATIC_UPDATE_SKIP=1`, `touch()` re-submits the empty `lightData_` to `addLightDataStructure` → all-zero lighting slot → black tree. The mission-load H4 fix doesn't cover this because re-registration happens at a different code site.

## Worktree

`A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`. All paths relative.

## Required reading (in order)

1. `CLAUDE.md` — worktree rules. The "Load-Bearing Cull Infrastructure" section is mandatory before touching any cull/registration code.
2. **`docs/superpowers/specs/2026-05-07-lod-swap-static-registry-churn.md`** — the spec that documents this bug + the four-fix-path proposal. Fix B is "Fix 2" in that spec (extend the H4 mission-load fix to the per-frame re-register sites).
3. **`docs/superpowers/specs/2026-05-06-update-skip-touch-residual-debug-strategy.md`** — H4 framework doc. Read the H4 section to understand what the existing `needsFullBakeNextFrame` mechanism does.
4. `memory/cull_gates_are_load_bearing.md` — load-bearing.
5. `memory/black_tree_bug_investigation_state.md` — historical context on the bug class.
6. **`mclib/bdactor.cpp:4358-4406`** — the per-frame TreeAppearance re-registration block. This is the code site you're modifying. Note that the H4 fix (commit `396effa`) already added `needsFullBakeNextFrame = true` in `TreeAppearance::registerStatic()` at the mission-load site (`bdactor.cpp:4920`); you're mirroring that pattern at the per-frame site at line 4401.
7. **`mclib/bdactor.cpp:1622-1686`** — same pattern for BldgAppearance (per-frame re-register at the damage-state swap path; mission-load site already H4-fixed at line 2727).
8. Commits `9e45718`, `4c8f9a4`, `996aff4`, `396effa` — read the commit messages to understand the predecessor fixes.

## The fix (mechanically)

Two near-identical edits, one in TreeAppearance, one in BldgAppearance.

### TreeAppearance (`mclib/bdactor.cpp:4396-4405`)

The current code:

```cpp
if (submittedToGpu && !staticReg.registered
        && GpuStaticPropRegistry::isEnabled()
        && !needsFullBakeNextFrame) {
    const auto& batch =
        GpuStaticPropBatcher::instance().getLastBuiltBatch();
    staticReg.recipeIndex = GpuStaticPropRegistry::registerRecipe(
        treeShape, batch);
    staticReg.registered  = (staticReg.recipeIndex >= 0);
    staticReg.shape        = treeShape;
}
```

Add `needsFullBakeNextFrame = true;` after `staticReg.shape = treeShape;`. Mirror the comment from the mission-load `registerStatic` call site at line 4920 (the H4 comment block) for consistency.

### BldgAppearance (`mclib/bdactor.cpp` near line 1670 — verify exact line via grep at write-time)

Same shape: find the per-frame re-registration block in `BldgAppearance::render`. It mirrors the tree path. Apply the same `needsFullBakeNextFrame = true` after the registerRecipe call. Comment block mirrors `registerStatic` at line 2727.

## Why this works

- After per-frame re-registration, `IsStaticNow()` checks `!needsFullBakeNextFrame` (`bdactor.cpp:2735` / `:4831`). Setting the flag forces it to return `false` on the very next frame.
- That routes the actor through full `update()` instead of `touch()` for ONE frame.
- Full `update()` runs `TransformMultiShape` which populates `lightData_` correctly.
- After that update, the flag self-clears at `bdactor.cpp:2313` / `:4313`.
- Subsequent frames hit `IsStaticNow() = true` again, skip path runs, `touch()` re-submits the now-valid `lightData_` → correct lighting.

Cost: one extra full `update()` per LOD-swapping or damage-state-swapping actor per swap event. For trees hovering at the LOD distance boundary (the worst case), this is one full bake per frame instead of zero — same cost as pre-Track-B behavior. Acceptable.

The architectural fix that ELIMINATES the LOD-swap cost entirely (Fix 3 / Fix 4 in the spec) is much bigger scope and explicitly out of this slice.

## Skill to invoke

**`superpowers:subagent-driven-development`** — slice is small enough to land inline.

## Gates

- **Build clean:** `cmake --build build64 --config RelWithDebInfo --target mc2`.
- **Visual canary 1 — default config:** 15s mc2_01 smoke. Should look identical (this fix is no-op when `MC2_STATIC_UPDATE_SKIP=0`).
- **Visual canary 2 — UPDATE_SKIP enabled:** `MC2_STATIC_UPDATE_SKIP=1 build64/RelWithDebInfo/mc2.exe -mission mc2_01`. Tree LOD-swap should NOT produce black trees during camera motion. This is THE confirmation gate.
- **`[DESTROY v1]` count parity vs baseline** (commits `396effa` should be HEAD when this slice starts).
- **Tracy:** `TreeAppr LOD_swap_reregister` zone count remains the same (this fix doesn't reduce LOD-swap rate, only the rendering correctness during the swap-to-skip transition).

## What NOT to do

- **Don't move LOD swap out of the render path.** That's Fix 4 territory (4-8 weeks of architectural work). This slice is correctness-not-architecture.
- **Don't try to reuse tombstoned recipe slots.** That's Fix 1 territory (~15 lines, separate slice). Could ship after Fix B, but not as part of it.
- **Don't disable LOD swap for static-eligible trees.** That changes visual quality (high-poly distant trees) and is its own decision.
- **Don't extend the fix to GenericAppearance or other classes** without verifying their re-registration semantics. The Bldg/Tree pattern is symmetric; other classes may not be.

## When blocked

- **Visual regression at LOD swap edges:** the fix is supposed to ELIMINATE the regression, not introduce one. If a regression appears, capture which actor is affected and whether the issue is at LOD-swap moment specifically. May indicate the flag self-clear logic doesn't fire when expected.
- **`[DESTROY v1]` count delta:** the fix shouldn't perturb lifecycle. If counts diverge, the extra `update()` may be hitting a returning-false condition that destroys the actor. Investigate via `MC2_DESTROY_TRACE=1`.
- **Per-frame perf regression in default config:** shouldn't happen — the fix is only active during the swap event. If Tracy shows extra cost without UPDATE_SKIP, the implementation isn't gated correctly. Verify the flag is set ONLY at the per-frame re-register site, not other paths.

## Deliverable

Single commit on `claude/nifty-mendeleev`:
- Title: `fix(static-update-skip): seed lightData_ on per-frame re-register (LOD/damage swap path)`
- Body: explain that this mirrors the H4 mission-load fix from commit `396effa` to the per-frame re-registration site; cite the spec at `docs/superpowers/specs/2026-05-07-lod-swap-static-registry-churn.md` (Fix 2); confirm visual canary under `MC2_STATIC_UPDATE_SKIP=1` shows no black trees.

## What this slice does NOT close

- **Per-frame CPU bake on LOD swap.** The full `update()` triggered by `needsFullBakeNextFrame` runs `TransformMultiShape` (full bake) on the new shape every LOD swap. For a tree hovering at the LOD boundary, this is full-bake-per-frame. This is the "LOD-swap-driven CPU bake" cost that Fix 3 (per-LOD pre-registration) addresses. Fix B does NOT improve this; it only fixes correctness.
- **Recipe slot leak from invalidate+register churn.** `gos_static_prop_registry.cpp:registerRecipe` always appends; tombstoned slots are never reclaimed. Fix 1 in the spec covers this. Fix B does NOT address it.
- **`mc_texturemanager::update scanNodes` 80ms spikes** under UPDATE_SKIP. Separate investigation; unrelated mechanism.

After this ships, the user has options: ship Fix 1 (slot reuse — small), tackle Fix 3 (per-LOD pre-registration — architectural), or pivot to Track A1/A2/B/C arc work depending on priorities.

## Coordination with Fix A

**Fix A** (`docs/superpowers/plans/progress/2026-05-07-fix-a-gather-lights-cache-prompt.md`) is independent of this fix. They can ship in either order. Fix A is perf, Fix B is correctness. Reasonable to ship B first since correctness > perf, then A.
