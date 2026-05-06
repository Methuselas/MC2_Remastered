# Handoff: `MC2_STATIC_UPDATE_SKIP=1` static-prop black-billboard bug — texture-cache framing

> **STATUS:** Active investigation. Prior framings (registration-timing race; alpha-test material-classification gap) confirmed wrong/incomplete for the user-visible symptom. **This handoff replaces them.** Read this in full before touching anything.
>
> **Worktree:** `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`
> **Branch:** `nifty-mendeleev` (`claude/nifty-mendeleev`)
> **HEAD at handoff:** `afcd75b feat(3c+3d): static-prop registry default-on with cull-aware replay`
>   - Working tree has un-committed Path 4 implementation (`gos_static_prop_batcher.{h,cpp}` modifications) shipped as Track B prep — **leave it; it's a separate concern from this bug.**
>   - LOAD-BEARING reference: `pause_unpause_diagnostic_for_static_render_bugs.md` (NEW memory file, 2026-05-06).
> **Deploy target:** `A:/Games/mc2-opengl/mc2-win64-v0.3/mc2.exe`
> **CMake:** `C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe`

---

## The bug — the visible symptom and the disambiguating clue

### Visible symptom

Run mc2_01 with `MC2_STATIC_UPDATE_SKIP=1`. Within first ~100 frames:

- Trees, props, fences render as **solid black quads** in the LOD-2 (billboard) form.
- Buildings render correctly.
- The black "edge" follows the camera-distance LOD-cutoff line — items close to the camera at LOD-0 render correctly, items past the line render black billboards.
- "Lit-then-stale" pattern at the LOD line: an item briefly renders correctly for a couple of frames as it crosses the line, then goes black.

### THE DISAMBIGUATING CLUE — pause/unpause

**User has told 3-4 prior sessions this and it didn't propagate. Captured now in `memory/pause_unpause_diagnostic_for_static_render_bugs.md`. Read that memory file FIRST.**

> Hit Esc to bring up the in-game pause / exit-confirmation menu. **Trees/props/fences render CORRECTLY while the menu is up.** Cancel/unpause. **They go black again immediately.**

**This single observation rules out every prior hypothesis** (registration timing race, alpha-test material-flag classification, leaf identity drift, LOD-swap recipe staleness) — none of those change between paused and running states. **Texture cache state is the only thing that changes per-frame in a way that suspends during pause.**

### The smoking-gun comment

[`code/mission.cpp:507-510`](code/mission.cpp:507) — original engine authors anticipated this exactly:

```cpp
//Do not UPDATE the textures during a pause.
//This uncaches things which only objectManager->update can cache back in!!!!!
if ( !missionInterface->isPaused() || MPlayer )
    { ZoneScopedN("GameLogic.Mission.TextureManager"); mcTextureManager->update(); }
```

The mechanism:
1. `mcTextureManager->update()` (per-frame, gameplay only) **evicts/uncaches** textures.
2. `objectManager->update()` (per-frame, gameplay only) **re-caches** them via per-actor `update()` → `TransformMultiShape` → `SetTextureHandle`.
3. **During pause**: both are skipped → no eviction this frame → textures persist correctly.
4. **During unpause**: eviction resumes; for `MC2_STATIC_UPDATE_SKIP=1`-substituted actors, `touch()` runs *instead of* `update()` → no `TransformMultiShape` → no `SetTextureHandle` → no re-cache → static-prop registry's draw-time slot→handle resolution returns invalid GL texture ID → samples render solid black.
5. **LOD-line dependency:** the LOD-2 typeID's textures haven't been touched recently (they're a different texture set than LOD-0's), so they evict more readily, so the bug shows there first.

## What's confirmed about the engine state

