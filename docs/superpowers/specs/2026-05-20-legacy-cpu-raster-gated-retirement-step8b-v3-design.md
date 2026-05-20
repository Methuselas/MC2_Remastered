# Step-8b v3: Predicate-hoisted gated retirement — design spec

Advisor: combined `mc2-terrain-indirect-expert` + `mc2-render-expert`.
HEAD: `c3caef3` (claude/nifty-mendeleev), post Tier 1.1. All file:line grep-verified at write-time against this HEAD.
Revision pass 2026-05-20 (rev v3.1): adversarial-review fold-ins applied per `HANDOFF_2026_05_20_stage_0_5_and_step_8b.md` Task #10 — full 20-reader `IsFrameSolidArmed()` census inlined in R-NEW-1, ArmEarly placement pinned in §3, R-NEW-2 CPU sub-cases split, primeMissionTerrainCache ordering grep added to OQ#1, §5 paragraph affirming "F2 changes ZERO per-mission lifecycle paths" added, OQ#2 accessor recommendation hardened to "shall". Reader-census re-greped at HEAD `316749b` (post-savegame-canary commits). The C++ symbols in §3.5's F2 split sketch unchanged.
Supersedes: `2026-05-19-legacy-cpu-raster-gated-retirement-step8b-v2-design.md` (v2).
Preserves: v1 (`2026-05-19-legacy-cpu-raster-retirement-step8b-design.md`) and v2 on disk.

---

## Provenance + diff-from-v2 (the one-paragraph)

v2 proposed gating the per-vertex projection+raster-store inside `Terrain::geometry slimReduce` on the conjunction `clipUsesOnScreen && IsFrameSolidArmed() && IsFrameOverlayArmed()`, evaluated as a per-frame-invariant hoist immediately above the slim loop at `mclib/terrain.cpp:1670`. That gate **never trips** because of a per-frame ordering bug v2 missed: `gosRenderer::endFrame()` at `GameOS/gameos/gameos_graphics.cpp:4384` calls `gos_terrain_indirect::BeginFrame()` (`GameOS/gameos/gos_terrain_indirect.cpp:2286-2299`) at the END of the previous frame's draw, and `BeginFrame()` unconditionally writes `s_frameSolidArmed = false` (`:2298`). The next frame's `Mission::update` -> `land->geometry()` (`code/mission.cpp:509`) enters the slim loop with `s_frameSolidArmed == false`, and the arming write at `gos_terrain_indirect.cpp:2397` / `:2427` from inside `ComputePreflight()` is reached only LATER in the same `Terrain::geometry` call, at `mclib/terrain.cpp:1883` (in the `quadSetupTextures` zone, +213 lines past slimReduce). At slim-loop time, `IsFrameSolidArmed()` returns false every frame regardless of mission state. v3 fixes the ordering by **moving the arming-state DECISION out of the slim-late `ComputePreflight()` point**, so the predicate is correct at slim-loop time. v3 keeps v2's overall gated-retirement shape; only the mechanism by which the gate's predicate is evaluated changes, plus a corresponding update to the risk surface and proof gate.

---

## Greybeard ruling (5 + verdict)

1. **Subsystem pin.** Artifact pinned identically to v2: the ~120-180us PROJ bucket in `Terrain::geometry slimReduce` (`mclib/terrain.cpp:1670-1869`) attributable to `eye->projectForTerrainAdmission(vertex3D, sp)` at `mclib/terrain.cpp:1783`, whose `sp` output feeds the per-vertex raster stores at `mclib/terrain.cpp:1843-1846` and the off-screen sentinel block at `:1850-1852`. Consumer is `TerrainQuad::draw()` (per v1/v2 spec; six emission blocks in `mclib/quad.cpp`). Conjunction gate that skips the consumer on stock-armed-in-mission frames is at `mclib/terrain.cpp:1113-1114`. Pin is grep-verified (see Required Verifications below).

2. **Symptom vs cause.** Symptom: producer (projection+writes) runs every vertex every frame; on stock-armed-in-mission its output is unconsumed because the consumer is gated off. v2's diagnosis of this is correct. v3 adds a SECOND symptom v2 missed: the predicate `IsFrameSolidArmed()` cannot be read at slim-loop time and yield the correct this-frame answer, because the arming-state write is downstream of the read site within the same `Terrain::geometry` body and the upstream reset (`BeginFrame()` in `endFrame()`) is the most recent writer. The cause is `ComputePreflight()` packs TWO responsibilities into one function call: (a) decide whether the frame arms (predicate evaluation), and (b) perform per-frame dispatch side effects (`FlushDirtyRecipeSlotsToGPU()` at `gos_terrain_indirect.cpp:2382`, the GPU-path arming bookkeeping at `:2395-:2407`, and the CPU thin-record packing at `:2411-:2437`). Responsibility (a) needs to be available early; responsibility (b) needs to be where it is.

