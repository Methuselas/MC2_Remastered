# `VertexProjectLoop` Retirement — Implementation Plan v3

> **Plan v3.5 amendment note (2026-05-15, consolidated corrected Step 8 gate graph; derives from `docs/superpowers/reviews/2026-05-15-step8-vpl-body-deletion-adversarial-review.md` [BLOCK] + `docs/superpowers/reviews/2026-05-15-overlay-pz-precursor-adversarial-review.md` [READY-WITH-FIXES, committed `a718219`]; grep-verified at HEAD `29ae435`):** The v3.0-v3.4 Step 8 design was internally broken (it cited a "Step 5 slim pass" / "5B" cull producer that v3.2 deleted and that never existed in code) and incomplete on the catastrophic placement axis. v3.5 records the COMPLETE corrected Step 8 gate graph. Nothing here is a new design decision — every item is settled by the two prior adversarial reviews + the prior advisor passes; v3.5 only transcribes the settled mechanism into the plan so an executor works from a coherent contract. The v3/v3.1/v3.2/v3.3/v3.4 trail and all prior prose are preserved verbatim; superseded blocks are tagged in place, not rewritten.
>
> **CRIT-0 — the real post-8c cull-cascade producer (replaces the phantom "5B slim pass").** Step 8a/8c prose references a "Step 5 slim pass" / "5B" as the post-8c source of `clipInfo`/`objBlockInfo[].active`/`objVertexActive[]`. That producer does NOT exist: Step 5 shipped instrumentation-only (the `MC2_VPL_CULL` probe) per the v3.2 correction, and the only code that writes the cull cascade today is the **VPL body itself** at `terrain.cpp:1790-1793` (`if (currentVertex->clipInfo)` → `setObjBlockActive`/`setObjVertexActive`; stock `clipInfo = onScreen` at `:1759`, dead `= inView` at `:1762`). The actual post-8c producer is: **8c-part-1 MERGES the per-vertex cull writes INTO the already-landed Step 6 slim reduction loop** (`terrain.cpp:~1923-2018`, the loop that already duplicates the `onScreenR` derivation byte-for-byte at `:1928-1982` and whose `onScreenR == onScreen` vertex-for-vertex is confirmed by the Step 8 review §8). Step 8a is REDUCED to verification + comment-correction at the 9 `code/objmgr.cpp` cull-cascade consumer sites (`:1697/1704/1828/1835/2019/2027/2610/3060/3067` — all grep-confirmed at HEAD `29ae435`, zero drift), documenting that the post-8c source of truth is the cull-merged Step 6 loop, NOT a "Step 5 slim pass" and NOT the VPL body.
>
> **CRIT-1 — the `terrain.cpp:1993` placement rule (catastrophic axis; slim⊇legacy CONDITIONALLY PROVEN).** The cull writes merged into the Step 6 loop MUST be emitted on the `onScreenR`/`clipR` decision **BEFORE** the slim loop's reduction-admission `continue` at `terrain.cpp:1993` (`if (!clipR || !inViewR) continue;` — grep-verified at HEAD `29ae435`, `:1993-1994`; the `if (!onScreenR) continue;` reduction-pre-gate is `:1984-1985`). Placed AFTER `:1993` (or gated on `inViewR`), the active-set becomes `{ onScreen ∩ inView }`, a strict SUBSET of the legacy `{ onScreen }` cull-set — because `inView`/`inViewR` (= `projectForTerrainAdmission` → `projectZ`, strict screen-rect containment) is strictly tighter than `onScreen` (the loose 768u-proximity / 384u-dilated-cone / `!IsGameSelectTerrainPosition` force-readmit superset that IS the legacy cull contract). A subset orphans the 9 `objmgr.cpp` consumers → edge/off-rect objects and mechs vanish = catastrophic (`cull_gates_are_load_bearing.md`; mechs are the canary, iterate last). `onScreenR == onScreen` is byte-identical (Step 8 review §8, confirmed), so the PROVEN-equal branch is achievable ONLY with this placement. This is a HARD, explicit implementation rule for 8c-part-1: do NOT `continue` on `!onScreenR` before the cull write; compute `clipInfo = clipR` and emit `setObjBlockActive`/`setObjVertexActive` when `clipR` is true; THEN apply the stricter `!clipR || !inViewR` gate ONLY for the `leastZ/mostZ/leastW/mostW/leastWY/mostWY` reduction accumulation. Cull-write and reduction-gate are decoupled inside the one loop — the reduction may legitimately use the tighter set; the cull MUST use the loose `{ onScreen }` set.
>
> **CRIT-2 — co-production masks the subset bug; camera-moving parity probe REQUIRED at 8c-part-1.** During 8c-part-1 BOTH the VPL body (`terrain.cpp:1638-1834`) and the cull-merged slim loop write the cull arrays inside one `land->geometry()` call (`mission.cpp:505`; frame-start clear `mission.cpp:501-502`). Both true-only; the union is the full legacy set REGARDLESS of whether the slim writes are a subset — so a CRIT-1 subset bug is INVISIBLE in 8c-part-1 and only manifests when 8c-part-2 deletes the VPL body and the set collapses to the slim subset. The divergence lives entirely in the 768u/384u-dilated off-rect band reached only by moving the camera (worst case = user-positioned corner+zoom, `feedback_cost_split_worst_case_camera.md`); default-static-camera smoke will NOT catch it. Step 8c's gate MUST therefore include a co-production parity probe (the relocated `MC2_VPL_CULL`, see MAJOR-d) that asserts `!(legacyActive && !slimActive)` (slim ⊇ legacy) **per vertex AND exercised with camera motion**, plus a slim-only validation (VPL body force-disabled) that the slim-only active-set equals the legacy set BEFORE 8c-part-2 lands. Co-production alone is NOT a sufficient gate.
>
> **MAJOR fixes folded into Step 8 (from the Step 8 body-deletion review):**
> - **(a) `Camera::getClosestVertex` is DEAD code — DELETE it in/with the 8b commit.** `camera.cpp:568` (def) + `camera.h:661` (decl); grep-verified ZERO callers anywhere in `mclib/`+`code/`+`GameOS/` at HEAD `29ae435`. It reads `topVertex->px/py` which 8b is about to make per-frame-stale. Leaving a dead function reading soon-stale state is the additive-debt reactivation trap (`mc_texture_manager_dual_queue_legacy_retirement_debt.md`) — delete-not-leave. Added to Step 8b scope.
> - **(b) clipInfo-survivor rationale CORRECTION.** `isTerrainQuadVisible` (`quad.cpp:420`, caller `:931`) reads the per-corner clipInfo OVERWRITE at `quad.cpp:1058/1125/1192/1259` (`vertices[N]->clipInfo = clipData`), written by `setupTextures` AFTER the VPL body every frame — NOT the VPL `onScreen`-derived clipInfo. So it is SAFE under VPL clipInfo retirement, but for a DIFFERENT reason than the plan states (the plan implies it needs VPL-clipInfo parity; it does not — it never reads VPL's clipInfo). The ONLY consumer of VPL's `onScreen`-derived clipInfo is the cull cascade itself (CRIT-1's set). Rationale text corrected so a future reader does not "preserve VPL clipInfo for isTerrainQuadVisible" and miss that the sole clipInfo invariant is the CRIT-1 cull-cascade set.
> - **(c) Legacy-lighting death must cover BOTH env entries + a defensive init.** `quad.cpp:1319-1321`: `s_lightingGpuAuth = IsEnabled() && !IsParityCheckEnabled()`; the `!s_lightingGpuAuth` `hazeFactor`-reading block is reachable under `MC2_TERRAIN_LIGHTING_GPU=0` **OR** `MC2_TERRAIN_LIGHTING_PARITY=<set>` (NOT just the `=0` path the v3.3 Step 8c prose names). After 8c deletes the VPL `hazeFactor` writes, terrain `Vertex::hazeFactor` has NO writer and NO defensive init (`mapdata.cpp` never sets it). 8c's legacy-lighting retirement/guard MUST cover both env entries (delete or neutralize the `quad.cpp:1471/1473/1625/1627/1779/1781/1931/1933` reads) AND zero-init terrain `Vertex::hazeFactor` so a stray surviving reader degrades to "no fog" (stock-playable per `stock_install_must_remain_playable.md`), not garbage.
> - **(d) Probe relocation must assert a NON-TAUTOLOGICAL invariant.** When 8c-part-2 deletes the VPL body, the `MC2_VPL_CULL` probe's host loop dies. Relocated onto the cull-merged slim loop it must assert a MEANINGFUL invariant: during the 8c-part-1 co-production window it compares the **slim-produced** cull `clipInfo`/active-set against the **still-live VPL-body** set per vertex (`!(legacyActive && !slimActive)` — the CRIT-1 invariant, the only check that catches the subset bug, exercised with camera motion per CRIT-2). It must NOT degrade to comparing the slim loop's output to itself (a pure tautology that prints "0 violations" and gives false confidence the subset bug did not happen). At 8c-part-2 (VPL body gone, no independent producer) the probe is explicitly RETIRED or demoted to `MC2_VPL_REDUCE`-style — it does NOT "survive 8c as the tripwire" indefinitely (that v3.3 claim is valid ONLY for the co-production variant during 8c-part-1).
>
> **Consolidated Step 8c gate + precursor list (supersedes the v3.3 four-condition gate's "Step 5 satisfies / soak" framing where it conflicts):** 8c may delete the VPL body ONLY when ALL hold: (1) **Step 1 landed** — its remainder: parity-infra is FULLY RETIRED (`be56e48`, recorded `5c22e28`); the `gos_terrain_indirect.cpp:1674` `q.vertices[c]->pz` reader (review cited `:1684`; drift to `:1674` at HEAD `29ae435`, `:1684` is now a comment) is operationally-retired-as-fallback, no edit needed. (2) **Step 3 landed** + its soak — the 7-day soak is WAIVED by orchestrator ruling 2026-05-15. (3) **Step 6 landed** (`cf7d596`; re-homes the projection into the decoupled slim min/max loop — necessary-not-sufficient). (4) **The M2d overlay-pz-gate precursor landed** (`29ae435`) — re-projection + mandatory `clipInfo == 0` sentinel guard (`quad.cpp:2147`) + `MC2_M2D_PZ_PARITY` probe (`quad.cpp:303`, `:2156`); this is what unblocks 8b's `cv->pz` deletion. The `MC2_M2D_PZ_PARITY` probe MUST read zero before 8b deletes `cv->pz` (both sources live to bit-compare; `quad.cpp:2169` `old_adj = vertices[c]->pz + TERRAIN_DEPTH_FUDGE` vs the re-projected source). (5) **Step 8b landed.**
>
> **Corrected sequencing (supersedes any prior 8a/8b/8c sub-ordering):** 8a (objmgr comment-correction + verification, no functional change) → 8b (delete VPL `px/py/pz/pw/clipInfo` writes + delete dead `Camera::getClosestVertex`; gated on the overlay-pz precursor which is now landed `29ae435`) → 8c-part-1 (cull-merge into the Step 6 loop with the `:1993` placement rule + relocate `MC2_VPL_CULL` non-tautologically + co-production camera-moving parity probe + slim-only validation) → 8c-part-2 (delete VPL body + projection + `hazeFactor` writes + parity scaffolding + legacy-lighting both-env guard + zero-init) → Step 9 (Fix A demote) → Step 10 (render-contract refresh + deferred cleanups). Strict overall order remains Step 5 → Step 6 → Step 7 → Step 8 (8a→8b→8c-1→8c-2) → 9 → 10. The §"Soak window parallelization" caveat is updated to match (soak parallelization does not authorize landing 8c before its five gates clear; Step 3's 7-day soak is waived).
>
> The v3/v3.1/v3.2/v3.3/v3.4 amendment trail and all prior Step 8/8a/8b/8c prose are retained verbatim below. Where prior text says "Step 5 slim pass" / "5B" as the post-8c cull producer, says the probe "survives 8c as the tripwire" indefinitely, names only `MC2_TERRAIN_LIGHTING_GPU=0` for legacy-lighting death, or omits `getClosestVertex` from the deletion list, **this v3.5 note and the in-place v3.5 tags govern.**

> **Plan v3.3 amendment note (2026-05-15, `mc2-terrain-indirect-expert` + `mc2-shader-expert` advisors, grep-verified at HEAD `55b1167`):** Two advisor-found plan deviations plus a Step 8c gate/ownership consolidation. (1) **Step 6 RE-HOMES, does not eliminate, the per-vertex projection** (`mc2-terrain-indirect-expert`). The reduction needs per-vertex projected `screenPos.z/.w/.y` over the *in-rect-visible* set; min/max of projected-z over that per-frame view-dependent set is provably NOT the projection of world-AABB extremes under the oblique cinematic camera (perspective-divide does not preserve ordering), so analytic-from-bounds / AABB-corner options are unsound. Step 6 extracts a self-contained slim min/max-only loop that keeps a minimal `projectForTerrainAdmission` call ONLY for the reduction, decoupled from the cull cascade. This slim loop SURVIVES Step 8c. Step 8c gate consolidated: 8c may delete the VPL body ONLY when ALL of {Step 1 landed, Step 3 landed + 7-day soak, Step 6 landed (re-homes the projection), Step 8b landed}; Step 6 is necessary-not-sufficient. (2) **Step 7 must NOT delete the terrain.cpp VPL hazeFactor writes** (`mc2-shader-expert`, HIGHEST PRIORITY). The `MC2_TERRAIN_LIGHTING_GPU=0` legacy CPU-lighting fallback (`quad.cpp:1321` gate, `s_lightingGpuAuth`) still consumes `vertices[]->hazeFactor`, which ONLY VPL writes; deleting VPL hazeFactor writes in Step 7 silently breaks legacy lighting (no compile error, stale/zero fog). Step 7 scope = shader + uniforms + populate-neutralize ONLY; the VPL C++ hazeFactor write deletion + the legacy-CPU-lighting-path death move to Step 8c. Lockstep handled keep-field-stop-reading (struct field stays, populate write neutralized to `0.0f`); actual field removal deferred to Step 10. Water bugfix is terrain-only; dedicated-water-path edge-clamp is a tracked follow-up. **Step 6 strictly precedes Step 7** (both touch the legacy `onScreen`/haze derivation). The corrected Step 6/7/8c sections and this note govern where prior prose conflicts. Line numbers refreshed at HEAD `55b1167`; drift from prior advisor citations noted inline.

> **Plan v3.1 sequencing correction (2026-05-15, `mc2-terrain-indirect-expert` advisor, grep-verified at HEAD `2e11617`):** v3's D1 framed Step 5 as "5B quad-AABB-only" and Step 6 as harvesting the `leastZ/mostZ/leastW/mostW/leastWY/mostWY` reductions "free" from Step 5's quad-AABB iteration. **Both were wrong.** The committed Step 5 shape is a **per-vertex point-AABB frustum test** (consumers need per-vertex `objVertexActive[]` granularity a per-quad pass cannot produce) that deliberately does NOT project and therefore produces NO clip-space extents. Consequences now reflected in Steps 5/6/8c: (1) Step 5 KEEPS the projection (`terrain.cpp:1685`) and reduction block (`terrain.cpp:1722-1742`) ALIVE — it swaps only the cull-cascade decision (`terrain.cpp:1705-1718`) for the slim world-space frustum pass; both run simultaneously, safe because true-only writes + the frame-start clear at `mission.cpp:501-502` (before `land->geometry()` at `mission.cpp:505`) reset each frame. (2) Step 6 must produce the reductions by its OWN design (Step 5 hands it nothing); how is Step 6's own decision, NOT settled by Step 5. (3) Step 8c (projection deletion) is now gated on Step 6 landing first (in addition to the Step 3 soak), since the projection cannot be deleted while the reductions still depend on it. Strict sequence: **Step 5 → Step 6 → Step 8c.** Soak parallelization is unchanged but explicitly does NOT relax this landing-order gate. Where v3 prose below still reads "quad-AABB-only" / "becomes free," the corrected Step 5/6/8c sections and this note govern.

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

**B1 amendment (render-expert, recursion resolution):** Step 3 does NOT call inverseProjectForPicking for the in-game Camera::inverseProject repoint. Camera::inverseProject performs no inverse projection - the cell-snap tail (camera.cpp:833-902, world coords from vertices[]->vx/vy/pVertex->elevation, forward-projected via projectForSelectionPicking) already determines the picked cell from closestTile alone. The repoint replaces only the tile-selection block :759-831 with (1) extractFrustumPlanes + quadAabbInFrustum AABB admission over all quads, then (2) a forward-projection screen-containment + nearest-distance refinement (port overThisTile's screen-triangle test to freshly forward-projected corners) producing closestTile. The :833-902 tail is unchanged. overThisTile is deleted. inverseProjectForPicking is used ONLY by the tacmap path (gametacmap.cpp:225/232/239/246), untouched. The usePerspective pre-flight gate is satisfied structurally - picking never enters inverseProjectZ, so the !usePerspective recursion branch is unreachable regardless of CTRL+ALT+P toggle state. Near-plane uses near=rZ (engine glClipControl native [0,1]); shader-expert to confirm the near/far pair sign (side planes are convention-independent).

**Verification:** tier1 smoke `--tier tier1 --duration 30 --kill-existing` + manual UAT on `mc2_01` / `mc2_10` / `mc2_17`: cursor placement (move-marker exactly under cursor), marquee drag-select (exact selection set), salvage/build placement ghost cell-accurate, AND tacmap F-key viewport trapezoid unchanged vs baseline (negative control proving Step 3 did NOT touch the tacmap path).

**Soak:** 7 days. Picking regressions are subtle (off-by-one cursor positioning) and may only show on specific mission layouts. Runs concurrently with Step 5 + Step 8c soaks per §"Soak window parallelization" below.

### Step 4: Mask-dispatch + parity-mask `pz` reads retire

**File:** `gos_terrain_mask_dispatch.cpp:211-223` (recon citation; verify).

**Strategy:** the mask-dispatch's pz cull is structurally identical to the indirect dispatch's pz cull; same lambda lives in `gos_terrain_indirect.cpp`. If mask-dispatch is reachable in production (recon flagged uncertainty), repoint to use the recipe SSBO + on-CPU one-shot projection per quad considered. Otherwise mark dead and skip.

**Pre-commit grep:** confirm mask-dispatch's actual reachability. If it's reached only under env-gated paths (`MC2_TERRAIN_MASK_DISPATCH=1`?), document and either gate-default-off → unreachable, or include the retirement.

### Step 5: Ship the `MC2_VPL_CULL` projection-independence regression-guard probe (instrumentation-only; no production change)

> **v3.2 CORRECTION (2026-05-15) — SUPERSEDES the 5B slim-frustum-pass design below.**
> The prior 5B "swap the cull-cascade decision to a slim per-vertex frustum
> pass" design (preserved verbatim from §"Reframe" through §"Soak" for the
> amendment trail) was BLOCKED by the adversarial review
> `docs/superpowers/reviews/2026-05-15-step5-cull-cascade-adversarial-review.md`
> (review artifact owned by a sibling review step; orchestrator reconciles
> its presence — it is not committed in this Step 5 isolation worktree at
> base `cb221b8`), and a follow-up focused data-flow verification then
> proved its core premise FALSE. The corrected reality:
>
> **(a) Step 5 is instrumentation-only. `clipInfo` is proven
> projection-independent.** The only stock-reachable production branch is
> `if (eye->usePerspective && Environment.Renderer != 3)` (`terrain.cpp:~1725`),
> which writes `currentVertex->clipInfo = onScreen;`. `Environment.Renderer`
> is ONLY ever assigned `0` (grep: `code/logmain.cpp:786`,
> `code/mechcmd2.cpp:2815`) — never `3` — so this branch is always taken in
> stock/smoke config and the `else` branch (`clipInfo = inView`, which IS
> projection-dependent) is dead. `onScreen` is computed entirely
> non-projectively at `terrain.cpp:~1594-1676` (from `cameraPos`,
> `Camera::cameraFrame.trans_to_frame`, `verticalSphereClipConstant` /
> `horizontalSphereClipConstant`, `CLIP_THRESHOLD_DISTANCE=768`,
> `VERTEX_EXTENT_RADIUS=384`, `IsGameSelectTerrainPosition`, and the vertex
> `vx/vy/elevation`). The projection call
> `eye->projectForTerrainAdmission(...)` at `terrain.cpp:~1685` runs AFTER
> `clipInfo` is finalized and writes ONLY `inView` (local) plus
> `px/py/pz/pw`, which feed the `inView`-gated `leastZ/mostZ/leastW/mostW`
> reduction. Conclusion: the projection cannot affect the cull-cascade
> admission decision; Step 5 changes NO production behavior.
>
> **(b) The prior 5B frustum-AABB design was wrong.** The old `onScreen`
> is NOT an approximation of a true frustum test that can be "tightened"
> into a strict frustum-AABB pass. It is a deliberately-LOOSE CONTRACT:
> 768u proximity admit OR 384u-dilated cone OR out-of-play force-admit
> (`!IsGameSelectTerrainPosition` → `onScreen = true`). A true-frustum
> test is a strict SUBSET of this contract and would DROP objects /
> terrain quads that the loose contract admits — exactly the
> false-negative failure mode that orphans the ~9 `objmgr.cpp`
> cull-cascade consumers (CRIT-1 in the adversarial review). Additionally
> `isTerrainQuadVisible` at `quad.cpp:~397` (sums `quad.vertices[*]->clipInfo`
> at `quad.cpp:~401-407`) is an undocumented `clipInfo` consumer the 5B
> design never accounted for (CRIT-2). Tightening the
> contract is therefore unsafe and is NOT what Step 5 does.
>
> **(c) Step 5 ships ONLY the `MC2_VPL_CULL` regression-guard probe.**
> Env-gated (`getenv("MC2_VPL_CULL")`), default-off, zero cost when off,
> demote-don't-delete per the worktree Debug instrumentation rule. The
> probe sits immediately after the production `clipInfo` write (after the
> projection call has already run) and asserts
> `(DWORD)onScreen == currentVertex->clipInfo` per vertex, with a
> rate-limited (first 16) `[VPL_CULL v1] event=parity_violation` stderr
> line on mismatch and a 600-frame `[VPL_CULL v1] event=summary` cadence
> matching `MC2_TGL_POOL_TRACE` (`mclib/tgl.cpp:~3781`,
> `g_mc2FrameCounter % 600`). The probe is NOT a Step 5 behavior change —
> it is defense-in-depth for Step 8c: if anything ever secretly feeds
> `clipInfo` through the projection, this probe (kept alive THROUGH 8c)
> fires loudly when 8c deletes the projection loop. The actual
> projection retirement is entirely Step 6 (reductions) + Step 8c (delete
> the projection loop).
>
> **(d) STANDING CAVEAT for Step 8c.** Step 8c may delete the per-vertex
> projection loop, but it MUST NOT remove the `px/py/pz/pw` writes nor the
> `inView`-gated `leastZ/mostZ/leastW/mostW/leastWY/mostWY` reduction
> (`terrain.cpp:~1685` + `~1737-1759`). Those ARE projection-fed and are
> consumed by the reductions / `setInverseProject`. ONLY the `clipInfo`
> derivation is projection-independent. Step 8c keeps the projection's
> reduction outputs alive until Step 6 replaces that dependency.
>
> **(e) Sequencing reaffirmed: Step 5 → Step 6 → Step 8c.** Step 6
> strictly precedes Step 8c (Step 6 removes the reduction's projection
> dependency; Step 8c cannot delete the projection until then). Step 5,
> now instrumentation-only, lands first and gates 8c with the probe.
>
> Memory cross-refs (corrected): `memory/cull_gates_are_load_bearing.md`
> (the loose contract is load-bearing — do not tighten),
> `memory/cull_cascade_wrap_and_reduce_pattern.md` (the wrap-and-reduce
> framing applies to Step 6's reductions, NOT to a Step 5 cull rewrite).
>
> Everything from §"Reframe" through §"Soak" below is the SUPERSEDED v3
> 5B design, retained unedited as the amendment trail. Do NOT implement
> it. The shipped Step 5 is the probe described in (c) above.

**Reframe (replaces v1's audit-and-retire blocker):** do NOT retire the cull-cascade side-effects (`objBlockInfo[].active` + `objVertexActive[]` updates). Per advisor + reviewer, those writes have ~9 live consumers in `code/objmgr.cpp:1697/1704/1828/1835/2019/2027/2610/3060/3067` (production object-iteration loops) and MUST keep firing. Instead, keep the side-effect writes alive in a slimmed-down CPU pass while retiring the per-vertex projection math that surrounds them.

**Approach committed: 5B slim per-vertex point-AABB frustum pass.** v2 carried 5A / 5B / 5C as orchestrator-selectable alternatives. The orchestrator has selected 5B; v3 deletes the alternative blocks and commits the plan to a single concrete implementation.

**Committed shape correction (`mc2-terrain-indirect-expert` advisor, 2026-05-15, grep-verified at HEAD `2e11617`):** the Step 5 slim pass is a **per-vertex point-AABB frustum test**, NOT a per-quad pass. The 9 `objmgr.cpp` consumers read `objVertexActive[vertexNum]` at per-vertex granularity (`setObjVertexActive(currentVertex->vertexNum, true)` at `terrain.cpp:1718`); a per-quad-only pass cannot produce that granularity. Each vertex's world position is point-tested against the camera frustum (a point is a degenerate AABB) and `objBlockInfo[].active` / `objVertexActive[]` are set per the per-vertex result, exactly as the legacy cull-cascade decision at `terrain.cpp:1705-1718` does today. The slim pass deliberately does **NOT project** — it works in world space against frustum planes — so it produces **NO clip-space extents** (`screenPos.z/.w/.y`). The reductions are therefore NOT a free by-product of this pass; see Step 6.

**What Step 5 swaps, and what it KEEPS ALIVE (grep-verified at HEAD `2e11617`):** Step 5 replaces ONLY the cull-cascade decision — the `clipInfo` + `setObjBlockActive` + `setObjVertexActive` block at `terrain.cpp:1705-1718` (legacy projection-derived path) — with the slim per-vertex world-space frustum-AABB test. Step 5 **KEEPS the projection call alive** (`eye->projectForTerrainAdmission` at `terrain.cpp:1685`) and **KEEPS the reduction block alive** (`terrain.cpp:1722-1742`, the `leastZ/mostZ/leastW/mostW/leastWY/mostWY` accumulation), because those reductions read screen-space `screenPos.z/.w/.y` that only exist via the projection. After Step 5 lands, BOTH the old projection+reduction AND the new slim cull-cascade pass run simultaneously. This is safe: `objBlockInfo` / `objVertexActive` writes only ever set `true` (never clear mid-frame — see `terrain.cpp:1715-1718`), and the frame-start clear at `code/mission.cpp:501-502` (`clearObjBlocksActive()` / `clearObjVerticesActive()`, before the `land->geometry()` call at `mission.cpp:505`) resets both arrays every frame, so the union of the two passes' true-writes is conservative and self-healing per frame. **Step 5 does NOT eliminate per-vertex projection — it eliminates the cull-cascade's DEPENDENCY on projection.** Full projection-loop elimination is Step 8c, gated on Step 6 landing first (Step 6 is what removes the reduction's projection dependency).

**Conservative-superset safety property.** The slim per-vertex frustum-AABB test is negative-rejection safe: it can over-include (mark a vertex/block active that a tighter test would reject) but can NEVER false-negative (it never marks inactive a vertex/block that has visible content). Over-inclusion costs one extra object-block iteration that discovers no visible content and is otherwise inert; under-inclusion (the dangerous mode — orphaning a visible block) is impossible because a point that is inside the true visible set is inside any conservative superset of it. The parity assertion for Step 5 is an env-gated `MC2_VPL_CULL` **superset check**, run while both passes are live: for every vertex, assert `!(legacyVis && !vplVis)` — i.e. the slim pass's active set must be a superset of the legacy projection-derived active set. A single `legacyVis && !vplVis` (legacy says visible, slim says not) is a catastrophic violation (a false-negative — the failure mode that orphans cull consumers). Zero violations across the soak = pass; the slim pass may legitimately mark MORE active (superset), and that is expected, not a failure.

**Rationale for 5B (vs the dropped alternatives):**
- (a) **No CPU readback sync trap.** 5A's GPU-visibility-readback would have reintroduced the implicit-sync-stall bug class documented in `memory/substrate_coalesce_sync_point_lesson.md` (the substrate 2x perf regression of 2026-05-11 was a 4-byte readback). 5B is pure CPU; no GPU readback exists.
- (b) **Math-infrastructure shared with Step 3.** Step 3 (picking repoint) is also replacing a per-quad walk with a CPU camera-frustum × quad-AABB intersection. 5B uses the same primitive. Building the shared frustum-test helper once serves both steps.
- (c) **Conservative over-inclusion is correctness-safe.** False positives in cull cause an object-block iteration to do one extra microsecond of work and discover the block has no visible content; the iteration is otherwise unaffected. False negatives — marking a vertex/block inactive when it has visible content — are the dangerous failure mode and are impossible under 5B's negative-rejection-safe point-in-frustum test (see "Conservative-superset safety property" above).
- (d) **Decouples the cull cascade from per-vertex projection.** The core architectural goal of this plan is to retire `VertexProjectLoop`. 5A and 5C keep the cull cascade's per-vertex projection dependency for longer; 5B severs it now (the slim pass is pure world-space frustum math, no projection). 5B does not by itself delete the projection — the reductions still need it until Step 6 — but it removes the cull cascade's dependency, which is the prerequisite for Step 8c's projection-loop deletion.

**Implementation sketch:**

Inside `Terrain::geometry`, the slim per-vertex frustum pass runs ALONGSIDE the surviving projection+reduction block (it does not replace the projection loop body — see "What Step 5 swaps, and what it KEEPS ALIVE" above). The slim pass replaces ONLY the legacy cull-cascade decision at `terrain.cpp:1705-1718`:

1. Iterate the per-vertex list once (same iteration as the surviving loop, or co-located with it).
2. For each vertex, take its world-space position (`currentVertex->vx/vy` + `pVertex->elevation`, as `terrain.cpp:1682` already constructs `vertex3D`).
3. Run a standard 6/8-plane camera frustum test against that point (a point is a degenerate AABB; the shared `Camera::quadAabbInFrustum` helper accepts `mn == mx`). Return one bool: `vertexVisible`.
4. If `vertexVisible`: `setObjBlockActive(currentVertex->getBlockNumber(), true)` and `setObjVertexActive(currentVertex->vertexNum, true)` — same per-vertex granularity the legacy block at `terrain.cpp:1715-1718` writes (the 9 `objmgr.cpp` consumers read `objVertexActive[vertexNum]`; per-vertex granularity IS required, contrary to v3's earlier per-quad framing).
5. Only true-writes are emitted; the frame-start clear at `mission.cpp:501-502` handles reset.

The slim pass does NOT project, so it produces NO `screenPos.z/.w/.y`. The `leastZ/mostZ/leastW/mostW/leastWY/mostWY` reductions are therefore **NOT a free by-product of this pass** — they continue to be produced by the surviving projection+reduction block (`terrain.cpp:1685` + `:1722-1742`) until Step 6 replaces that dependency by its own design. Do not assume Step 5 hands Step 6 clip-space extents; it does not.

**Cost estimate:** the slim per-vertex frustum test is ~6-8 plane dot-products per vertex (no matrix-multiply projection), dramatically cheaper than the full projection. Note: Step 5 alone does NOT recover the full ~475 µs, because the projection+reduction block is still running after Step 5. The bulk recovery lands at Step 8c (projection-loop deletion), which is unblocked only after Step 6 removes the reduction's projection dependency.

**Path C eliminated.** The wrap-and-reduce framing eliminates the need for the v1 "Path C" (dirty-flag skip with object-list generation tracking) entirely. The reduced pass is cheap enough at full frequency that we don't need a skip path.

**Shared frustum-AABB helper — REFERENCE, do not re-add (B5).** The 8-plane frustum test against quad AABBs uses `Camera::quadAabbInFrustum` / `Camera::extractFrustumPlanes`. These are **DEFINED by Step 3 commit 3a** (see Step 3 "Shared frustum-AABB helper"). Step 5B **references** them via the `Camera*` (`eye`) already in scope; it MUST NOT re-declare or re-define them (merge-conflict / double-add avoidance). Per current plan numbering Step 3 lands first and owns the definition; only if Step 5 somehow lands before Step 3 would the ownership invert (then Step 5 defines, Step 3 references) — but the plan ships Step 3 first, so Step 3 owns it.

**Co-location requirement.** The reduced pass MUST stay co-located with the rest of `Terrain::geometry`. Do not factor into a separate function in this step — that's a refactor for step 10 cleanup. (The shared helper is a `Camera` member per B3/B5, separate from this co-location rule.)

**Sensitivity consult:** the `mc2-render-perf-expert` advisor should confirm before commit that the 9 `objBlockInfo[].active` consumers in `code/objmgr.cpp` are insensitive to over-inclusion (objects whose owning quad-AABB intersects the frustum but whose actual on-screen footprint is empty). Expected answer: insensitive — the consumers do their own per-object visibility refinement downstream.

**Verification:** tier1 smoke + visual smoke (object-iteration cull is the canary class — Fix-A-style ghost-mech / streak symptoms during smoke = CRITICAL regression) + the env-gated `MC2_VPL_CULL` superset parity check run while both the legacy projection-derived cull and the slim pass are live (per the "Conservative-superset safety property" above: assert `!(legacyVis && !vplVis)` per vertex; zero violations = pass; the slim pass legitimately marking MORE active is the expected superset, not a failure). Run the substrate canary `mc2_10` first. See §"Step 5 + Step 8c shared `mc2_10` canary triage order" below for the protocol when parallel soaks both surface a regression simultaneously.

**Soak:** 7 days, parallel with Step 3 and Step 8c soaks.

### Step 6: Slim CPU min/max pass for shared reductions (wrap and reduce)

> **v3.3 CORRECTION (2026-05-15, `mc2-terrain-indirect-expert` advisor, grep-verified at HEAD `55b1167`) — Step 6 RE-HOMES the projection; it does NOT eliminate it.**
>
> The reduction (`leastZ/mostZ/leastW/mostW/leastWY/mostWY` → `eye->setInverseProject(mostZ, leastW, yzRange, ywRange)`) requires per-vertex projected `screenPos.z/.w/.y` over the **in-rect-visible** vertex set (the `onScreen`-gated set, not all vertices, not the world AABB). **PROVEN unsound:** min/max of projected-z over that set is NOT equal to the projection of the world-space AABB extremes. The reduced set is per-frame view-dependent under the oblique cinematic camera (`memory/camera_model_oblique_cinematic.md`), and perspective-divide does not preserve ordering — so option (ii) "project just the 8 AABB corners" and any analytic-from-bounds approach are **unsound** and are struck. Of the v3 option set, only option (i) survives: **keep a minimal `projectForTerrainAdmission` call alive ONLY for the reduction.**
>
> **Committed Step 6 shape:** extract a self-contained **slim min/max-only loop** that iterates the in-rect-visible vertex set, calls a minimal `eye->projectForTerrainAdmission(vertex3D, screenPos)` per vertex purely to feed `screenPos.z/.w/.y` into the `leastZ/mostZ/leastW/mostW/leastWY/mostWY` accumulation, then calls `eye->setInverseProject(mostZ, leastW, yzRange, ywRange)` once per frame as today. This loop is **decoupled from the cull cascade** (Step 5 proved the cull-cascade decision is projection-independent — `clipInfo` derives from `onScreen`, computed non-projectively). It is therefore a **SURVIVING artifact**: Step 8c deletes the VPL body but MUST NOT delete this slim re-homed min/max loop.
>
> **Line-number refresh (grep-verified at HEAD `55b1167`; prior advisor citations had drift):**
> - Reduction accumulation: fast path `terrain.cpp:1592-1595` (prior plan said `:1590-1596`); legacy fallback `terrain.cpp:1765-1786` (prior plan said `:1763-1786`).
> - File-scope accumulators `terrain.cpp:1367-1369`; per-frame reset `terrain.cpp:1408-1410`.
> - `yzRange`/`ywRange` derivation `terrain.cpp:1942-1949`; `eye->setInverseProject(...)` call `terrain.cpp:1949`.
> - The surviving projection call this loop preserves: `eye->projectForTerrainAdmission(vertex3D, screenPos)` at `terrain.cpp:1543` (fast path) / `:1702` (legacy fallback).
>
> **`MC2_VPL_REDUCE` parity probe (defense-in-depth, same shape as Step 5's `MC2_VPL_CULL`).** Env-gated (`getenv("MC2_VPL_REDUCE")`), default-off, demote-don't-delete per the worktree Debug instrumentation rule. **Exact-equality (NOT epsilon)** comparison: the slim loop and the old inline reduction call the *same* `projectForTerrainAdmission`, so the four `setInverseProject` args (`mostZ`, `leastW`, `yzRange`, `ywRange`) must be **bit-identical**. Rate-limited (first 16) `[VPL_REDUCE v1] event=parity_violation` stderr line on mismatch + a 600-frame `[VPL_REDUCE v1] event=summary` cadence (matches `MC2_TGL_POOL_TRACE` / `MC2_VPL_CULL`). Zero `parity_violation` over a tier1 30s run = pass. **Honest gap:** passive smoke cannot open the tacmap, so the byte-compare of unprojected viewport corners is not exercised by smoke; the `MC2_VPL_REDUCE` probe is the real gate, and the tacmap F-key UAT (§Verification) is user-driven confirmation, not an automated gate.
>
> **Sequencing (load-bearing): Step 6 strictly precedes Step 7.** Both Step 6's slim loop and Step 7's haze rewrite touch the legacy `onScreen`/haze derivation region (`terrain.cpp:~1472-1533` fast / `~1611-1693` legacy). Step 6's slim min/max loop copies the current `onScreen` admission logic to gate which vertices it projects; Step 7 then rewrites haze in that same region. **Step 7's execution checklist MUST include: "update the Step 6 slim-reduce `onScreen` copy in lockstep with any `onScreen`/admission edit."** Strict order: **Step 5 → Step 6 → Step 7 → Step 8c.** (Step 6-before-8c was already load-bearing; Step 6-before-7 is the v3.3 addition.)
>
> Everything from "Reviewer-correction note (A4)" through "Verification" below is the prior v3 text, retained for the amendment trail. Where it says option (ii)/(iii) is open or that Step 6 "produces the reductions by its own design (not settled)", the v3.3 block above governs: the design IS settled — re-home option (i).

**Reviewer-correction note (A4):** the adversarial reviewer's CRIT-4 was WRONG — their grep missed the `inverseProjectForPicking` wrapper at `mclib/camera.h:634` which is what `code/gametacmap.cpp:225/232/239/246` actually calls. The inventory doc `projectz-callsite-inventory.md:24` was written pre-policy-split. Reductions ARE live.

**Live chain:** VPL writes `leastZ/mostZ/leastW/mostW/leastWY/mostWY` at `terrain.cpp:1547-1571` → `eye->setInverseProject(mostZ, leastW, yzRange, ywRange)` at `:1891` → `Camera::inverseProjectZ` at `mclib/camera.cpp:1882` → 4 tacmap callsites at `code/gametacmap.cpp:225/232/239/246`.

**Change:** replace the reduction's *dependency* on the full per-vertex projection with a slim CPU min/max pass that Step 6 designs (see "Step 6 produces the reductions by its OWN design" below for the option set Step 6 owns). The replacement is dramatically cheaper than VPL's 33k-vertex projection (advisor estimate ~100× cheaper for an AABB-corner-only approach). Land it as a small inline pass in `Terrain::geometry` that derives `leastZ/mostZ/leastW/mostW/leastWY/mostWY` once per frame and then calls `eye->setInverseProject(mostZ, leastW, yzRange, ywRange)` exactly as today. Note: until Step 6 lands, the post-Step-5 status quo is that the original projection+reduction block (`terrain.cpp:1685` + `:1722-1742`) is still running — Step 5 deliberately kept it alive (see Step 5).

**Step 6 produces the reductions by its OWN design — NOT harvested free from Step 5 (`mc2-terrain-indirect-expert` advisor, 2026-05-15).** v3's earlier text claimed Step 5's 5B pass is "already iterating quad AABBs" so the `leastZ/mostZ/leastW/mostW/leastWY/mostWY` min/max "becomes free." **That is wrong.** Step 5's committed shape is a per-vertex point-AABB *frustum* test that deliberately does NOT project and therefore produces NO clip-space extents (`screenPos.z/.w/.y`) — see Step 5 "What Step 5 swaps, and what it KEEPS ALIVE." Step 5 hands Step 6 nothing. Step 6 must produce the reductions by its own means, and **how is Step 6's own design decision, explicitly NOT settled by Step 5.** The options Step 6 owns deciding between include: (i) keep the residual projection alive only for the reduction (the post-Step-5 status quo — the projection+reduction block at `terrain.cpp:1685` + `:1722-1742` keeps running), formalized into a deliberately-slim min/max-only pass; (ii) derive clip-space min/max from quad-AABB bounds projected by Step 6's own added projection of just the 8 AABB corners (far fewer projections than per-vertex); or (iii) another approach the execution session justifies. Whichever Step 6 chooses, it ends with `eye->setInverseProject(mostZ, leastW, yzRange, ywRange)` called exactly once per frame as today.

**Sequencing constraint (load-bearing): Step 6 strictly precedes Step 8c.** Step 8c deletes the projection loop. The reduction code depends on the projection until Step 6 replaces that dependency. Therefore Step 8c cannot land until Step 6 has landed (in addition to Step 8's existing Step 3 7-day soak gate). Sequence: **Step 5 → Step 6 → Step 8c.** See Step 8 and §"Soak window parallelization."

**Verification:** tier1 smoke + tacmap F-key UAT (viewport corners must unproject to identical world positions byte-for-byte) + parity check that `eye->setInverseProject` arguments are identical to pre-retirement values within float epsilon. Frame-perfect screen byte-compare on 30s capture.

### Step 7: hazeFactor port to GPU compute (inline-worldPos edge-clamp)

> **v3.3 CORRECTION (2026-05-15, `mc2-shader-expert` advisor, grep-verified at HEAD `55b1167`) — HIGHEST PRIORITY. Step 7 must NOT delete the terrain.cpp VPL `hazeFactor` writes.**
>
> The v3 "C++ side VPL retirement (in this same commit)" instruction below is **WRONG and is struck.** The `MC2_TERRAIN_LIGHTING_GPU=0` legacy CPU-lighting fallback still consumes `vertices[]->hazeFactor`, and **VPL is the ONLY writer of that field.** Grep-verified at HEAD `55b1167`:
> - `quad.cpp:1319` declares `static const bool s_lightingGpuAuth = gos_terrain_lighting::IsEnabled() ...`; `quad.cpp:1321` is the gate `if (!s_lightingGpuAuth && terrainHandle != 0xffffffff)` — the legacy CPU-lighting path.
> - That path reads `vertices[N]->hazeFactor` at `quad.cpp:1471/1473` (corner 0), `:1625/1627` (corner 1), `:1779/1781` (corner 2), `:1931/1933` (corner 3).
> - The ONLY writers of `Vertex::hazeFactor` are the VPL fast path and legacy fallback in `terrain.cpp` (line refresh below). Deleting them in Step 7 leaves the `MC2_TERRAIN_LIGHTING_GPU=0` path reading stale/zero fog with **no compile error** — a silent legacy-lighting regression.
>
> **Step 7 scope is REDUCED to: shader + uniforms + populate-neutralize ONLY.** Specifically Step 7 lands: (1) the new `g_playAreaBounds` / `g_minHazeDistance` / `g_maxClipDistance` / `g_distanceFactor` uniforms + their host upload; (2) the inline-worldPos edge-clamp `hazeFactor` computation in `shaders/gos_terrain_lighting.comp`; (3) the populate-write neutralization at `gos_terrain_lighting.cpp:553` (see "Lockstep" below). Step 7 does **NOT** touch any `terrain.cpp` VPL `hazeFactor` write and does **NOT** retire the GLSL/C++ `hazeFactor` struct field.
>
> **The VPL C++ `hazeFactor` write deletion + the legacy-CPU-lighting-path death move to Step 8c.** Step 8c is where the whole VPL loop body dies behind the soak gate; the `MC2_TERRAIN_LIGHTING_GPU=0` path is itself legacy and dies in that same window (it has no surviving non-VPL `hazeFactor` source once the GPU lighting path is authoritative). Step 8c OWNS: deleting the VPL `hazeFactor` writes (line refresh below) AND retiring the `quad.cpp:1321` legacy-CPU-lighting fallback. See the amended Step 8c.
>
> **Lockstep = keep-field-stop-reading (smallest blast radius).** `GpuTerrainVertexInput` is `alignas(16)` at `gos_terrain_lighting.h:35` with `static_assert(sizeof(GpuTerrainVertexInput) == 32, ...)` at `:42`; `float hazeFactor;` is at `:38`. The field is 4 bytes that are padded anyway — removing it is stride-neutral and the `==32` static_assert stays valid either way. For the smallest blast radius, Step 7 **KEEPS** the GLSL field (`terrain_lighting_shared.hglsl`) and the C++ field (`gos_terrain_lighting.h:38`) — comment each as dead — and changes ONLY the populate write at `gos_terrain_lighting.cpp:553` from `vi.hazeFactor = v.hazeFactor;` to `vi.hazeFactor = 0.0f;` (defensive — avoids reading a stale `v.hazeFactor` after Step 8c deletes the VPL write). **Actual field removal is deferred to Step 10 cleanup** (after the Step 8c soak), as one lockstep commit editing GLSL + C++ + static_assert together per `memory/cpp_glsl_ubo_struct_lockstep.md`. The v3 "Lockstep retirement (in this same commit)" block below is superseded by this paragraph.
>
> **Water bugfix is terrain-only; dedicated-water-path edge-clamp is a TRACKED FOLLOW-UP.** The inline edge-clamp fogs out-of-bounds water IF water consumes the lighting compute's `fogRGB` output. The dedicated water draw path (`shaders/gos_terrain_water_mdi.frag`, `shaders/gpu_driven_water.comp` — both confirmed present at HEAD `55b1167`) is unverified: if water bypasses the lighting-compute fog output, the same `outsidePlayArea` clamp must be added there. **Step 7 fixes terrain-lit water only.** The dedicated-water-path edge-clamp is a tracked follow-up (see §"Deferred / tracked follow-ups"), NOT a Step 7 blocker.
>
> **Formula / uniform notes.** CPU `distanceToEye` used `GetApproximateLength` (octagonal approx) after a length-preserving camera-frame transform; the GLSL `length()` is exact — this is an intentional re-derivation, NOT bit-parity. [**v3.4 ANNOTATION:** the original v3.3 phrase "parity-correct upgrade ... the binary clamp is identical when `MinHazeDistance == MaxClipDistance == 50000`" is narrowed by the v3.4 ruling above. Step 7's inline `hazeFactor` does NOT achieve bit-parity with VPL and is NOT required to: VPL's off-screen blanket-`1.0` (culling artifact) vs inline real-distance diverges on ~100% of sampled verts by design, and `MC2_HAZE_PARITY` reporting that is EXPECTED, not a defect. The "binary clamp identical" claim holds ONLY for the in-bounds in-view subset under the dead stock ramp; the `GetApproximateLength`→`length()` ~10% delta is a tracked known item that surfaces under non-stock fog constants. Do NOT read this line as "Step 7 must reach hazeFactor parity" or "the probe must read zero".] The camera haze constants (`Camera::MinHazeDistance` / `MaxClipDistance` / `DistanceFactor`) are runtime mission state, so the new `g_minHazeDistance` / `g_maxClipDistance` / `g_distanceFactor` uniforms MUST be uploaded as live per-frame uniforms, **not baked** at mission load.
>
> **VPL `hazeFactor` write line refresh (grep-verified at HEAD `55b1167`; for Step 8c's deletion list, NOT Step 7):**
> - Fast path ramp `terrain.cpp:1512-1514`; edge clamp `:1520`; not-onScreen branch `:1526`; not-eligible neutral `:1531`; final live write `cv->hazeFactor = hazeL` at `:1573` (snapshot mirror `s.hazeFactor = hazeL` at `:1568`).
> - Legacy fallback `terrain.cpp:1661-1692` (ramp `:1663/:1667/:1671`, edge clamp `:1680`, not-onScreen `:1686`, not-eligible neutral `:1691`).
> - (Prior plan cited `:1473/1512-1526/1573` fast and `:1661-1691` legacy — refreshed above.)
>
> Everything from "Context (CRIT-1 resolution + v3 rewrite)" through "Verification" below is prior v3 text retained for the amendment trail. Where the v3 "C++ side VPL retirement (in this same commit)" and "Lockstep retirement (in this same commit)" blocks conflict with the v3.3 block above, **the v3.3 block governs**: those deletions move to Step 8c; lockstep is keep-field-stop-reading.

> **v3.4 RULING (2026-05-15, focused three-way reconciliation + orchestrator ruling — SETTLED, not a design fork) — Step 7's inline `hazeFactor` is an INTENTIONAL re-derivation, NOT bit-parity with VPL. This is the verdict (B) of `memory/parity_probe_100pct_can_be_correct_redesign_report.md`.**
>
> The `MC2_HAZE_PARITY` probe firing on ~100% of sampled vertices every frame (`[HAZE_PARITY v1] event=summary ... max_abs_delta=1 violations≈samples*frames`, full [0,1] categorical mismatch) is the **EXPECTED, CORRECT** output of this probe. It is the probe correctly reporting that inline ≠ VPL **by design** — it is NOT noise (verdict A, rejected) and NOT a regression (block-Step-7, rejected). A focused three-way reconciliation (probe recompute vs GPU shader vs the legacy VPL write) proved the probe is a faithful transcription of the shader (same camera-pos swizzle, same play-area `m`, same predicate, same ramp); probe == producer, so the divergence is REAL and the only remaining question was the design ruling, which the orchestrator settled: **the non-parity is intended; Step 7 ships as-is.**
>
> **Future-reader contract (load-bearing):** "screenshots pixel-identical to baseline + `MC2_HAZE_PARITY` screaming ~100% / `max_abs_delta=1`" must be read as **GREEN-by-design for Step 7** — NOT as a defect, and NOT as a naive screenshot-green that would falsely greenlight a hidden divergence. The divergence is real, intended, and confined to non-visible outputs in the stock config; the screenshot match proves only the visible (in-bounds in-view) subset agrees, which is exactly what is required of Step 7.
>
> **Formula-level reason (why inline ≠ VPL on ~100% of sampled verts):**
> - Legacy VPL stamps `hazeFactor = 1.0` for **off-screen** vertices via its `onScreen` clip gate — a culling artifact, not a fog computation. Grep-verified at HEAD `d520967`, `mclib/terrain.cpp`: the fast-path `onScreen` clip-reject runs at `terrain.cpp:1519-1533` (`distanceToClip > CLIP_THRESHOLD_DISTANCE` → angle test → `onScreen = false` at `:1525`/`:1531`); when `onScreen` is false the code falls to the `else` at `:1549-1552` which stamps `hazeFactor = 1.0f` at `:1551` (also the not-`IsGameSelectTerrainPosition` clamp at `:1545`). The final live write `cv->hazeFactor = hazeL` is at `terrain.cpp:1598` (snapshot mirror `s.hazeFactor = hazeL` at `:1593`). `CLIP_THRESHOLD_DISTANCE` is defined at `terrain.cpp:1362`. (Line drift from the v3.3 `55b1167` citation `:1520/:1526/:1573/:1568` to HEAD `d520967` `:1519-1533/:1551/:1598/:1593` is expected; symbols stable, numbers re-verified here.)
> - The inline formula has **no `onScreen` concept** and computes real distance: `shaders/gos_terrain_lighting.comp:298-305` — `outsidePlayArea` predicate at `:298-299`, real `distanceToEye = length(hpos - u_cameraWorldPos)` at `:302`, ramp at `:303-305`. For in-bounds-near vertices it computes `0.0`. The lighting pack loop walks the camera-windowed vertex list, which includes many VPL-off-screen verts → `|0.0 - 1.0| = 1` on ~every sampled vertex. Uniforms at `:67-70`.
> - Secondary: VPL's CPU `distanceToEye` used `GetApproximateLength` (octagonal approx, `terrain.cpp:1512`) after a length-preserving camera-frame transform; the GLSL `length()` is exact — a ~10% per-vertex precision delta. It is **masked in stock** because the stock fog ramp is dead (`Camera::MinHazeDistance == MaxClipDistance`, so the `else if` ramp branch at shader `:304` / CPU `terrain.cpp:1538` is never taken — `hazeFactor` is binary 0.0/1.0). It is a tracked known item (see §"Deferred / tracked follow-ups"), not a Step 7 concern.
> - Invisible in stock because (a) the diverged verts (off-screen) draw no visible pixels in the test config AND (b) the stock fog ramp is dead per the point above, so `hazeFactor` is binary and the only places inline/VPL disagree (off-screen blanket-1.0) are exactly the places that paint nothing.
>
> **This divergence IS the intended out-of-bounds-water fix** — the original Step 7 goal (see "Bug-fix annotation (positive side effect)" below). VPL's off-screen blanket-`1.0` was a culling artifact that never fogged out-of-bounds water (water never went through VPL's `IsGameSelectTerrainPosition` clamp); the inline real-distance + `outsidePlayArea` rectangle does. Accepted by orchestrator ruling. The probe is a faithful transcription per `GameOS/gameos/gos_terrain_lighting.cpp` (decl `:52-59`, CPU-recompute + compare `:595-616`, summary emit `[HAZE_PARITY v1]` at `:650`). Cite `memory/parity_probe_100pct_can_be_correct_redesign_report.md`.

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

**Lockstep retirement (in this same commit):** [SUPERSEDED by the v3.3 block at the top of Step 7 — lockstep is keep-field-stop-reading; field removal deferred to Step 10. Retained for the amendment trail; do NOT implement the field deletion in Step 7.] once the GPU path produces `hazeFactor` inline, the SSBO field is dead. Retire all three in lockstep per `memory/cpp_glsl_ubo_struct_lockstep.md`:
- GLSL struct: `shaders/include/terrain_lighting_shared.hglsl:7` — remove `float hazeFactor;` from `struct GpuTerrainVertexInput`.
- C++ struct: `GameOS/gameos/gos_terrain_lighting.h:38` — remove `float hazeFactor;` field declaration.
- C++ writer: `GameOS/gameos/gos_terrain_lighting.cpp:553` — remove `vi.hazeFactor = v.hazeFactor;`.
- The struct size drops by 4 bytes; verify any `sizeof(GpuTerrainVertexInput)` static_assert or `offsetof` assertion is updated in lockstep. Cite `memory/cpp_glsl_ubo_struct_lockstep.md` in the commit message; this is the canonical hazard from that lesson.

**C++ side VPL retirement (in this same commit):** [SUPERSEDED by the v3.3 block at the top of Step 7 — this deletion moves to Step 8c (which OWNS the VPL `hazeFactor` write deletion + the `quad.cpp:1321` legacy-CPU-lighting-path death). Step 7 does NOT delete any `terrain.cpp` VPL `hazeFactor` write. Refreshed line numbers are in the v3.3 block + amended Step 8c. Retained below for the amendment trail; do NOT implement in Step 7.] delete the VPL hazeFactor writes:
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

**Verification:** tier1 smoke + frame-perfect screen byte-compare on 30s capture inside the play area (haze in-bounds must match baseline). Out-of-bounds water UAT on `mc2_17` is a positive-delta expected change, not a regression. Visible canary: the world boundary's `hazeFactor = 1.0` clamp must produce identical fog at the edge of terrain (in-bounds) as baseline; water extending past the boundary must NOW render fogged (was rendering clear pre-Step-7). [**v3.4 ANNOTATION:** the `MC2_HAZE_PARITY` whole-vertex-list count is **NOT** a Step 7 pass/fail signal — it is EXPECTED-nonzero (`max_abs_delta=1`, ~100% of samples) by the v3.4 ruling and reports the intended inline≠VPL re-derivation. Step 7 verification is the visible in-bounds byte-compare + out-of-bounds-water positive-delta UAT only. Do NOT gate Step 7 on `MC2_HAZE_PARITY` reading zero or low.]

### Step 8: VertexProjectLoop body retirement (three-commit split)

**Precondition:** after steps 1-7, the loop body's per-vertex projection writes have no live consumers (`hazeFactor` GPU-ported in step 7 — but its **VPL C++ write deletion is owned by Step 8c**, NOT step 7, per the v3.3 Step 7/8c correction, because the `MC2_TERRAIN_LIGHTING_GPU=0` legacy fallback at `quad.cpp:1321` still reads it and only VPL writes it; `pz`/picking repointed in step 3; cull-cascade decoupled from projection in step 5; the reduction's projection RE-HOMED into Step 6's decoupled slim min/max loop — not eliminated). **Step 8c is gated on the consolidated four-condition gate in the amended Step 8c below (Step 1 + Step 3+soak + Step 6 + Step 8b all landed); Step 6 is necessary-not-sufficient.** Step 6 RE-HOMES the projection (it does not free it for outright deletion — the re-homed slim reduction loop SURVIVES 8c). Strict commit order: **Step 5 → Step 6 → Step 7 → Step 8c.** The legacy CPU SOLID/TES path at `quad.cpp:2299-2840` still reads `pz`/`px`/`py`/`pw` but is itself unreachable under `MC2_TERRAIN_INDIRECT=1` (default); its retirement is the CPU-pack plan's commit 2 scope, not this plan.

**Why split into three commits (A6):** the loop body is woven into multiple consumer chains. A single-commit "delete body + parity + struct" lands too much in one revert window. Split:

- **Step 8a — no objmgr.cpp consumer code change required; comment-only update + retire VPL's per-vertex projection writes.** Since Step 5 picked 5B which keeps `objBlockInfo[].active` and `objVertexActive[]` as the CPU-side cull state (now written by the slim quad-AABB-frustum-test pass), the 9 consumer sites at `code/objmgr.cpp:1697/1704/1828/1835/2019/2027/2610/3060/3067` need no functional change. The commit lands two things: (1) a one-line comment update at each consumer site documenting that the new source of truth is the Step 5 slim pass (not VPL); (2) retirement of the now-dead per-vertex projection writes (`px/py/pz/pw`) that no longer have any production consumers — these writes still live in the loop body but feed nothing. Step 8a becomes a smaller, lower-risk commit than v2 envisioned.

  > **v3.5 CORRECTION (CRIT-0) — SUPERSEDES the "Step 5 slim pass" framing in the Step 8a paragraph above.** There is NO "Step 5 slim pass" cull producer (Step 5 shipped instrumentation-only per v3.2; the v3.2 5B design was never implemented). The post-8c cull-cascade source of truth is the **cull-merged Step 6 slim reduction loop** (`terrain.cpp:~1923-2018`), into which 8c-part-1 merges the per-vertex `clipInfo`/`setObjBlockActive`/`setObjVertexActive` writes (subject to the CRIT-1 `:1993` placement rule). Step 8a's actual scope: (1) verification at the 9 `objmgr.cpp` consumer sites (all grep-confirmed at HEAD `29ae435`, zero drift) + a one-line comment at each documenting that the post-8c source is the cull-merged Step 6 loop (NOT a "Step 5 slim pass", NOT the VPL body); (2) NO `px/py/pz/pw` write retirement here — that is **Step 8b** (it requires the overlay-pz precursor, landed `29ae435`). Step 8a is a no-functional-change comment-correction + verification commit. Where the paragraph above says "Step 5 slim pass" or folds the `px/py/pz/pw` retirement into 8a, this v3.5 tag governs.

- **Step 8b — retire VPL's per-vertex writes (`px/py/pz/pw`).** Fields survive on the `cv` struct because non-VPL consumers (`quad.cpp` reads `pz` at `:357` debug-trace path, etc.) still reference them; VPL simply no longer writes them. Downstream consumers see stale data from previous frames, which is correct because their VPL-dependency was retired in earlier steps. `clipInfo` writes also retire here (their consumer went away in step 3).

  > **v3.5 ADDITION to Step 8b scope (MAJOR-a + gate, grep-verified at HEAD `29ae435`):** (1) **Delete `Camera::getClosestVertex`** — `mclib/camera.cpp:568` (def) + `mclib/camera.h:661` (decl). Grep-confirmed ZERO callers in `mclib/`+`code/`+`GameOS/`; it reads `topVertex->px/py` which 8b makes per-frame-stale. Delete-not-leave (the additive-debt reactivation trap, `mc_texture_manager_dual_queue_legacy_retirement_debt.md`) — delete it in/with the 8b commit. (2) **8b is now UNBLOCKED**: 8b's `cv->pz` deletion was gated on the M2d overlay-pz-gate precursor, which has LANDED (`29ae435`: re-projection + mandatory `clipInfo == 0` sentinel guard at `quad.cpp:2147` + `MC2_M2D_PZ_PARITY` probe at `quad.cpp:303`/`:2156`). Before 8b deletes `cv->pz`, the `MC2_M2D_PZ_PARITY` probe MUST read zero (both sources live to bit-compare: `quad.cpp:2169` `old_adj = vertices[c]->pz + TERRAIN_DEPTH_FUDGE` vs the re-projected source).

- **Step 8c — delete the projection loop body + scaffolding.** Removes the VPL projection loop body, the parity comparator, the `VPParitySnap` struct, the `MC2_VERTEX_PROJECT_FAST` / `MC2_VERTEX_PROJECT_PARITY` env vars, and the `vertexProjectLoop` Tracy zones (grep-verify all ranges at write-time — line numbers below were refreshed at HEAD `55b1167` but will drift again before 8c lands). `Terrain::geometry` itself stays — it has non-VPL responsibilities (`beginFrameTexResolve`, `quadSetupTextures`, `cloudUpdate`).

  > **v3.3 CARVE-OUT (load-bearing) — what Step 8c MUST NOT delete:** the Step 6 **re-homed slim min/max-only loop** (the minimal `projectForTerrainAdmission` call kept alive ONLY to feed `leastZ/mostZ/leastW/mostW/leastWY/mostWY` → `setInverseProject`) is a SURVIVING artifact. Step 6 RE-HOMES the projection out of the VPL body into that decoupled slim loop — it does NOT eliminate per-vertex projection for the reduction. Step 8c deletes the VPL body but the re-homed slim reduction loop survives 8c (it has no soak gate of its own beyond Step 6's landing). Likewise the Step 5 `MC2_VPL_CULL` probe survives 8c as the tripwire (it reads cull-derived `clipInfo`, not projection). The `MC2_VPL_REDUCE` probe (Step 6) may be demoted-not-deleted alongside it.

  > **v3.5 CARVE-OUT CORRECTION (CRIT-0 + CRIT-1 + MAJOR-d) — SUPERSEDES the v3.3 carve-out's cull/probe framing above.** Two corrections to the v3.3 carve-out: (1) The Step 6 slim reduce loop survives 8c — but 8c-part-1 also **MERGES the cull-cascade writes INTO it** (it is the post-8c cull producer; there is no "Step 5 slim pass"). Those merged cull writes (`clipInfo`/`setObjBlockActive`/`setObjVertexActive`) MUST be emitted on the `onScreenR`/`clipR` decision **BEFORE** the reduction-admission `continue` at `terrain.cpp:1993` (`:1993-1994` at HEAD `29ae435`; `if (!onScreenR) continue;` is `:1984-1985`) — placing them after `:1993` or gating on `inViewR` makes the active-set a strict subset of legacy `{ onScreen }` and objects/mechs vanish (CRIT-1). The reduction's `!clipR || !inViewR` gate stays as-is but applies ONLY to the `leastZ/mostZ/...` accumulation, decoupled from the cull write. (2) The `MC2_VPL_CULL` probe does NOT "survive 8c as the tripwire" indefinitely. It survives ONLY through 8c-part-1 as a co-production probe asserting slim ⊇ legacy (`!(legacyActive && !slimActive)`) per vertex against the still-live VPL body, exercised with **camera motion** (CRIT-2; default static smoke misses the off-rect band). At 8c-part-2 the VPL body is gone, no independent producer remains, and a self-comparison is a pure tautology — the probe is explicitly RETIRED or demoted to `MC2_VPL_REDUCE`-style (MAJOR-d). Where the v3.3 carve-out says the probe "survives 8c as the tripwire", this v3.5 tag governs.

  > **v3.3 OWNERSHIP (load-bearing — `mc2-shader-expert` advisor, 2026-05-15) — Step 8c OWNS the VPL `hazeFactor` write deletion + the legacy-CPU-lighting-path death.** Step 7 was REDUCED to shader+uniforms+populate-neutralize and explicitly does NOT touch any `terrain.cpp` VPL `hazeFactor` write (deleting them in Step 7 would silently break the `MC2_TERRAIN_LIGHTING_GPU=0` legacy fallback, the only consumer, which has no other `hazeFactor` source). Therefore Step 8c additionally deletes:
  > - The VPL `hazeFactor` writes (grep-verify at write-time; refreshed at `55b1167`): fast path ramp `terrain.cpp:1512-1514`, edge clamp `:1520`, not-onScreen `:1526`, neutral `:1531`, final write `:1573` (+ snapshot `:1568`); legacy fallback `:1661-1692`.
  > - The `quad.cpp:1321` legacy-CPU-lighting fallback (gated on `!s_lightingGpuAuth`, declared `quad.cpp:1319`) and its `vertices[N]->hazeFactor` reads at `quad.cpp:1471/1473/1625/1627/1779/1781/1931/1933`. This legacy path dies in the same 8c soak window — once the GPU lighting path is authoritative it has no surviving non-VPL `hazeFactor` source, so the VPL `hazeFactor` write and the path that reads it die together.
  > - NOT the GLSL/C++ `hazeFactor` struct field — that is keep-field-stop-reading until Step 10 (per Step 7's amended Lockstep paragraph + `memory/cpp_glsl_ubo_struct_lockstep.md`).

  > **v3.5 LEGACY-LIGHTING CORRECTION (MAJOR-c) — SUPERSEDES the "only `MC2_TERRAIN_LIGHTING_GPU=0`" scope in the v3.3 ownership block above.** `quad.cpp:1319` is `s_lightingGpuAuth = gos_terrain_lighting::IsEnabled() && !gos_terrain_lighting::IsParityCheckEnabled();`. The `!s_lightingGpuAuth` `hazeFactor`-reading block is reachable under **EITHER** `MC2_TERRAIN_LIGHTING_GPU=0` (`IsEnabled()==false`) **OR** `MC2_TERRAIN_LIGHTING_PARITY=<set>` (`IsParityCheckEnabled()==true` — the lighting parity probe the team uses). The v3.3 prose names only the `=0` path; that is incomplete. After 8c deletes the VPL `hazeFactor` writes, terrain `Vertex::hazeFactor` has NO writer AND NO defensive init (`mapdata.cpp` never sets it — grep-confirmed). 8c's legacy-lighting retirement/guard MUST: (i) cover BOTH env entries (delete or constant-neutralize the `quad.cpp:1471/1473/1625/1627/1779/1781/1931/1933` reads so `MC2_TERRAIN_LIGHTING_PARITY=1` does NOT silently read stale fog post-8c); AND (ii) zero-init terrain `Vertex::hazeFactor` at vertex construction so any stray surviving reader degrades to "no fog" (stock-playable per `stock_install_must_remain_playable.md`), not garbage. Where the v3.3 block names only `MC2_TERRAIN_LIGHTING_GPU=0`, this v3.5 tag governs.

  > **v3.4 STEP 8c GATE REQUIREMENTS (2026-05-15, orchestrator ruling — SETTLED). Once 8c deletes VPL's `hazeFactor` write, the inline formula becomes the SOLE source and the intentional inline≠VPL divergence becomes the only behavior. Therefore 8c's parity gate must be re-scoped — the existing whole-vertex-list `MC2_HAZE_PARITY` count is NOT a valid 8c pass/fail (it is expected-nonzero by the v3.4 Step 7 ruling).** Add to Step 8c's requirements:
  > - **(a) Visible-pixel parity gate (REQUIRED at 8c).** The 8c parity check is scoped ONLY to in-bounds, in-view vertices — the subset where inline and VPL DO agree (both `0.0` in stock). Step 8c must **NOT** use the whole-vertex-list `MC2_HAZE_PARITY` violation count as pass/fail (it is expected-nonzero by design per v3.4). Instead the 8c gate is "**no visible-pixel fog regression on in-bounds in-view terrain**", verified via the smoke screenshot canary + (if feasible) a probe variant filtered to drawn/visible verts only. The unfiltered `MC2_HAZE_PARITY` may still run as diagnostic context but its nonzero count is informational, not a gate.
  > - **(b) Documented non-goal (REQUIRED in 8c plan text).** Off-screen and out-of-play-area `hazeFactor` is INTENTIONALLY re-derived by the inline formula and is **NOT required to match legacy VPL**. VPL's off-screen blanket-`1.0` was a culling artifact (the `onScreen` clip-gate stamp at `terrain.cpp:1551`, grep-verify at 8c write-time — drifts); the inline real-distance result is accepted as the new (arguably more correct) behavior per the v3.4 Step 7 ruling. 8c MUST NOT treat the loss of VPL's off-screen blanket-`1.0` as a regression.
  > - **(c) Tracked known item (cross-ref §"Deferred / tracked follow-ups").** The `GetApproximateLength`→`length()` ~10% per-vertex precision delta is masked by the stock dead ramp (`Camera::MinHazeDistance == MaxClipDistance`, ramp branch never taken). It becomes VISIBLE (a fog-band shift vs legacy) the moment any mission/mod ships `MinHazeDistance != MaxClipDistance`. Not a blocker for stock; not an 8c gate; surfaces only under non-default fog constants. Carried as a characterized known item below.

  **Gate (load-bearing — consolidated, `mc2-terrain-indirect-expert` + `mc2-shader-expert` advisors, 2026-05-15):** Step 8c may delete the VPL body ONLY when **ALL** of the following hold (Step 6 is necessary-but-not-sufficient):
  1. **Step 1 has landed.** Step 1 kills the `quad.cpp` thin-emit / indirect-pack `pz` consumers; without it the VPL `pz` writes still have live readers.
  2. **Step 3 has landed AND its 7-day soak has elapsed.** Step 3 kills the `Camera::inverseProject` consumers (`pz`/`px`/`py`/`clipInfo`); the soak fences 8c (and 8b) per the existing soak discipline.
  3. **Step 6 has landed.** Step 6 RE-HOMES the projection out of the VPL body into the decoupled slim min/max loop, removing the reduction's dependency on the *VPL-body* projection. Until Step 6 lands, deleting the VPL projection orphans the `setInverseProject` chain (`leastZ/mostZ/leastW/mostW/leastWY/mostWY` → `eye->setInverseProject` → tacmap unprojection). Step 5 does NOT satisfy this gate — Step 5's slim pass produces no clip-space extents and is projection-independent by design. Only Step 6 does. **Step 6 is necessary, not sufficient** — all four conditions must hold.
  4. **Step 8b has landed.** 8b retires the VPL `px/py/pz/pw`/`clipInfo` writes; 8c removes the now-write-free loop body.

  **The Step 5 `MC2_VPL_CULL` probe is EXEMPT from deletion and survives 8c as the tripwire.** It reads cull-derived `clipInfo` (projection-independent), not projection; keeping it alive THROUGH 8c is its entire purpose (it fires loudly if 8c's deletion ever secretly perturbs the cull cascade). Strict sequence: **Step 5 → Step 6 → Step 7 → Step 8c.**

  > **v3.5 GATE + PROBE CORRECTION (CRIT-0/1/2 + MAJOR-d) — SUPERSEDES the four-condition gate's framing and the "probe survives 8c as the tripwire" claim above.** Two corrections: (1) **The gate is FIVE conditions, not four, and condition 2's soak is waived.** (1) Step 1 landed — its remainder is parity-infra FULLY RETIRED (`be56e48`, recorded `5c22e28`) + the `gos_terrain_indirect.cpp:1674` `q.vertices[c]->pz` fallback reader operationally-retired (no edit; review cited `:1684`, drift to `:1674` at HEAD `29ae435`). (2) Step 3 landed — its 7-day soak WAIVED by orchestrator ruling 2026-05-15 (the v3.3 gate's "AND its 7-day soak has elapsed" no longer gates 8c). (3) Step 6 landed (`cf7d596`). (4) **NEW — the M2d overlay-pz-gate precursor landed (`29ae435`)**: re-projection + mandatory `clipInfo == 0` sentinel guard (`quad.cpp:2147`) + `MC2_M2D_PZ_PARITY` probe (`quad.cpp:303`/`:2156`); this is what unblocks 8b's `cv->pz` deletion (the `MC2_M2D_PZ_PARITY` probe must read zero before 8b deletes `cv->pz`). (5) Step 8b landed. (2) **The `MC2_VPL_CULL` probe does NOT "survive 8c as the tripwire" indefinitely** — it reads VPL-body-written `clipInfo`, which 8c-part-2 deletes; a relocated self-comparison is a tautology (MAJOR-d). It survives ONLY through 8c-part-1 as the slim-vs-still-live-VPL co-production probe (`!(legacyActive && !slimActive)`, camera-moving per CRIT-2), then is RETIRED or demoted to `MC2_VPL_REDUCE`-style at 8c-part-2. Corrected sequence: Step 5 → Step 6 → Step 7 → 8a → 8b → 8c-part-1 → 8c-part-2 → Step 9 → Step 10. Where the gate above says four conditions / Step-3-soak-elapsed / probe-survives-8c, this v3.5 tag governs.

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

**Sequencing caveat (load-bearing — v3.3 consolidated; does NOT relax the Step 8c gate):** soak *parallelization* is about test-surface non-overlap, not landing order. Step 8c's commit cannot land until **ALL FOUR** consolidated gate conditions in the amended Step 8c hold: (a) Step 1 landed, (b) Step 3 landed AND its 7-day soak elapsed, (c) Step 6 landed (it RE-HOMES the projection into the decoupled slim min/max loop — Step 6 is necessary-not-sufficient), (d) Step 8b landed. Additionally **Step 6 strictly precedes Step 7** (both touch the legacy `onScreen`/haze derivation; Step 7's checklist must update the Step 6 slim-reduce `onScreen` copy in lockstep). The commit order is strict: **Step 5 → Step 6 → Step 7 → Step 8c.** What runs in parallel is the *soak observation windows* of already-landed commits; it does not authorize landing Step 8c before its four gates clear, nor Step 7 before Step 6.

> **v3.5 SEQUENCING CORRECTION — SUPERSEDES the four-gate / Step-3-soak framing in the caveat above.** The Step 8c gate is **FIVE** conditions, not four, and condition (b)'s 7-day soak is **WAIVED** by orchestrator ruling 2026-05-15: (1) Step 1 landed (parity-infra fully retired `be56e48`/`5c22e28`; `gos_terrain_indirect.cpp:1674` pz fallback operationally-retired, no edit), (2) Step 3 landed (7-day soak WAIVED — no longer gates 8c), (3) Step 6 landed (`cf7d596`), (4) the M2d overlay-pz-gate precursor landed (`29ae435` — re-projection + `clipInfo==0` sentinel guard `quad.cpp:2147` + `MC2_M2D_PZ_PARITY` probe; unblocks 8b's `cv->pz` deletion), (5) Step 8b landed. The corrected strict order is **Step 5 → Step 6 → Step 7 → 8a → 8b → 8c-part-1 → 8c-part-2 → Step 9 → Step 10** (8c-part-1 = cull-merge into the Step 6 loop with the `terrain.cpp:1993` placement rule + non-tautological `MC2_VPL_CULL` relocation + camera-moving co-production parity probe + slim-only validation; 8c-part-2 = delete VPL body + projection + `hazeFactor` writes + parity scaffolding + legacy-lighting both-env guard + zero-init). Soak parallelization still does NOT authorize landing 8c before its five gates clear. Where the caveat above says four gates / Step-3-soak-elapsed, this v3.5 tag governs.

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
- **Step 6 (reductions, re-homed):** the **`MC2_VPL_REDUCE` exact-equality probe is the real automated gate** (zero `[VPL_REDUCE v1] event=parity_violation` over tier1 30s — bit-identical, not epsilon, because the slim loop calls the same `projectForTerrainAdmission`). Tacmap F-key UAT (viewport corners unproject byte-identically) is user-driven confirmation, NOT an automated gate — passive smoke cannot open the tacmap (honest gap).
- **Step 7 (hazeFactor GPU port — shader+uniforms+populate-neutralize ONLY):** frame-perfect screen byte-compare in-bounds; world-edge `hazeFactor=1.0` clamp visible canary. Step 7 does NOT delete any `terrain.cpp` VPL `hazeFactor` write (that is Step 8c per v3.3); legacy `MC2_TERRAIN_LIGHTING_GPU=0` path must still render correct fog after Step 7 (regression check: it still reads the live VPL `hazeFactor` writes). Out-of-bounds terrain-lit water now fogged is the expected positive delta; dedicated-water-path fog is a tracked follow-up, not a Step 7 gate.
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

## Deferred / tracked follow-ups (not blockers)

- **Dedicated-water-path edge-clamp (v3.3, `mc2-shader-expert`).** Step 7's inline-worldPos `outsidePlayArea` clamp fixes out-of-bounds fog only for water that consumes the lighting compute's `fogRGB` output (terrain-lit water). The dedicated water draw path — `shaders/gos_terrain_water_mdi.frag` + `shaders/gpu_driven_water.comp` (both confirmed present at HEAD `55b1167`) — is unverified: if water bypasses the lighting-compute fog output, the same `outsidePlayArea` edge-clamp must be added to the dedicated water shader(s). This is a **tracked follow-up after Step 7**, not a Step 7 blocker. Owner: a post-Step-7 slice; grep-confirm at that time whether dedicated water consumes lighting-compute `fogRGB`.
- **`GetApproximateLength`→`length()` haze precision delta under non-stock fog constants (v3.4, characterized known item — not a blocker for stock).** VPL's CPU `distanceToEye` used `GetApproximateLength` (octagonal approx, `terrain.cpp:1512` — grep-verify, drifts) after a length-preserving camera-frame transform; the inline shader uses exact `length()` (`gos_terrain_lighting.comp:302`). This is a ~10% per-vertex distance delta. It is **fully masked in the stock config** because `Camera::MinHazeDistance == MaxClipDistance`, so the fog ramp branch (shader `:304`, CPU `terrain.cpp:1538`) is never taken and `hazeFactor` is binary 0.0/1.0 (the approx-vs-exact delta cannot change a binary clamp). It becomes **visible as a fog-band shift vs legacy** the moment any mission/mod ships `MinHazeDistance != MaxClipDistance` (a live ramp). NOT a Step 7 or Step 8c blocker; NOT a regression for stock. Tracked so a future non-default-fog mission/mod author (or a later parity investigation) recognizes the shift as this characterized, accepted re-derivation rather than a new defect. Cite `memory/parity_probe_100pct_can_be_correct_redesign_report.md`.
- **`GpuTerrainVertexInput.hazeFactor` field removal (v3.3 lockstep deferral).** Step 7 uses keep-field-stop-reading (populate write neutralized to `0.0f` at `gos_terrain_lighting.cpp:553`; GLSL + C++ field kept, commented dead). The actual field removal is **Step 10 cleanup**, after the Step 8c soak, as one lockstep commit editing `shaders/include/terrain_lighting_shared.hglsl` + `gos_terrain_lighting.h` + the `static_assert(sizeof==32)` together per `memory/cpp_glsl_ubo_struct_lockstep.md` (the field is 4 padded bytes; `==32` static_assert holds either way — verify at write-time).

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