- `MC2_STATIC_UPDATE_SKIP=1` substitution gate at [`code/terrobj.cpp:703-716`](code/terrobj.cpp:703). Calls `appearance->touch()` instead of `appearance->update()` when `IsStaticNow() && gpuPath`.
- `TreeAppearance::touch()` at [`mclib/bdactor.cpp:4740-4752`](mclib/bdactor.cpp:4740) calls only `treeShape->ResubmitCachedGpuLightData()` and `treeShape->Touch()`. **No texture work.**
- `BldgAppearance::touch()` at [`mclib/bdactor.cpp:2654-2674`](mclib/bdactor.cpp:2654) is structurally identical. Buildings escape the bug by other means (their textures stay hot somehow; verify per the investigation below).
- The static-prop registry's draw-time texture resolve at [`gos_static_prop_batcher.cpp:1734-1737`](GameOS/gameos/gos_static_prop_batcher.cpp:1734):
  ```cpp
  if (src && src->listOfTextures && pkt.textureSlot < src->numTextures) {
      gosHandle = src->listOfTextures[pkt.textureSlot].gosTextureHandle;
  }
  const uint32_t glTexId = gos_GetGLTextureId(gosHandle);
  ```
  `src` is the `TG_TypeShape*`. Its `listOfTextures[].gosTextureHandle` was primed once at register time with the multi-type's `mcTextureNodeIndex` (line 696-715). If that node-index now resolves to an evicted/invalid texture at draw time, `glTexId` is invalid and the sampler returns black.

## Investigation starting points

### Step 1 — confirm the framing with focused instrumentation (no fix yet)

The `MC2_REG_TYPE_TRACE=1 MC2_ALPHA_TEST_TRACE=1` instrumentation already in tree provides the recipe-level data. **What's missing for THIS bug is draw-time texture-handle resolution diagnostics.** Add:

- At [`gos_static_prop_batcher.cpp:1737`](GameOS/gameos/gos_static_prop_batcher.cpp:1737) (after `gos_GetGLTextureId`): log `nodeIdx=%u gosHandle=0x%x glTexId=%u` per draw, when the bug is reproducible. The existing `TEX_HANDOFF` macro at line 1738-1741 logs this but is capped at 8 lines per process. Lift the cap (or add a fresh-named env-gated trace) so per-frame data is visible.
- Or simpler: instrument `mcTextureManager->update()` itself to log evictions. What gets evicted and at what cadence?
- Also instrument `actor.update() → SetTextureHandle` re-cache pathway. What gets re-cached?

The paired instrumentation answers: **"during gameplay, what texture nodes are evicted that aren't being re-cached?"** Cross-reference with the typeIDs that emit `effective=0x1` but render black to identify the actual broken slots.

### Step 2 — verify the proposed mechanism

Run instrumented build. Observe:
- Reproduce bug under `MC2_STATIC_UPDATE_SKIP=1` (immediate, within ~100 frames).
- Hit Esc. Pause the mission. Trees render correctly. Confirm via instrumentation: `mcTextureManager->update()` log shows evictions stop during pause.
- Unpause. Confirm: evictions resume, no matching re-caches for static-prop typeIDs that were just evicted.
- (Optional, if Tracy is connected) capture timing.

If the data shows eviction without matching re-cache → mechanism confirmed.

### Step 3 — pick a fix shape

Three candidate fixes once the mechanism is confirmed:

#### Fix A — make `touch()` re-cache textures
Add a texture-touch step to `TreeAppearance::touch()` and `BldgAppearance::touch()` that pings `mcTextureManager` to keep the type's slots warm. Fragile — requires knowing exactly which slots and is duplicating work `update()` does.

#### Fix B — pin static-prop registry textures (recommended starting point)
The static-prop registry knows which texture slots its recipes use. At `registerType` time, pin those slots in the texture manager so the per-frame eviction can't touch them. Add `mcTextureManager->pinNode(nodeIdx)` API.

#### Fix C — change the eviction policy
Make `mcTextureManager->update()` not evict slots that have been recently used by the static-prop registry. Looser, may have other side-effects, but simpler.

Each requires a separate spec + adversarial-plan-review per the worktree CLAUDE.md "Review Discipline." Don't rush to implementation; **the prior-session pattern (this session and the 3-4 before it) was to chase fixes without fully confirming the mechanism, and each fix attempt revealed it was solving a different bug.**