3. **The meta-fix.** The deeper meta-fix is unchanged from v2 and explicitly NOT done by v3: migrate the unarmed-frame paths (mission intro/outro that aren't already in `Mission::update`, mech-bay, editor) to the GPU indirect path too, so v1's wholesale-delete of `Vertex::px/py/pz/pw` + `TerrainQuad::draw()` body + producer writes can land. That is multi-quarter scope (intro/outro cinematics path + editor renderer init + savegame post-restore parity). v3 retains v2's PATCH stance, with one extra surface: the ComputePreflight split/hoist (option F1 or F2 below). The PRE-EXISTING patch-vs-meta verdict from v2 ("PATCH justified, meta-fix filed, deferral reason = scope") carries forward unchanged.

4. **Substitutive test.** Identical to v2: done = on an in-mission armed stock-perspective frame, `[SLIMSPLIT v1]` `proj_calls_per_frame` is ~0 and the projection call site at `terrain.cpp:1783` executes zero times. Additional v3 gate: confirm the new predicate hoist (F1 or F2) reports `true` on the same frame, AND the existing `IsFrameSolidArmed()` consumer at `terrain.cpp:1113-1114` continues to return `true` for the per-quad draw skip (no regression to the post-`60f2ef8` default-on retirement). Without this second confirmation, the spec cannot distinguish "fix works" from "predicate stalled, gate trips false-positive on a frame where draw still runs."

5. **Verdict.** `PATCH (justified)`. Two sub-rulings:
   - The **gated-retirement** vs **wholesale-delete** choice carries forward from v2: `PATCH (justified)`; meta-fix = unarmed-frame GPU migration; deferral = scope.
   - The **F1 (`WillArmThisFrame()` predicate hoist) vs F2 (`ComputePreflight` split)** choice: `F2 (META-FIX of the in-function responsibility tangle), PATCH for everything else`. Rationale: F2 splits the predicate decision out of `ComputePreflight()` into a separate `ComputePreflightArm()` that runs EARLY (immediately at the top of `Terrain::geometry`, before slimReduce), leaving the dispatch/upload responsibilities in a renamed `ComputePreflightDispatch()` at the existing slim-late call site. F1 introduces a parallel predicate (`WillArmThisFrame()`) that MUST stay in lockstep with the actual arming logic inside `ComputePreflight()` -- the codebase has a documented drift hazard for exactly this pattern (`memory/cpp_glsl_ubo_struct_lockstep.md` and the consolidate-scattered-tuning-constants feedback memory). F2 eliminates the predicate entirely from the read-site (`IsFrameSolidArmed()` already exists and now returns this-frame-correct after the early arm), so no second source can drift. The blast radius F2 worries about (regressing dispatch ordering by splitting) is bounded: `FlushDirtyRecipeSlotsToGPU()` is the only side effect that must stay paired with the arming write, and the GPU-path arming sets `s_frameSolidArmed=true` BEFORE returning (`:2397`) while CPU-path arming sets it at `:2427` AFTER `PackThinRecordsForFrame` and `BuildIndirectCommands` -- those CPU-only paths constitute the only ordering subtlety. F2 handles this by putting the predicate-arming work (env gates + `IsDenseRecipeReady` + `ResourcesReady` + `InMissionTransition` + `IsTerrainSolidEnabled` + `!g_uniqueTerrainNodeIds.empty()`) in `ComputePreflightArm()` and leaving `FlushDirtyRecipeSlotsToGPU` + `PackThinRecordsForFrame` + `BuildIndirectCommands` + the final arming-on-CPU-path write in `ComputePreflightDispatch()`. See section 3.5 for the exact code shape.

---

## 1. Subsystem pin (carry forward from v2; gated retirement, not wholesale delete)

Identical to v2 section 1. The producer (`projectForTerrainAdmission` + the `rv->p[xyzw]` writes) and the consumer (`TerrainQuad::draw()` + `Vertex::p*` fields) are RETAINED. v3 still gates the producer such that on stock-armed-in-mission frames it skips work that would feed a skipped consumer. Editor / ortho / unarmed / cheat-grid / `MC2_TERRAIN_INDIRECT_CPU_FALLBACK=1` paths keep the legacy producer running bit-identical. Per `feedback_stock_playable_is_not_relic_preservation.md`, stock playability is preserved through this gating, not through code preservation of the relic-eligible `MC2_TERRAIN_INDIRECT_OVERLAY=0` revert.

---

## 2. Substitutive contract

### What is GATED (Commit 2 — projection+writes skipped on armed-stock; renumbered, see Section 4)

| # | File:line | Today | After |
|---|---|---|---|
| G1 | `mclib/terrain.cpp:1780-1785` | `if (onScreenR || !clipUsesOnScreen) { ...projectForTerrainAdmission(vertex3D,sp); }` | Wrapped under `if (!skipRaster)` (see Section 3) |
| G2 | `mclib/terrain.cpp:1841-1853` | `if (onScreenR) { rv->p[xyzw] = sp.[xyzw]; } else { sentinel }` | Wrapped under same `if (!skipRaster)` |

`skipRaster` is a per-frame-invariant hoisted ONCE per `Terrain::geometry` call, immediately AFTER the new `ComputePreflightArm()` early-arm point and immediately BEFORE the slim loop. Carry-forward unchanged from v2 in shape; the only difference is that the predicate the hoist reads (`IsFrameSolidArmed()`) is now this-frame-correct.

### What is REFACTORED (Commit 1 — ComputePreflight split; new in v3)

| # | File:line | Today | After |
|---|---|---|---|
| R1 | `GameOS/gameos/gos_terrain_indirect.cpp:2331-2438` | `bool ComputePreflight()` body | Split into `ComputePreflightArm()` (env gates + readiness predicates; sets `s_frameSolidArmed`) and `ComputePreflightDispatch()` (`FlushDirtyRecipeSlotsToGPU` + CPU-path `PackThinRecordsForFrame` / `BuildIndirectCommands` + final CPU-arm write). Detailed in Section 3.5. |
| R2 | `GameOS/gameos/gos_terrain_indirect.h:429` | `bool ComputePreflight();` | Replace with `bool ComputePreflightArm();` + `void ComputePreflightDispatch();` |
| R3 | `mclib/terrain.cpp:1883` | `gos_terrain_indirect::ComputePreflight();` | Replace with `gos_terrain_indirect::ComputePreflightDispatch();` (the old slim-late call site keeps the dispatch responsibilities); add NEW early call `gos_terrain_indirect::ComputePreflightArm();` immediately above the slim-loop hoist point at `terrain.cpp:1670`. |

### What is DELETED (Commit 3 — env-var revert relic retirement; carry forward from v2 unchanged in content)

Same table as v2 D1..D9 (the `MC2_TERRAIN_INDIRECT_OVERLAY=0` revert deletion). v3 keeps this as a separate independently-shippable commit AFTER the gate lands.

### What is KEPT UNTOUCHED

Identical to v2:

- `mclib/vertex.h` `Vertex::px/py/pz/pw` field declarations
- `mclib/quad.cpp` `TerrainQuad::draw()` body
- `mclib/mapdata.cpp:1157` mission-init sentinel write
- `quad.cpp:300-310 / 404-406` `pzv[i]` probe
- `quad.cpp:2092-2151` `MC2_M2D_PZ_PARITY` probe
- `quad.cpp:3874-4663` `drawLine` / `drawDebugCellLine` / `drawLOSLine`
- `terrain.cpp:1116-1133` un-armed per-quad draw() loop
- `GameOS/gameos/gos_terrain_indirect.cpp:1762-1779,1846-1864` `MC2_TERRAIN_INDIRECT_CPU_FALLBACK` path (still reads `vertices[c]->pz`; gated off by skipRaster's `!s_cpuFallbackOn` conjunct, carried forward from v2 4(d))
- `terrain.cpp:1795` cull-cascade write (`rv->clipInfo = clipR`)
- `terrain.cpp:1858` reduction-admission gate (`if (!clipR || !inViewR) continue;`)

---

## 3. The actual gate condition

After the v3 split, `IsFrameSolidArmed()` is this-frame-correct at the slim-loop hoist point. The gate is:

**ArmEarly placement pin (M-1):** the new `ComputePreflightArm()` call is placed in `mclib/terrain.cpp` **AFTER the existing `gos_terrain_indirect::BeginFrameSolidWindow()` call at `:1667`** AND **immediately above the slim-loop `ZoneScopedN("Terrain::geometry slimReduce")` at `:1670`**. The BeginFrameSolidWindow precondition is load-bearing: it resets the per-frame solid-window admit cursor that `ComputePreflightArm` depends on for the GPU-path arming check (`gpu_driven::IsTerrainSolidEnabled() && !g_uniqueTerrainNodeIds.empty()`). Inserting ArmEarly before BeginFrameSolidWindow would arm against stale window state. Inserting it below `:1670` would re-introduce the v2 slim-late-arm bug.

```cpp
// terrain.cpp, inserted AFTER BeginFrameSolidWindow() at :1667 and immediately
// above `ZoneScopedN("Terrain::geometry slimReduce");` at :1670.
gos_terrain_indirect::ComputePreflightArm();  // NEW early call (R3 above)

// per-frame-invariant hoist; identical formula to v2 §4(c) + 4(d) mitigations.
const bool clipUsesOnScreen_hoist =
    (eye->usePerspective && Environment.Renderer != 3);

// One-time read of the CPU-fallback env var; cached for the rest of the process.
// (Alternative: gos_terrain_indirect::IsCpuFallbackEnabled() accessor — see Section 7 open question.)
static const bool s_cpuFallbackOn =
    (getenv("MC2_TERRAIN_INDIRECT_CPU_FALLBACK") != nullptr);

const bool skipRaster =
    clipUsesOnScreen_hoist
    && gos_terrain_indirect::IsFrameSolidArmed()
    && gos_terrain_indirect::IsFrameOverlayArmed()
    && !s_cpuFallbackOn
    && !drawTerrainGrid && !DrawDebugCells && !drawLOSGrid;
```

After Commit 3's deletion of `IsFrameOverlayArmed()` (env-var revert retirement), the conjunction collapses to drop the `&& IsFrameOverlayArmed()` clause; bit-identical on stock because `IsFrameOverlayArmed()` returned `true` unconditionally pre-deletion.

`skipRaster` is consumed at G1 and G2 in Section 2.

**Correctness invariant** (carry forward from v2, re-verified):
- On `skipRaster == true` in stock perspective: `clipUsesOnScreen == true`, so `clipR = clipUsesOnScreen ? onScreenR : inViewR` resolves to `onScreenR` — byte-identical regardless of whether projection ran.
- `inViewR` defaults to `false`. The reduction gate at `terrain.cpp:1858` is `if (!clipR || !inViewR) continue;`. With `inViewR == false`, every vertex falls through `continue` — but the reduction body at `terrain.cpp:1861-1867` is empty except for the `_ssR` RDTSC envelope. No observable effect.
- The cull-cascade write at `terrain.cpp:1795` (`rv->clipInfo = clipR`) and the `objBlockInfo[].active = true` / `objVertexActive[] = true` writes at `:1804`,`:1807` run UNCHANGED on skipRaster frames because they depend on `clipR` not on `inViewR` or `sp`. The `s_solidNarrowOn` append at `:1816-1820` also runs unchanged.

---

## 3.5 NEW: Predicate selection mechanism (F2: ComputePreflight split)

Greybeard verdict above: **F2**, because F1's parallel predicate is a known-drift pattern in this codebase. The split is mechanical and bounded; below is the exact shape.

### F2 target: `GameOS/gameos/gos_terrain_indirect.cpp`

Today's `ComputePreflight()` body (`:2331-2438`) gets split into two functions whose union is byte-identical to the existing body in side-effect order, just with the arming-state writes moved EARLIER.

```cpp
// === NEW: ComputePreflightArm() — predicate-only, no GPU side effects ===
// Called EARLY in Terrain::geometry (before slimReduce). Decides whether
// this frame arms. Idempotent: re-calling it later in the same frame is a
// no-op (it would just re-evaluate the same predicates).
bool ComputePreflightArm() {
    ZoneScopedN("Terrain::IndirectPreflightArm");
    // One-shot lifecycle marker carried forward from existing
    // ComputePreflight() at gos_terrain_indirect.cpp:2334-2345 (parity-infra-
    // retirement lifecycle print). Kept here so the print stays grep-visible.
    {
        static bool s_loggedParityRetired = false;
        if (!s_loggedParityRetired) {
            s_loggedParityRetired = true;
            printf("[TERRAIN_INDIRECT v1] event=parity_infra_retired "
                   "path=gos_terrain_indirect scope=full "
                   "removed=s_packParityMask,ComputeDispatchParity_Check,"
                   "getPackParityMask,txmmgr_call\n");
            fflush(stdout);
        }
    }

    s_frameSolidArmed                = false;
    s_frameSolidPackedThinCount      = 0;
    s_frameSolidCmdCount             = 0;
    s_solidGpuDispatchRanThisFrame   = false;

    if (s_processArmingDisabled) return false;
    if (!IsEnabled()) {
        static bool s_warnedEnabled = false;
        if (!s_warnedEnabled) {
            s_warnedEnabled = true;
            fprintf(stderr, "[TERRAIN_INDIRECT v1] event=preflight_fail reason=not_enabled\n");
            fflush(stderr);
        }
        return false;
    }
    if (!IsDenseRecipeReady()) {
        static bool s_warnedRecipe = false;
        if (!s_warnedRecipe) {
            s_warnedRecipe = true;
            fprintf(stderr, "[TERRAIN_INDIRECT v1] event=preflight_fail reason=recipe_not_ready\n");
            fflush(stderr);
        }
        return false;
    }
    if (!ResourcesReady()) {
        static bool s_warnedResources = false;
        if (!s_warnedResources) {
            s_warnedResources = true;
            fprintf(stderr, "[TERRAIN_INDIRECT v1] event=preflight_fail reason=resources_not_ready\n");
            fflush(stderr);
        }
        return false;
    }
    if (InMissionTransition()) return false;

    if (gpu_driven::IsTerrainSolidEnabled() && !g_uniqueTerrainNodeIds.empty()) {
        s_frameSolidPackedThinCount = -1;   // sentinel: GPU path
        s_frameSolidCmdCount        = 1;
        s_frameSolidArmed           = true;
        static bool s_firstArm = false;
        if (!s_firstArm) {
            s_firstArm = true;
            fprintf(stderr, "[TERRAIN_INDIRECT v1] event=first_arm path=gpu nodeIds=%zu source=ArmEarly\n",
                    g_uniqueTerrainNodeIds.size());
            fflush(stderr);
        }
        return true;
    }

    // CPU path: ARMING decision deferred to ComputePreflightDispatch() because
    // the arm depends on the OUTPUT of PackThinRecordsForFrame /
    // BuildIndirectCommands (the thinCount/cmdCount may be 0). Set a tentative
    // "may arm CPU later" signal; ComputePreflightDispatch confirms or rejects.
    // For predicate-correctness at the slim-loop hoist: the CPU path is NEVER
    // the GPU-indirect cost-dominant case; the slim-loop gate will see
    // IsFrameSolidArmed()==false from this branch and will NOT skipRaster, which
    // is the correct decision (CPU thin-record path still consumes Vertex::p*
    // via the legacy quad draw on its mission frame; do not gate it off).
    return false;
}

// === NEW: ComputePreflightDispatch() — side effects (upload, pack, dispatch) ===
// Called at the EXISTING slim-late call site (terrain.cpp:1883), so all per-frame
// GPU upload + indirect-command-build work runs WHERE IT ALWAYS RAN.
void ComputePreflightDispatch() {
    ZoneScopedN("Terrain::IndirectPreflightDispatch");
    // If ArmEarly already rejected the frame, nothing to dispatch.
    if (s_processArmingDisabled) return;
    if (!IsEnabled()) return;
    if (!IsDenseRecipeReady()) return;
    if (!ResourcesReady()) return;
    if (InMissionTransition()) return;

    FlushDirtyRecipeSlotsToGPU();

    // GPU path already armed by ArmEarly — nothing more to do here. Dispatch
    // happens in ComputeDispatch() further downstream as today.
    if (s_frameSolidArmed) return;

    // CPU fallback path: build indirect commands now. If zero, frame is genuinely
    // unarmed (matches existing semantics at :2412-:2422).
    const int thinCount = PackThinRecordsForFrame();
    if (thinCount == 0) {
        if (traceOn())
            printf("[TERRAIN_INDIRECT v1] event=preflight_skip reason=zero_thin\n");
        return;
    }
    const int cmdCount = BuildIndirectCommands(thinCount);
    if (cmdCount == 0) {
        if (traceOn())
            printf("[TERRAIN_INDIRECT v1] event=preflight_skip reason=zero_cmd\n");
        return;
    }
    s_frameSolidPackedThinCount = thinCount;
    s_frameSolidCmdCount        = cmdCount;
    s_frameSolidArmed           = true;
    static bool s_firstArm = false;
    if (!s_firstArm) {
        s_firstArm = true;
        fprintf(stderr, "[TERRAIN_INDIRECT v1] event=first_arm path=cpu thin=%d cmd=%d source=DispatchLate\n",
                thinCount, cmdCount);
        fflush(stderr);
    }
}
```

### Side-effect order invariant (load-bearing, comment-cite this in code)

Today's `ComputePreflight()` runs side effects in this order at the slim-late call site (`terrain.cpp:1883`):

1. Lifecycle one-shot print (no-op effect).
2. Reset arming state to false (`:2347-:2350`).
3. Process-disabled / IsEnabled / IsDenseRecipeReady / ResourcesReady / InMissionTransition predicates with first-fail warning prints (`:2352-:2380`).
4. `FlushDirtyRecipeSlotsToGPU()` (`:2382`) — GPU upload, runs only if all predicates passed.
5. GPU-path arming write OR CPU-path pack+commands+arming write.

After F2 split:

- `ComputePreflightArm()` runs (1), (2), (3), and the GPU-path arm in (5). NO upload, NO CPU pack.
- `ComputePreflightDispatch()` runs (3) again (idempotent re-check), (4), and the CPU-path part of (5) if needed.

The GPU upload (`FlushDirtyRecipeSlotsToGPU`) is unchanged in placement -- it still runs at the old slim-late point, before `ComputeDispatch()` and before any indirect draw. Predicate (3) runs twice (once in Arm, once in Dispatch); both calls evaluate the same env-gates and readiness predicates which are stable across the slim-loop body (none of the slim-loop writes change `MC2_TERRAIN_INDIRECT`, `IsDenseRecipeReady`, `ResourcesReady`, `InMissionTransition`, `IsTerrainSolidEnabled`, or `g_uniqueTerrainNodeIds.size()`). Double-evaluation is idempotent and safe.

### Recommended trace addition (m-1): arming-transition event

Add a `[TERRAIN_INDIRECT v1]` env-gated one-line trace inside `ComputePreflightArm()` that fires on the EDGE when this-frame's GPU-path arm decision differs from last-frame's. Schema:

```
[TERRAIN_INDIRECT v1] event=arm_transition frame=N prev=<true|false> now=<true|false> cause=<intro_pan|mission_active|outro_pan|unarmed_path>
```

Implementation: file-static `s_prevFrameArmed` cached, compared post-decision, fprintf when changed. Env-gated on the existing `MC2_TERRAIN_INDIRECT_TRACE` (no new env var). Cost: one cached read + compare + conditional fprintf per frame — irrelevant under the 100ns floor. Diagnostic value: lets the Stage 0.5 §3 precondition (worst-case-zoom) and Step-8b user-driven canaries pin exactly which frames take the un-armed path (intro pan vs outro pan vs editor) vs the armed in-mission path.

### Why F2 over F1

F1 (`WillArmThisFrame()` predicate hoist) would declare a parallel predicate next to `ComputePreflight()` that returns true iff `ComputePreflight()` *would* arm if called now, without touching state. Risk: the codebase has a documented pattern of scattered tuning constants / parallel predicates drifting from their source of truth (`feedback_single_source_scattered_tuning_constants.md`, `cpp_glsl_ubo_struct_lockstep.md`). Anyone editing `ComputePreflight()`'s predicate set must remember to mirror the change in `WillArmThisFrame()`. Even with a comment-cite at both sites, drift IS the failure mode for this pattern. F2 eliminates the second predicate entirely; `IsFrameSolidArmed()` is the single source of truth and is now this-frame-correct.

---

## 4. Commit ordering + class-layout discipline

v3 ships in THREE commits, in this order:

**Commit 1 — F2 split.** Refactor `ComputePreflight()` -> `ComputePreflightArm()` + `ComputePreflightDispatch()` per Section 3.5. Update header (`GameOS/gameos/gos_terrain_indirect.h:429`). Update the one call site (`mclib/terrain.cpp:1883`) to call `ComputePreflightDispatch()` and add the early `ComputePreflightArm()` call immediately above `terrain.cpp:1670` (slim loop). NO behavior change for any consumer: `IsFrameSolidArmed()` now returns the same-frame value at any point in `Terrain::geometry` from the early-arm onward, but every existing consumer reads it at slim-late or later (where it was already correct).

Per `feedback_class_layout_change_needs_clean_first.md`: this commit touches function bodies in `gos_terrain_indirect.cpp` and adds two function decls in `gos_terrain_indirect.h`. Header changes are pure additions (no class layout change, no struct change). A standard `cmake --build build64 --config RelWithDebInfo --target mc2` is sufficient; `--clean-first` is NOT required. If smoke regression appears, escalate to `--clean-first` per memory.

**Commit 2 — gate insertion.** Adds the `skipRaster` hoist above `terrain.cpp:1670` and wraps G1, G2. Adds an env-gated lifecycle print `[STEP8B_GATE v1] event=skipRaster_trip frame=N` (one-shot) on the first frame `skipRaster==true`, per the worktree CLAUDE.md debug-instrumentation rule. No header changes. No class layout change. Standard incremental build.

**Commit 3 — env-var revert relic deletion.** D1..D9 from v2 §2 carry forward. Independent from Commits 1 and 2: ships only AFTER the user-driven non-COST_SPLIT total-frame Tracy proof confirms the decal-bake substitutive endpoint per `drawpass_retirement_decal_bake_state_and_raster_sheet_trap.md`. If Commit 3 has to wait, Commits 1+2 stand on their own with the `IsFrameOverlayArmed()` conjunct still in `skipRaster` (bit-identical on stock, since `IsFrameOverlayArmed()` defaults true).

---

## 5. Risk surface

### Carry-forward risks from v2 (still apply, mitigations unchanged)

- **(a) Is `IsFrameSolidArmed()` true on the in-mission intro camera pan?** — v2 §4(a). v3 resolution: with F2, `ComputePreflightArm()` runs BEFORE slim loop on the same frame, so the question is now "does the in-mission intro frame pass the env-gate + readiness predicates and reach the GPU-path arm at `ComputePreflightArm`'s `g_uniqueTerrainNodeIds` check?" YES — first in-mission frame arms via the early call exactly the same way the old slim-late call armed.

  **M-3 ordering grep proof** (mission-load → recipe build → first `Mission::update`):
  - Mission::init calls `land->primeMissionTerrainCache(loadProgress, 4.0f)` at `code/mission.cpp:2263` (inside the load-screen progress sweep, BEFORE Mission::start).
  - `Mission::load` (savegame-restore parallel path) calls it at `code/saveload.cpp:1138` post commit `4008185` (Block A of the Mission::load init-mirror fix).
  - `primeMissionTerrainCache` body at `mclib/terrain.cpp:603-679` invokes `gos_terrain_indirect::BuildDenseRecipe()` at `:646` which populates `g_uniqueTerrainNodeIds`.
  - `Mission::update` (game loop) is called from `code/mechcmd2.cpp:DoGameLogic` AFTER `mission->start()` activates the mission. Both `Mission::init` and `Mission::load` complete fully before `start()` is invoked.
  - `InMissionTransition()` stub returns false unconditionally (grep-verified).

  Together: the GPU-path arm conjunction `gpu_driven::IsTerrainSolidEnabled() && !g_uniqueTerrainNodeIds.empty()` evaluates true on the first `Mission::update` post-load (intro pan frame 0) under the canonical stock-enabled env regime.

  Required write-time grep BEFORE Commit 2 ships: confirm `TerrainQuad::draw()` is called from exactly one site (the conjunction-gated block at `terrain.cpp:1131`). Carry forward v2's grep command.

- **(c) Cheat-flag debug-grid trap** — v2 §4(c). Carried forward: `skipRaster` conjunction includes `!drawTerrainGrid && !DrawDebugCells && !drawLOSGrid`.

- **(d) `MC2_TERRAIN_INDIRECT_CPU_FALLBACK=1` reads `vertices[c]->pz`** — v2 §4(d). Carried forward: `skipRaster` conjunction includes `!s_cpuFallbackOn` (or accessor equivalent per Section 7 OQ).

- **(e) Future GPU-cull-readback adds another `Vertex::p*` consumer** — v2 §4(e). Carried forward: comment-cite at gate site naming this conjunction as the load-bearing list of `Vertex::p*` consumers as of HEAD `c3caef3`.

### R-NEW: F2-specific risks (added by v3)

- **R-NEW-1: ComputePreflight side-effect ordering regression.** Splitting `ComputePreflight()` into two phases means the GPU-path arm (today at `:2397`) now fires from `ComputePreflightArm()` BEFORE `FlushDirtyRecipeSlotsToGPU()` runs (in `ComputePreflightDispatch()`). Is there any consumer between the early-arm and the dispatch-late call that reads `IsFrameSolidArmed()` AND depends on `FlushDirtyRecipeSlotsToGPU` having already run?
  - **Required grep before Commit 1 lands** (re-verify line numbers; symbols stable): `grep -rn "gos_terrain_indirect::IsFrameSolidArmed()" --include="*.cpp" --include="*.h" mclib/ code/ GameOS/gameos/`. For each reader: classify as PRE-DISPATCH SAFE (reader fires before `ComputePreflightDispatch` OR only on CPU-fallback frames where deferred CPU-path arm is the correct semantics) or POST-DISPATCH SAFE (reader fires after dispatch in the per-frame render pass, sees both GPU- and CPU-path arming as final).
  - **Full census at HEAD `316749b` (20 reader sites, all safe under F2):**

    | # | file:line | Call shape | Frame position | Verdict |
    |---|-----------|-----------|---------------|--------|
    | 1 | `mclib/txmmgr.cpp:1964` | `if (IsFrameSolidArmed()) modernHandled = DrawIndirect();` | `renderLists()` flush, post-`Terrain::geometry` | POST-DISPATCH SAFE |
    | 2 | `mclib/terrain.cpp:1113` | `if (!(IsFrameSolidArmed() && IsFrameOverlayArmed())) drawPass();` | Per-quad drawPass gate inside `Terrain::render`, post-dispatch | POST-DISPATCH SAFE |
    | 3 | `mclib/terrain.cpp:1219` | un-armed cinematic per-quad CPU draw gate | Inside per-quad CPU draw loop; only iterates on un-armed frames | PRE-DISPATCH SAFE (CPU-fallback-only) |
    | 4 | `mclib/terrain.cpp:1930` | `if (IsFrameSolidArmed()) ...` | Inside `quadSetupTextures` zone, AFTER `ComputePreflightDispatch` | POST-DISPATCH SAFE |
    | 5 | `mclib/quad.cpp:252` | `BeginLegacySolidCluster() { return !IsFrameSolidArmed(); }` | Per-cluster gate, fires only inside CPU draw loop (un-armed frames) | PRE-DISPATCH SAFE (CPU-fallback-only) |
    | 6 | `mclib/quad.cpp:2100` | `pzNeeded \|\|= (terrainHandle != 0 && !IsFrameSolidArmed());` | Inside `TerrainQuad::setupTextures`, post-dispatch | POST-DISPATCH SAFE |
    | 7 | `mclib/quad.cpp:2211` | `thinEmitArmed = IsFrameSolidArmed();` (CPU thin-emit demotion gate) | Inside `setupTextures`, post-dispatch | POST-DISPATCH SAFE |
    | 8 | `GameOS/gameos/gameos_graphics.cpp:2168` | water non-MDI MVP selection (`armed ? dispatchMvp : terrainMvp`) | Inside `renderWaterFastPath`, post-`Terrain::geometry` | POST-DISPATCH SAFE |
    | 9 | `GameOS/gameos/gameos_graphics.cpp:2328` | water MDI MVP selection | Same path | POST-DISPATCH SAFE |
    | 10 | `GameOS/gameos/gameos_graphics.cpp:2354` | `reflOn = (armed && atlasTex != 0);` | Same path | POST-DISPATCH SAFE |
    | 11 | `GameOS/gameos/gameos_graphics.cpp:2360` | trace reason string (`!armed ? "solid0" : ...`) | Same path | POST-DISPATCH SAFE |
    | 12 | `GameOS/gameos/gameos_graphics.cpp:2752` | `const bool armed = IsFrameSolidArmed();` (PR-1 continuous-surface) | Post-`Terrain::geometry` | POST-DISPATCH SAFE |
    | 13 | `GameOS/gameos/gameos_graphics.cpp:7387` | fixB MVP for `TerrainOverlaysStatic` | Post-`Terrain::geometry` | POST-DISPATCH SAFE |
    | 14 | `GameOS/gameos/gameos_graphics.cpp:7499` | fixB MVP for `DrawDecalStatic` | Post-`Terrain::geometry` | POST-DISPATCH SAFE |
    | 15 | `GameOS/gameos/gameos_graphics.cpp:7565` | fixB MVP for decals (alpha) | Post-`Terrain::geometry` | POST-DISPATCH SAFE |
    | 16 | `GameOS/gameos/gos_terrain_indirect.cpp:3302` | `if (!IsFrameSolidArmed()) return;` (bridge draw gate) | Inside indirect bridge draw, far post-dispatch | POST-DISPATCH SAFE |
    | 17 | `GameOS/gameos/gos_terrain_water_stream.cpp:1153` | `if (IsFrameSolidArmed()) { ... narrow window ... }` in `BuildQuadWindowSSBO` | GPU water-stream compute dispatch, post-`Terrain::geometry` | POST-DISPATCH SAFE |
    | 18 | `GameOS/gameos/gos_terrain_water_stream.cpp:1410` | water MVP selection | Inside `renderWaterFastPath` | POST-DISPATCH SAFE |
    | 19 | `GameOS/gameos/gos_terrain_water_stream.cpp:1454` | `const bool armed = IsFrameSolidArmed();` (diagnostic livecam/water FP) | Inside `renderWaterFastPath` | POST-DISPATCH SAFE |
    | 20 | `GameOS/gameos/gos_terrain_water_stream.cpp:1526` | `const bool armed2 = IsFrameSolidArmed();` (second diagnostic block) | Inside `renderWaterFastPath` | POST-DISPATCH SAFE |

    Zero readers fall in the failure mode "post-dispatch-required + reads true pre-dispatch". F2 split is behaviourally safe.
  - **R-NEW-1 mitigation:** comment-cite this census table in `ComputePreflightArm()`'s docstring so a future reader adding a new consumer in the slim loop knows to verify against the contract. Any new reader fitting the failure mode (slim-loop-time consumer that requires `FlushDirtyRecipeSlotsToGPU` to have run) re-opens R-NEW-1.

- **R-NEW-2: CPU-path arming is now DEFERRED to dispatch-late.** Two distinct sub-cases of "CPU-path frame" (M-2: enumerated explicitly per adversarial review):

  - **(a) Explicit CPU fallback: `MC2_TERRAIN_INDIRECT_CPU_FALLBACK=1` operator opt-in.** Read at file-static cached `s_cpuFallbackOn` (Section 3); when set, the operator has explicitly requested the legacy CPU raster path. `skipRaster` correctly stays false on these frames via the `!s_cpuFallbackOn` conjunct independently of arming.
  - **(b) Implicit CPU fallback: `g_uniqueTerrainNodeIds.empty()` (legacy `terrainTextures` map regime).** When the dense recipe is unavailable (mission that pre-dates the GPU recipe, or recipe build skipped), `ComputePreflightArm` cannot take the GPU-path arm branch. The frame falls through to the CPU-path arm in `ComputePreflightDispatch` (Section 3.5). `IsFrameSolidArmed()` returns false at slim-loop time on these frames — correct, since the CPU draw consumer at `terrain.cpp:1131` legitimately needs `Vertex::p*` populated.

  Both sub-cases are CORRECT under F2: the slim-loop hoist's `IsFrameSolidArmed()` returns false for both, `skipRaster` stays false, and the producer runs as it did pre-Step-8b. Risk: someone adding a NEW early consumer of CPU-path arming gets a stale (last-frame) read. Mitigation: comment in `ComputePreflightArm()` explicitly stating "CPU-path arming is set in ComputePreflightDispatch(); early read of `IsFrameSolidArmed()` reflects ONLY the GPU-path arming decision (sub-case (a) above takes the early `g_uniqueTerrainNodeIds.empty()` exit; sub-case (b) operator-opt-in is independent of arming). CPU-fallback callers must read it at the slim-late dispatch point or later."

- **R-NEW-3: Predicate double-evaluation in Dispatch is harmless but observer-visible in first-fail warning prints.** Existing `s_warnedEnabled` / `s_warnedRecipe` / `s_warnedResources` static-once-print idioms are unchanged in semantics, but a frame that fails the predicate in ArmEarly also fails it again in DispatchLate. Both call sites would attempt to set the `s_warned*` flag; since the flag is `static bool`, the second call is a no-op. Net effect: prints still fire at most once per process, just sourced from whichever call ran first. No regression. **Perf cost (m-3):** the duplicated predicate set (`IsEnabled`, `IsDenseRecipeReady`, `ResourcesReady`, `InMissionTransition`) is five branch-predictable boolean function calls per-frame — well under the worktree CLAUDE.md "100ns floor" for instrumentation regions and orders of magnitude below the slim-loop body it gates. Non-issue.

### NOT a v3 risk (explicitly enumerated)

- F1 drift hazard: NOT applicable; v3 chose F2.
- Class-layout change requiring `--clean-first`: NOT a risk for v3 because no header `struct` / `class` body changes; only function decls added.
- **Parallel-mission-setup-path drift (M-4):** F2 changes ZERO per-mission lifecycle code paths. The split refactors `ComputePreflight()` ↔ `ComputePreflightArm()` + `ComputePreflightDispatch()` entirely within `GameOS/gameos/gos_terrain_indirect.cpp` (R1) and adds one new call site in `Terrain::geometry` per-frame path (R3). It does NOT touch `Mission::init`, `Mission::load`, `Mission::destroy`, `Mission::start`, `Logistics::beginMission`, or any of the parallel mission-setup paths catalogued in `memory/parallel_mission_setup_paths_probe_which_one.md` and `memory/mission_load_inits_mirror_init_per_subsystem.md`. The arming-state lifecycle reset is owned by `BeginFrame()` at `gos_terrain_indirect.cpp:2298` (called at `gosRenderer::endFrame()` of the prior frame); F2 does not alter that. Per-mission init/teardown of the gpu_cull substrate / compute / readback subsystems and `gos_terrain_indirect` recipe state is also untouched — those flow through `Mission::init`/`Mission::load`/`Mission::destroy` independently of where `ComputePreflightArm` runs within a frame.

---

## 6. Substitutive proof gate

### Automated gates (per-commit)

- Tier1 smoke 5/5 (`py -3 scripts/run_smoke.py --tier tier1 --duration 30 --kill-existing --keep-logs`); no `--with-menu-canary`; `GL_INVALID_OPERATION=0`. Run after EACH commit independently (1, 2, 3) per worktree CLAUDE.md smoke discipline.
- Build clean (RelWithDebInfo). Commit 1: standard incremental build (no class-layout change). Commit 2: same. Commit 3: same.
- `[STEP8B_GATE v1]` env-gated print (added in Commit 2) one-shot fires on the first armed-in-mission frame post-deploy; mission-start tier1 sample is sufficient.

### `[SLIMSPLIT v1]` PROJ-bucket retirement check (the load-bearing perf gate)

This is the v3-specific addition demanded by the dispatch. Re-arm `[SLIMSPLIT v1]` via `MC2_TERRAIN_COST_SPLIT=1` for ONE diagnostic mission post-Commit-2 deploy. Read `proj_calls_per_frame` from the summary line:

- **BEFORE Commit 2:** `proj_calls_per_frame` ~= `numberVertices` (every vertex projected; ~120-180us PROJ bucket).
- **AFTER Commit 2 on armed-in-mission stock-perspective frame:** `proj_calls_per_frame` ~= 0 (gate trips). If this value is NOT ~0, the F2 fix isn't actually flipping `IsFrameSolidArmed()` at slim-loop time; abort Commit 2 and investigate.
- **AFTER Commit 2 on intro / outro / editor / unarmed frame:** `proj_calls_per_frame` unchanged at ~`numberVertices` (gate does NOT trip; producer keeps running for the legacy CPU draw consumer).

Demote `[SLIMSPLIT v1]` after the gate confirms, per `cost_split_instrumentation_is_observer_effect_dominated.md`. The post-demote total-frame Tracy step below is the load-bearing perf endpoint.

### User-driven substitutive proof (load-bearing, non-`COST_SPLIT`)

- Non-`COST_SPLIT` clean total-frame Tracy at worst-case zoomed-out corner-of-map camera (per `feedback_terrain_surface_campaign_execution_ops.md` and `feedback_cost_split_worst_case_camera.md`). BEFORE vs AFTER Commit 2.
  - Expected: `Terrain::geometry slimReduce` zone self-time drops by the PROJ bucket charter (~120-180us); total frame drops comparably; no displaced cost in any other zone (per `feedback_offload_must_be_substitutive_not_additive.md`).
- Visual canary: mission-intro camera pan on a cement-heavy mission (`mc2_01`) — no terrain corruption during the pan; gameplay normal after. Repeat on decal-heavy mission (`mc2_24`).
- Mission outro: complete a mission, confirm post-mission camera shows no terrain corruption.
- Editor canary: open the MC2 mission editor against a stock map; terrain renders correctly. (Editor uses `!eye->usePerspective` => `clipUsesOnScreen_hoist=false` => `skipRaster=false` => same path as today.)
- Savegame-restore canary: load a saved game; terrain renders correctly on the first frame post-restore.

### Forbidden as "done" signals (carry forward from v2)

- smoke-PASS alone (per `smoke_pass_does_not_verify_gpu_driven_shader_fix.md`)
- `MC2_TERRAIN_COST_SPLIT` absolutes (observer-effect dominated)
- capped FPS A/B (per `capped_fps_is_not_a_cpu_cost_ab_signal.md`)
- zone-local Tracy without total-frame validation

---

## 7. Open questions

1. **R-NEW-1 grep verification of `IsFrameSolidArmed()` readers before Commit 1.** This session's grep enumerated current readers but the spec implementer must re-run before landing Commit 1 in case a new reader has been added since `c3caef3`. Grep command:
   ```
   grep -rn "IsFrameSolidArmed\b" --include="*.cpp" --include="*.h" \
       mclib/ code/ GameOS/gameos/ shaders/
   ```
   For each reader: classify as pre-dispatch-safe or post-dispatch-required.

2. **`gos_terrain_indirect::IsCpuFallbackEnabled()` accessor vs raw `getenv` in `skipRaster` hoist.** v2 §4(d) recommended the accessor. v3 **shall add the accessor in Commit 2** (m-2: hardened from "recommendation" to "shall" per adversarial review). The in-spec sketch at Section 3 uses `static const bool s_cpuFallbackOn` (raw getenv, cached) as fallback shape, but Commit 2 lands the accessor in `gos_terrain_indirect.h` alongside the existing `IsFrameSolidArmed` / `IsFrameOverlayArmed` accessors and `skipRaster` reads it via the accessor. Rationale: documents the cross-TU dependency, matches existing accessor pattern, single source-of-truth for the env-var-resolution, eliminates the second cached static.

3. **Editor renderer init path.** Confirm at write-time that the editor binary builds from this same source tree and reaches `Terrain::geometry`. If it uses a separate renderer init that never calls `Terrain::geometry`, the editor canary in Section 6 is a no-op (which is fine). Carry forward from v2 §7(2).

4. **`Environment.Renderer != 3` invariant.** `terrain.cpp:1779` asserts in-comment that `Environment.Renderer is only ever 0 in stock.` Re-grep at write-time. Per `feedback_offload_scope_stock_only.md`, mod paths where Renderer == 3 are explicitly out of scope. Carry forward from v2 §7(3).

5. **Cheat-flag mutability.** Confirm `drawTerrainGrid`, `DrawDebugCells`, `drawLOSGrid` are loop-invariant within `Terrain::geometry` (keypress-mutated only). Grep for writes during write-time. Carry forward from v2 §7(4).

6. **DEFERRED meta-fix.** Unarmed-frame GPU migration (mission intro/outro that aren't already in `Mission::update`, mech-bay, editor) — the deeper meta-fix that would allow v1's wholesale-delete. NOT in v3 scope; file as separate planning item once the v3 gated-retirement is fully proven and the cost has been recovered.

---

## File

`docs/superpowers/specs/2026-05-20-legacy-cpu-raster-gated-retirement-step8b-v3-design.md`
