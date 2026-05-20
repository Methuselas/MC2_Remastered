# Per-Object Cull GPU-Side — Recon

- **Status:** RECON — verdict reached, no plan written
- **Date:** 2026-05-20
- **Worktree:** `claude/nifty-mendeleev`
- **Verdict:** `PATCH (justified)` — DEFER. This slice is Stage 0.5 §4 by another framing; treat as already-investigated-and-blocked. Two trivial preparatory moves are no-risk and unlock a future attempt.
- **All file:line citations grep-verified at write time against `nifty-mendeleev`.**

---

## 1. The finding that prompted this recon

F3 tier1 baseline (stationary, automated): `eventdriven_projection_total = 950 calls/frame p50`. Caller speculation initially was "AI LOS or per-frame selection/targeting." That was wrong.

Grep-verified caller chain:

- Single physical `projectForObjectAdmission` site: [code/gameobj.cpp:2090](code/gameobj.cpp) inside `GameObject::onScreen()` (lines `2076-2099`).
- `GameObject::onScreen()` callers in the runtime: [code/light.cpp:123](code/light.cpp) (`Light::update` — per-frame, unconditional) and [code/light.cpp:139](code/light.cpp) (`Light::render` — `gamePaused`-only). NO other live caller.
- The "12 sibling subclass sites" (`mech.cpp:6045/6111/6183`, `gvehicl.cpp:3273/3324/3821`, `bldng.cpp:383/807/1126`, `gate.cpp:241/357/637`, `turret.cpp:581/812/2085`, `artlry.cpp:872/1409`, `terrobj.cpp:601/814`, `weaponbolt.cpp:373`) are direct `windowsVisible = turn;` writes — they reach the stamp via `appearance->recalcBounds()` (gvehicl:3267, mech:6043) or other paths, NOT via additional `projectForObjectAdmission` calls.

**Conclusion:** the 950/frame are dominated by `Light::update` invocations, with one `projectForObjectAdmission` per light per frame. Lights, not mechs, are the population. Per [memory/cull_gates_are_load_bearing.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\cull_gates_are_load_bearing.md) the `windowsVisible == turn` stamp is the load-bearing gate read by `light.cpp:141`, `mech3d.cpp:4256-4262`, `gvactor.cpp:2773-2779`, and the `canBeSeen` chain. Migrating the producer means migrating or repointing every consumer.

## 2. Cost — we still don't know

`SIDECAR_EVENTDRIVEN_PROJECT_Z` at [mclib/cpu_proj_cost_split.h:43](mclib/cpu_proj_cost_split.h) is **count-only**. [cpu_proj_cost_split.cpp:336-339](mclib/cpu_proj_cost_split.cpp):

```
void add_workload_eventdriven_projectZ() {
    if (!g_cpuProjEnabled) return;
    s_curFrameSidecar[SIDECAR_EVENTDRIVEN_PROJECT_Z].workload += 1;
}
```

No `Scope` / RAII timer wraps the call. Rough estimate per [mc2-render-perf-expert advisor]: one `projectForObjectAdmission` is one 4x4 mat-vec multiply + `LegacyProjectionResult` writeback + screen-rect bool + a few isfinite checks under `MC2_PROJECTZ_FINITE_CHECK` ≈ **30-80 ns**. 950 × 50 ns ≈ **~48 µs/frame**, conservative band 30-95 µs.

Against the F3 100 µs CPU-projection budget: at the upper bound, the entire budget. At midpoint, half of it. Against `mlr_total p95 ~407 µs`: 12% at worst. Against a 9.5 ms wolfman frame: ~0.5%.

**Worth-it threshold:** an isolated 30-95 µs slice is below the "ship on its own merit" bar (smoke variance hides 50-100 µs) UNLESS it falls out of a coarser retirement. It is too small to take architectural risk for, but large enough to track as a piggyback on the existing object/prop iteration GPU port.

**Critical caveat:** the F3 capture is stationary, non-combat. Lights from weapon trails / muzzle flashes / explosions are dormant. Combat may multiply this count. Worst-case mc2_10 with motion+combat is unmeasured. **Promoting the sidecar to a timed `BucketId` is the no-risk first move** — it answers this without architectural commitment.

## 3. The slice IS Stage 0.5 §4 by another name

The candidate "Per-object cull GPU-side investigation — sibling to gpu_cull_compute.cpp/substrate-coalesce" framing obscures the inheritance:

