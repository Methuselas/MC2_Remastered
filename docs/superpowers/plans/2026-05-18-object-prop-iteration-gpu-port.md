# Object/Prop Iteration GPU Port Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate the residual ~1.43ms `GameLogic.Units.TerrainObjects` CPU cost by deleting the per-object `recalcBounds()` screen-space projection body for terrain statics (Bldg/Tree), re-homing the rare mouse-pick consumer to lazy compute-on-demand, and proving the elimination is substitutive (CPU zone -> ~0, total frame drops).

**Architecture:** `recalcBounds()` currently does two things per object per active block every frame: (1) a cheap matrix-free angular sphere-clip that sets a coarse `inView`, then (2) an expensive screen-space projection (`projectForScreenXY` + 8-corner selection-box + fog/on-screen-rect refinement) that further narrows `inView` and writes `screenPos`/`upperLeft`/`lowerRight`. The GPU compute cull (already shipped, `gpu_cull::readback_isActorVisibleLagged`) is the redundant twin of (2)'s visibility output. This slice **keeps (1)** (coarse `inView`, superset-safe), **deletes (2)** (the projection body), repoints the render + shadow gates to the lagged readback (motion-safe via the already-shipped conservative-OR + frustum-dilation primitive), and re-homes the only surviving projection consumer (mouse-pick) to a lazy per-candidate projection that runs per-click instead of per-frame.

**Tech Stack:** C++ (MC2 engine, mclib/code split), OpenGL 4.3 compute readback, RDTSC cost-split instrumentation (`[*SPLIT v1]` pattern), env-gated parity probes, user-driven Tracy total-frame capture, tier1 smoke gate.

**Authoritative inputs (read before executing):**
- Contract: `docs/superpowers/specs/2026-05-18-object-prop-iteration-gpu-port-stage0.md`
- Open-questions resolution: `docs/superpowers/specs/2026-05-18-object-prop-iteration-open-questions-resolution.md`
- Handoff: `docs/superpowers/progress/2026-05-18-object-prop-iteration-port-HANDOFF.md`
- Worktree CLAUDE.md (load-bearing rules + memory pointers); memory files: `cull_gates_are_load_bearing.md`, `feedback_offload_must_be_substitutive_not_additive.md`, `cost_split_instrumentation_is_observer_effect_dominated.md`, `capped_fps_is_not_a_cpu_cost_ab_signal.md`, `mc2_selection_picking_model_water_terrain_never_picked.md`, `stock_install_must_remain_playable.md`, `zoomed_out_big_map_is_an_unexercised_stress_path_that_hides_full_map_regressions.md`.

**Grounding discipline:** every `file:line` below was grep-verified at HEAD `4d9529a`. Symbols are stable; lines drift. **Re-grep every cited symbol at the moment you touch it** — do not trust these line numbers if commits have landed since. Build ALWAYS `--config RelWithDebInfo`. Full relink before deploy when load-bearing functions change. Deploy per-file `cp -f` + `diff -q`, never `cp -r`. No emoji in any file.

**Why this is not standard TDD:** this is a C++ game engine with no unit-test harness for the render/cull path. The verification mechanism here is the established pattern: env-gated RDTSC/parity probes that emit log markers, the tier1 smoke gate, and a user-driven total-frame Tracy capture. Each task's "test" is its probe/marker assertion or smoke pass, not a `pytest` invocation. This is deliberate and matches every prior slice in this campaign.

---

## File structure

| File | Responsibility | Change |
|---|---|---|
| `mclib/bdactor.cpp` | `BldgAppearance::recalcBounds` / `TreeAppearance::recalcBounds` | Add RDTSC split probe; delete projection body; keep coarse angular clip |
| `code/terrobj.cpp` | Terrain-static render gate (`:796`) + shadow gate (`:866`) + update-loop driver (`:694`) | Repoint render/shadow gates to readback; host the split-probe accumulators/summary |
| `code/objmgr.cpp` | `findObjectByMouse` / `findTerrainObjectByMouse` pick path | Lazy per-candidate projection helper; re-express pick guards against readback |
| `mclib/appear.h` | `Appearance` members (`screenPos`/`upperLeft`/`lowerRight`/`canBeSeen`) | NO change to `canBeSeen()` (type-agnostic, mover blast radius); members stay declared |
| `GameOS/gameos/gpu_cull_readback.h` / `.cpp` | `readback_isActorVisibleLagged(uint32_t actorId)` (`.h:82`), `readback_isEnabled()` (`.h:28`), `readback_buildActorVisSnapshot(uint32_t)` (`.h:75`) | Consume only; no change (conservative-OR + dilation already applied inside snapshot build). NOTE: file is in `GameOS/gameos/`, NOT `mclib/` (review MAJOR-A) |
| `scripts/run_smoke.py` | Smoke env-var allowlist | Forward `MC2_TOBJ_COST_SPLIT` + the parity-probe env var |

**The load-bearing framing (do not violate):** the deliverable is the **projection-body DELETE**. The render-gate edit is *consequential* — `g_useGpuStaticProps` already makes render `inView`-independent, so adding a readback OR-term without deleting the projection body is the additive-not-substitutive failure mode (`feedback_offload_must_be_substitutive_not_additive.md`) and the zone will not move. Every reviewer and executor must verify the delete is the primary change.

---

## Task 0: Stage-0.5 sizing gate — RDTSC cost-split probe

**Purpose:** partition the ~1.43ms `GameLogic.Units.TerrainObjects` baseline into ANGULAR (kept), PROJ (to-delete), UPDATE (refill risk from the wider coarse-only gate). The recoverable amount is `PROJ - extra_UPDATE`, not the whole 1.43ms. This probe must run and be analyzed BEFORE the delete tasks; if PROJ is not the dominant term the slice premise is wrong and we stop.

**Files:**
- Modify: `mclib/bdactor.cpp` (`BldgAppearance::recalcBounds` ~`:1152`, `TreeAppearance::recalcBounds` ~`:4278`)
- Modify: `code/terrobj.cpp` (update-loop driver ~`:694`, summary roll point)
- Modify: `scripts/run_smoke.py` (env allowlist)
- Reference precedent: `mclib/terrain.cpp:1459-1822` (`[SLIMSPLIT v1]`, `MC2_SLIM_COST_SPLIT`, `__rdtsc()`, `g_ssProjCyc`/`g_ssCullCyc`/`g_ssRedCyc`, summary every 600 frames at `:1494`, once-per-frame roll at `:1822`)

- [ ] **Step 1: Read the SLIMSPLIT precedent in full**

Read `mclib/terrain.cpp:1459-1822`. Replicate its exact shape: `__rdtsc()` deltas accumulated into file-static `unsigned long long` accumulators, env-gated by a dedicated var, `[<TAG> v1] event=summary` print every 600 frames, accumulators rolled once per frame. Do NOT use `std::chrono` per-call — `cost_split_instrumentation_is_observer_effect_dominated.md` documents that chrono per-call in this exact loop class fabricated ~5ms of phantom cost. RDTSC is ~5-10ns.

