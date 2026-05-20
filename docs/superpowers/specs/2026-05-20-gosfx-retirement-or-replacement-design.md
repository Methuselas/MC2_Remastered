# gosFX Retirement / Replacement — Design Spec

- **Status:** DRAFT — open questions resolved 2026-05-20, ready for plan-phase (design only; no code changes)
- **Date:** 2026-05-20
- **Authoring branch:** `claude/nifty-mendeleev`
- **Companion docs:** `docs/superpowers/specs/2026-05-20-cpu-projection-cost-baseline-design.md` (F3 telemetry plan); `docs/superpowers/specs/2026-05-20-unified-projection-spec.md` (DRAFT, per HANDOFF)
- **Greybeard verdict:** META-FIX (delete gosFX-as-MLR-consumer); see §3
- **All file:line citations grep-verified against the `nifty-mendeleev` worktree at write time.**

---

## 1. Inventory

### 1.1 Module shape

`mclib/gosfx/` is ~19 .cpp/hpp pairs implementing the GameOS effects subsystem. Class hierarchy (one-line per type):

- **`Effect`** — base lifecycle/draw class; every concrete effect is a subclass.
- **`ParticleCloud`** — abstract base for particle systems with lifespan/fade.
- **`PointCloud`** — forward-declares `MLRPointCloud` (`mclib/gosfx/pointcloud.hpp:14`).
- **`CardCloud`** + singleton **`Card`** — billboards via `MLRCardCloud` (`card.hpp:11`, `cardcloud.hpp:11`).
- **`PertCloud`** — perturbed n-gon particles via `MLRNGonCloud` (`pertcloud.hpp:14`).
- **`ShardCloud`** — triangle shards via `MLRTriangleCloud`.
- **`ShapeCloud`** + singleton **`Shape`** — instanced TG_Shapes via `MLRShape` (`shape.hpp:14`, `shapecloud.hpp:14`).
- **`DebrisCloud`** — rigid body debris via `MLRShape`.
- **`Tube`** — indexed triangle tube (contrails) via `MLRIndexedTriangleCloud`.
- **`EffectCloud`** — container that spawns child effects at particle positions.
- **`EffectLibrary`** — singleton spec registry / factory loaded from `mc2.fx`.
- **`LightManager`** — runtime light pool used by effects.
- **`SpinningCloud`** — rotating particle base.

Every concrete particle type forward-declares an MLR primitive in its header. **All live MLR consumers in the runtime game build are inside `mclib/gosfx/`.** (Confirmed by `grep` for `MLR\w*Cloud\|MLRShape\|MLRClipper\|MLRIndexedTriangleCloud` outside `mclib/mlr/` and `mclib/gosfx/` returning only `Viewer/` and `data_tools/` — both separate non-game binaries.)

### 1.2 `theClipper` lifecycle (the MLR seam)

Single global `MLRClipper` constructed/destructed at process-mission scope:

- **Construction (game):** `code/mechcmd2.cpp:1647` — `theClipper = new MidLevelRenderer::MLRClipper(0, cameraSorter);`
- **Construction (mission reload):** `mclib/txmmgr.cpp:503` — same call inside `MC_TextureManager` init.
- **Destruction:** `code/mechcmd2.cpp:1940`; `mclib/txmmgr.cpp:369`, `:475`.
- **Per-frame driver:** `code/gamecam.cpp:142` — `theClipper->StartDraw(cameraOrigin, cameraToClip, fColor, &fColor, default_state, &z);`
- **Per-frame flush:** `code/gamecam.cpp:269` — `theClipper->RenderNow();    //Draw the FX`
- **Editor / SimpleCamera path:** `code/simplecamera.cpp:168` — duplicate `StartDraw` call.
- **Offline binaries (NOT in scope of game retirement):** `Viewer/View.cpp:542` constructs its own clipper; `data_tools/aseconv.cpp` uses MLR primitives in offline import.