- `gpu_cull.comp:239` already writes `rb_actorVisible[i] = visible ? 1u : 0u;` per actor. The GPU-side per-object cull producer **already exists**.
- `gpu_cull_readback.cpp` already feeds `readback_isActorVisibleLagged(handle)` to consumers.
- [memory/stage_0_5_section_4_blocked_on_readback_non_superset.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\stage_0_5_section_4_blocked_on_readback_non_superset.md) documents the 2026-05-20 EVENING attempt to gate render on this readback. **Result: empirical NO-GO.** Tentative-ship commit `40a54b7` reverted as `dc2e8f6`. Two bug classes emerged:
  1. **Readback non-superset.** mc2_10 worst-case canary: sustained violations 2.814% (gate < 0.5%), worst 120-frame window 30.308% (gate < 2%), 30 of 62 windows >= 2%. 5.6× to 15× over budget. Visually: static-prop / tree / mech POPPING on camera motion.
  2. **Resurrected black-tree bug class** (`memory/black_tree_bug_investigation_state.md`). When `inView=FALSE, readback=TRUE` (4th truth-table row), update is cull-skipped but render fires — `cachedGpuLightIndex_` is stale → black textures + z-fight + LOD-blank.

`light.cpp:141` reads `windowsVisible == turn`. Repointing it to `readback_isActorVisibleLagged` is the same migration §4 attempted on `code/terrobj.cpp` static props. The lights-vs-statics population is different, but the producer is the same, the consumer-coupling shape is the same, and **the two bug classes apply directly**.

Building a new sibling producer (a fourth parallel visibility path on top of coarse `inView`, sticky `blockVisBits[]`, readback `rb_actorVisible[]`) would be the additive anti-pattern [memory/feedback_offload_must_be_substitutive_not_additive.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\feedback_offload_must_be_substitutive_not_additive.md) warns against — no path to delete the CPU stamps.

## 4. Greybeard ruling

1. **Subsystem pin.** Producer: `GameObject::onScreen()` at [gameobj.cpp:2090](code/gameobj.cpp) calling `projectForObjectAdmission`. Population: lights (dominant) via [light.cpp:123](code/light.cpp). Consumer of the resulting `windowsVisible == turn` stamp: `Light::render` at `light.cpp:141`, the `canBeSeen()` chain, and per-actor lifecycle gates (`mech3d.cpp:4256-4262`, `gvactor.cpp:2773-2779`).
2. **Symptom vs cause.** Symptom: 950 unmeasured CPU `projectForObjectAdmission` calls/frame. Upstream condition: lights still use the legacy CPU `windowsVisible` cull-chain because the GPU readback ring is wired only for movers (mech / gvehicl / bdactor / gvactor consume `readback_isActorVisibleLagged`; `light.cpp` does NOT).
3. **The meta-fix.** Repoint `Light::update` at `gpu_cull::readback_isActorVisibleLagged(handle)` — same architectural endpoint as Stage 0.5 §4 for static props/movers/mechs. Single producer, all consumers.
4. **Substitutive test.** The `projectForObjectAdmission` call at `gameobj.cpp:2090` must be **deleted** (or fall behind a fallback gate) and the F3 sidecar count must drop to ~0. Sticky-bit (`91b6991`) did NOT retire these — it operates on `blockVisBits[]`, a different channel. Indirect-draw bucket coverage did NOT retire these — it doesn't feed `windowsVisible`.
5. **Verdict.** `PATCH (justified)` — DEFER.
   - **Named meta-fix:** repoint `Light::update` onto `readback_isActorVisibleLagged`, deleting the `GameObject::onScreen()` call site.
   - **Deferral reason:** the producer (`rb_actorVisible[]`) is empirically a non-superset of coarse `inView` (5.6×-15× over the §3 gate); update-gate / render-gate split resurrects the black-tree bug class. Lights are EXACTLY the consumer class §4 found unsafe to repoint.
   - **Filed debt:** unblocks when alpha-Stage 1 fixes readback non-superset (see §4 blocker memory meta-fix option 3) OR when Stage 0.5 v4 reframes to `blockVisBits[]`-based gating (meta-fix option 2).

## 5. Two no-risk preparatory moves (do these instead)

Neither requires architectural commitment; both unblock a future attempt with better data.

### (i) Promote `SIDECAR_EVENTDRIVEN_PROJECT_Z` to a timed `BucketId`

Trivial scope. Add a new `BUCKET_EVENTDRIVEN_PROJECTZ` to [mclib/cpu_proj_cost_split.h:32-38](mclib/cpu_proj_cost_split.h); wrap the `gameobj.cpp:2090` call site in a `mc2_cpu_proj_cost::Scope` RAII guard. The existing scope machinery handles ring-buffer commit at `frame_end()`. No new instrumentation infrastructure.