## Anti-patterns to avoid (lessons from the prior sessions)

The user has been patient through ~4 sessions on this bug. Don't repeat these:

1. **Do not chase the alpha-test material flag.** Path 4 is in tree (rev 3.3) as Track B prep. It addresses a real but DIFFERENT problem (registration-time material classification gap). It does NOT fix the visible black-billboard symptom. Confirmed by user-visual: Path 4 binary still showed all-black trees+props+fence under `UPDATE_SKIP=1`, and the pause/unpause diagnostic ruled out alpha-test entirely.

2. **Do not chase leaf identity drift / `cachedGpuLightIndex_` snapshot patterns.** A previous session's snapshot fix (move `lightData_` from leaf to `TG_MultiShape`) was reverted because it (a) didn't fix the visible bug, (b) introduced a baseline regression of 0.5% → 10-20%, and (c) papered over the actual mechanism.

3. **Do not chase the registration timing race.** Cross-referenced data confirms `TreeAppearance` register order IS correct (`TreeAppearanceType::init`'s `SetAlphaTest(true)` runs BEFORE `TreeAppearance::init`'s `registerMultiShape`). The 36+ "permanent-black types" identified in earlier diagnostics are real but unrelated to the visible symptom.

4. **Do not skip the pause/unpause test.** First diagnostic after reproducing the bug. Done in 30 seconds. Eliminates 4 hypothesis classes immediately.

5. **Do not paper over with a draw-time alpha-test rescue widening.** The existing `textureAlpha` rescue at `gos_static_prop_batcher.cpp:1750-1753` already provides one safety net. Path 4 (rev 3.3, in tree) provides a register-time second safety net. If a third "always force alpha-test" fix is proposed, it's papering over.

## Memory file pointers (read these)

Under `~/.claude/projects/A--Games-mc2-opengl-src/memory/`:

| File | Why |
|---|---|
| `pause_unpause_diagnostic_for_static_render_bugs.md` | **READ FIRST.** Captures the disambiguator that didn't propagate across prior sessions. |
| `mc2_texture_handle_is_live.md` | Texture handles mutate per-frame; registry stores slot indexes, not handles. The cache-eviction bug is what makes the slot→handle resolve fail. |
| `update_skip_touch_regression.md` | Original handoff for this bug. Now mis-scoped (says "lighting wrong"); supersede with the texture-cache framing. |
| `feedback_dont_paper_over_bugs.md` | Discipline reminder. Confirm the mechanism before fixing. |
| `feedback_dont_pile_fixes_pre_verification.md` | Apply ONE candidate fix at a time, verify, then stack. |
| `feedback_subagent_write_mode_verify.md` | Always `git diff` after subagent writes. |
| `cull_gates_are_load_bearing.md` | The cull/lifecycle/allocation chain gates per-actor update; bypass-style fixes cascade. |
| `tg_shape_static_state_lifecycle_trap.md` | Class-static reset by per-instance init must keep related fields together. |
| `feedback_smoke_policy_30s_mc2_01.md` | Smoke is 30s mc2_01 only; never tier1 5/5; "it either happens or it doesnt." |
| `feedback_always_dispatch_adversarial_review.md` | High-stakes plans (this is one) get adversarial review without asking. |
| `msvc_incremental_link_silent_staleness.md` | Class-layout commits → `rm -f mc2.exe` before relink. (Path 4 doesn't change layout; the cache fix likely won't either; just remember the rule.) |

## Working environment summary

```bash
# Worktree
cd "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev"

# Build
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2

# Deploy (NEVER cp -r — silently fails on Windows/MSYS2)
cp -f "build64/RelWithDebInfo/mc2.exe" "A:/Games/mc2-opengl/mc2-win64-v0.3/mc2.exe"
diff -q "build64/RelWithDebInfo/mc2.exe" "A:/Games/mc2-opengl/mc2-win64-v0.3/mc2.exe"

# Smoke (reproduces the bug)
MC2_STATIC_UPDATE_SKIP=1 \
py -3 scripts/run_smoke.py --mission mc2_01 --duration 30 --kill-existing --keep-logs

# Smoke artifacts land at:
# tests/smoke/artifacts/<timestamp>/mc2_01.log

# To get instrumentation traces:
MC2_STATIC_UPDATE_SKIP=1 MC2_REG_TYPE_TRACE=1 MC2_ALPHA_TEST_TRACE=1 \
py -3 scripts/run_smoke.py --mission mc2_01 --duration 30 --kill-existing --keep-logs
```