- [ ] **Step 2: Add the env-gated accumulators and probe points**

In `code/terrobj.cpp` near the other instrumentation statics, add (re-grep for the actual current location of the `recalcBounds` driver at `:694`):

```cpp
// [TOBJSPLIT v1] env-gated RDTSC cost split for the recalcBounds slice.
// MC2_TOBJ_COST_SPLIT=1 -> partition GameLogic.Units.TerrainObjects into
// ANGULAR (kept coarse clip) / PROJ (to-delete projection body) / UPDATE
// (appearance->update refill). RDTSC only; chrono per-call is observer-effect
// dominated here (cost_split_instrumentation_is_observer_effect_dominated.md).
static bool s_tobjSplitEnabled = (getenv("MC2_TOBJ_COST_SPLIT") != nullptr);
static unsigned long long g_tobjAngularCyc = 0ULL;
static unsigned long long g_tobjProjCyc    = 0ULL;
static unsigned long long g_tobjUpdateCyc  = 0ULL;
static unsigned long long g_tobjFrameCount = 0ULL;
```

**Cross-file linkage design — pick this concrete one (review MAJOR-2; the SLIMSPLIT precedent keeps everything in one TU but this probe straddles `bdactor.cpp` (ANGULAR/PROJ probe points) and `terrobj.cpp` (UPDATE probe + summary roll), so a definite decision is required):** the four accumulators (`g_tobjAngularCyc`/`g_tobjProjCyc`/`g_tobjUpdateCyc`/`g_tobjFrameCount`) are **defined in `code/terrobj.cpp`** (where the summary roll lives) and **`extern`-declared at the top of `BldgAppearance::recalcBounds`/`TreeAppearance::recalcBounds` in `mclib/bdactor.cpp`** (a 4-line `extern unsigned long long ...;` block — no new header). The `s_tobjSplitEnabled` bool is duplicated as a file-static in EACH TU (one `getenv("MC2_TOBJ_COST_SPLIT")` call per TU; process-start-constant so both observe the same value). Do NOT use an anonymous namespace for the accumulators (that would prevent the cross-TU `extern`).

In `mclib/bdactor.cpp` `BldgAppearance::recalcBounds` (re-grep `bool BldgAppearance::recalcBounds`): bracket the coarse angular clip (currently `:1163-1198`, the `inView=true` through the hClip reject) with an `__rdtsc()` delta into `g_tobjAngularCyc`, and bracket the projection block (currently the `if (inView)` at `:1202` through its close before `return(inView)` at `:1587`) with an `__rdtsc()` delta into `g_tobjProjCyc`. Mirror in `TreeAppearance::recalcBounds`. Gate every probe on the bdactor-side file-static `s_tobjSplitEnabled`.

In `code/terrobj.cpp` around the `appearance->update()` call (re-grep; resolution cites `:752`) bracket it with an `__rdtsc()` delta into `g_tobjUpdateCyc`, gated on `s_tobjSplitEnabled`.

- [ ] **Step 3: Add the once-per-frame roll + 600-frame summary**

In `code/terrobj.cpp` at the end of the per-frame terrain-object update pass (mirror `terrain.cpp:1822`'s once-per-frame roll and `:1492`'s summary), add — **use `fprintf(stderr, ...)` + `fflush(stderr)` to match the SLIMSPLIT precedent exactly (`terrain.cpp:1492` uses `fprintf(stderr,...)`, NOT `printf`; review MAJOR-1). The smoke runner merges stderr into stdout (`run_smoke.py` `stderr=subprocess.STDOUT`) so capture is equivalent, but the grep'd marker pattern must match the established `[SLIMSPLIT v1]` shape):**

```cpp
if (s_tobjSplitEnabled) {
    if (++g_tobjFrameCount % 600ULL == 0ULL) {
        fprintf(stderr, "[TOBJSPLIT v1] event=summary frames=%llu "
               "angular_cyc=%llu proj_cyc=%llu update_cyc=%llu\n",
               (unsigned long long)g_tobjFrameCount,
               (unsigned long long)g_tobjAngularCyc,
               (unsigned long long)g_tobjProjCyc,
               (unsigned long long)g_tobjUpdateCyc);
        fflush(stderr);
        g_tobjAngularCyc = g_tobjProjCyc = g_tobjUpdateCyc = 0ULL;
    }
}
```

- [ ] **Step 4: Forward the env var in the smoke allowlist**

In `scripts/run_smoke.py`, find the env-var forwarding allowlist (grep `MC2_SLIM_COST_SPLIT` or `MC2_ASSET_SCALE_TRACE` — the existing instrumentation vars). Add `MC2_TOBJ_COST_SPLIT` alongside them so a smoke run can carry it.

- [ ] **Step 5: Build (full relink — recalcBounds is load-bearing)**

```bash
cd A:/Games/mc2-opengl-src/.claude/worktrees/gpu-driven-rendering
rm -f build64/RelWithDebInfo/mc2.exe
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo
```
Expected: clean build, `build64/RelWithDebInfo/mc2.exe` regenerated.

- [ ] **Step 6: Deploy + run an instrumented smoke**

