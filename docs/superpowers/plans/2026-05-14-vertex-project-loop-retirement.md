# `VertexProjectLoop` Retirement — Implementation Plan v3

> **Plan v3 amendment note (2026-05-14):** v3 lands the pre-execution amendments closing the re-review's READY-WITH-MINOR-FIXES verdict (`docs/superpowers/reviews/2026-05-14-vpl-retirement-rereview.md`). Key shifts from v2: (D1) Step 5 committed to a single approach — **5B quad-AABB-only**; the 5A and 5C option blocks are deleted. Step 6 and Step 8a are unbranched accordingly. (D2) Step 7 hazeFactor port is rewritten — the mission-load edge-flag-bit precompute is dropped in favor of inline-worldPos edge-clamp evaluated on the GPU per-vertex; the play-area bounds (`Terrain::IsGameSelectTerrainPosition`, `terrain.cpp:687-700`) are exposed as a new `vec4 g_playAreaBounds` uniform and the distance-haze constants (`Camera::MinHazeDistance`, `Camera::MaxClipDistance`, `Camera::DistanceFactor`) as scalar uniforms. The change supersedes NEW-MED-1 (mission-load timing) entirely and additionally fixes the existing out-of-bounds-water-renders-unfogged bug because the inline-worldPos check is pipeline-agnostic. (D3) NEW-MED-2 triage order: a dedicated subsection in §Verification documents the `mc2_10` shared-canary triage protocol for Step 5 vs Step 8c simultaneous regressions, and explicitly calls out `beginFrameTexResolve` at `terrain.cpp:1402-1405` as a load-bearing init that survives VPL retirement. (D4) NEW-LOW-1 water-stream barrier sibling timing is cross-referenced in the cmd-patch plan as a tracked future slice. Final plan is still 10 steps.

> **Plan v2 amendment note (2026-05-14):** v2 lands the adversarial-review-driven amendments off the BLOCK verdict in `docs/superpowers/reviews/2026-05-14-vpl-retirement-adversarial-review.md`, advisor briefings (`mc2-cpu-gpu-offload-expert`, `mc2-shader-expert`, `mc2-render-perf-expert`), and grep-verified file:line corrections. Key shifts from v1: (a) `hazeFactor` now has a dedicated GPU-port step (CRIT-1 resolution) as new step 7; (b) the cull-cascade step is reframed as "wrap-and-reduce" with three options for the orchestrator to choose from in a follow-up turn, eliminating Path C entirely; (c) mouse-picking step expanded to also cover `Camera::inverseProject` tile walk + tacmap viewport unprojection; (d) shared-reductions step keeps a slim CPU pass alive instead of trying to retire it; (e) step 1 framing corrected — `clipInfo` survives step 1, only `pz` writes retire; (f) loop-body deletion split into three commits; (g) Fix A scaffolding demoted behind `MC2_RING_TRACE=1` (user decision) rather than deleted; (h) soak parallelization documented; (i) verification gates standardized. Final plan is 10 steps, not 9.

