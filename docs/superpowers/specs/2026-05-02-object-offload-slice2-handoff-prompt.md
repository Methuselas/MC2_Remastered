# Slice 2 (object-offload) — Implementation hand-off prompt

> **Role for a fresh session reading this:** You are picking up the
> object-offload arc at slice 2. **Stages 2.A, 2.B, and 2.C are already
> complete and committed; do not re-implement them.** The late-reg
> allowlist is RESOLVED (commit `06ac847`). Stage 2.D is unblocked.
>
> **For Stage 2.D specifically, use the focused handoff at**
> `docs/superpowers/specs/2026-05-02-object-offload-slice2-stage2d-handoff-prompt.md`
> **— it's a self-contained fresh-session handoff.** This document
> remains the authoritative slice-2 status reference (commit history,
> Stage 2.D pre-conditions resolution, late-reg corrections, operational
> warnings).

---

## Current state (as of 2026-05-02)

**Stages 2.A, 2.B, 2.C are COMPLETE and GREEN behind `MC2_GPU_OBJECTS=1`.**
Do not re-implement them. Verify each commit is present before starting 2.D.

- **Stage 2.A commit:** `cdcdb7d` — substrate edits (positions-only
  variant + GatherGpuObjectLightDataOnly + eligibility helpers + per-actor
  needsFullBakeNextFrame).
- **Stage 2.B commit:** `bd1bd25` — eligibility hoist wired into
  `BldgAppearance/TreeAppearance/GenericAppearance::update` inside the
  existing cull gate; late-reg recovery flag wired in `*Appearance::render`
  (defensive — see late-reg correction below); fixed latent
  `TG_Shape::init()` access bug via friend declaration in `tgl.h`.
- **Stage 2.C commits (split into 2 for clean bisection):**
  - **2.C.1** `ad96c1f` — GLSL kernel + UBO schema lockstep:
    `ENABLE_VERTEX_LIGHTING=1`, `calc_light()` 4-param 6-type dispatch,
    `GetFalloff` GLSL helper, `TG_HWLightsData` extended with
    `lightFalloff[16][4]` byte-for-byte lockstep with `ObjectLights
    light_falloff[16]`. `submitMultiShape` hoists per-actor
    `GatherGpuObjectLightDataOnly()` between its two for-loops and
    broadcasts `lightDataIndex` into per-leaf `submit()`. Also fixes a
    second latent bug: `TG_Shape::init()` cleared `s_listOfLights = NULL`
    without clearing `s_numLights` — never fired pre-Stage-2.C, crashed
    mc2_10 at frame ~2400 once render-time `GatherLightsParameters` was
    added (see memory `tg_shape_static_state_lifecycle_trap.md`).
  - **2.C.2** `eb2a837` — flip the static_prop draw-side: per-vertex
    `aRGBLight` written at VBO offset 36 in `registerType`, per-type
    hot-color SSBO at slot 2 built in `finalizeGeometry`, `static_prop.vert`
    invokes `calc_light()` per vertex with `inst.lightDataIndex` and
    `worldPos`. Per-frame `.argb` memcpy retired from the main draw path
    (debug modes still see it via `v_argb`).
- **Slice 2 PR-ready checkpoint (2.A + 2.B + 2.C):** tier1 5/5 PASS in
  three configs (unset / `MC2_GPU_OBJECTS=1` / `+MC2_OBJBATCHER_TRACE=1`),
  +0 destroys delta on every mission. Visual restored under
  `MC2_GPU_OBJECTS=1`. Smoke-camera Tracy showed **~15.7%
  `appearanceUpdate` reduction**, **above the 10% surface-to-user floor
  (spec line 187) but below the 17% target (spec line 182)**. This is
  expected: the recon's 17-21% prediction was at a building-heavy camera
  with 759 actors/frame; smoke runs at default camera with ~4 actors
  visible. **Pinned-camera Tracy is required for apples-to-apples
  validation** and is part of Stage 2.E's harness.

### Stage 2.D pre-conditions — RESOLVED 2026-05-02

**Full campaign late-reg inventory found only two unique types, both
outside static-prop offload scope; no registration-walk fix required.**

Inventory was captured by direct-tracing all 24 campaign missions
(tier1 + tier2-only) under `MC2_GPU_OBJECTS=1 MC2_OBJBATCHER_TRACE=1`.
Every late-reg event reduces to one of two nodeIds:

| nodeId | caller | Mission count | Decision | Reason |
|---|---|---|---|---|
| `Cylinder01` | `skybox` | 24/24 | **allowlist** | Vestigial post terrain CPU→GPU migration; sky comes from terrain shader's post-process. Memory: `skybox_actor_vestigial_post_terrain_gpu.md` |
| `compassplane` | `compass` | 22/24 | **allowlist** | In-game compass HUD overlay; HUD element, not world geometry; loaded outside the static-prop registration walk by design |