Deploy per-file (`cp -f` + `diff -q` mc2.exe to `A:/Games/mc2-opengl/mc2-win64-v0.4/`). Then:
```bash
py -3 scripts/run_smoke.py --tier tier1 --duration 30 --kill-existing
```
(set `MC2_TOBJ_COST_SPLIT=1` per the runner's env mechanism). Read `tests/smoke/artifacts/<latest>/mc2_*.log`, grep `[TOBJSPLIT v1] event=summary`. **If no summary line appears (fewer than 600 frames rendered in a mission — heavy-load missions can be slow), re-run the sizing capture with `--duration 60` so at least one 600-frame window completes (review MINOR-3).**

- [ ] **Step 7: GATE — analyze the split, decide go/no-go**

Compute `PROJ / (ANGULAR+PROJ+UPDATE)`. The slice premise requires PROJ to be the dominant term and the expected post-delete refill (extra UPDATE from the wider coarse gate) to be small relative to PROJ. **If PROJ is not dominant, STOP and report to the user — the ~1.43ms is not where the contract assumed and the slice must be re-scoped.** Record the measured split in `docs/superpowers/specs/2026-05-18-object-prop-iteration-open-questions-resolution.md` under a new "Stage-0.5 measured split" section.

- [ ] **Step 8: Commit**

```bash
git add mclib/bdactor.cpp code/terrobj.cpp scripts/run_smoke.py docs/superpowers/specs/2026-05-18-object-prop-iteration-open-questions-resolution.md
git commit -m "$(cat <<'EOF'
perf(static-decor): Stage-0.5 RDTSC cost-split probe for recalcBounds

[TOBJSPLIT v1] env-gated (MC2_TOBJ_COST_SPLIT) RDTSC partition of
GameLogic.Units.TerrainObjects into ANGULAR/PROJ/UPDATE. Sizing gate
before the projection-body delete; SLIMSPLIT precedent (terrain.cpp).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 1: CRIT-2 — confirm or re-home the texture/LOD loader side effect

**Purpose:** the projection block being deleted (`bdactor.cpp:1383-1583` Bldg, Tree mirror) also performs LOD selection + per-LOD texture (re)load as a side effect. The contract did NOT enumerate this. Deleting the block wholesale removes that loader. This task proves a second (type-init) loader exists, or re-homes the load. **This must resolve before Task 2's delete.**

**Files:**
- Investigate: `mclib/bdactor.cpp` (`:1383-1583` Bldg projection-body tail, the `selectLOD`/texture-load region; `:4455`+ Tree mirror), type-init path (`appearType->reinit()` at `:1238`, `BldgAppearanceType` init)
- Possibly modify: `mclib/bdactor.cpp` (re-home the loader if it is the sole loader)

- [ ] **Step 1: Map the texture-load side effect**

Re-grep the projection-body tail in `BldgAppearance::recalcBounds` for texture-handle assignment / `gos_GetTextureHandle` / `bldgShape->...Texture` / `selectLOD` (resolution cites `:1389-1415` TEMP LOD-0 pin `selectLOD = 0; (void)useHighObjectDetail;`, loader through `:1583`). Document exactly which texture handles are written inside the block.

- [ ] **Step 2: Opposite-direction grep for a second loader**

Per `feedback_data_flow_audit_asymmetry.md`: do NOT grep the obvious symbol. Grep the *type-init* path — `BldgAppearanceType::init`, `appearType->reinit()` (called at `bdactor.cpp:1238`), `TreeAppearanceType` init, and the appearance-creation path (`primeAppearanceForMissionLoad`, `appearanceSetup` — see the sequence comment at `terrobj.cpp:561-562`). Determine whether the texture handles deleted in Step 1 are independently initialized there.

- [ ] **Step 3: Decide and record (HARD GATE for Task 2)**

The design-gate review (CRIT-B) already grep-traced the expected outcome: the per-LOD-swap texture loader in the deleted block (`bdactor.cpp` old `:1418`/`:1485-1518`/`:1537-1570`) is reachable ONLY via `if (selectLOD != currentLOD)` / `if (currentLOD && baseLOD)`, and the 2026-05-12 TEMP pin (`:1389`, `selectLOD=0`) with `currentLOD` init `0` (`:647`) makes **both loaders dead code today**. The sole live LOD-0 loader is `BldgAppearance::init` `bdactor.cpp:667` (shape create) + `:671-704` (texture load), Tree analogue. Confirm this by independent grep at write-time.

Two outcomes:
- **(a) Second loader confirmed (expected):** record outcome (a) in the resolution doc citing the specific second loader site `bdactor.cpp:667` + `:671-704` (and the Tree analogue). The projection-block loader is redundant and currently unreachable. Task 2's delete is texture-safe.
- **(b) Projection block is the sole loader:** re-home the minimal texture-load (NOT the projection) into the retained coarse-clip path or the type-init path. Specify the exact moved lines. The re-home must be load-only — do not drag projection math with it.

**HARD STOP GATE:** Task 2 MUST NOT begin until this step records outcome (a)-with-the-second-loader-site-cited OR outcome (b)-re-home-implemented. A Task 1 that ends with Step 2's opposite-direction grep inconclusive (no cited second loader, no re-home) is a STOP — surface to the user, do not proceed to the delete.

**LATENT-HAZARD NOTE (review CRIT-B — must be written in BOTH the resolution doc AND a code comment at the Task 2 deletion point):** "The deleted projection block contained the per-LOD-swap texture (re)loader (old `:1418`/`:1485`). It is currently dead under the 2026-05-12 TEMP LOD-0 pin (`bdactor.cpp:1389`). If that pin is ever reverted (when the LOD-1 invisibility root cause is fixed), LOD selection + the per-LOD texture loader MUST be re-homed BEFORE the revert lands — the init-time loader at `:667-704` covers LOD-0 ONLY." This converts a silent future regression into a guarded one.

- [ ] **Step 4: If (b), implement the re-home + build + smoke**

If re-home required: make the minimal edit, full-relink build (Task 0 Step 5 commands), deploy, run tier1 smoke (`--tier tier1 --duration 30 --kill-existing`), confirm exit 0 and no missing-texture visual report from the user-driven smoke. If (a), skip to Step 5.

- [ ] **Step 5: Commit (only if code changed; otherwise commit the doc finding)**

```bash
git add mclib/bdactor.cpp docs/superpowers/specs/2026-05-18-object-prop-iteration-open-questions-resolution.md
git commit -m "$(cat <<'EOF'
fix(static-decor): re-home texture loader out of recalcBounds projection body

CRIT-2: the projection block also (re)loads per-LOD texture handles as a
side effect. [Outcome a: documented redundant; OR b: re-homed to <site>.]
Precondition for the projection-body delete (Task 2).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Delete the Bldg projection body; keep the coarse angular clip

**Purpose:** the load-bearing substitutive change. Delete `BldgAppearance::recalcBounds`'s post-angular-clip projection block so `recalcBounds` returns the coarse angular `inView` only and no longer writes `screenPos`/`upperLeft`/`lowerRight`/fog refinement.

**Files:**
- Modify: `mclib/bdactor.cpp` `BldgAppearance::recalcBounds` (currently `:1152-1588`; KEEP `:1155-1198` coarse angular clip; DELETE `:1202-1583` projection block; keep `return(inView);` at `:1587`)

- [ ] **Step 1: Re-grep the exact boundaries**

`grep -n "bool BldgAppearance::recalcBounds" mclib/bdactor.cpp`, then read the function. Confirm: `inView=false` init; `inView=true` + matrix-free angular sphere-clip (the `eye->usePerspective` block ending at the hClip reject) — **KEEP**; the `if (inView)` projection block (`eye->projectForScreenXY(position,screenPos)` + fog/distance refine + on-screen-rect + 8-corner box loop + LOD/texture tail) — **DELETE**; `return(inView);` — KEEP. If lines have drifted, the structure is the anchor, not the numbers.

- [ ] **Step 2: Delete the projection block**

Remove from the `if (inView)` that follows the angular clip (currently `:1202`) through the end of the projection/box/LOD tail, up to but not including `return(inView);`. The retained RDTSC ANGULAR probe (Task 0) stays around the angular clip; **delete the PROJ probe bracket along with the block it measured** (it has nothing left to measure). Leave a one-line comment at the deletion point:

```cpp
	// recalcBounds projection body deleted 2026-05-18: GPU compute cull
	// (gpu_cull::readback_isActorVisibleLagged) is the substitutive twin of
	// the per-frame screen projection. inView is now coarse-angular-only
	// (strict superset of the old projected value; over-inclusion is
	// correctness-safe per cull_gates_are_load_bearing.md). screenPos/
	// upperLeft/lowerRight are computed lazily at pick time (objmgr.cpp).
	// LATENT HAZARD: the deleted block also held the per-LOD-swap texture
	// (re)loader, dead today under the 2026-05-12 TEMP LOD-0 pin (:1389).
	// If that pin is reverted, re-home LOD selection + per-LOD texture load
	// BEFORE the revert; init-time loader (:667-704) covers LOD-0 only.
	return(inView);
```

- [ ] **Step 3: Build (full relink)**

Task 0 Step 5 commands (`rm -f build64/RelWithDebInfo/mc2.exe` then cmake build `--config RelWithDebInfo`). Expected: clean build. A compile error here usually means a non-pick consumer of a deleted byproduct exists — if so, STOP and grep that consumer (do not stub it away).

- [ ] **Step 4: Deploy + tier1 smoke (regression gate, NOT proof)**

Deploy mc2.exe per-file. Run `py -3 scripts/run_smoke.py --tier tier1 --duration 30 --kill-existing`. Expected exit 0. This only proves no crash/init regression — props will currently render via the existing `g_useGpuStaticProps`/legacy gate; pick is expected broken until Task 4. The user-driven smoke is the visual observer; if they report missing props at default camera, STOP (coarse `inView` should be a superset — a drop means the angular clip was damaged).

- [ ] **Step 5: Commit**

```bash
git add mclib/bdactor.cpp
git commit -m "$(cat <<'EOF'
perf(static-decor): delete BldgAppearance::recalcBounds projection body

Substitutive deliverable. Keeps the matrix-free coarse angular sphere-clip
(writes superset inView); deletes per-frame projectForScreenXY + 8-corner
selection-box + fog/on-screen-rect refinement. GPU compute cull readback is
the visibility twin; pick re-homed lazily (objmgr.cpp, Task 4).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Delete the Tree projection body (mirror of Task 2)

**Purpose:** apply the identical delete to `TreeAppearance::recalcBounds`. Trees are NEVER pick targets (`objmgr.cpp:2497`/`:2505` skip `getObjectClass() != TREE`), so the Tree body delete has zero pick-path consumer — no Task 4 re-home needed for trees.

**Files:**
- Modify: `mclib/bdactor.cpp` `TreeAppearance::recalcBounds` (currently `:4278`+; coarse angular clip `:4281-4322` KEEP; projection block from the post-clip `if (inView)` ~`:4327` through the box/LOD tail DELETE)

- [ ] **Step 1: Re-grep + confirm Tree mirror structure**

`grep -n "bool TreeAppearance::recalcBounds" mclib/bdactor.cpp`, read it, confirm it mirrors Bldg (coarse angular clip then `if (inView)` projection block then `return(inView)`). Confirm trees are pick-excluded: `grep -n "TREE" code/objmgr.cpp` around `findObjectByMouse` (`:2497`/`:2505`).

- [ ] **Step 2: Delete the Tree projection block + its PROJ probe**

Same shape as Task 2 Step 2: delete the post-angular-clip projection block, keep the coarse clip + `return(inView)`, leave the same one-line provenance comment, remove the Tree PROJ RDTSC bracket.

- [ ] **Step 3: Build (full relink) + tier1 smoke**

Task 0 Step 5 build; deploy; `--tier tier1 --duration 30 --kill-existing`. Expected exit 0, no tree pop/vanish at default camera from the user-driven smoke.

- [ ] **Step 4: Commit**

```bash
git add mclib/bdactor.cpp
git commit -m "$(cat <<'EOF'
perf(static-decor): delete TreeAppearance::recalcBounds projection body

Mirror of the Bldg delete. Trees are never pick targets (objmgr.cpp:2497/
2505 skip non-TREE), so this body delete is pick-consumer-free.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: CRIT-1 — lazy per-candidate projection at pick time (buildings/props)

**Purpose:** re-home the only surviving consumer of the deleted projection. Compute the screen rect lazily per pick candidate (per-click, not per-frame) and re-express the visibility guard against the readback-visible set.

**CRIT — scope the edit to `findTerrainObjectByMouse`, NOT the shared 5-param `findObjectByMouse` (review CRIT-1, plan-gate sonnet):** the 5-param `findObjectByMouse` (`objmgr.cpp:2459`) is called from TWO sites — `:2641` (via `findTerrainObjectByMouse`, terrain-static-only) AND `:2681` `findObjectByMouse(mouseX,mouseY,&objList[1],getMaxObjects(),false)` (ALL objects incl. movers). Movers carry their own `recalcBounds`/`upperLeft`/`lowerRight` (NOT deleted) and a separate readback wiring (`mech3d.cpp:2452-2455`/`gvactor.cpp:2118-2121`). Replacing the guard inside the shared 5-param overload unconditionally would gate movers on a readback slot they have no entry for -> silent mover-pick failure. **All edits below go in `findTerrainObjectByMouse` (`objmgr.cpp:2625`, terrain-static-only by construction) and its per-candidate inner loop, leaving the 5-param `findObjectByMouse` overload UNCHANGED.** Re-grep `findTerrainObjectByMouse` at write-time to confirm it iterates `objBlockInfo[].active` and calls into the per-object test the deleted byproducts feed.

**Files:**
- Modify: `code/objmgr.cpp` (`findTerrainObjectByMouse` `:2625`, dispatch `:2685`; the terrain-static-only per-candidate path — NOT the shared 5-param `findObjectByMouse:2459`)
- Reference (unchanged): `mclib/bdactor.cpp:1055` `BldgAppearance::PerPolySelect` (geometry-space, screenPos-independent, survives); `GameOS/gameos/gpu_cull_readback.h` `readback_isActorVisibleLagged`(`:82`)/`readback_isEnabled`(`:28`); `mclib/camera.h:1125` `extern CameraPtr eye;` (the global pick camera); `camera.h:611-613` `Camera::projectForScreenXY(Vector3D&,Vector4D&)`; `mclib/objectappearance.h:58` `ObjectAppearance::position` (public); `mclib/apprtype.h:52-53` `AppearanceType::typeUpperLeft/typeLowerRight`; `mclib/bdactor.h:192` `BldgAppearance::appearType`

- [ ] **Step 1: Confirm the pick consumer chain (windowsVisible is a STATED, REVIEW-VERIFIED FACT — do not re-litigate)**

`grep -n "findObjectByMouse\|findTerrainObjectByMouse\|getWindowsVisible\|upperLeft\|lowerRight\|PerPolySelect" code/objmgr.cpp`. Confirm: `:2475` `canBeSeen()` guard; `:2477` `getWindowsVisible() == (turn - VISIBLE_THRESHOLD)`; `:2480-2483` `upperLeft`/`lowerRight` rect; `:2485-2488` coarse rect pre-filter; `:2499`/`:2508` `PerPolySelect`.

**windowsVisible — DESIGN-GATE-VERIFIED FACT (review CRIT-A, opus, grep-traced):** `windowsVisible` is stamped at `terrobj.cpp:699` gated by `if(inView)` at `:697`, where `inView` is the **return value** of `recalcBounds()` consumed at `:694` — NOT any projection-internal state. The projection-body delete changes that return value from projected to coarse-angular, which is a **strict superset** (CRIT-D proof: the entire projection block is `if(inView)`-gated at `bdactor.cpp:1202` and can only ever set `inView=false`, never resurrect a coarse reject). Therefore the stamp fires for a SUPERSET of today's objects; the `:2477` equality `getWindowsVisible() == (turn - 1)` (`#define VISIBLE_THRESHOLD 1`, objmgr.cpp:297; `turn` global timing.cpp:19, ++ mission.cpp:268) continues to hold for every pick-eligible object, and newly-admitted objects are correctly filtered by the lazy screen-rect (`:2485-2488`) + geometry-space `PerPolySelect`. **No false pick, no missed pick.**

**HARD PROHIBITION:** DO NOT re-home or alter the `windowsVisible` stamp *logic*. DO NOT change the control flow or gating of `terrobj.cpp:694`, `:697`, or `:699`. The resolution doc's original CRIT-1 ("windowsVisible must be re-homed or pick silently fails") was traced FALSE by the design-gate review and amended to RESOLVED there — if you read a re-home instruction anywhere, it is the superseded claim; ignore it.

**Comment-only annotations are REQUIRED at BOTH sites (review MAJOR-3 — a comment is not a logic change and the prohibition above explicitly permits it):**
- At `terrobj.cpp:699` (the STAMP site), append:
  ```cpp
  windowsVisible = turn; // pick-path dependency (objmgr.cpp:2477): inView is
                         // coarse-only post projection-body-delete; coarse is a
                         // strict superset of projected, so this stamp still
                         // fires for every pick-eligible object. DO NOT gate
                         // this narrower than inView.
  ```
- At `objmgr.cpp:2477` (the CONSUMER site), append a one-line comment recording that the stamp survives via the coarse return path so a future reader does not "fix" it.

The genuine gap is ONLY `upperLeft`/`lowerRight`/`screenPos` (written ONLY in the deleted block — grep all writers in `mclib/bdactor.cpp` to confirm zero surviving writer; resolution: Bldg `:1247-1257`/`:1372-1375`, all deleted).

**Non-per-frame recalcBounds callers (review MAJOR-C — enumerate so the Task 2 build does not panic):** besides the per-frame `terrobj.cpp:694`, `recalcBounds` is also called at `terrobj.cpp:1075`/`:1132` (`handleWeaponHit` destruction branch), `:616` (`primeAppearanceForMissionLoad`), and `:499` (`isVisible()` — DEAD, zero callers in `code/` or `mclib/`, grep-verified). None are per-frame; all become coarse-only post-delete; their projection side-effect is intentionally dropped and is covered by the Task 4 lazy pick-time projection. **No action needed for these — do NOT mistake them for the per-frame consumer during the Task 2 build.**

- [ ] **Step 2: Add the lazy projection helper**

In `code/objmgr.cpp`, add a file-local helper that reproduces the deleted block's screen-rect math for ONE candidate building/prop. **The helper MUST be typed `BldgAppearance*`, not `AppearancePtr` (review CRIT-2): `Appearance` has NO `getPosition()` method; `position`/`rotation` are public fields on `ObjectAppearance` (the intermediate parent), and the box source `typeUpperLeft`/`typeLowerRight` live on `appearType` (`BldgAppearanceType`, reachable only via `BldgAppearance::appearType`). Calling `a->getPosition()` on `Appearance*` is a compile error.**

```cpp
// Lazy pick-time screen projection. Replaces the per-frame recalcBounds
// projection body (deleted 2026-05-18) for the ONLY surviving consumer:
// mouse-pick (per-click over already-cull-narrowed active blocks, not
// per-frame). Populates a local screen rect; PerPolySelect (geometry-
// space) does the precise hit-test unchanged. Typed BldgAppearance* —
// Appearance* has no position/getPosition (review CRIT-2).
static bool projectPickCandidateRect(BldgAppearance* ba,
                                     long& outMinX, long& outMinY,
                                     long& outMaxX, long& outMaxY)
{
	if (!ba || !eye) return false;          // `eye` = extern CameraPtr (camera.h:1125)
	Stuff::Vector4D sp;
	eye->projectForScreenXY(ba->position, sp);   // ObjectAppearance::position (public)
	outMinX = outMaxX = (long)sp.x;
	outMinY = outMaxY = (long)sp.y;
	// 8-corner selection box: reproduce the deleted boxCoords loop VERBATIM
	// from the pre-delete bdactor.cpp (old :1264-1375). Recover the exact
	// source with: git show <pre-delete-rev>:mclib/bdactor.cpp
	// The deleted code built the 8 corners from ba->appearType->typeUpperLeft
	// / ba->appearType->typeLowerRight (apprtype.h:52-53) transformed by
	// ba->position + ba->rotation. Lift that construction EXACTLY — do not
	// invent box geometry; pick must stay byte-equivalent to the old rect.
	Stuff::Vector3D boxCoords[8];
	/* ... populate boxCoords[0..7] exactly as the deleted block did ... */
	for (int i = 0; i < 8; ++i) {
		Stuff::Vector4D c;
		eye->projectForScreenXY(boxCoords[i], c);
		if ((long)c.x < outMinX) outMinX = (long)c.x;
		if ((long)c.x > outMaxX) outMaxX = (long)c.x;
		if ((long)c.y < outMinY) outMinY = (long)c.y;
		if ((long)c.y > outMaxY) outMaxY = (long)c.y;
	}
	return true;
}
```

**Implementation note for the executor:** find the pre-delete revision with `git log --oneline -- mclib/bdactor.cpp` (the commit before Task 2's delete), then `git show <rev>:mclib/bdactor.cpp` and lift the `boxCoords[0..7]` construction (the 8-corner build from `appearType->typeUpperLeft`/`typeLowerRight` + `position`/`rotation`) verbatim into the `/* ... */` slot. Do not invent the box geometry — reproduce the deleted math exactly so pick behavior is byte-equivalent. The candidate is obtained at the call site by a class-guarded cast: `BldgAppearance* ba = (obj->getObjectClass()==BUILDING) ? (BldgAppearance*)obj->getAppearance() : NULL;`. **No `BldgAppearance*` cast exists anywhere in `objmgr.cpp` (round-3 grep, MAJOR-C3-2) — do not look for one there.** Real precedent for the cast form: `(BldgAppearance*)appearance` at `code/bldng.cpp:1260`. Class constants: `BUILDING`/`TREEBUILDING` at `objmgr.cpp:1028-1029` (re-grep exact constant + whether `GATE`/`TREEBUILDING` candidates also reach this path and need the same cast).

- [ ] **Step 3: Re-express the pick guards in `findTerrainObjectByMouse` (terrain-static-only — NOT the shared 5-param overload)**

**Verified structure (round-3 grep, MAJOR-C3-1):** `findTerrainObjectByMouse` (`objmgr.cpp:2625-2648`) has NO per-candidate inner test loop — it iterates `objBlockInfo[].active` terrain blocks and delegates the entire per-object test to the shared 5-param `findObjectByMouse` at `:2641` (def `:2459-2519`). The shared overload is ALSO called for movers at `:2681`, so it must stay byte-unchanged (review CRIT-1).

**Primary instruction:** duplicate the ~45-line per-object loop body (`objmgr.cpp:2470-2515` — re-grep exact bounds: the canBeSeen guard, windowsVisible equality, upperLeft/lowerRight rect test, PerPolySelect) INTO `findTerrainObjectByMouse` as a terrain-static-only inline test, and STOP routing terrain statics through the shared `:2641` call (route only the inline duplicate; the `:2681` mover path keeps using the untouched 5-param overload). The lists `findTerrainObjectByMouse` walks are terrain-statics-only by construction (`objBlockInfo` population — re-grep `countTerrainObjects`/the `objBlockInfo[].active` fill), so the duplicated body needs no class filter for the static/mover split, only the existing Tree exclusion. In the duplicated body: (1) replace the `canBeSeen()` guard with `gpu_cull::readback_isEnabled() ? gpu_cull::readback_isActorVisibleLagged(static_cast<uint32_t>(obj->getHandle())) : true` (fail-open — `stock_install_must_remain_playable.md`); (2) class-guard-cast to `BldgAppearance*` and call `projectPickCandidateRect(ba, minX,minY,maxX,maxY)` into locals, run the coarse-rect pre-filter using the locals instead of `a->upperLeft`/`a->lowerRight`; (3) leave the `windowsVisible` equality UNCHANGED, add only the Step-1 comment-only annotation; `PerPolySelect` unchanged; Trees pick-excluded already (`:2497`/`:2505`).

All loop-body symbols are public GameObject/Appearance API in the same TU, so the duplication is mechanically sound. If at write-time the duplication proves infeasible (e.g. the loop body has hidden coupling to the 5-param signature that cannot be cleanly lifted), STOP and surface to the user — do NOT edit the shared overload as a shortcut (that is the CRIT-1 mover-pick regression).

- [ ] **Step 4: Build (full relink) + deploy**

Task 0 Step 5 build commands; deploy mc2.exe per-file.

- [ ] **Step 5: User-driven pick verification**

Run `py -3 scripts/run_smoke.py --tier tier1 --duration 30 --kill-existing`. The smoke is user-driven and they are the visual observer — ask the user to confirm building/prop selection works (click a building, confirm selection box appears) and drag-select still ignores statics correctly. **This is the load-bearing CRIT-1 verification.** If pick returns nothing, the most likely cause is the `windowsVisible` equality (Step 1's claim was wrong — re-verify the stamp survives) or the lazy rect math diverging from the deleted original. Do NOT silence the guard; root-cause it.

- [ ] **Step 6: Commit**

```bash
git add code/objmgr.cpp
git commit -m "$(cat <<'EOF'
fix(static-decor): re-home building/prop pick to lazy per-candidate projection

CRIT-1 / VPL-Step-6 survivor re-home. findObjectByMouse projects the
candidate screen rect on demand (per-click, not per-frame); visibility
guard repointed to gpu_cull readback (fail-open when disabled).
PerPolySelect (geometry-space) unchanged. Trees pick-excluded already.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: Repoint the render gate (terrobj.cpp:796) to the lagged readback

**Purpose:** make the render-consumer edit consistent with the substitutive contract. `terrobj.cpp:796` is currently `if (appearance->canBeSeen() || g_useGpuStaticProps)`. Replace the `canBeSeen()` term with the lagged readback (fail-open when disabled). This is *consequential, not primary* — `g_useGpuStaticProps` already makes render `inView`-independent; the value of this edit is removing the dependency on the now-coarse `canBeSeen()` and aligning the render path with the readback the proof gate measures.

**Files:**
- Modify: `code/terrobj.cpp:796` (render gate)
- Reference: `GameOS/gameos/gpu_cull_readback.h` `readback_isActorVisibleLagged`/`readback_isEnabled`; snapshot built `objmgr.cpp:1908-1911` before the `:1967` zone (conservative-OR + dilation applied transparently inside `readback_buildActorVisSnapshot` — NO render-site OR-flip exists or is needed; do not invent one)

- [ ] **Step 1: Re-grep the render gate + the snapshot precondition**

`grep -n "appearance->canBeSeen() || g_useGpuStaticProps" code/terrobj.cpp` (render gate). `grep -n "readback_buildActorVisSnapshot\|readback_isEnabled" code/objmgr.cpp` — confirm the snapshot is built (currently `:1908-1911`) BEFORE the `GameLogic.Units.TerrainObjects` loop (`:1967`). Confirm `readback_isActorVisibleLagged` signature in `GameOS/gameos/gpu_cull_readback.h` (resolution cites `~:82`, key = `obj->getHandle()`).

- [ ] **Step 2: Edit the render gate**

Replace at `:796`:
```cpp
	if (appearance->canBeSeen() || g_useGpuStaticProps)
```
with:
```cpp
	// readback is the substitutive visibility source (recalcBounds projection
	// deleted 2026-05-18). Fail-open all-visible when disabled
	// (stock_install_must_remain_playable.md). g_useGpuStaticProps OR-term
	// retained: it is the GPU-batcher render-submission killswitch, an
	// orthogonal machine, NOT a second visibility source.
	const bool tobjVisible = gpu_cull::readback_isEnabled()
		? gpu_cull::readback_isActorVisibleLagged(static_cast<uint32_t>(obj->getHandle()))
		: true;
	if (tobjVisible || g_useGpuStaticProps)
```
(Re-grep the local name for the object handle in this scope — it may be `obj`, `objList[objIndex]`, etc. Match the surrounding code.)

- [ ] **Step 3: Build (full relink) + deploy + tier1 smoke**

Task 0 Step 5 build; deploy; `--tier tier1 --duration 30 --kill-existing`. Expected exit 0, no prop pop/vanish at default camera (user-driven visual observer). Over-inclusion is correctness-safe; a dropped prop is catastrophic — if the user reports vanishing props, the readback is under-inclusive vs the old projected set: re-check conservative-OR/dilation is active (it is default-on inside the snapshot build) and `readback_isEnabled()` is true in this run.

- [ ] **Step 4: Commit**

```bash
git add code/terrobj.cpp
git commit -m "$(cat <<'EOF'
perf(static-decor): repoint terrain-static render gate to lagged readback

Consequential edit (g_useGpuStaticProps already makes render inView-
independent). Replaces canBeSeen() with gpu_cull readback; fail-open when
disabled; g_useGpuStaticProps OR-term retained (orthogonal batcher
killswitch). Aligns render path with the proof-gate readback set.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: Repoint the shadow gate (terrobj.cpp:866) in lockstep

**Purpose:** the shadow gate `terrobj.cpp:866` is `if (appearance->canBeSeen())` with NO `g_useGpuStaticProps` OR-term — an asymmetry from the render gate. With the projection deleted, `canBeSeen()` returns coarse-only `inView`; shadows widen to the coarse set unless repointed. Coarse is a superset of projected (no dropped shadow — safe direction) but the slice should repoint to the readback for consistency and to bound the wider shadow-draw cost.

**Files:**
- Modify: `code/terrobj.cpp:866` (shadow gate)

- [ ] **Step 1: Re-grep the shadow gate**

`grep -n "if (appearance->canBeSeen())" code/terrobj.cpp` — confirm `:866` is the shadow (renderShadows) consumer and that it lacks the `|| g_useGpuStaticProps` term (asymmetry per the resolution's MAJOR-3 risk).

- [ ] **Step 2: Edit the shadow gate to match the render gate's visibility source**

Replace at `:866`:
```cpp
	if (appearance->canBeSeen())
```
with the same `tobjVisible` pattern as Task 5 Step 2 (re-grep the handle local in this scope; do NOT add `g_useGpuStaticProps` here — preserve the existing semantic that shadows are not gated by the batcher killswitch; only swap `canBeSeen()` for the readback with fail-open):
```cpp
	// Shadow gate repointed in lockstep with the render gate (Task 5).
	// canBeSeen() is coarse-only after the projection delete; readback is
	// the substitutive source. Fail-open when disabled. No g_useGpuStaticProps
	// term here by design (shadows are not batcher-killswitch gated).
	const bool tobjShadowVisible = gpu_cull::readback_isEnabled()
		? gpu_cull::readback_isActorVisibleLagged(static_cast<uint32_t>(obj->getHandle()))
		: true;
	if (tobjShadowVisible)
```

- [ ] **Step 3: Build (full relink) + deploy + tier1 smoke**

Task 0 Step 5 build; deploy; `--tier tier1 --duration 30 --kill-existing`. Expected exit 0. User-driven visual check: no terrain-static shadow pop/vanish on a fast zoomed-out pan (the Fix-A ghost/streak class is CRITICAL per the contract).

- [ ] **Step 4: Commit**

```bash
git add code/terrobj.cpp
git commit -m "$(cat <<'EOF'
perf(static-decor): repoint terrain-static shadow gate to lagged readback

Lockstep with the render gate. Resolves the :866 vs :796 asymmetry
(:866 had no g_useGpuStaticProps term and depended on projected inView).
Readback substitutive source; fail-open; no batcher-killswitch term by
design (shadows are not g_useGpuStaticProps-gated).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: Superset parity probe (proof-gate item #2)

**Purpose:** the contract's proof gate #2 requires a logic/counter superset-parity check: the readback-visible set MUST be a superset of legacy `inView` for every terrain static; zero `(legacyCanBeSeen && !readbackVisible)` violations. This is a counter probe, NOT chrono.

**Files:**
- Modify: `code/terrobj.cpp` (near the render/shadow gates; env-gated counter)
- Modify: `scripts/run_smoke.py` (forward the parity env var)

- [ ] **Step 1: Add the env-gated parity counter**

In `code/terrobj.cpp`, env-gated on `MC2_TOBJ_PARITY=1`, near the render gate: each frame, for each terrain static, compute legacy coarse `inView` (the retained `canBeSeen()` / returned `recalcBounds` value) and `readbackVisible` (the Task 5 `tobjVisible`). Accumulate two counters: **`g_tobjParitySamples`** (incremented for every coarse-visible terrain static — the denominator) and **`g_tobjParityViolation`** (`coarseInView && !readbackVisible` — the catastrophic class). Emit `[TOBJPARITY v1] event=summary` every 600 frames with BOTH counters (`samples=%llu violations=%llu`), same `fprintf(stderr,...)`+`fflush` roll/print shape as Task 0's `[TOBJSPLIT v1]`. (Counter names are authoritative as `g_tobjParitySamples`/`g_tobjParityViolation` — matching the Step-1 code block; the earlier draft name `g_tobjReadbackSuperset` is retired.)

```cpp
// [TOBJPARITY v1] proof-gate #2 superset parity. A violation
// (legacyVisible && !readbackVisible) is the catastrophic dropped-prop
// class; expected count is ZERO. Legitimate over-inclusion (readback &&
// !legacy) is fine and not counted as a violation. Counter, never chrono.
static bool s_tobjParity = (getenv("MC2_TOBJ_PARITY") != nullptr);
static unsigned long long g_tobjParityViolation = 0ULL;
static unsigned long long g_tobjParitySamples   = 0ULL;
```

The 600-frame summary print MUST include BOTH counters (review MINOR-4 — do not declare `g_tobjParitySamples` and leave it unprinted): `fprintf(stderr, "[TOBJPARITY v1] event=summary samples=%llu violations=%llu\n", ...); fflush(stderr);` then reset both. `samples` is the denominator that makes a zero-violation result meaningful (zero violations over zero samples is not a pass).

Note (review MAJOR-B — state the transitive chain explicitly in the probe comment): "legacy `inView`" post-delete is coarse-only. The probe asserts `readback ⊇ coarse` (zero `(coarseInView && !readbackVisible)`). This implies the contract's intended `readback ⊇ projected_original` **ONLY because `coarse ⊇ projected` is independently proven** (the projection block is entirely `if(inView)`-gated at `bdactor.cpp:1202` and can only narrow, never widen — the CRIT-D superset proof). If that superset proof were ever invalidated, this parity gate would pass while silently masking a regression vs the original projected set the contract meant. The probe comment MUST record this transitive dependency verbatim so the proof is not misread as standalone.

- [ ] **Step 2: Forward the env var + build + smoke**

Add `MC2_TOBJ_PARITY` to `scripts/run_smoke.py` allowlist. Task 0 Step 5 build; deploy; run `--tier tier1 --duration 30 --kill-existing` with `MC2_TOBJ_PARITY=1`. Grep `tests/smoke/artifacts/<latest>/mc2_*.log` for `[TOBJPARITY v1] event=summary`.

- [ ] **Step 3: GATE — assert zero violations**

`g_tobjParityViolation` MUST be 0 across all tier1 missions. Any nonzero count = a dropped prop = catastrophic = STOP and root-cause (the readback is under-inclusive vs coarse `inView`; check dilation/conservative-OR). **This gate is valid ONLY conjoined with the CRIT-D superset proof (Task 2 Step 1 / the `if(inView)`-gated projection structure): both must hold, or a projected-set regression can pass silently (review MAJOR-B).** Demote-don't-delete the probe after (gate it off by default, keep the code).

- [ ] **Step 4: Commit**

```bash
git add code/terrobj.cpp scripts/run_smoke.py
git commit -m "$(cat <<'EOF'
test(static-decor): [TOBJPARITY v1] superset-parity probe (proof gate #2)

Env-gated (MC2_TOBJ_PARITY) counter: zero (coarseInView && !readback)
violations required (dropped-prop is catastrophic). Over-inclusion is
expected and safe. Counter only; demote-not-delete after the gate.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 8: Substitutive proof gate (user-driven, contamination-immune)

**Purpose:** the campaign bar. Prove the slice is substitutive, not additive: `GameLogic.Units.TerrainObjects` self-time -> ~0 AND total frame time drops, in a user-driven non-`MC2_TERRAIN_COST_SPLIT` total-frame Tracy at worst-case zoomed-out big-map. This is inherently USER-DRIVEN — the user captures the Tracy; the agent does not assert success from a smoke FPS average (`capped_fps_is_not_a_cpu_cost_ab_signal.md`).

**Files:** none (verification only)

- [ ] **Step 1: Request the user-driven before/after Tracy**

Ask the user for a total-frame Tracy capture (NO `MC2_TERRAIN_COST_SPLIT`, latest deployed 0.4 exe, worst-case zoomed-out big-map e.g. wolfman) on the post-Task-7 build. Compare `GameLogic.Units.TerrainObjects` self-time against the ~1.43ms baseline AND total frame time. Required: zone -> ~0 AND total frame drops (anti-mirage: zone->0 alone is never the proof — a displaced cost elsewhere fails the gate).

- [ ] **Step 2: Visual canary**

During the same user-driven session: fast zoomed-out pan across a big map. Required: zero prop/shadow pop/vanish (the Fix-A ghost/streak class = CRITICAL). The user is the visual observer; their first-hand report outranks any silent probe.

- [ ] **Step 3: Record the proof**

If the gate passes, record the measured before/after numbers in `docs/render-perf-snapshot.md` (mark the slice SHIPPED) and in the resolution doc. If it fails (zone moved but total didn't, or a visual regression), STOP — do not claim success; the cost was displaced not eliminated, re-open with the RDTSC split (Task 0) to find where it went.

- [ ] **Step 4: Commit the snapshot update**

```bash
git add docs/render-perf-snapshot.md docs/superpowers/specs/2026-05-18-object-prop-iteration-open-questions-resolution.md
git commit -m "$(cat <<'EOF'
docs(static-decor): record substitutive proof — object/prop iteration GPU port

User-driven total-frame Tracy: GameLogic.Units.TerrainObjects <baseline
1.43ms> -> <measured> self-time, total frame <before> -> <after>. Visual
canary clean (no prop/shadow pop on fast zoomed-out pan). Slice SHIPPED.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Self-review (run before adversarial review)

**1. Spec coverage** (against contract + resolution):
- Substitutive contract "DELETE the projection body" -> Tasks 2, 3. COVERED.
- "REPLACE the render-consumer with readback" -> Tasks 5, 6. COVERED.
- "KEEP UNTOUCHED: cull cascade / lifecycle gate / screenPos consumers re-homed" -> coarse clip kept (Tasks 2/3), `terrobj.cpp:694/697` not structurally changed (Task 4 Step 1 verifies windowsVisible survives), CRIT-1 re-home (Task 4). COVERED.
- Path-A 1-frame-lag avoidance via 89e35ac conservative-OR + dilation -> consumed transparently inside the snapshot build; Tasks 5/6 require `readback_isEnabled()` + fail-open. COVERED (resolution MINOR-6/7 note: no render-site OR-flip needed).
- Preconditions 1-4 commit-verified satisfied (handoff); precondition 5 resolved (Task 0 Step 7 / Q4) — sizing refined, not reopened. COVERED.
- Proof gate: #1 user-driven Tracy (Task 8), #2 superset parity (Task 7), #3 visual canary (Task 8 Step 2), #4 adversarial review (next phase). COVERED.
- Open question 1 (render repoint site) -> Tasks 5/6 narrow site, appear.h untouched. COVERED.
- Open question 2 (CRIT-1 screenPos) -> Task 4 lazy projection. COVERED.
- Open question 3 (lifecycle gate) -> Task 0 RDTSC split + Task 4 Step 1 windowsVisible-survives verification. COVERED.
- Open question 4 (measurement gate) -> Task 0 Step 7 + Task 8. COVERED.
- Resolution CRIT-2 (texture loader side effect) -> Task 1 (gates the delete). COVERED.
- Resolution MAJOR-3 (shadow gate asymmetry) -> Task 6. COVERED.
- Resolution MAJOR-4 (net-win sizing) -> Task 0. COVERED.
- Resolution MAJOR-5 (additive trap) -> framing section + Task 5 "consequential not primary". COVERED.

**2. Placeholder scan:** the lazy projection helper (Task 4 Step 2) intentionally instructs recovering exact `boxCoords` geometry from git history rather than inlining invented math — this is a deliberate "reproduce the deleted code verbatim" instruction with a concrete recovery command, not a TBD. All other steps have concrete code/commands.

**3. Type consistency:** `tobjVisible`/`tobjShadowVisible` (Tasks 5/6) both use `gpu_cull::readback_isEnabled()` + `gpu_cull::readback_isActorVisibleLagged(handle)`; probe tags consistent (`[TOBJSPLIT v1]`/`[TOBJPARITY v1]`); env vars consistent (`MC2_TOBJ_COST_SPLIT`/`MC2_TOBJ_PARITY`). Handle local name flagged for re-grep at every site (scope-dependent).

**windowsVisible question — RESOLVED by the mandated review (no longer an open soft spot):** the design-gate review (round 1, opus, CRIT-A) grep-traced this definitively: `windowsVisible` SURVIVES the projection-body delete because `terrobj.cpp:697 if(inView)` consumes the *returned* coarse `inView` (a strict superset), so the stamp fires for a superset of pick-eligible objects and the `:2477` equality still holds. The resolution doc's original CRIT-1 was traced FALSE and amended to RESOLVED (split verdict: `canBeSeen()`->readback re-expression stands; `windowsVisible` re-home struck). No re-home; comment-only annotations at `terrobj.cpp:699` + `objmgr.cpp:2477`. This is settled — do not re-open.