This converts "950 calls/frame, cost unknown" → "950 calls/frame, p95 = X µs." With X measured, the slice becomes a real decision:
- X < 25 µs → file as documented debt, no slice ever (smoke variance floor).
- 25 ≤ X < 100 µs → wait for alpha-Stage 1 readback-quality work, then piggyback as a small retirement step.
- X ≥ 100 µs → architecturally significant; alpha-Stage 1 unblock becomes higher-priority.

The cost-baseline spec already anticipates this — [docs/superpowers/specs/2026-05-20-cpu-projection-cost-baseline-design.md](docs/superpowers/specs/2026-05-20-cpu-projection-cost-baseline-design.md) explicitly flags "n_calls > 0 AND time UNMEASURED with high n_calls → open separate follow-up slice." That trigger has fired.

### (ii) Capture worst-case (motion+combat) F3 numbers

The stationary baseline suppresses lights from weapon trails / muzzle flashes / explosions. Worst-case mc2_10 with combat is the bound. This is a one-mission F3 capture, not a code change.

Without (ii), any decision about whether the slice is worth pursuing is missing its worst-case data.

## 6. Open question for the user

If you want to ship the F3 timing-bucket promotion (5.i above) as a small standalone slice **now** while the gosFX and per-object-cull architectural slices are deferred, that's the actionable move. It's a single bucket addition with no consumer change.

Otherwise, this recon's deliverable is the verdict: **DEFER. Don't open this as a slice. Stage 0.5 §4 evidence applies directly.**

---

## Appendix — citations re-grepped at write time

- `projectForObjectAdmission` call site: [code/gameobj.cpp:2090](code/gameobj.cpp) (sole live invocation).
- `GameObject::onScreen()` callers: [code/light.cpp:123](code/light.cpp), [code/light.cpp:139](code/light.cpp) (paused-only).
- `SIDECAR_EVENTDRIVEN_PROJECT_Z` definition: [mclib/cpu_proj_cost_split.h:43](mclib/cpu_proj_cost_split.h); increment [mclib/cpu_proj_cost_split.cpp:336-339](mclib/cpu_proj_cost_split.cpp); count-only, no timer.
- `add_workload_eventdriven_projectZ` increment site: [mclib/camera.h:447-449](mclib/camera.h) — gated on `tls_inRenderLoop==false`.
- `windowsVisible == turn` consumer: [code/light.cpp:141](code/light.cpp) (light render lifecycle gate).
- `rb_actorVisible[]` producer: `shaders/gpu_cull.comp:239` (per §4 blocker memory).
- `windowsVisible = turn;` stamp sites (the 12 the prior thinking conflated with the projection call): `artlry.cpp:872/1409`, `bldng.cpp:383/807/1126`, `gate.cpp:241/357/637`, `gvehicl.cpp:3273/3324/3821`, `mech.cpp:6045/6111/6183`, `turret.cpp:581/812/2085`, `terrobj.cpp:601/814`, `weaponbolt.cpp:373`, `gameobj.cpp:2094`, `objmgr.cpp:3570/3591` — all direct writes, none call `projectForObjectAdmission`.

## Cross-references

- [memory/stage_0_5_section_4_blocked_on_readback_non_superset.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\stage_0_5_section_4_blocked_on_readback_non_superset.md) — the block
- [memory/cull_gates_are_load_bearing.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\cull_gates_are_load_bearing.md) — why naive repoint cascades
- [memory/feedback_offload_must_be_substitutive_not_additive.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\feedback_offload_must_be_substitutive_not_additive.md) — why count-only isn't enough
- [memory/feedback_capped_fps_is_not_a_cpu_cost_ab_signal.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\feedback_capped_fps_is_not_a_cpu_cost_ab_signal.md) — why direct A/B is impossible without bucket
- [memory/policy_split_wrapper_grep_trap.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\policy_split_wrapper_grep_trap.md) — why the call site was hidden from naive `projectZ` greps
- [memory/f3_tier1_baseline_2026_05_20.md](C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\f3_tier1_baseline_2026_05_20.md) — the baseline that prompted this recon
- [docs/superpowers/specs/2026-05-20-cpu-projection-cost-baseline-design.md](docs/superpowers/specs/2026-05-20-cpu-projection-cost-baseline-design.md) — F3 spec with the "open follow-up slice" trigger