**The smoke harness can't see "black billboards"** — the user's interactive verification is required for visual confirmation. Smoke ≈ "did the engine boot and render frames cleanly." User test ≈ "do trees look correct."

**Build hygiene reminder:** Path 4's changes are non-class-layout (just code in batcher); `--config RelWithDebInfo` incremental link is safe. Per `msvc_incremental_link_silent_staleness.md`, only purge on class-layout-changing commits.

## Reference: existing instrumentation in tree

| Env var | Effect | Source |
|---|---|---|
| `MC2_REG_TYPE_TRACE=1` | `[REG_TYPE v2]` events at `registerType` (event=type, event=pkt, event=warn) | `gos_static_prop_batcher.cpp` ~line 41-46 |
| `MC2_ALPHA_TEST_TRACE=1` | `[ALPHA_TEST]` events at draw time with `effectiveSource=...` | `gos_static_prop_batcher.cpp` ~line 30-32 |
| `MC2_LIGHT_DEBUG=1` | Umbrella that enables `MC2_REG_TYPE_TRACE`, plus the prior-session [FIRST_LEAF v1 / v2], [GATHER v1], [SET_LIGHTS v1], [RENDER_PATH v1], [LEAF_DRIFT v1] traces (still in tree, gated off, demote-don't-delete) | various in `mclib/msl.cpp`, `tgl.cpp`, `txmmgr.cpp`, `bdactor.cpp` |
| `MC2_STATIC_UPDATE_SKIP=1` | Triggers the bug. | `code/terrobj.cpp:88` |

There's a Python parser at `scripts/parse_light_debug.py` for the older `[FIRST_LEAF]` / `[GATHER]` / etc. traces. It does NOT parse `[REG_TYPE v2]` events. If you need to grok those: simple grep + awk.

## Definition of done

When you can:

1. With `MC2_STATIC_UPDATE_SKIP=1` set: run mc2_01 30s smoke + interactive turn-away-and-back testing. **Zero black billboards visually.**
2. Pause/unpause test on the deployed binary: **rendering identical between paused and running states.** (No "trees correct only when paused.")
3. No tier1 regression in the default `MC2_STATIC_UPDATE_SKIP=0` baseline.
4. Written root-cause explanation in `memory/update_skip_touch_regression.md` (replace the current contents — it's currently mis-scoped).
5. Update `memory/pause_unpause_diagnostic_for_static_render_bugs.md` if anything new is discovered about the eviction policy (e.g., "the fix is X, the mechanism is Y").

…then this bug closes and `MC2_STATIC_UPDATE_SKIP=1` becomes a default-on-flip candidate.

## Discipline (non-negotiable, lessons from prior sessions)

1. **Don't paper over** — characterize before fixing. `feedback_dont_paper_over_bugs.md`. The mechanism here is texture cache eviction; confirm via instrumentation BEFORE proposing a fix.
2. **Apply ONE candidate fix at a time** — `feedback_dont_pile_fixes_pre_verification.md`.
3. **PAUSE/UNPAUSE TEST FIRST** for any "static asset goes wrong" bug — `pause_unpause_diagnostic_for_static_render_bugs.md`. 30 seconds, eliminates 4 hypothesis classes.
4. **Verify subagent file changes via `git diff`** — `feedback_subagent_write_mode_verify.md`.
5. **Visual smoke can miss bugs** — interactive verification by the user is required. `feedback_smoke_policy_30s_mc2_01.md`.
6. **For high-stakes plans, dispatch adversarial review without asking** — `feedback_always_dispatch_adversarial_review.md`. This investigation IS high-stakes: closing the engine push, default-on-flip candidate.

## Files modified by this session (un-committed; left in tree as Track B prep)

```
GameOS/gameos/gos_static_prop_batcher.h    — registerType signature change (added TG_TypeMultiShape* parameter)
GameOS/gameos/gos_static_prop_batcher.cpp  — Path 4: per-pkt + per-type a_-prefix detection + REG_TYPE v2 schema + effectiveSource on ALPHA_TEST
docs/superpowers/specs/2026-05-06-static-prop-alpha-test-self-awareness.md — spec for the Path 4 work, status: Track B prep ONLY (does not fix visible symptom)
docs/superpowers/specs/2026-05-06-static-prop-texture-cache-handoff.md — THIS DOC
```

(Plus older instrumentation from earlier in the session: `mclib/msl.cpp`, `mclib/tgl.cpp`, `mclib/bdactor.cpp`, `mclib/txmmgr.cpp`, `code/terrobj.cpp`, `scripts/parse_light_debug.py` — all env-gated off-by-default per `debug_instrumentation_rule.md`.)

The Path 4 work IS valuable independently — it covers `treeDmgShape`, `treeDmgShadowShape`, `GenericAppearance` paths that load alpha-test assets without `SetAlphaTest`. When those recipes ARE rendered (e.g., destroyed-tree damage state), they would have rendered black without this fix. Track B's mission-load bulk register will inherit and benefit from this engine-side detection. Don't revert it unless you have a specific reason.

## Failed approaches (so you don't repeat)

Earlier sessions in this codebase attempted these for the same visible symptom; all failed:

| Attempt | Why it failed |
|---|---|
| Snapshot fix (move `lightData_` from leaf to `TG_MultiShape`) | Didn't fix visible bug AND introduced 0.5% → 10-20% baseline regression. Reverted. |
| Cull-aware skip in `registry::flush()` (commit `afcd75b`) | Solved a *different* bug (offscreen actor frame-stamp staleness). Real, useful, in tree. NOT this bug. |
| Path 4 alpha-test self-detection (this session, in tree as rev 3.3) | Solved a *different* bug (registration-time material classification gap). Real, useful, in tree. NOT this bug. |
| Per-leaf and per-type widening on Path 4 | Same as above; widening helped marginally, didn't fix visible symptom. |
| `MC2_FORCE_DYNAMIC_TREES=1 MC2_FORCE_DYNAMIC_BUILDINGS=1` as bypass | Doesn't actually bypass under `UPDATE_SKIP=1` — those env checks live in `render()` which the substitution gate skips. |

The pattern: each session converged on a fix that addressed a real but adjacent issue, then ran out of session-time before the user could communicate the pause/unpause clue (or the clue didn't propagate to the next session). **Capturing it in memory now (`pause_unpause_diagnostic_for_static_render_bugs.md`) breaks this cycle.**

## Adversarial-review trigger

This will close the last engine push for the MC2 OpenGL port and is a default-on-flip candidate. Per worktree CLAUDE.md "Review Discipline" + `memory/feedback_always_dispatch_adversarial_review.md`: dispatch adversarial-plan-review on any candidate fix BEFORE implementing. Don't wait to be asked.

The prompt should explicitly include: "use the adversarial-plan-review skill in `.claude/skills/`" verbatim.

---

## TL;DR for the next session

1. Read `memory/pause_unpause_diagnostic_for_static_render_bugs.md`.
2. Reproduce: `MC2_STATIC_UPDATE_SKIP=1` smoke. Run interactively. Confirm pause-fixes-it / unpause-breaks-it.
3. Investigate `mcTextureManager->update()` eviction policy at [`mission.cpp:509`](code/mission.cpp:509) and `actor.update() → SetTextureHandle` re-cache pathway.
4. Propose ONE fix candidate. Adversarial-plan-review. Implement. Verify visually. Done.

Don't chase alpha-test, leaf identity, or registration timing. Those are the sirens that pulled the prior 4 sessions onto the rocks.