Both are in `data/objbatcher_late_register_allowlist.txt` with explicit
reasoning. **Allowlist matching is by nodeId**, not pointer (pointers are
not stable across runs); the allowlist file header documents this.
`allowed=1` now appears for these types in
`[OBJBATCHER v1] event=late_register` log lines.

`allowed=1` is informational only — the actor still falls through to
legacy CPU `Render()`, which is the correct draw path. The flag exists
so an operator scanning logs sees "we knew this would happen" rather
than "real registration walk gap."

**Tier2 24/24 PASS in BOTH configs** (default and `MC2_GPU_OBJECTS=1`),
+0 destroys delta on every mission. Notable per-mission FPS improvements
under `MC2_GPU_OBJECTS=1` on building/tree-heavy missions
(mc2_24: 96→141, mc2_02: 112→138, mc2_03: 121→140).

**Stage 2.D parity is unblocked from the late-reg side.** No need for
the parity harness to special-case late-reg-CPU-fallback actors —
they are documented, expected, and outside the scope of slice 2's
GPU lighting comparison.

### Late-reg correction (committed; do NOT re-introduce skip-render)

An earlier draft of this hand-off implied a `lateRegSkip` short-circuit
that suppressed legacy `Render()` when `wasLastFailureLateRegistration()`
was true. **That was wrong** for stock missions: the two known unregistered
types NEVER get registered, so positions-only never runs for them; legacy
`Render()` is the only valid draw path and suppressing it makes them
permanently invisible.

**Correct shape (now in tree):** set `needsFullBakeNextFrame = true`
(defensive hygiene), then **fall through to legacy `Render()`**. The
eligibility hoist at update uses the same `s_typeIndex` check, so any
unregistered actor runs full-bake at update (fresh `.argb`) and legacy
`Render()` is correct. Confirmed in commit `bd1bd25`.

### Late-reg type identification (Stage 2.C+ instrumentation)

The `[OBJBATCHER v1] event=late_register` log line was extended to print
the TG_TypeShape's nodeId AND the owning actor's appearType->name (when
the caller provides it via `submitMultiShape`'s new `callerName`
parameter). Plus a one-shot `[GPUPROPS_REG] event=registered_dump` line
on the first late-reg event lists registered nodeIds for cross-reference.
Use this output to populate
`data/objbatcher_late_register_allowlist.txt` with intent or to track
down the registration walk gap.