`gamecam.cpp:142` is where `cameraToClip` enters the MLR domain. This is the per-frame CPU projection that the unified-projection arc needs to retire (per memory `alpha-Stage 1 POSTSCRIPT`, `mclib/mlr/mlrclipper.cpp:206` reads `cameraToClip(2,2)` directly — any reverse-Z / [0,1] depth change in the unified spec immediately corrupts MLR's clip math).

### 1.3 `drawInfo.m_clipper = theClipper` site census (corrected)

The user's 8-site count is **incomplete by ~2×**. Grep-verified in `nifty-mendeleev`:

| File | Lines | Context |
|---|---|---|
| `mclib/mech3d.cpp` | `:2648`, `:2993`, `:3035` | mech draw paths |
| `mclib/gvactor.cpp` | `:2201`, `:2234`, `:2269`, `:2310` | ground vehicle / actor draw |
| `mclib/bdactor.cpp` | `:1544`, `:1588` | building actor draw |
| `code/artlry.cpp` | `:1339`, `:1423` | artillery hit / muzzle |
| `code/carnage.cpp` | `:830` | mech death / unit explosion |
| `code/terrobj.cpp` | `:974` | building destruction dust |
| `code/weaponbolt.cpp` | `:1610` | weapon bolt visuals |
| `code/missiongui.cpp` | `:2904`, `:2927` | VTOL dust + recovery beam |

**Total: 16 live assignment sites across 8 source files.** The user's stated 6 in-game + 2 briefing-only sites missed every site in `mech3d/gvactor/bdactor` and the second artlry site, and miscategorized `missiongui` (see §1.6).

### 1.4 Effect content & loading

- Effect specs live in `mc2srcdata/effects/mc2.fx` (~898 KB compiled binary library; ~100+ named specs including `Fireball`, `Gauss_flare`, `LRM_Smoke`, `Mech_Explosion`, `Critical_hit`, `VTOL_Effect`, `Recovery_Effect`, `Damaged_fire`, `Flamejet`, `Mech_disable`, etc.).
- **Load path:** `code/mechcmd2.cpp:1661` builds `effectsPath/mc2.fx` filename; `:1674` calls `gosFX::EffectLibrary::Instance->Load(effectStream)`. Init happens once at game startup; `gosFX::EffectLibrary::Instance` is set at `mechcmd2.cpp:1657`.
- **No mission-specific .fx files**; one global library compiled at content-author time.
- **Runtime lookup:** `gosFX::EffectLibrary::Instance->Find(<name>)` — pattern observed in `code/artlry.cpp`, `code/carnage.cpp`, `code/missiongui.cpp`. Names indexed via `weaponEffects` manager and per-call hardcoded IDs (e.g. `WATER_MISS_FX`, `DUST_POOF_ID`, `VTOL_DUST_CLOUD`).

### 1.5 Save-game compatibility

**gosFX is NOT persisted across save/load.** Effects are transient runtime objects:

- `EffectLibrary` is re-instantiated fresh from `mc2.fx` on game start (`mechcmd2.cpp:1657`); never serialized.
- `weaponEffects` manager is destroyed/rebuilt per mission, never saved.
- Active effects expire naturally via `Execute()` returning false → `delete gosEffect; gosEffect = NULL`.

**Implication:** Disabling effects has zero save-game compatibility risk.

### 1.6 "Briefing-only" is wrong: VTOL effects run mid-mission

`missiongui.cpp:2904` and `:2927` are inside the dropship / recovery-VTOL animation path. This runs during pickup, dropoff, and recovery sequences — in-mission animations, not the briefing screen proper. They are **cosmetic** (no progression gate) but they are NOT "skippable like the briefing." Treat as in-game for retirement scope.

### 1.7 ABL / mission-script triggers

Grep of `code/abl/` and 991 .abl files in `mc2srcdata/missions/` produced **no progression gates on effect completion.** No script waits on explosion-finished, hit-effect-done, or VTOL-dust-complete. Mission progression is independent of effect state.

### 1.8 Lifecycle hook summary

| Stage | Where | Per |
|---|---|---|
| `gosFX::Heap` init | `mclib/gosfx/gosfx.cpp:11` declares; assigned in `mechcmd2.cpp` startup | process |
| `EffectLibrary` create | `mechcmd2.cpp:1657` | process |
| `mc2.fx` load | `mechcmd2.cpp:1674` | process |
| `theClipper` create | `mechcmd2.cpp:1647` and `txmmgr.cpp:503` | process + mission re-init |
| Effects spawned | hundreds of game-event sites | per-event |
| Effect natural death | `Execute()` returns false | per-effect |
| `theClipper` delete | `mechcmd2.cpp:1940`, `txmmgr.cpp:369`, `:475` | process shutdown / mission re-init |

---

## 2. Three approaches

Honest tradeoff matrix follows; no recommended-bias here — that's §3.

### (A) Delete gosFX entirely

**What it is.** Stub `gosFX::Effect::Draw()` to no-op (or remove all 16 `drawInfo.m_clipper = theClipper` sites + their surrounding `Effect::Draw(&drawInfo)` calls); delete `theClipper` construction/teardown (3 sites); delete `gamecam.cpp:142` and `:269`; mark `mclib/mlr/` as unreferenced; gate `MC2_DISABLE_GOSFX=1` for the soak-waiver flip; eventually delete `mclib/gosfx/` and `mclib/mlr/` trees from the build.

**What you ship.** A game with zero in-mission particles, zero weapon visuals, zero explosions, zero VTOL effects. All ~100 specs in `mc2.fx` become dead data. MLR retires atomically as collateral.

**Pros.**
- One slice retires the entire `theClipper` seam — the only CPU consumer of `cameraToClip` that mathematically blocks unified-projection (per `mlrclipper.cpp:206` reverse-Z risk).
- Substitutive by construction: producer and consumer both deleted. No additive-slice anti-pattern.
- Soak-waiver pattern viable (`feedback_soak_waiver_with_probes_and_reviews_validated.md`): env-gate, ship default-off, run tier1 with `Effect::Draw` invocation counter, flip default-on, soak, delete code.
- Reversibility is clean via `git revert` — the deletion is layer-clean (gosFX consumes MLR; MLR has zero other live consumers in the runtime exe).

**Cons.**
- Stock missions become **visually impoverished**. Mech explosions, weapon muzzle flashes, hit sparks, building dust, contrails, VTOL effects, recovery beams — all gone. Per §1.7 nothing breaks gameplay progression, but the perceptual gap is large (16 sites × per-event count = thousands of effect invocations per mission).
- Editor / Viewer / aseconv keep their own MLR construction (`Viewer/View.cpp:542`, `editor/Editor.cpp` per `engine-standalone` worktree). They survive deletion only because they are separate binaries — but `feedback_editor_must_converge_with_runtime_paths.md` flags this as latent re-divergence debt.
- `stock_install_must_remain_playable.md` is satisfied (playable ≠ same-looking) but a literal reading of the project's minimum-viable bar is debatable.

**Estimated visual regression scope.** Mission *playability*: 100% unaffected. Mission *visual fidelity*: substantial degradation in every combat exchange and every dropship animation. Tier1 mc2_01 through mc2_24 all progress; what changes is the lack of explosion/muzzle/hit/dust visuals during gunfire and the lack of VTOL atmosphere on insertion/extraction.

### (B) Minimum-viable GPU particle pipeline

**What it is.** Implement just enough of a new GPU-direct particle path to cover the 16 in-game `drawInfo.m_clipper` sites. Bypass MLR entirely. Particles emit world-space quads/billboards to a new SSBO-backed batcher; draw post-`renderLists()` (per `render_order_post_renderlists_hook.md`); consume the unified-projection UBO directly so it shares the matrix authority the arc is converging on.

Defer (or stub) the lower-volume / weirder paths:
- `Tube` (contrails) — defer.
- `DebrisCloud` (rigid debris) — defer (rare; ship as solid-color quads or stub).
- `PertCloud` (perturbed n-gons) — defer.
- Cover `CardCloud`, `PointCloud`, `ShardCloud` for the 6 in-game effect types (artillery / carnage / terrobj / weaponbolt / mech / gvactor / bdactor / VTOL).

**Pros.**
- Preserves the visual feel of the stock install.
- Brings effects onto the modern post-`renderLists()` GPU path with no `theClipper` dependency.
- Vulkan-prep friendly (`vulkan_prep_explicit_device_discipline.md`): a new batcher built today can use explicit device-mediated binding from day one.

**Cons.**
- Scope dwarfs A. New batcher, new shaders, sort/blend ordering, particle lifecycle, billboard math, texture atlas management for ~100 named effect specs.
- **No parity oracle.** GPU particles are mathematically different from MLR particles by design; you cannot byte-compare against the legacy path. The validated soak-waiver pattern doesn't fit (it relies on parity probes); validation becomes per-effect visual canary.
- **Additive-slice risk.** Until every one of the 16 sites is repointed, MLR stays alive. If the slice ships before the gate-off is wired, you ship two particle systems. This is the exact anti-pattern that produced ~0ms net gain in the two-week CPU→GPU campaign (`feedback_offload_must_be_substitutive_not_additive.md`).
- Briefing/VTOL fidelity at `missiongui.cpp` and `Tube` contrails may sit in the deferred bucket for an indefinite period.
- Mission-data deferred work: the GPU path needs to interpret `mc2.fx` spec parameters; either keep the parser and re-emit GPU instructions, or write a content-side spec convertor. Either way is real work.

### (C) Keep gosFX-via-MLR; gate retirement on F3 data

**What it is.** Ship nothing structural now. Wait for F3 telemetry (`mlr_total p95`, `n_prims_clipped p95`) to land per the CPU-projection baseline spec. If `mlr_total p95 < 5us OR n_prims_clipped p95 == 0`, retire as low-priority cleanup. Otherwise schedule a full backend rewrite when data justifies.

**Pros.**
- Zero code risk; zero current effort; zero visual regression.
- Defers the architecture decision to a data point.

**Cons.**
- **Does not unblock the unified-projection arc.** `mlrclipper.cpp:206` keeps reading `cameraToClip(2,2)`. Every unified-projection sub-stage carries an MLR-parity gate it would not otherwise have.
- **Not a retirement option.** Greybeard rejects this as a verdict for the retirement question — it produces an additive outcome ("we measured it"). It's better classified as a *sequencing input* to A/B than a third arm.
- Treats the cost-axis as the only axis; ignores the schema-coupling axis. Even at `mlr_total p95 = 0us`, MLR's clip-math coupling to `cameraToClip` is the live blocker, not its cost.

---

## 3. Recommendation

**Ship (A) — delete gosFX — gated behind `MC2_DISABLE_GOSFX=1` for soak, then flip default-on, then delete `mclib/gosfx/` + `mclib/mlr/`. File (B) — GPU particle pipeline — as named debt under `stock_install_must_remain_playable.md`.**

### 3.1 Greybeard ruling (verbatim, all five answers)

1. **Subsystem pin.** Owner is `mclib/gosfx/` (sole live MLR primitive consumer) plus the `theClipper` global lifecycle (`mechcmd2.cpp:1647`, `txmmgr.cpp:503`, `gamecam.cpp:142`+`:269`). MLR is not blocking unified-projection in its own right; gosFX is blocking by virtue of being the lone consumer that keeps `theClipper->StartDraw(..., cameraToClip, ...)` hot every frame.
2. **Symptom vs cause.** Symptom: `cameraToClip` is consumed CPU-side by `MLRClipper::StartDraw` and `mlrclipper.cpp:206` reads `cameraToClip(2,2)` directly. Upstream condition: gosFX hardcodes MLR primitives as its render-time backend, so any unified-projection schema change must keep MLR semantically alive OR retire MLR.
3. **The meta-fix.** Delete gosFX-as-an-MLR-consumer. Retire `theClipper`, `StartDraw`, `RenderNow`, and every `drawInfo.m_clipper = theClipper` site (16 across 8 files) in one stroke. `mclib/mlr/` becomes dead code in the runtime exe; only `Viewer/` and `data_tools/aseconv` retain it.
4. **Substitutive test.** Option A deletes both producer and consumer — substitutive by construction. Option B is substitutive ONLY if the spec mandates removing the gosFX-MLR draw path in the same slice (not "GPU path armed alongside"). Option C is explicitly additive — it ships nothing structural. Codebase has documented "additive slices netting ~0ms" history; C is rejected as a verdict.
5. **Verdict: META-FIX (Option A).** Upstream change: delete the gosFX backend; retire `theClipper`; retire MLR runtime-side. Blast radius: 16 draw sites + 3 lifecycle sites + 1 per-frame driver site + 1 per-frame flush site, all layer-clean. Bug class retired: every "this code reads `cameraToClip` CPU-side and feeds MLR clip math" instance.

### 3.2 Load-bearing rationale

- **Unified-projection arc dependency.** The arc does *not* semantically block on gosFX retirement, but every sub-stage carries an MLR-parity gate it would not otherwise. The user's HANDOFF specifically flags `mlrclipper.cpp:206` as HIGH-RISK for unified-projection. Killing the consumer dissolves the gate.
- **F3 perf data (added post-resolution, 2026-05-20).** Tier1 automated baseline with stationary camera measures `mlr_total` p95 worst-window = **~407us — single-handedly busts the 100us CPU-projection budget by 4×**. Recorded with motion-suppressed buckets (`recalcBounds`/`tgl_transform`/`skinning_chain` at 0us because stationary_pct=99.7% triggers the static-update-skip optimization). Implication: gosFX retirement is now **perf-justified in addition to META-FIX-justified**. The schema-coupling rationale (above) is still the load-bearing reason for the architecture call; the ~400us is a bonus that strengthens (A) from "structural unblock" to "structural unblock + 400us savings." Note: `matrix_build` p95 is ~0.4us — even full retirement is immeasurable; do NOT plan around `matrix_build` savings from the unified-projection slice.
- **MLR-exception property.** MLR is the documented immediate-draw exception (`render_functions_are_enqueuers_not_submitters.md`, `render_order_post_renderlists_hook.md`). Its draw happens at `theClipper->RenderNow()` *inside* the enqueue phase, *before* `renderLists()`. Option A deletes that call site outright, simplifying the hook structure. Option B inserts a new post-`renderLists()` GPU pass AND needs to retire `RenderNow()` to be substitutive — strictly larger scope.
- **Reversibility.** Option A is one PR + a feature gate. `git revert` is clean because the deletion is layer-clean (gosFX → MLR → nothing else in the runtime exe).
- **Soak-waiver fit.** Option A is a textbook soak-waiver candidate (`feedback_soak_waiver_with_probes_and_reviews_validated.md`): env-gate, ship default-off, tier1 probe counts `Effect::Draw` invocations, flip default-on, soak, delete. Option B has no parity oracle and does not fit.
- **Stock-assets constraint.** `stock_install_must_remain_playable.md` requires *playability*, not visual identity. Per §1.7 every stock tier1 mission progresses without effects. The constraint is satisfied. The user-facing visual gap is acknowledged debt — that's what filing (B) records.

### 3.3 The shape of the slice

Suggested staging (not a plan; informs the next plan-phase spec):
1. **Stage 0 — instrument.** Add `MC2_GOSFX_TRACE=1` counter on `Effect::Draw` invocations per `debug_instrumentation_rule.md`. Land soak probe. Tier1 baseline.
2. **Stage 1 — gate.** `MC2_DISABLE_GOSFX=1` short-circuits `Effect::Draw` to no-op AND stubs `gamecam.cpp:142`+`:269` AND skips the 16 `drawInfo.m_clipper = theClipper` sites. Default-off. Tier1 visual canary (regression expected; document scope).
3. **Stage 2 — flip default-on.** Soak. User-driven canary across all 5 tier1 missions; check progression, save/load, mission completion.
4. **Stage 3 — atomic retirement.** Delete `theClipper` construction sites + `RenderNow`/`StartDraw` calls + `mclib/mlr/` + `mclib/gosfx/` from the runtime exe build (keep Viewer/aseconv intact). MLR comes out with gosFX in the same PR — substitutive test enforced.
5. **Stage 4 — file (B) debt.** Open a tracked issue / memory file naming the missing visual fidelity, with the named effect IDs from `mc2.fx` as the work surface.

---

## 4. Negative space

What might break / surprise that isn't obvious:

1. **Census drift.** 16 sites today; if a slice lands between spec-write and execute that touches mech / actor draw, the count moves. The plan must drive site enumeration off a *compile-time* mechanism (e.g., remove `m_clipper` from the `Effect::DrawInfo` struct and let the compiler enumerate broken call sites), not a grep list.
2. **`txmmgr.cpp:503` mission re-init.** `theClipper` has TWO construction paths; deleting one leaks the other. The retirement must hit `mechcmd2.cpp:1647` AND `txmmgr.cpp:503` lockstep, plus `:369`/`:475` deletes. (Reference: `mission_load_inits_mirror_init_per_subsystem.md` — saveload-style multi-path init bug class.)
3. **`simplecamera.cpp:168` is a third `StartDraw` site.** Easily missed because it's outside the main camera. Pre-edit grep before claiming "all StartDraw sites deleted."
4. **Editor convergence debt.** `editor/Editor.cpp` (in `engine-standalone` worktree) deletes its own `theClipper`. Per `feedback_editor_must_converge_with_runtime_paths.md` the editor should converge with runtime paths — don't ship gosFX retirement in the runtime exe while leaving the editor on the MLR rails, or you accumulate another forked-loop divergence.
5. **`Viewer/View.cpp` and `data_tools/aseconv.cpp`.** These are offline binaries with their own MLR uses. They survive Stage 4 deletion only if the build keeps `mclib/mlr/` compiled in for *those* targets while excluding it from `mc2.exe`. CMake target scoping matters; coordinate with build-system-expert during plan-phase.
6. **`Tube` contrails.** Visually the most missed effect (long-tail trails behind missiles / VTOLs). Worth calling out by name in the (B) debt note.
7. **`mc2.fx` becomes dead data.** Stage 3 leaves a ~900 KB content file referenced by nothing. Keep it on disk (no harm) until (B) ships, or remove from the deploy step. Don't delete from `mc2srcdata/` — it's the spec library (B) will need.
8. **`gosFX::LightManager`.** Effects spawn light sources. The `LightsData` SSBO pipeline (lockstep with `deploy_shaders_and_exe_in_lockstep.md`) consumes them. Retiring gosFX retires `LightManager` too — confirm no other subsystem expects gosFX-spawned dynamic lights. Spec author: grep `gosFX::LightManager::Instance` outside `mclib/gosfx/` before plan-phase.
9. **Save-game canary.** Per §1.5 effects don't persist, but the savegame canary (`HANDOFF_2026_05_20_evening_post_savegame_canary.md`) recently flushed multiple PATCHes on Mission::load init-mirror. Add gosFX-disabled savegame to the smoke matrix to be safe.
10. **Stuff matrix convention.** `clip_w_sign_trap.md` says MC2 clip.w sign is not a front/back indicator. The 16 sites pass `drawInfo` containing world-space matrices that MLR consumed under those conventions. If any deferred GPU particle path (B) reuses the same matrices, it must respect the convention or use `projectZ()` for in-front tests.
11. **Tracy zones.** `mlr_total` and its children will report zero after retirement. Confirm the cost-baseline spec's evaluation rules treat "zone disappears" as PASS (per `feedback_offload_must_be_substitutive_not_additive.md`), not as instrumentation failure.
12. **Briefing / VTOL ≠ briefing.** Per §1.6 `missiongui.cpp:2904`/`:2927` runs mid-mission. Don't scope them as "briefing-only" in the plan or you'll under-document the visible regression area.

---

## 5. Open questions — RESOLVED 2026-05-20

User decisions captured below. These were the scoping calls needed before plan-phase.

1. **Visual fidelity floor — TRANSITIONAL ONLY.** "No in-game particles" is acceptable as a transitional state only. Option (B) — particle reinstatement — is **required-for-ship debt**, not optional. The (B) debt entry must be filed at the same time as the (A) retirement lands and must be tracked toward a ship gate, not parked indefinitely. The minimum-viable GPU particle pipeline is on the critical path for the next shipping milestone.
2. **Editor scope — ENGINE ONLY.** This slice retires MLR from the runtime exe (`mc2.exe`) only. The editor (`engine-standalone` worktree) transitions separately as part of the existing editor-convergence debt (`feedback_editor_must_converge_with_runtime_paths.md`). CMake target scoping must keep `mclib/mlr/` linkable into editor / Viewer / `aseconv` targets while excluding it from the runtime exe.
3. **Tube / contrails — NO CARVE-OUT.** Clean delete. `Tube` (contrails) goes with the rest of gosFX in Stage 3. No stand-alone GPU contrail path before (B) lands. Accept the contrail visual gap during the transitional period.
4. **F3 telemetry — CONCURRENT, NOT GATED.** Stage 0 of this slice runs concurrently with the F3 cost-baseline work (`docs/superpowers/specs/2026-05-20-cpu-projection-cost-baseline-design.md`). F3 doesn't gate the retirement decision (the schema-coupling axis already settles it), but its data informs the post-retirement perf claim. The two specs are independent in execution order.

### 5.1 Implications for plan-phase

- **Stage 4 deletion is final.** `mc2srcdata/effects/mc2.fx` stays on disk (the (B) work surface needs it as the spec library); `mclib/gosfx/` and `mclib/mlr/` are removed from the runtime exe build only — CMake must scope linkage to editor / Viewer / `aseconv` targets explicitly.
- **(B) must follow soon.** Because (B) is required-for-ship, the spec author for the (A) plan should also draft an initial scope sketch / dependency note for (B) so the next milestone planning has a stub to fill in. The (B) plan does NOT block (A) shipping, but (A) cannot be the last slice before ship.
- **No Tube carve-out simplifies (A).** The deletion is uniform across all gosFX particle types — no special-case path to maintain.
- **Concurrent F3 means no sequencing constraint.** Stage 0 instrumentation (`MC2_GOSFX_TRACE=1` counter) can land independently of F3 telemetry. F3's `mlr_total` zone will register the retirement as "zone disappears" (PASS per `feedback_offload_must_be_substitutive_not_additive.md`).

---

## Appendix A — grep-verification log (write-time)

All file:line citations in this document were re-grepped at write time against the `nifty-mendeleev` worktree on 2026-05-20. Specifically:

- `drawInfo.m_clipper = theClipper` — 16 matches, listed in §1.3.
- `new MidLevelRenderer::MLRClipper` — 3 matches (`mechcmd2.cpp:1647`, `txmmgr.cpp:503`, `Viewer/View.cpp:542`).
- `delete theClipper` — 3 matches (`mechcmd2.cpp:1940`, `txmmgr.cpp:369`, `txmmgr.cpp:475`).
- `theClipper->StartDraw` / `theClipper->RenderNow` — 3 matches (`gamecam.cpp:142`, `gamecam.cpp:269`, `simplecamera.cpp:168`).
- `gosFX::EffectLibrary::Instance->Load` — 1 game-side match (`mechcmd2.cpp:1674`; plus `txmmgr.cpp:511` and `Viewer/View.cpp:550` for Instance assignment).
- `mc2.fx` referenced at `mechcmd2.cpp:1661` (effectsName.init path build).

No fictional symbols; no inherited citations from upstream session messages were used without re-grep.