> **Foundation:** This plan completes the 2026-04-13 projectZ peeling arc whose previous stages routed all 99 forward + 4 inverse callsites through 8 categorical wrappers and deprecated raw `projectZ()` (`docs/superpowers/specs/projectz-policy-split-report.md`). The CPU-side D1 hoist slice (`memory/vertexproject_loop_asymptotic.md`) closed at 2026-04-30 with ~0% mean improvement on a 96M-vertex sweep — the function is compiler-ceiling-bound; the only remaining lever is **elimination**.
>
> **Architectural unlock:** Fix B (commit `005ebc7`) moved per-corner projection into `gpu_driven_terrain_solid.comp` for the indirect terrain path. The GPU is now the projection authority for every terrain quad in the production render path. CPU `vertices[c]->pz` is redundant for every consumer that lives downstream of the indirect SOLID dispatch.
>
> **Recon report:** Consumer enumeration done 2026-05-14 (this plan's recon agent). Adjacent-field coupling table in §3. Methodology guidance from `mc2-cpu-gpu-offload-expert` advisor invocation, same date.

**Goal:** Retire `VertexProjectLoop` and its per-vertex CPU projection state (`pz`, `px`, `py`, `pw`, `clipInfo`, `hazeFactor`) by retiring its consumers, not by porting the loop to GPU. ~475 µs/frame of CPU recovered (D1 baseline measurement). Downstream unlock: the object/prop iteration loop's cull-cascade dependencies on `objBlockInfo[]` and `objVertexActive[]` (which VPL also writes) become free to move to GPU AABB hit-tests.

**Path selected:** Path A (consumer retirement). Path B (GPU port + readback) rejected per advisor — would reintroduce the 1-frame-lag bug class Fix A/B just eliminated (`memory/substrate_coalesce_sync_point_lesson.md`). Path C (dirty-flag skip) reserved as fallback if a consumer cannot be retired or repointed.

---

## Out of scope

- **GPU port of VertexProjectLoop itself.** No new compute shader is added; the GPU compute that already projects every quad corner (`gpu_driven_terrain_solid.comp` post-Fix-B) is the surviving projection authority.
- **`lightRGB` / `fogRGB` retirement.** Separate writer (`TerrainQuad::setupTextures` at `quad.cpp:1362-1605`); separate workstream. Comment at `quad.cpp:1299` references an existing GPU writeback path ("CopyResultsToVertexPool") — retired as part of `quadSetupTextures-gpu-compute-port.md` Phase 1, not this plan.
- **`vertexNum` retirement.** Init-time only at `mapdata.cpp:1114-1119`; permanent state, not per-frame transient.
- **Mouse picking rewrite to GPU AABB hit-test.** Picking is rewritten in step 3 to compute its own projection on demand via the existing `inverseProjectForPicking` wrapper (`mclib/camera.h:634-637`; the `:422-622` range cited in v1 is the forward-projection wrappers, not the inverse-projection ones — corrected per adversarial review MINOR finding). The GPU AABB hit-test (`memory/gpu_mech_aware_mouse_pick_queued.md`) is the **downstream unlock** this plan enables but does not implement.
- **Object/prop iteration loop port.** Downstream consumer of `objBlockInfo[]` / `objVertexActive[]` (which VPL writes as side effects). This plan retires VPL's writes once the loop's consumers are themselves retired or repointed. The iteration loop's own GPU port is a separate plan that this plan unblocks.

---

## Architectural references (read before opening any phase)

1. **`docs/plans/2026-04-13-gpu-projection-migration-design.md`** — the strangler-fig design that started the projectZ peeling. Read for the philosophical foundation.
2. **`docs/superpowers/specs/projectz-callsite-inventory.md`** — 99+4 callsite inventory with stable `[PROJECTZ:Category id=...]` markers. Line numbers may have drifted; markers haven't.
3. **`docs/superpowers/specs/projectz-policy-split-report.md`** — the 8-wrapper split. `mclib/camera.h:422-622` is the implementation surface. Raw `projectZ()` is `[[deprecated]]`.
4. **`memory/vertexproject_loop_asymptotic.md`** — D1 closure measurement; rationale for elimination over speedup.
5. **`memory/ring_slot_state_must_travel_with_slot.md`** — Fix A / Fix B retirement context. Once VPL's pz writes are gone, there's only one projection authority.
6. **`memory/cull_gates_are_load_bearing.md`** — `inView` / `canBeSeen` / `objBlockInfo` / `objVertexActive` cascade. Critical: VPL writes `objBlockInfo[]` and `objVertexActive[]` as side effects; the retirement sequence must NOT orphan these consumers.
7. **`memory/substrate_coalesce_sync_point_lesson.md`** — why GPU port + readback (Path B) is rejected.
8. **`memory/parity_finds_gpu_substrate_bugs_visual_smoke_misses.md`** — visual smoke passes while data flow is wrong. Trust the structured verification, not just the visual.
9. **`memory/feedback_data_flow_audit_asymmetry.md`** — every "X is NOT consumed by Y" claim in this plan must be verified by greppping Y, not just X.

---

## Load-bearing constraints

- **Interleaved sequencing.** Each commit retires both a consumer AND the field-write that fed it. No commit leaves the loop running with retired consumers (deferred savings); no commit removes a field-write while consumers still read it (orphaned reads). Advisor's "retire the function, not the field" guidance — every commit answers the question "is the loop body provably smaller after this commit?"
- **Cull-cascade safety.** VPL writes `objBlockInfo[].active` and `objVertexActive[]` per `terrain.cpp:1547-1571` (recon citation; grep-verify at write time). These feed object/prop iteration's visibility decisions. The retirement sequence must NOT delete these writes until the downstream object iteration is itself retired or repointed. See §4 step 5.
- **Shared file-scope reductions.** VPL contributes to `leastZ/mostZ/leastW/mostW/leastWY/mostWY` reductions per `terrain.cpp:1549-1552` (recon citation; verify). Three writers exist (water path in `quad.cpp`, non-water in `terrain.cpp:1549-1552`, legacy fallback in `terrain.cpp:1696-1715` per recon). Retiring VPL's contribution requires either: (a) retiring the reduction itself if its consumers are gone, (b) keeping VPL alive for that contribution while retiring its other outputs, or (c) jointly porting all three writers. Decision point §4 step 5.
- **No env-gated dead code.** Per advisor + `memory/mc_texture_manager_dual_queue_legacy_retirement_debt.md`: do NOT keep VPL alive behind an env var "for parity." Either it dies cleanly (Path A complete) or it stays alive as primary code (Path A blocked). The middle ground is the additive-debt anti-pattern.
- **Soak windows.** Per `track_b_widen_static_prop_registry.md` precedent: 7-day soak between major retirement commits and dependent retirement commits. No back-to-back same-day retirements.
- **`projectZ()` is deprecated but not deleted.** Calls via the 8 wrappers stay; raw `projectZ()` is `[[deprecated]]`. This plan does NOT touch `projectZ()` itself — that retirement comes after all wrappers are themselves retired (separate, much-later plan).

---

## Consumer / writer table (recon 2026-05-14 — verify at write-time per commit)

### Writers of `vertices[c]->pz`

| Site | File:Line | Status |
|---|---|---|
| `vertexProjectLoop` fast path | `mclib/terrain.cpp:1559` | **Retirement target** — written every frame for every vertex |
| `vertexProjectLoop` legacy fallback | `mclib/terrain.cpp:1689` / `:1700` | **Retirement target** — same data |
| `Clouds::vertex.pz` | `mclib/clouds.cpp:216` | **Out of scope** — distinct struct, separate consumer chain |

### Consumers of vertex `pz` (production paths)

| Consumer | File:Line | Class | Retirement strategy |
|---|---|---|---|
| `TerrainQuad::setupTextures` M2 thin emit | `quad.cpp:2089-2094` | (a) Chopping block | Dies with CPU pack retirement |
| Legacy SOLID/TES `gVertex` emit | `quad.cpp:2301-2824` | (a) Chopping block | Dies with `MC2_TERRAIN_INDIRECT=0` legacy retirement |
| Indirect terrain dispatch pz cull | `gos_terrain_indirect.cpp:1639-1656` | (c) Re-derivable | Same logic already runs in GPU compute; CPU mirror retired with CPU pack |
| Mask-dispatch pz cull | `gos_terrain_mask_dispatch.cpp:211-223` | (c) Re-derivable | Same as above |
| **`Camera::inverseProject` (mouse picking)** | `camera.cpp:797-822` | (b) Survivor | **Repoint to `projectForSelectionPicking` wrapper** for tiles under cursor; ~50 µs per click vs ~500 µs every frame |
| `pz_emit_terrain_tris` trace | `quad.cpp:357` | (d) Debug/LAB | Already env-gated; not in production |
| Debug-line draws | `quad.cpp:3667-3805` | (d) Debug | Not production-reachable |

### Adjacent fields written by VPL (same retirement bundle)

| Field | Production consumers | Strategy |
|---|---|---|
| `vertex->px` / `vertex->py` | Same set as `pz` | Retire with `pz` |
| `vertex->pw` | Debug-line draws only (d) | Retire with `pz` |
| `vertex->clipInfo` | `Camera::inverseProject` (b) + parity (a) + debug (d) | Picking repoint repoints both `pz` and `clipInfo` reads |
| `vertex->hazeFactor` | Per recon: read site not located. Suspected `quad.cpp` fog packing | **Pre-commit: grep `hazeFactor` opposite-direction before retiring its write** |
| File-scope `leastZ/mostZ/...` reductions | Unknown without further recon | See step 5 — separate audit |
| `objBlockInfo[].active` / `objVertexActive[]` | Object/prop iteration cull cascade | Step 5 — downstream unlock |

---

## Interleaved retirement sequence

Each step is one commit unless noted. Each commit lands with:
- Tier1 smoke pass at default config (`--tier tier1 --duration 30 --kill-existing`)
- Adversarial review for any commit touching the cull cascade
- Memory update if the commit closes or supersedes a memory entry

### Step 1: CPU pack retirement (already queued — see separate prompt)

**Deletes / demotes:** retires the per-vertex `pz` reads in the M2 thin emit at `quad.cpp:2089-2106` and the indirect-pack pz cull at `gos_terrain_indirect.cpp:1638-1656`; demotes `PackThinRecordsForFrame` body behind `MC2_TERRAIN_INDIRECT_CPU_FALLBACK=1` (default-off) rather than deleting it (CPU pack remains the only GPU-arm-failure fallback today; demote-not-delete per user decision); retires `s_shadow`, `s_packParityMask`, `ComputeDispatchParity_Check`, parity infrastructure as in v1.

**Scope correction (A5):** v1 framing said step 1 "retires `pz`/`clipInfo` writes." That was wrong. `clipInfo` has a SECOND writer at `quad.cpp:1021/1023/1088/1090/1155/1157/1222/1224` (per-corner clip logic) that survives VPL retirement entirely and feeds `quad.cpp`'s own clip-decision logic at `:387-393/:719-720/:803-804/:983-989`. CPU pack retirement does NOT orphan `clipInfo`. Step 1 retires only the per-vertex `pz` writes that the M2 thin emit and the indirect-pack pz cull consume. `clipInfo` is untouched by step 1; it is handled in step 3 (picking repoint) where the `inverseProject` consumer goes away.

**This plan's contribution:** none — this commit ships independently. Marks consumers (a) at `quad.cpp:2089-2094` and (c) at `gos_terrain_indirect.cpp:1638-1656` as gone. VPL's `pz` write is now read by fewer consumers; loop stays alive.

### Step 2: Cmd-patch dispatch retirement (already queued)

**Deletes:** the second `glDispatchCompute` for cmd-patch + bucket header dependency.

**This plan's contribution:** none — independent.

### Step 3: Mouse picking + tacmap repoint (Camera::inverseProject + tacmap viewport)

**Scope correction (B1 — cpu-gpu-offload advisor 2026-05-14, escalation):** v2/v3 framing said Step 3 repoints "BOTH `pz` AND `clipInfo`". That is **under-specified**. `Camera::inverseProject` (`mclib/camera.cpp:749`, body runs to just before `:968`) has **THREE** VPL dependencies, not two (all grep-verified at HEAD `63c9ee7`):

1. `vertices[c]->clipInfo` — tile admission, `camera.cpp:770-773`.
2. `vertices[c]->pz` — closest-tile selection, `camera.cpp:797-822`.
3. `vertices[c]->px` / `->py` — point-in-screen-triangle test inside `overThisTile()` (`camera.cpp:633-640`), called from the tile walk at `camera.cpp:775`.

Missing the `px/py` dependency is the escalation: Step 8b retires `px`/`py`/`pz`/`clipInfo` VPL writes, so **leaving any of the three reads live makes Step 8b silently break picking** (no compile error — `overThisTile` would read stale per-frame `px/py`). The `px/py` dependency (not just `pz`/`clipInfo`) MUST be covered by Step 3.

**Change for (a):** the repoint must replace the **ENTIRE tile-walk + `overThisTile` + closest-pz block as one unit** — `camera.cpp:759-831` (grep-verified: `:760` `numTiles = land->getNumQuads()`, `:768` walk loop, `:770-773` clipInfo admission, `:775` `overThisTile` call, `:797-822` closest-pz selection, block closes at the `else if (currentClosest)` at `:827-831`; the `closestTile`-resolve tail at `:833+` consumes the walk's output and is repointed to the new world-point lookup). Replace with a CPU frustum cull over tile AABBs (same math as Step 5 — both share the camera-frustum × quad-AABB primitive; see "Shared frustum-AABB helper" below) + the existing `inverseProjectForPicking` wrapper (`mclib/camera.h:634-637`, NOT the `:422-622` forward-projection range cited in v1) for the per-cursor screen-to-world ray, then a tile lookup after the unprojected world point lands. **Delete `overThisTile`** — grep-confirmed its only caller is the tile walk at `camera.cpp:775` (sole reference; `camera.cpp:628` is the definition), so it has no surviving caller after the block is replaced.

- (b) Tacmap viewport unprojection at `code/gametacmap.cpp:225/232/239/246` — four callsites calling `inverseProjectForPicking` on viewport corners. Already use the wrapper; zero-cost to keep working. **Requirement:** the wrapper's underlying state (`startZInverse`, `zPerPixel`, etc., set by `setInverseProject` at `terrain.cpp:1891`) must continue to be populated. This couples to Step 6 (shared reductions), which feeds `setInverseProject`. The cross-step dependency is load-bearing — Step 6's slim CPU min/max pass exists in part to keep this wrapper alive.

**Per-click cost:** ~50 µs (one projection, one tile lookup) vs the current per-frame walk over ~64k-256k tiles.

**Per-frame caller correction (B2 — cpu-gpu-offload advisor 2026-05-14):** v2/v3 verification said "confirm drag-select / marquee calls `inverseProject` once per drag-operation, not once per frame." Grep disproves the premise: the only production `Camera::inverseProject` caller is `code/missiongui.cpp:742` (`eye->inverseProject(mouseXY, wPos);`) inside `MissionInterfaceManager::update(void)` (`missiongui.cpp:423`) — that runs **PER FRAME**, not per drag-operation. Consequently the per-frame delta-cache is **MANDATORY infrastructure**, not an optional optimization: cache `(prevMouseX, prevMouseY, cachedWPos)` and invalidate on a camera `worldToClip` change; the on-demand projection only re-fires when the cursor moved or the camera moved. **Caller inventory (grep-verified):** the sole `Camera::inverseProject` production caller is per-frame `missiongui.cpp:742`; tacmap (`gametacmap.cpp:225/232/239/246`) uses `inverseProjectForPicking`, which under `usePerspective == true` does NOT enter `Camera::inverseProject` (it routes through `inverseProjectZ`'s perspective branch) and reads no vertex VPL field.

**Shared frustum-AABB helper (B3 — defined by Step 3, referenced by Step 5B).** Step 3 (`Camera::inverseProject` repoint) and Step 5B (slim cull-cascade pass) both need a CPU camera-frustum × quad-AABB intersection. Specify it once as two `Camera` members:

- `Camera::extractFrustumPlanes(float planes[6][4]) const` — Gribb-Hartmann 6-plane extraction from `Camera::worldToClip` (grep-verified: `worldToClip` is a `Stuff::Matrix4D` member at `mclib/camera.h:138`). Pure CPU, no GL.
- `Camera::quadAabbInFrustum(const float planes[6][4], const Stuff::Vector3D& mn, const Stuff::Vector3D& mx) const` — conservative AABB-vs-frustum test (negative-rejection safe; never false-negative). Pure CPU, no GL.

**Location:** inline decls in `mclib/camera.h` near the projection wrappers; bodies in `mclib/camera.cpp` near `inverseProject`. Members of `Camera` so Step 3 (camera.cpp) and Step 5B (terrain.cpp, via the `eye` / `Camera*` already in scope) share one implementation. Co-located, no new translation unit.

**Ownership pin (merge-conflict avoidance):** **Step 3 commit 3a DEFINES the helper; Step 5B REFERENCES it.** Step 5 must NOT re-add it. Per current plan numbering Step 3 lands first and owns the definition. (Mirror note in Step 5 — see B5 cross-link there.)

**Step 3 commit shape (B4):** two commits.
- **3a:** shared frustum-AABB helper (`extractFrustumPlanes` + `quadAabbInFrustum`) + `Camera::inverseProject` repoint (replace `camera.cpp:759-831`, delete `overThisTile`) + env-gated `[VPL_PICK v1]` lifecycle print per the worktree CLAUDE.md debug-instrumentation rule.
- **3b:** the per-frame caller delta-cache at `missiongui.cpp:742` (per B2 — mandatory infra).

**MANDATORY pre-flight gate (B4 — `usePerspective` recursion):** before relying on `inverseProjectForPicking`, the implementer MUST grep-confirm `usePerspective` is unconditionally true on the in-game (non-editor) path. Rationale: `inverseProjectForPicking` → `inverseProjectZ`, and `inverseProjectZ`'s `!usePerspective` branch at `camera.cpp:1884-1892` recurses into `inverseProject` — the function being retired. If any in-game mode can be orthogonal, the repoint MUST break that recursion or explicitly exclude the editor path. **Escalate to orchestrator + `mc2-render-expert` if ambiguous.** Cross-ref `memory/camera_model_oblique_cinematic.md` (MC2 camera is perspective oblique-cinematic, so this should resolve clean) — but it MUST be grep-verified, not assumed.

**Verification:** tier1 smoke `--tier tier1 --duration 30 --kill-existing` + manual UAT on `mc2_01` / `mc2_10` / `mc2_17`: cursor placement (move-marker exactly under cursor), marquee drag-select (exact selection set), salvage/build placement ghost cell-accurate, AND tacmap F-key viewport trapezoid unchanged vs baseline (negative control proving Step 3 did NOT touch the tacmap path).

**Soak:** 7 days. Picking regressions are subtle (off-by-one cursor positioning) and may only show on specific mission layouts. Runs concurrently with Step 5 + Step 8c soaks per §"Soak window parallelization" below.

### Step 4: Mask-dispatch + parity-mask `pz` reads retire

**File:** `gos_terrain_mask_dispatch.cpp:211-223` (recon citation; verify).

**Strategy:** the mask-dispatch's pz cull is structurally identical to the indirect dispatch's pz cull; same lambda lives in `gos_terrain_indirect.cpp`. If mask-dispatch is reachable in production (recon flagged uncertainty), repoint to use the recipe SSBO + on-CPU one-shot projection per quad considered. Otherwise mark dead and skip.

**Pre-commit grep:** confirm mask-dispatch's actual reachability. If it's reached only under env-gated paths (`MC2_TERRAIN_MASK_DISPATCH=1`?), document and either gate-default-off → unreachable, or include the retirement.

### Step 5: Reduce VPL to a slim cull-cascade pass (quad-AABB-only — option 5B committed)

**Reframe (replaces v1's audit-and-retire blocker):** do NOT retire the cull-cascade side-effects (`objBlockInfo[].active` + `objVertexActive[]` updates). Per advisor + reviewer, those writes have ~9 live consumers in `code/objmgr.cpp:1697/1704/1828/1835/2019/2027/2610/3060/3067` (production object-iteration loops) and MUST keep firing. Instead, keep the side-effect writes alive in a slimmed-down CPU pass while retiring the per-vertex projection math that surrounds them.

**Approach committed: 5B quad-AABB-only.** v2 carried 5A / 5B / 5C as orchestrator-selectable alternatives. The orchestrator has selected 5B; v3 deletes the alternative blocks and commits the plan to a single concrete implementation.

**Rationale for 5B (vs the dropped alternatives):**
- (a) **No CPU readback sync trap.** 5A's GPU-visibility-readback would have reintroduced the implicit-sync-stall bug class documented in `memory/substrate_coalesce_sync_point_lesson.md` (the substrate 2x perf regression of 2026-05-11 was a 4-byte readback). 5B is pure CPU; no GPU readback exists.
- (b) **Math-infrastructure shared with Step 3.** Step 3 (picking repoint) is also replacing a per-quad walk with a CPU camera-frustum × quad-AABB intersection. 5B uses the same primitive. Building the shared frustum-test helper once serves both steps.
- (c) **Conservative over-inclusion is correctness-safe.** False positives in cull cause an object-block iteration to do one extra microsecond of work and discover the block has no visible content; the iteration is otherwise unaffected. False negatives — marking a block inactive when it has visible content — are the dangerous failure mode and are impossible under 5B (a quad's AABB by definition contains all its vertices).
- (d) **Eliminates per-vertex projection entirely.** The core architectural goal of this plan is to retire `VertexProjectLoop`. 5A and 5C both keep some per-vertex CPU projection alive; 5B is the only option that fully achieves the goal.

**Implementation sketch:**

Inside the slimmed `Terrain::geometry` pass (replacing the per-vertex projection loop body):

1. Iterate the per-quad list once.
2. For each quad, extract the world-space 8-corner AABB. The AABB is already known from the quad metadata that drives the indirect SOLID dispatch (same data fed to `gpu_driven_terrain_solid.comp` post-Fix-B); re-derive locally if not already exposed on the CPU side.
3. Run a standard 8-plane camera frustum test against the AABB. Return one bool per quad: `quadVisible`.
4. Write `objBlockInfo[block].active = quadVisible` for the block owning this quad.
5. Write `objVertexActive[vertex] = quadVisible` for every vertex (4-9 vertices) belonging to this quad. The same bit is written to every vertex of one quad — conservative over-inclusion is correct here; per-vertex granularity is not what the downstream 9 `objmgr.cpp` consumers need.

The min/max reduction work falls naturally into the same loop (see Step 6 — `leastZ/mostZ/leastW/mostW/leastWY/mostWY` are accumulated alongside the frustum test).

**Cost estimate:** ~50-80 µs CPU for ~16k quads × 8 plane tests each. Recovers the bulk of the ~475 µs from VPL since per-vertex projection (25-100 projections per quad) is gone entirely.

**Path C eliminated.** The wrap-and-reduce framing eliminates the need for the v1 "Path C" (dirty-flag skip with object-list generation tracking) entirely. The reduced pass is cheap enough at full frequency that we don't need a skip path.

**Shared frustum-AABB helper — REFERENCE, do not re-add (B5).** The 8-plane frustum test against quad AABBs uses `Camera::quadAabbInFrustum` / `Camera::extractFrustumPlanes`. These are **DEFINED by Step 3 commit 3a** (see Step 3 "Shared frustum-AABB helper"). Step 5B **references** them via the `Camera*` (`eye`) already in scope; it MUST NOT re-declare or re-define them (merge-conflict / double-add avoidance). Per current plan numbering Step 3 lands first and owns the definition; only if Step 5 somehow lands before Step 3 would the ownership invert (then Step 5 defines, Step 3 references) — but the plan ships Step 3 first, so Step 3 owns it.

**Co-location requirement.** The reduced pass MUST stay co-located with the rest of `Terrain::geometry`. Do not factor into a separate function in this step — that's a refactor for step 10 cleanup. (The shared helper is a `Camera` member per B3/B5, separate from this co-location rule.)

**Sensitivity consult:** the `mc2-render-perf-expert` advisor should confirm before commit that the 9 `objBlockInfo[].active` consumers in `code/objmgr.cpp` are insensitive to over-inclusion (objects whose owning quad-AABB intersects the frustum but whose actual on-screen footprint is empty). Expected answer: insensitive — the consumers do their own per-object visibility refinement downstream.

**Verification:** tier1 smoke + visual smoke (object-iteration cull is the canary class — Fix-A-style ghost-mech / streak symptoms during smoke = CRITICAL regression). Run the substrate canary `mc2_10` first. See §"Step 5 + Step 8c shared `mc2_10` canary triage order" below for the protocol when parallel soaks both surface a regression simultaneously.

**Soak:** 7 days, parallel with Step 3 and Step 8c soaks.

### Step 6: Slim CPU min/max pass for shared reductions (wrap and reduce)

**Reviewer-correction note (A4):** the adversarial reviewer's CRIT-4 was WRONG — their grep missed the `inverseProjectForPicking` wrapper at `mclib/camera.h:634` which is what `code/gametacmap.cpp:225/232/239/246` actually calls. The inventory doc `projectz-callsite-inventory.md:24` was written pre-policy-split. Reductions ARE live.

**Live chain:** VPL writes `leastZ/mostZ/leastW/mostW/leastWY/mostWY` at `terrain.cpp:1547-1571` → `eye->setInverseProject(mostZ, leastW, yzRange, ywRange)` at `:1891` → `Camera::inverseProjectZ` at `mclib/camera.cpp:1882` → 4 tacmap callsites at `code/gametacmap.cpp:225/232/239/246`.

**Change:** retire the per-vertex projection that produced the inputs to the reductions, but keep a slim CPU min/max pass alive. The min/max over visible-quad AABBs is dramatically cheaper than VPL's 33k-vertex projection (advisor estimate ~100× cheaper). Land it as a small inline loop in `Terrain::geometry` that derives `leastZ/mostZ/leastW/mostW/leastWY/mostWY` from existing quad AABBs once per frame, then calls `eye->setInverseProject(mostZ, leastW, yzRange, ywRange)` exactly as today.

**Coordinate with Step 5 (5B committed):** since Step 5 picked 5B (quad-AABB-only), the min/max reduction becomes free — the Step 5 loop is already iterating quad AABBs for the frustum test. Accumulate `min/max` of the relevant components alongside the per-quad `quadVisible` write. No separate pass needed. The `setInverseProject(mostZ, leastW, yzRange, ywRange)` call happens once after the Step 5 loop closes.

**Verification:** tier1 smoke + tacmap F-key UAT (viewport corners must unproject to identical world positions byte-for-byte) + parity check that `eye->setInverseProject` arguments are identical to pre-retirement values within float epsilon. Frame-perfect screen byte-compare on 30s capture.

### Step 7: hazeFactor port to GPU compute (inline-worldPos edge-clamp)

**Context (CRIT-1 resolution + v3 rewrite):** `hazeFactor` is per-vertex atmospheric distance fog, range 0.0 (clear) to 1.0 (fully fogged). Currently written by VPL at `terrain.cpp:1495-1497, :1503, :1509, :1514, :1521, :1556` (fast path) and `:1646, :1654, :1663, :1669, :1674, :1702` (legacy fallback). Currently consumed by:
- (a) CPU SOLID/TES emit at `quad.cpp:1457/1611/1765/1917` (dies with step 1 CPU pack retirement)
- (b) GPU lighting compute at `gos_terrain_lighting.comp:277` via the `GpuTerrainVertexInput` SSBO populated at `gos_terrain_lighting.cpp:553`

The visible use today is the play-area boundary clamp — the `Terrain::IsGameSelectTerrainPosition` check at `terrain.cpp:1500/1660` forces `hazeFactor = 1.0` for vertices outside the world edge. The smooth distance-ramp at `:1495-1497` is computed but not visually prominent.

**Port shape (v3 — inline-worldPos, supersedes v2 edge-flag precompute):** compute `hazeFactor` entirely on the GPU from the vertex's world-space position at lighting-shader execution time. No mission-load precompute, no schema-extension bit-pack — the edge check is evaluated inline against a uniform rectangle that describes the play-area bounds. This both simplifies the port AND incidentally fixes the existing water-boundary unfogged bug (see "Bug-fix annotation" below).

**Uniform additions (lighting compute):**
- `uniform vec4 g_playAreaBounds;` — `(min.x, min.y, max.x, max.y)` in world meters. Derived host-side from `Terrain::worldUnitsMapSide` and `Terrain::worldUnitsPerVertex` using the same `metersCheck = worldUnitsMapSide/2 - worldUnitsPerVertex*2` math that `Terrain::IsGameSelectTerrainPosition` (`terrain.cpp:687-700`) uses. Uploaded once per mission load (constant for mission lifetime) at lighting-compute init, OR once per frame as part of the lighting uniforms (cost identical at this volume; pick whichever is simpler).
- `uniform float g_minHazeDistance;` — mirror of `Camera::MinHazeDistance` (CPU constant).
- `uniform float g_maxClipDistance;` — mirror of `Camera::MaxClipDistance`.
- `uniform float g_distanceFactor;` — mirror of `Camera::DistanceFactor` (precomputed `1.0 / (MaxClipDistance - MinHazeDistance)`).

**Shader change (`shaders/gos_terrain_lighting.comp:276-285`):** replace the `if (v.hazeFactor != 0.0)` block with inline computation:

```glsl
// v3: hazeFactor computed inline from worldPos + cameraDistance
// (was: v.hazeFactor read from SSBO; CPU-side VPL retired in this commit)
vec3 worldPos = ...;                            // derive from v.xy + v.elevation (already in GpuTerrainVertexInput)
float hazeFactor;
bool outsidePlayArea = (worldPos.x <= g_playAreaBounds.x) ||
                       (worldPos.x >= g_playAreaBounds.z) ||
                       (worldPos.y <= g_playAreaBounds.y) ||
                       (worldPos.y >= g_playAreaBounds.w);
if (outsidePlayArea) {
    hazeFactor = 1.0;                           // edge clamp — pipeline-agnostic
} else {
    float cameraDistance = ...;                 // derive from worldPos vs camera (or v.clipPos.z if available)
    if (cameraDistance > g_maxClipDistance)      hazeFactor = 1.0;
    else if (cameraDistance > g_minHazeDistance) hazeFactor = (cameraDistance - g_minHazeDistance) * g_distanceFactor;
    else                                          hazeFactor = 0.0;
}
if (hazeFactor != 0.0) {
    float fogFactor = 1.0 - hazeFactor;
    uint distFog = uint(int(fogFactor * 255.0));
    if (distFog < fogResult) fogResult = distFog;
    fogRGB_out = (fogResult << 24) | (specR << 16) | (specG << 8) | specB;
}
```

The exact source of `worldPos` and `cameraDistance` is left to the execution session — at minimum, `v.xy + v.elevation` is already on the input SSBO; the camera world position is already a per-frame uniform; `cameraDistance = length(camPos - worldPos)` matches the CPU-side `distanceToEye` semantics at `terrain.cpp:1495-1497`.

**Lockstep retirement (in this same commit):** once the GPU path produces `hazeFactor` inline, the SSBO field is dead. Retire all three in lockstep per `memory/cpp_glsl_ubo_struct_lockstep.md`:
- GLSL struct: `shaders/include/terrain_lighting_shared.hglsl:7` — remove `float hazeFactor;` from `struct GpuTerrainVertexInput`.
- C++ struct: `GameOS/gameos/gos_terrain_lighting.h:38` — remove `float hazeFactor;` field declaration.
- C++ writer: `GameOS/gameos/gos_terrain_lighting.cpp:553` — remove `vi.hazeFactor = v.hazeFactor;`.
- The struct size drops by 4 bytes; verify any `sizeof(GpuTerrainVertexInput)` static_assert or `offsetof` assertion is updated in lockstep. Cite `memory/cpp_glsl_ubo_struct_lockstep.md` in the commit message; this is the canonical hazard from that lesson.

**C++ side VPL retirement (in this same commit):** delete the VPL hazeFactor writes:
- Fast path at `terrain.cpp:1495-1521` (the `distanceToEye`-based ramp and the `IsGameSelectTerrainPosition` boundary clamp).
- Fast path final write at `terrain.cpp:1556` (`cv->hazeFactor = hazeL`).
- Legacy fallback at `terrain.cpp:1646-1674` (mirror computation).
- Legacy fallback at `terrain.cpp:1702` (`currentVertex->hazeFactor = 0.0f`).
- The parity-comparator field reference at `terrain.cpp:1770` and `:1786` if those lines survive Step 8c — verify at write-time.

Update Step 8c's deletion list to reflect that these VPL lines are already gone by the time Step 8c runs (Step 7 removes them; Step 8c removes the surrounding loop body and parity scaffolding).

**Water pipeline coverage (pipeline-agnostic check):** the inline-worldPos check applies to ANY vertex flowing through the lighting compute, including water vertices. **Pre-flight requirement:** grep `shaders/*water*` and `gos_terrain_water_stream.cpp` to confirm whether water lighting routes through the SAME `gos_terrain_lighting.comp` (in which case no extra change) OR through a separate compute shader (in which case the same inline math must be added there). Today's grep:
- `shaders/gpu_driven_water.comp` — primary water cull/pack, not lighting.
- `shaders/gos_terrain_water_fast*.vert` / `gos_terrain_water_mdi.frag` — direct draw path, not compute lighting.
- `shaders/gos_terrain_lighting.comp` — the only terrain-lighting compute; if water vertices share this lighting path the change covers them, if they don't they currently miss the haze treatment entirely (the pre-existing bug).

The execution session MUST grep-confirm at write-time which water vertices go through `gos_terrain_lighting.comp` vs an independent lighting site, and add the inline edge-clamp to any sibling lighting shader found.

**Bug-fix annotation (positive side effect):** the v3 inline-worldPos approach incidentally fixes a pre-existing bug — water vertices that extend past the play-area boundary currently render unfogged because water never went through VPL's `IsGameSelectTerrainPosition` clamp. With the inline check living in the lighting compute, ANY vertex (terrain or water) outside the play-area rectangle gets `hazeFactor = 1.0`. This is a positive visual change, not a regression. Tier1 visual UAT must treat the new edge-fog on out-of-bounds water as the **expected** post-Step-7 delta, not as a regression. Missions to check: `mc2_17` (water-heavy with maps that have water extending to map edges).

**Supersedes NEW-MED-1.** The re-review's NEW-MED-1 finding requested that Step 7 name the exact mission-load function and line range owning the edge-flag-bit precompute, to avoid a first-frame-of-mission transient where the precompute hadn't run before GPU lighting consumed it. The v3 rewrite eliminates the precompute entirely — there is no mission-load precompute to time, no recipe-schema bit to pack, no first-frame transient hazard. The finding is rendered moot. Re-review trail audit: explicitly noted closed-by-supersession; not closed by satisfying the original ask.

**Risk surface:** lockstep schema change on `GpuTerrainVertexInput` is the load-bearing hazard (per `cpp_glsl_ubo_struct_lockstep.md` — the canonical case study is exactly this struct). Coordinate with `mc2-shader-expert` consult during execution. The new uniforms are simple constants; their upload site is a one-time-per-mission write or per-frame write that joins the existing lighting-uniform upload path.

**Verification:** tier1 smoke + frame-perfect screen byte-compare on 30s capture inside the play area (haze in-bounds must match baseline). Out-of-bounds water UAT on `mc2_17` is a positive-delta expected change, not a regression. Visible canary: the world boundary's `hazeFactor = 1.0` clamp must produce identical fog at the edge of terrain (in-bounds) as baseline; water extending past the boundary must NOW render fogged (was rendering clear pre-Step-7).

### Step 8: VertexProjectLoop body retirement (three-commit split)

**Precondition:** after steps 1-7, the loop body's per-vertex projection writes have no live GPU-side consumers (`hazeFactor` ported in step 7; `pz`/picking repointed in step 3; cull-cascade slimmed in step 5; reductions reduced in step 6). The legacy CPU SOLID/TES path at `quad.cpp:2299-2840` still reads `pz`/`px`/`py`/`pw` but is itself unreachable under `MC2_TERRAIN_INDIRECT=1` (default); its retirement is the CPU-pack plan's commit 2 scope, not this plan.

**Why split into three commits (A6):** the loop body is woven into multiple consumer chains. A single-commit "delete body + parity + struct" lands too much in one revert window. Split:

- **Step 8a — no objmgr.cpp consumer code change required; comment-only update + retire VPL's per-vertex projection writes.** Since Step 5 picked 5B which keeps `objBlockInfo[].active` and `objVertexActive[]` as the CPU-side cull state (now written by the slim quad-AABB-frustum-test pass), the 9 consumer sites at `code/objmgr.cpp:1697/1704/1828/1835/2019/2027/2610/3060/3067` need no functional change. The commit lands two things: (1) a one-line comment update at each consumer site documenting that the new source of truth is the Step 5 slim pass (not VPL); (2) retirement of the now-dead per-vertex projection writes (`px/py/pz/pw`) that no longer have any production consumers — these writes still live in the loop body but feed nothing. Step 8a becomes a smaller, lower-risk commit than v2 envisioned.

- **Step 8b — retire VPL's per-vertex writes (`px/py/pz/pw`).** Fields survive on the `cv` struct because non-VPL consumers (`quad.cpp` reads `pz` at `:357` debug-trace path, etc.) still reference them; VPL simply no longer writes them. Downstream consumers see stale data from previous frames, which is correct because their VPL-dependency was retired in earlier steps. `clipInfo` writes also retire here (their consumer went away in step 3).

- **Step 8c — delete the projection loop body + scaffolding.** Removes the projection loop body at `mclib/terrain.cpp:1417-~1718` (verify at write-time), the parity comparator at `terrain.cpp:1750-1820`, the `VPParitySnap` struct at `:1384-1390`, env vars `MC2_VERTEX_PROJECT_FAST` / `MC2_VERTEX_PROJECT_PARITY` at `:1425-1426`, and the Tracy zones `vertexProjectLoop` at `:1447/1588`. `Terrain::geometry` itself stays — it has non-VPL responsibilities at `:1402-1405` (`beginFrameTexResolve`), `:1809` (`quadSetupTextures`), `:1897` (`cloudUpdate`).

**Sole caller of `Terrain::geometry` is `code/mission.cpp:505`; no caller deletion needed.**

**Expected savings:** the full ~475 µs/frame on every frame the loop ran. Loop already operates on a smaller surface post Fix-B + CPU-pack-retirement; full deletion captures the remaining cost.

**Verification:** tier1 smoke after each of 8a/8b/8c. Tracy capture before/after on wolfman zoom in `mc2_10`; `vertexProjectLoop` Tracy zone disappears after 8c. Frame-perfect screen byte-compare on 30s capture: identical between 8a/8b/8c (no behavior change expected at default config). Soak 7 days after 8c, parallel with Step 3 and Step 5 soaks per §"Soak window parallelization."

### Step 9: Demote Fix A scaffolding behind MC2_RING_TRACE=1

**User decision (A7):** keep, don't delete.

**Scope:** demote `g_thinSlotMVP[]` array (`gos_terrain_indirect.cpp:1455-1456/1602-1603/2484-2485/2844-2848`) and Probe 8 `[RING_MVP_DELTA v1]` infrastructure (`:1440-1445`) behind `if (g_envRingTrace)` guard, default-off. Net cost when off: ~192 bytes of static state, zero per-frame work. Preserves the regression probe for future temporal-misalignment bug class.

**Vertex-shader comment update:** `shaders/gos_terrain_thin.vert:56-61` currently reads "Fix A's terrainOverrideThinMVP path is similarly inert (cached loc -1)." Update to: "Fix A scaffolding demoted behind `MC2_RING_TRACE=1` env-gate for regression probing; default-off. The vertex shader has no `terrainMVP` uniform; `clipPos` in the thin record is the sole projection authority."

**Verification:** tier1 smoke with `MC2_RING_TRACE` unset (default) — Probe 8 silent, snapshot writes skipped. Tier1 smoke with `MC2_RING_TRACE=1` — Probe 8 emits as today.

### Step 10: Render contract refresh + cleanup

**Trigger:** dispatch `mc2-render-contract-synthesizer` agent after step 9.

**Action:** the synthesizer reads current state, updates `docs/render-contract.md` to reflect "GPU is sole projection authority for terrain quads in the indirect path; CPU has a slim cull-cascade pass and a slim min/max reduction pass that survive in `Terrain::geometry` for `objBlockInfo[].active`/`objVertexActive[]` consumers and `setInverseProject` callers respectively; no per-frame per-vertex CPU projection state."

**Optional refactor:** the surviving slim passes from steps 5 and 6 can be factored into named helper functions if convenient. Keep co-located by default; refactor only if the inline form impedes readability.

---

## Soak window parallelization (A8)

The 7-day soaks for step 3 (cursor UX + tacmap UX), step 5 (reduced cull-cascade), and step 8c (full-frame render after loop-body deletion) MAY run concurrently on the same tier1 nightly because their test surfaces don't overlap:
- Step 3 soak surfaces picking / cursor / tacmap regressions (UI-class).
- Step 5 soak surfaces object-iteration cull regressions (Fix-A-class ghost-mech / streak symptoms).
- Step 8c soak surfaces whole-frame render regressions (visual byte-compare class).

Each soak retains its own UAT checklist. Avoids serial 21+-day burnout.

---

## Verification strategy (A9 standardization)

**Per-commit smoke gate:**

```
py -3 A:/Games/mc2-opengl-src/.claude/worktrees/gpu-driven-rendering/scripts/run_smoke.py --tier tier1 --duration 30 --kill-existing
```

NO `--with-menu-canary` (project rule, per `memory/feedback_smoke_gate_no_menu_canary.md`). All 5 tier1 missions must PASS.

**Parity probes (named):**
- **`[RING_MVP_DELTA v1]`** — Fix A regression probe. Active under `MC2_RING_TRACE=1` after step 9.
- **Frame-perfect screen byte-compare on 30s capture** — for steps 6, 7, 8b, 8c. Three captures (baseline, mid-step, post-step) must be byte-identical screen-by-screen across the 30-second mc2_01 run.
- **`eye->setInverseProject` argument byte-compare** — for step 6. Pre-retirement vs post-retirement argument tuples within float epsilon.

**Per-step gates:**
- **Step 3 (picking + tacmap):** manual UAT — cursor tracking, mech selection by click, marquee drag-select, build-menu placement, salvage placement, tacmap F-key viewport. User-driven smoke at `mc2_01`, `mc2_10` (substrate-heavy), `mc2_17` (water-heavy).
- **Step 5 (cull cascade):** Fix A-style ghost-mech / streak symptoms during smoke = CRITICAL regression. Substrate canary `mc2_10` first.
- **Step 6 (reductions):** tacmap F-key UAT (viewport corners unproject byte-identically) + `setInverseProject` argument byte-compare.
- **Step 7 (hazeFactor port):** frame-perfect screen byte-compare; world-edge `hazeFactor=1.0` clamp visible canary.
- **Step 8c (loop deletion):** Tracy capture before + after. `vertexProjectLoop` zone disappears. Wolfman zoom CPU time drops by the measured loop cost (~475 µs).
- **Step 9 (Fix A demote):** Probe 8 silent under default env; emits under `MC2_RING_TRACE=1`.

**Soak windows:** seven-day soaks for steps 3, 5, and 8c, run in parallel per §"Soak window parallelization." Soak surfaces the kind of edge cases that show up on rare mission layouts (`mc2_24` reactor environment, `mc2_17` water-only zones).

### Step 5 + Step 8c shared `mc2_10` canary triage order (NEW-MED-2 closure)

Both Step 5 (slim cull-cascade pass) and Step 8c (full VPL body deletion) treat substrate-heavy `mc2_10` as the primary canary mission. Their soaks run in parallel under §"Soak window parallelization," so a tier1 regression on `mc2_10` may surface during a window where both commits are live. When that happens, follow this triage order:

1. **Bisect to identify the offending commit.** `git bisect` between the last known-good baseline (pre-Step-5) and the current HEAD; the Step 5 commit and Step 8c commit are on the bisect path.
2. **Step 5 regression class — symptoms.** Fix-A-style ghost-mech / streak symptoms, objects rendering at stale positions, or objects flickering between visible/invisible. Root cause: either over-inclusion in 5B's frustum test that activates an object-block at a wrong moment, OR a missed `objBlockInfo[].active` / `objVertexActive[]` write for a quad whose AABB the frustum-test misclassified. Diagnostic: enable `MC2_DESTROY_TRACE=1` and `MC2_TGL_POOL_TRACE=1`; check whether the lifecycle prints fire for the affected objects.
3. **Step 8c regression class — symptoms.** Whole-frame visual delta (terrain looks different in any way), missing geometry, or a Tracy zone that should be gone but still emits cost. Root cause: a VPL side-effect not enumerated in the deletion list — typical candidates are counter increments the rest of the code reads as a heartbeat, file-scope statics that VPL updated, or a debug-logging path that downstream code keys off. Diagnostic: grep for any global written inside the deleted `:1417-1718` range and verify each is covered by Steps 5/6/7 or has no surviving reader.
4. **Specifically verify `beginFrameTexResolve` at `terrain.cpp:1402-1405` still runs.** This is a load-bearing per-frame init that lives inside `Terrain::geometry` but OUTSIDE the VPL body. Step 8c deletes only `:1417-~1718` (the loop body); `:1402-1405` survives. If `beginFrameTexResolve` stops firing the entire texture-resolve pipeline goes stale and the symptom looks like a generalized texture regression. This is the highest-prior diagnostic for a Step 8c-class regression.

If both Step 5 and Step 8c regressions surface simultaneously: triage Step 5 first (structural-correctness layer — its bugs propagate). Step 8c's symptoms are usually visual-delta-only and don't cascade into cull state.

---

## Downstream unlocks (informational — not in scope)

This plan does not implement these, but they become available once VPL is gone:

- **GPU AABB hit-test for mouse picking** (`memory/gpu_mech_aware_mouse_pick_queued.md`). Picking already projects its own cursor ray (step 3). Upgrading to GPU AABB hit-test against the recipe SSBO becomes a small, self-contained slice.
- **Object/prop iteration GPU port.** The iteration loop's consumption of `objBlockInfo[]` / `objVertexActive[]` is what blocks moving its cull decisions to GPU. Once those CPU side-effects are gone (step 5), the iteration loop's cull cascade is free to consume GPU-resident visibility data directly.
- **Static-prop registry GPU consumer.** `Track B` (`memory/track_b_widen_static_prop_registry.md`) widened the registry; once VPL stops feeding its cull cascade, the static-prop renderer can read visibility from the same GPU buffers the terrain compute already writes.
- **`leastZ/mostZ` reduction GPU port.** If step 6 retires these CPU reductions and their consumers, the analogous GPU reductions in `gpu_driven_terrain_solid.comp` (which already computes per-quad clip extents for pzOk) can become the source of truth.

---

## Pending grep verifications (must complete in execution session)

This plan was drafted with recon-agent file:line citations that have NOT been re-verified in the plan-author session. Per worktree CLAUDE.md documentation discipline, every cited file:line must be grep-verified AT WRITE-TIME by the executing session. Specifically:

- `terrain.cpp:1547-1571` (VPL writes per-vertex pz, px, py, pw, clipInfo, hazeFactor + sets objBlockInfo/objVertexActive)
- `terrain.cpp:1549-1552` (file-scope reduction writes)
- `terrain.cpp:1696-1715` (legacy fallback reduction writes)
- `quad.cpp:2089-2094` (M2 thin emit pz read)
- `gos_terrain_indirect.cpp:1639-1656` (indirect pz cull)
- `gos_terrain_mask_dispatch.cpp:211-223` (mask-dispatch pz cull; ALSO grep mask-dispatch's reachability in production)
- `camera.cpp:797-822` (`Camera::inverseProject`)
- `mclib/camera.h:422-622` (8 categorical wrappers; pick the right one for picking repoint)

The executing session: grep first, cite second, edit third.

---

## Adversarial review trigger

Per worktree CLAUDE.md Review Discipline, this plan hits two high-stakes triggers:

1. **Architectural endpoint** — sole projection authority shift.
2. **Legacy retirement** — function deletion across multiple commits.

Adversarial review is REQUIRED before any code change. Dispatch with the verbatim incantation "use the adversarial-plan-review skill" per the worktree rule.

Expected findings density (based on Fix B precedent): 2-4 CRITICAL, 4-6 MAJOR, 5-8 MINOR. Plan v2 landed the original adversarial review's CRIT-1/CRIT-4 + advisor + user-decision amendments and carried three Step 5 options for orchestrator selection. Plan v3 (this document) closes the re-review's READY-WITH-MINOR-FIXES verdict by (a) committing Step 5 to option 5B, (b) rewriting Step 7 to use inline-worldPos edge-clamp (supersedes NEW-MED-1), (c) documenting the Step 5 / Step 8c shared-canary triage order (closes NEW-MED-2), and (d) cross-referencing the water-stream barrier sibling timing in the cmd-patch plan (closes NEW-LOW-1). All original BLOCK findings and all re-review new findings are now addressed; plan is execution-ready.