**Known late-reg entry (allowlist, do NOT fix registration):**
`nodeId=Cylinder01 caller=skybox` is the skybox `GenericAppearance`,
vestigial post the terrain CPU→GPU migration. The actor still goes
through render every frame but its visible rendering is gone (sky now
comes from the terrain shader's post-process pipeline). See
`memory/skybox_actor_vestigial_post_terrain_gpu.md`.

### Operational warning carried forward

**Clean rebuild required after header/interface changes.** Stale
`.obj`/binary artifacts can mask access-control violations or
friend-declaration bugs. Both Stage 2.A's `eligibleForGpuObjects` C2248
issue (caught only by Stage 2.B's clean rebuild) and Stage 2.C.1's
latent `init()` bug (caught only when render-time gather was added)
were undetectable until the dependents recompiled from scratch.

- **Stage 2.A commit:** `cdcdb7d` on `claude/nifty-mendeleev` —
  *"feat: object-offload slice 2 stage 2.A — substrate edits (no behavior change)"*.
  Confirm with `git log --oneline -5` from the worktree.
- **Stage 2.A green gate (recorded 2026-05-02):**
  - tier1 5/5 PASS in two configs: unset / `MC2_GPU_OBJECTS=1`.
  - +0 destroys delta on every mission in both configs.
  - mc2_24 isolation PASS @ 142 FPS post-revert (the regression below).
- **Stage 2.A regression resolved:** initial implementation extended
  `TG_HWLightsData` with a `lightFalloff[16][4]` field on the C++ side
  WITHOUT the lockstep GLSL `ObjectLights` extension; the legacy
  `addRenderShape` path uploads to the existing-size UBO so per-element
  stride for `light[i]` with `i>0` was corrupted, crashing mc2_24
  silently ~17s in. The C++ field was reverted; falloff fields are
  now scoped to **Stage 2.C only** where C++/GLSL/`calc_light()` ship
  in lockstep. This is captured as Sign-Off #6 in the design spec and
  as the load-bearing scope rule in this handoff at "Step 1".
- **Step 0 adversarial review log:** initial review found 12 CRITICAL +
  8 MAJOR + 3 MINOR; correction pass + fresh-subagent delta review
  reduced to 0 CRITICAL / 0 MAJOR / 6 MINOR (one disambiguation
  applied: M6 — `submitMultiShape` has TWO loops with the same
  `for (int i = 0; i < n; ++i)` signature; the per-actor light gather
  goes BETWEEN them, not inside either). Spec sign-off log items #1-5
  are locked architectural decisions; do not re-litigate.

**Start at Stage 2.B.** Do not touch Stage 2.A substrate except to
verify the commit is present and the smoke gate is still green
on your local checkout. If `git log --oneline | grep "stage 2.A"`
returns nothing, escalate to user before doing anything — the
substrate may have been reverted or the worktree may have drifted.

---

## Worktree + branch

- **Worktree:** `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`
- **Branch:** `claude/nifty-mendeleev` (continuing the long-running nifty branch; slice 2 builds on slice 1's substrate)
- **Slice 2 design tip:** the design doc commit + this hand-off prompt commit (look for "object-offload slice 2 spec + hand-off" in `git log`).
- **Deploy target:** `A:/Games/mc2-opengl/mc2-win64-v0.3/`

## REQUIRED READS (in order — non-skippable)

1. **Slice 2 design spec:** `docs/superpowers/specs/2026-05-02-object-offload-slice2-design.md` — full architecture, stages 2.A through 2.E, gate ladders. **This is your primary work plan.** Each stage names files, edit shapes, and gates.

2. **Recon Zero:** `docs/superpowers/explorations/2026-05-02-object-offload-slice2-recon-zero.md` — especially Section 9 (pre-spec hardening resolved). Sections 1, 3, 4, 5, 7 contain the architectural reasoning the spec assumes.

3. **Slice 1 design:** `docs/superpowers/specs/2026-05-02-object-offload-slice1-design.md` — the substrate slice 2 builds on. Pay attention to: R1 mutual exclusion (line 504-531), Layer-B per-child eligibility (line 135-167), late-registration accounting (line 346-359).

4. **Worktree CLAUDE.md** — `.claude/worktrees/nifty-mendeleev/CLAUDE.md`. Load-bearing project rules:
   - Documentation Discipline (grep at write-time)
   - Review Discipline (this slice qualifies as architectural-endpoint-class; FULL adversarial-plan-review skill before plan write — see Step 0 below)
   - Load-Bearing Cull Infrastructure (still applies — slice 2 does not bypass cull)
   - Critical Rules (build / deploy / shader version)
   - Tier-1 Instrumentation Env Vars (MC2_TGL_POOL_TRACE, MC2_DESTROY_TRACE, etc.)
   - Smoke Gate command

5. **Memory files (load-bearing — read before touching related code):**
   - `memory/cull_gates_are_load_bearing.md` ⭐
   - `memory/tgl_pool_exhaustion_is_silent.md` ⭐
   - `memory/mc2_texture_handle_is_live.md`
   - `memory/static_prop_projection.md`
   - `memory/gpu_direct_renderer_bringup_checklist.md`
   - `memory/render_order_post_renderlists_hook.md`
   - `memory/feedback_offload_scope_stock_only.md`
   - `memory/feedback_smoke_no_canary.md` (do NOT run menu canary)
   - `memory/feedback_pool_peak_compare_same_mission.md` (Gate E methodology)
   - `memory/feedback_subagent_no_cmake_configure.md` (build env safety — never `cmake -B build64`, only `cmake --build build64`)
   - `memory/deferred_vs_direct_uniforms.md`
   - `memory/blend_state_inheritance_in_post_process.md`
   - `memory/gpu_direct_depth_state_inheritance.md`

6. **Skill:** `.claude/skills/adversarial-plan-review.md` — required for Step 0. Also `.claude/skills/mc2-build.md`, `mc2-deploy.md`, `mc2-build-deploy.md`, `mc2-check.md` for the build/deploy cycle.

---

## Step 0 — Adversarial review of the design spec (MANDATORY before code edits)

Per worktree CLAUDE.md "Review Discipline" line 24-39, slice 2 is architectural-endpoint-class. Before writing any code:

1. Invoke `.claude/skills/adversarial-plan-review.md` against `docs/superpowers/specs/2026-05-02-object-offload-slice2-design.md`.
2. Grep every cited symbol at write-time. The spec already cites file:line for most claims; verify them live.
3. List CRITICAL / MAJOR / MINOR findings.
4. Surface CRITICAL findings to user before proceeding.

If review uncovers a blocker, surface to user. Do NOT silently rework the spec.

---

## Step 1 — Stage 2.A: substrate edits (no behavior change)

Per spec section "Stage 2.A — Substrate edits (no behavior change under `MC2_GPU_OBJECTS=0`)":

**LOAD-BEARING SCOPE RULE (regression-discovered 2026-05-02):** Stage 2.A must NOT change GPU-visible buffer layouts. The legacy `addRenderShape` path is active in stock and uploads `TG_HWLightsData` to the `LightsData` UBO declared as `ObjectLights light[32]` in `lighting.hglsl:25-28`. Extending the C++ struct without the lockstep GLSL change breaks per-element stride for `light[i]` with `i>0` and corrupts the legacy shader path (mc2_24 specifically crashes ~17s in due to its airbase's many distinct light setups). **The `closeDistance`/`farDistance`/`oneOverDistance` fields belong to Stage 2.C, where C++ struct + GLSL `ObjectLights` + `calc_light()` 4-param rewrite ship in one commit.** The same rule applies to `GpuStaticPropInstance` — the `_pad0`→`lightDataIndex` rename is allowed because it is name-only at offset 76; size and layout do not change.

Files to modify (Stage 2.A scope ONLY):
- `mclib/tgl.h` — declare `MultiTransformShape_PositionsOnly`, `GatherGpuObjectLightDataOnly`. **DO NOT extend `TG_HWLightsData`** (deferred to Stage 2.C per the scope rule above).
- `mclib/tgl.cpp` — define both functions. `_PositionsOnly` is a copy-and-strip of `MultiTransformShape` per spec architecture section. Add `eligibleForGpuObjects(TG_Shape*)` helper. Add `!eligibleForGpuObjects(this)` to the gate at line 2522.
- `mclib/txmmgr.cpp` — **NO Stage 2.A changes here** (the `GatherLightsParameters` falloff-field writes are Stage 2.C work, deferred).
- `GameOS/gameos/gos_static_prop_batcher.h` — declare `isMultiShapeEligibleForGpuObjects`. Repurpose `_pad0` slot in `GpuStaticPropInstance` as `lightDataIndex` (offset 76; same size, layout unchanged — rename is safe). Update `static_assert` for offsets.
- `GameOS/gameos/gos_static_prop_batcher.cpp` — define `isMultiShapeEligibleForGpuObjects` (mirror slice 1's render-time per-child gates except late-registration, per Recon Section 9 Item 4).
- `mclib/bdactor.h`, `mclib/bdactor.cpp` — add a NEW `bool needsFullBakeNextFrame;` member on `BldgAppearance` and `TreeAppearance`. Initialize false. **DO NOT** name the field `appearanceFlags_needsFullBakeNextFrame` and **DO NOT** "pack into existing `appearanceFlags`" — those classes have NO `appearanceFlags` aggregate byte (grep returns zero hits in `mclib/`). They use individual `bool` members like `isReversed`, `forceLightsOut`, `beenInView`, etc. Add a new `bool` next to those.
- `mclib/genactor.h`, `mclib/genactor.cpp` — same NEW `bool needsFullBakeNextFrame;` on `GenericAppearance`. Same anti-pattern note: no `appearanceFlags` aggregate exists.

**No call sites are switched.** This stage adds infrastructure; existing code paths unchanged.

**Status: COMPLETE on commit `cdcdb7d`.** A fresh worker reading this handoff should NOT re-execute Step 1; instead, verify the commit is present (`git log --oneline | grep "stage 2.A"`) and proceed to Step 2 (Stage 2.B). If the commit is missing, escalate to user — do not silently re-implement.

**For historical reference, the completed Stage 2.A green gate was:**
- tier1 5/5 PASS in two configs (unset, `MC2_GPU_OBJECTS=1`).
- +0 destroys delta in every mission in both configs.
- TGL pool peak unchanged.
- Tracy `appearanceUpdate` zone unchanged.

---

## Step 2 — Stage 2.B: wire eligibility hoist + positions-only

Per spec section "Stage 2.B" — also see the locked Sign-Off #3 in the design spec ("Eligibility hoist lives INSIDE the existing cull gate"). The eligibility branch must NOT lift the call out of the existing `if (inView || g_useGpuStaticProps)` gates; doing so risks regressing slice 1's R1 cull invariant.

**Line-drift caveat (added post-Stage-2.A 2026-05-02):** the line numbers below were the spec/handoff's original grep at write-time. Since then commit `c4c4e96` (recon Tracy instrumentation) and commit `cdcdb7d` (Stage 2.A substrate) have shifted `tgl.cpp` / `bdactor.cpp` / `genactor.cpp` cited lines by 3-9. **Locate every edit site by structure, not literal line:** find the existing `if (inView || g_useGpuStaticProps)` cull gate, then the `*Shape->TransformMultiShape (&xlatPosition,&rot)` call inside it. Same rule for the Stage 2.C `submitMultiShape` between-the-two-`for`-loops anchor. The site numbers below are advisory; the structural anchors are authoritative.

Files (line references advisory — re-derive structurally per caveat above):
- `mclib/bdactor.cpp` `BldgAppearance::update` (function start at line 1957; existing `bldgShape->TransformMultiShape` call at line 2200, INSIDE `if (inView || g_useGpuStaticProps)` opening at line 2191) and `TreeAppearance::update` (function start at line **4213**, NOT 4209; existing `treeShape->TransformMultiShape` call at line 4313, INSIDE `if (inView || g_useGpuStaticProps)` opening at line 4300) — wrap the existing call in the eligibility branch from the spec's "Eligibility hoist" architecture section. **The branch lives INSIDE the existing cull gate.** The shadow-shape companion calls (`bldgShadowShape->TransformMultiShape` at `bdactor.cpp:2206`, `treeShadowShape->TransformMultiShape` at `bdactor.cpp:4321`) are NOT touched by Stage 2.B.
- `mclib/genactor.cpp` `GenericAppearance::update` (function start at line 1049; existing `genShape->TransformMultiShape` call at line 1189, INSIDE `if (inView || g_useGpuStaticProps)` opening at line 1185) — same pattern: branch INSIDE the cull gate.
- `mclib/bdactor.cpp` / `mclib/genactor.cpp` post-`submitMultiShape` consumer wiring: when the slice 1 `submitMultiShape` path is exercised in `*Appearance::render` and returns false, query `GpuStaticPropBatcher::instance().wasLastFailureLateRegistration()` (already added in Stage 2.A, see commit `cdcdb7d`). If true, set the actor's NEW `bool needsFullBakeNextFrame = true;` field (also from Stage 2.A). The flag is consumed by the eligibility branch above on the NEXT frame's update — full `TransformMultiShape` runs, flag is cleared.

  **Late-reg behavior (corrected 2026-05-02 post-Stage-2.B trace):** **DO NOT skip rendering for late-reg actors.** An earlier draft of this hand-off implied a `lateRegSkip` short-circuit that suppressed the legacy `Render()` fallback when `wasLastFailureLateRegistration()` was true; that is wrong for stock missions because the two known unregistered types (slice 1 spec lines 489-490) NEVER get registered, so positions-only never runs for them and the only valid draw path is legacy CPU `Render()`. Suppressing it makes the actor permanently invisible.

  Correct shape: set `needsFullBakeNextFrame = true` (defensive hygiene; cheap), then **fall through to legacy `Render()`**. The eligibility hoist at update uses the same `s_typeIndex` check as `submitMultiShape`'s first loop, so any actor whose type is unregistered runs full-bake at update (fresh `.argb`) and legacy `Render()` is correct. The high `late_register_recovery_skips` count this produces is a slice-1 allowlist/registration cleanup issue, not a Stage 2.B blocker — see "Stage 2.B gate" below.
- `GameOS/gameos/gos_static_prop_batcher.cpp` `submitMultiShape` — the late-registration setter and `late_register_recovery_skips` counter were already wired in Stage 2.A (commit `cdcdb7d`) at the unregistered-type branch (lines `674-693`, the full `if (s_typeIndex.find(ts) == s_typeIndex.end())` block; the inner `if (count == 0)` print runs ~683-690 and the `++count; return false;` tail is at ~691-692). **DO NOT** re-implement; verify via grep that `s_lastSubmitWasLateReg` and `s_late_register_recovery_skips` are present.

**Field naming reminder:** the actor flag is `bool needsFullBakeNextFrame;` — NOT `appearanceFlags_needsFullBakeNextFrame`. There is no `appearanceFlags` aggregate byte on these classes; do not pack into one.

**Visual behavior at this stage**: with `MC2_GPU_OBJECTS=1`, eligible static-prop actors run positions-only. Their `.argb` is stale or zero. Slice 1's batcher continues to memcpy `listOfVertices[j].argb` into the per-instance color SSBO and draw with stale colors. **This is intentional** — the kernel split is verified before the GPU lighting kernel comes online in Stage 2.C. PR description must call this out.

**Stage 2.B gate (intentionally narrow — do NOT evaluate visual parity here):**

- No crash / no hang in tier1 5/5 PASS in three configs (unset / `MC2_GPU_OBJECTS=1` / `MC2_GPU_OBJECTS=1 + MC2_OBJBATCHER_TRACE=1`).
- +0 destroys delta in every mission.
- TGL pool peak unchanged.
- No cull/lifecycle regression.
- F-gate counters: `gpu_drawn_instances > 0` per registered static-prop population. `cpu_fallback_rate < 5%` (slice-1 Gate F threshold).
- `late_register_recovery_skips`: monitor; **NOT a Stage 2.B blocker even when high.** The spec's original "≤2 per mission" assumed transient late-reg recovery; in stock the two known artillery/bomber types per slice 1 spec lines 489-490 are permanently unregistered and produce ~5500 events per mission. That's a slice-1 allowlist/registration cleanup follow-up — it does NOT block Stage 2.B (the actors render correctly via legacy CPU fallback with fresh `.argb`).
- **Carry-forward concern (do not act on at 2.B, monitor at 2.C):** post-Stage-2.B `cpu_fallback_rate` was 4.97% (just under the 5% gate). **Do not tighten the gate.** Before Stage 2.D parity validation or any default-on flip, either add the two known late-reg types to `data/objbatcher_late_register_allowlist.txt` with explicit reasoning, or fix their registration site so `finalizeGeometry`/`onMapLoad` picks them up. For 2.C, the 4.97% rate is acceptable but should be re-checked on every config.
- Render zone Tracy delta neutral. Do NOT gate on Tracy magnitude or on visual quality at 2.B; both come at Stage 2.C.

**Anti-pattern to avoid**: do not "fix" the temporary visual break by undoing positions-only or re-introducing color writes. Colors come back at Stage 2.C when GPU lighting goes live.

**Operational warning carried forward to all later stages:** **clean rebuild required after header/interface changes.** Stale `.obj` / binary artifacts can mask access-control violations or friend-declaration bugs. Stage 2.A's free function `eligibleForGpuObjects(TG_Shape*)` shipped without the friend declaration it needed to read the protected `myType`; the existing `mc2.exe` predated the commit, so the pre-commit smoke gate ran against a binary that didn't include the new code. Stage 2.B's clean build (forced by the `msl.h` header touch) surfaced the C2248 error, fixed via a one-line `friend bool eligibleForGpuObjects(class TG_Shape*);` add to `tgl.h`. **If a stage edits a header that other translation units include, verify the rebuild compiled the dependents from scratch — don't trust a "no-op no-rebuild" link.**

Commit on green.

---

## Step 3 — Stage 2.C: GPU vertex lighting (the meat)

Per spec section "Stage 2.C". This is the biggest stage. Consider splitting into 2.C.1 (shader work) + 2.C.2 (batcher wiring) for cleaner bisection.

Files:
- `shaders/include/lighting.hglsl` — set `ENABLE_VERTEX_LIGHTING 1` (line 3). Finish `calc_light()` (lines 119-137) with full 6-type dispatch: AMBIENT, INFINITE, INFINITEWITHFALLOFF, POINT, SPOT, TERRAIN. Add `GetFalloff` GLSL helper (linear interp per Recon Section 9 Item 1).
- `shaders/static_prop.vert` (or new `static_prop_lit.vert`) — invoke `calc_light()` per vertex with per-instance `lightDataIndex` and per-vertex `aRGBLight` tag.
- `shaders/static_prop.frag` — consume VS-produced lit ARGB.
- `GameOS/gameos/gos_static_prop_batcher.cpp` `submitMultiShape` — call `multi->listOfShapes[0].node->GatherGpuObjectLightDataOnly()` **ONCE per multishape, AFTER the registration/eligibility pass and BEFORE the child submit loop. Do NOT place it inside either `for (int i = 0; i < n; ++i)` loop.** Concrete placement: the function spans ~641-737. There are TWO loops with the identical signature — the first at line 667 is the registration-check / early-out loop; the second at line 698 is the per-leaf submit loop. The gather call sits BETWEEN them, between approximately line 694 (close of loop-1) and line 696 (open of loop-2). The returned `lightDataIndex` is broadcast into each leaf's per-instance struct INSIDE the second loop's body. (Per-actor not per-leaf — Recon Section 9 Item 5 confirmed all leaves see identical `lightData_`. Placing the call inside loop-1 wastes work on rejected actors; placing it inside loop-2 makes it per-leaf and incurs N-fold redundant `GatherLightsParameters` calls per multi-shape per frame. The duplicate-loop signature ambiguity was caught in Step 0 delta review M6.)
- `GameOS/gameos/gos_static_prop_batcher.cpp` — stop memcpying `listOfVertices[j].argb` into the per-instance color SSBO (now redundant; GPU lights it). **Note in Stage 2.C PR description**: this retires slice 1's color-stream memcpy. The slice 1 substrate code path becomes "memcpy-less" from slice 2 onwards. Does NOT affect slice 1's CPU-only path (which doesn't go through the batcher).
- `GameOS/gameos/gos_static_prop_batcher.cpp` `registerType` (line 444-468) — write per-vertex `aRGBLight` at offset 36 (currently zero-padded). Source: `typeShape->listOfTypeVertices[localVertIdx].aRGBLight`.
- `GameOS/gameos/gos_static_prop_batcher.cpp` `finalizeGeometry` — build per-type SSBO with hot-color fields (`hotPinkRGB`, `hotYellowRGB`, `hotGreenRGB`); bind at draw time.

**Spec invariant** (per spec R5): a binary that contains slice 2 code unconditionally writes `aRGBLight` at registration time. Slice 1 binaries with the old VBO layout are not interop-compatible.

**Build, deploy, smoke** per Step 1. Test with `MC2_GPU_OBJECTS=1`.

**Pass criteria**:
- tier1 5/5 PASS in three configs.
- Visual canary at fixed camera (`mc2_01` airbase region recommended): no visible regression.
- `appearanceUpdate` Tracy zone shows ≥17% reduction with `MC2_GPU_OBJECTS=1` (per spec target). Use the `MC2_OBJECT_RECON_TRACY=1` instrumentation from commit `c4c4e96` to verify the slice-2-scoped per-population reduction.
- Render zone Tracy delta: no regression.
- +0 destroys.

Commit on green.

If Tracy reduction is below 10%, surface to user — the recon's perf prediction was wrong and the slice 2 framing needs reconsideration before merge.

---

## Step 4 — Stage 2.D: parity instrumentation

Per spec section "Stage 2.D":

**Merge policy (decided in spec; do not re-litigate)**: Stages 2.A-2.C may merge behind `MC2_GPU_OBJECTS=1` flag once their respective gates pass. **Stage 2.D parity is NOT a slice 2 PR blocker.** It IS a hard pre-condition for declaring slice 2 "validated" or for any default-on flip. If 2.D's tooling effort threatens to stall 2.C's perf merge, ship 2.A-2.C first and follow up with 2.D in a separate PR. Stage 2.E's pinned-camera diff is similarly pre-default-on, not pre-merge.

**Scope warning**: Stage 2.D requires a GPU→CPU readback harness via PBO that doesn't exist in tree today. Budget accordingly — this is non-trivial: allocate PBO, dispatch async readback after the slice 1 batcher's draw, retain the readback for next-frame compare against fresh CPU recompute. Existing terrain/water arcs may have a similar pattern to crib from; check `GameOS/gameos/gos_terrain_indirect.cpp` and `gos_static_prop_batcher.cpp` for any existing PBO usage. If none exists, Stage 2.D's PBO harness is itself a sub-stage worth ~half the stage's effort.

**Compare-target caveat**: parity compares at triangle-corner granularity (`listOfTriangles[].aRGBLight[i]`) even though GPU output is per-vertex-lit. This is intentional. Because `useFaceLighting=false` in stock, the corner value equals the per-vertex-lit value modulo alpha/packing. Any mismatch in 2.D indicates packing / fog / highlight / terrain-light / shader-math divergence, NOT missing per-face lighting. See spec Stage 2.D for the full caveat.

Files:
- `GameOS/gameos/gos_static_prop_batcher.cpp` (or a new sidecar `gos_object_parity.cpp`) — implement P3 dual-emit at first frame post-mission-start: run BOTH `MultiTransformShape` and `_PositionsOnly` for all actors, bytewise-compare CPU `listOfTriangles[j].aRGBLight[i]` against GPU output (read back via PBO). Mismatch logs `[OBJECT_PARITY v1] event=lighting_mismatch actor=X tri=Y corner=Z cpu=ARGB gpu=ARGB`. ULP tolerance ±2 LSB per channel.
- P1 sampled bytewise in steady state: 1 actor per type per frame, round-robin. Compare GPU output (1-frame-stale OK via async PBO) against fresh CPU recompute.
- 600-frame summary: counts of compared/passed/mismatched.

**Pass criteria**: zero mismatches across tier1 stock with `MC2_OBJECT_PARITY_CHECK=1`.

If mismatches are nonzero but bounded (< 0.1% of compared corners) AND visible only at extreme corner cases (lighting transitions, etc.), surface to user with examples; spec may need to widen ULP tolerance or accept GPU as new ground truth.

Commit on green.

---

## Step 5 — Stage 2.E (separate PR): pinned-camera screenshot diff

Per spec section "Stage 2.E". This stage is a separate PR that gates the default-on flip. NOT a merge blocker for slice 2 PR itself.

Files: `tests/smoke/object_visual_diff.py` per spec.

Slice 1 may have a Stage 1.E in flight. If yes, slice 2 reuses it. If no, slice 2 builds it.

Default-on flip cannot happen until this PR clears. Slice 2 ships behind `MC2_GPU_OBJECTS=1` (default off) until then.

---

## Project constraints (load-bearing — re-confirm at every stage)

- **Stock missions only** for validation per `memory/feedback_offload_scope_stock_only.md`. Do NOT validate against Carver5O / Magic / MCO / Wolfman / MC2X.
- **Never run the menu canary** per `memory/feedback_smoke_no_canary.md`. Drop `--menu-canary` / `--with-menu-canary` from smoke commands.
- **Build**: `cmake --build build64 --config RelWithDebInfo --target mc2` ONLY. Never `cmake -B build64 -S .` or any configure variant — clobbers SDL2/GLEW prefix paths per `memory/feedback_subagent_no_cmake_configure.md`.
- **Deploy**: `A:/Games/mc2-opengl/mc2-win64-v0.3/`. Per-file `cp -f` + `diff -q`. NEVER `cp -r`.
- **Pool peak comparisons**: same-mission baseline-vs-test only per `memory/feedback_pool_peak_compare_same_mission.md`. Cross-mission comparisons produce false alarms; trust +0 destroys delta as the primary Gate-E proxy.
- **Git**: never push. HEREDOC for commit messages. NEVER amend.
- **Tracy zones with MTPC < 1µs**: don't add them; the recon's per-vertex/per-face zones are intentionally NOT split per-iteration. Use the MC2_OBJECT_RECON_TRACY accumulators from commit `c4c4e96` for finer measurements.
- **Shader #version**: never in shader files. Pass `"#version 430\n"` as prefix to `makeProgram()` (matches the 4.3 context required for SSBO / std430).
- **Uniform API**: `setFloat`/`setInt` BEFORE `apply()`, not after. `apply()` flushes dirty uniforms.
- **`GL_FALSE` for terrainMVP**: direct-uploaded row-major matrices use `GL_FALSE`. Material cache uses `GL_TRUE`. Per-instance `modelMatrix` in slice 1's SSBO is `v*M`, terrain-style `worldToClip` uniform uploaded with `GL_TRUE`.
- **Shader hot-reload fails silently**: bad compile = old shader stays active. Check console for errors after every shader edit.

---

## Useful commands

```bash
# Build (run in worktree directory)
cd A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2

# Deploy
cp -f build64/RelWithDebInfo/mc2.exe   A:/Games/mc2-opengl/mc2-win64-v0.3/mc2.exe
cp -f build64/RelWithDebInfo/mc2.pdb   A:/Games/mc2-opengl/mc2-win64-v0.3/mc2.pdb
diff -q build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-v0.3/mc2.exe

# Per-file shader deploy (when shader files change)
cp -f shaders/static_prop.vert         A:/Games/mc2-opengl/mc2-win64-v0.3/data/shaders/static_prop.vert
diff -q shaders/static_prop.vert       A:/Games/mc2-opengl/mc2-win64-v0.3/data/shaders/static_prop.vert
# (Confirm actual deploy shader paths via /mc2-deploy skill or by reading existing deploy state.)

# Smoke (tier1, NO menu canary, fast iteration)
MC2_GPU_OBJECTS=1 py -3 scripts/run_smoke.py --tier tier1 --duration 20 --fail-fast --kill-existing

# Recon Tracy (verify per-stage perf)
MC2_OBJECT_RECON_TRACY=1 MC2_GPU_OBJECTS=1 py -3 scripts/run_smoke.py --tier tier1 --duration 20 --fail-fast --kill-existing

# Parity check (Stage 2.D onward)
MC2_OBJECT_PARITY_CHECK=1 MC2_GPU_OBJECTS=1 py -3 scripts/run_smoke.py --tier tier1 --duration 20 --fail-fast --kill-existing
```

---

## When you finish

**Merge gate (slice 2 PR)** — Stages 2.A-2.C complete:

1. Run final tier1 smoke in all three configs. Capture artifact.
2. Run `MC2_OBJECT_RECON_TRACY=1` smoke and capture the `[OBJECT_RECON v1] summary` line. Confirm the per-population reduction matches the spec target (~17-21% on `appearanceUpdate`).
3. Surface to user: "slice 2 PR ready (Stages 2.A-2.C). Slice 2 validation pending Stage 2.D parity. Default-on flip blocked on Stage 1.E / 2.E pinned-camera diff."

**Validation gate** — Stage 2.D parity complete (separate PR):

4. Run `MC2_OBJECT_PARITY_CHECK=1` smoke. Confirm zero mismatches.
5. Update memory:
   - `memory/object_update_cost_baseline.md` — capture post-slice-2 Tracy numbers.
   - `memory/enum_mismatch_was_fabricated_claim.md` — note the recon-zero error so future arcs don't re-investigate.
6. Surface to user: "slice 2 validated. Default-on flip blocked only on Stage 1.E / 2.E pinned-camera diff."

**Default-on gate** — Stage 1.E / 2.E pinned-camera diff complete (separate PR):

7. Pixel-diff harness clears. Default-on flip lands as a one-line edit (env-var default).

If a stage gate fails:
- Do NOT push past the failure with hacks. Fix the root cause or surface the failure to user with the captured evidence.
- Pool exhaustion or destroys delta != 0: revert and investigate per `memory/cull_gates_are_load_bearing.md` and `memory/tgl_pool_exhaustion_is_silent.md`.
- Tracy delta < 10% at Stage 2.C: surface to user; spec's perf claim was wrong.
- Visual regression at Stage 2.C: surface to user with screenshots; do not merge slice 2 PR.
- Parity mismatches at Stage 2.D: surface to user; investigate (probably packing / fog / shader-math). Slice 2 PR may already be merged at this point — fix-forward in 2.D PR rather than reverting.

---

## Out of arc (do not pursue without separate brainstorm)

- GPU shadow port (Task 13-14 from prior batcher) — separate slice or arc.
- Mover offload (Mech3D / GV) — separate arc per brainstorm Q4.
- General lighting refactor (per-face A/B options from Recon R-arch-1) — not viable on stock; separate arc only if mod-stable contract requires.
- Removal of legacy `g_useGpuStaticProps` and the 5 cull-bypass sites — post-arc cleanup; mechanical follow-up after default-on flip soaks.

---

## Hand-off

The slice 2 design spec is your work plan. The recon doc is your reasoning trail. The slice 1 spec is the substrate you build on. The memory files are the load-bearing constraints. Worktree CLAUDE.md is the project rules. Skills are the tooling.

Execute the 5 stages in order. Surface real blockers to user; never paper over them with hacks or skipped tests.

**Slice 2 PR ships when Stages 2.A–2.C pass their gates** (Stage 2.A is already complete on commit `cdcdb7d`; Stages 2.B and 2.C are this session's work). **Slice 2 is validated when Stage 2.D parity passes** (separate PR; not a 2.C-PR blocker). **Default-on flip happens only when Stage 2.E (or slice 1's Stage 1.E, whichever comes first) — pinned-camera screenshot diff — clears.** This sequence is the merge-policy decision locked at design time; do not re-litigate.
