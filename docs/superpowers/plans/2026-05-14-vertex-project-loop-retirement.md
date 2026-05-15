# `VertexProjectLoop` Retirement — Implementation Plan v1

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
- **Mouse picking rewrite to GPU AABB hit-test.** Picking is rewritten to compute its own projection on demand (one of the 8 pre-existing categorical wrappers — `projectForSelectionPicking` at `mclib/camera.h:422-622`) for the tiles under the cursor, NOT to a full GPU port. The GPU AABB hit-test (`memory/gpu_mech_aware_mouse_pick_queued.md`) is the **downstream unlock** this plan enables but does not implement.
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

**Deletes:** `PackThinRecordsForFrame`, `s_shadow`, `s_packParityMask`, `ComputeDispatchParity_Check`, parity infrastructure.

**This plan's contribution:** none — this commit ships independently. Marks consumers (a) at `quad.cpp:2089-2094` and (c) at `gos_terrain_indirect.cpp:1639-1656` as gone. VPL's `pz` write is now read by fewer consumers; loop stays alive.

### Step 2: Cmd-patch dispatch retirement (already queued)

**Deletes:** the second `glDispatchCompute` for cmd-patch + bucket header dependency.

**This plan's contribution:** none — independent.

### Step 3: Mouse picking repoint

**File:** `mclib/camera.cpp:797-822` (`Camera::inverseProject`).

**Change:** instead of walking every tile and reading `vertices[c]->pz`, project the cursor's screen-space ray into world space via `inverseProjectForPicking` (existing wrapper in `mclib/camera.h:422-622`), intersect with the terrain plane to get a world-space cursor point, then look up the tile under that point via `RecipeForVertexNum` or terrain mapdata. Per-click cost: ~50 µs (one projection, one tile lookup).

**Verification:** mouse cursor tracking, mech selection by click, build-menu placement preview, salvage marker placement — manual UAT on tier1 missions.

**Soak:** 7 days before step 4. Picking regressions can be subtle (off-by-one cursor positioning) and may only show on specific mission layouts.

### Step 4: Mask-dispatch + parity-mask `pz` reads retire

**File:** `gos_terrain_mask_dispatch.cpp:211-223` (recon citation; verify).

**Strategy:** the mask-dispatch's pz cull is structurally identical to the indirect dispatch's pz cull; same lambda lives in `gos_terrain_indirect.cpp`. If mask-dispatch is reachable in production (recon flagged uncertainty), repoint to use the recipe SSBO + on-CPU one-shot projection per quad considered. Otherwise mark dead and skip.

**Pre-commit grep:** confirm mask-dispatch's actual reachability. If it's reached only under env-gated paths (`MC2_TERRAIN_MASK_DISPATCH=1`?), document and either gate-default-off → unreachable, or include the retirement.

### Step 5: Cull-cascade audit + `objBlockInfo` / `objVertexActive` retirement

**This is the blocker step.** Per `memory/cull_gates_are_load_bearing.md`, `objBlockInfo[].active` and `objVertexActive[]` gate `update()`, allocation, AND lifecycle for objects/props/mechs. VPL writes these as side effects. Retiring them re-introduces Fix A's bug class if downstream cull cascades aren't first repointed.

**Required sub-audit (spawn as a separate recon agent, do NOT do inline in the executing session):**

- Grep every read of `objBlockInfo[]` and `objVertexActive[]` across the worktree.
- For each consumer: classify as (a) chopping block / (c) re-derivable from GPU AABB hit-test / (b) genuine CPU survivor / (d) dead.
- If any consumer is (b), this plan blocks until that consumer is itself retired or repointed.
- If all consumers are (a)/(c)/(d), this step retires VPL's writes to these fields **in the same commit** as the consumer retirements.

**Decision point:** if step 5's sub-audit reveals an irreducible CPU survivor, fall back to **Path C (dirty-flag skip)** for VPL: keep the loop alive but skip it when `terrainMVP` hasn't changed since last frame. Saves ~475 µs on static-camera frames; preserves correctness on moving frames.

### Step 6: Shared reduction audit

**Targets:** `leastZ/mostZ/leastW/mostW/leastWY/mostWY` per recon (verify at write time).

**Audit shape:**
- Grep every reader of these globals.
- Classify per the same (a)/(b)/(c)/(d) scheme.
- If all readers are on the chopping block, retire the reduction in the same commit as the VPL fast-path retirement.
- If readers survive, keep the contribution alive (Path A partial) — they MUST then come from somewhere else (GPU? a smaller bounded compute?).

### Step 7: VertexProjectLoop body deletion

**Precondition:** after steps 1-6, the loop body's writes are either (a) deleted because their consumers are gone, or (b) gated behind a dirty-flag skip for Path C survivors.

**Change:** delete the function definition. Delete its callers (`Terrain::geometry` at `mclib/terrain.cpp:~1372-1583` — verify). The Tracy zone `vertexProjectLoop` disappears.

**Expected savings:** the full ~475 µs/frame on every frame where the loop ran (CPU pack path was reached). With Fix B + CPU pack retirement landed, the loop was already running on a smaller surface; full deletion captures the remaining cost.

**Verification:** tier1 smoke. `vertexProjectLoop` Tracy zone gone. No new mismatches.

### Step 8: Fix A retirement + final cleanup

**File:** `gos_terrain_indirect.cpp` — per-slot MVP snapshot, `terrainOverrideThinMVP`, `gos_terrain_indirect_getRingSlotMvp`.

**Rationale:** after VPL is gone and the GPU compute is the sole projection authority, the per-slot MVP snapshot has no defense-in-depth role — there's no "other path" to misalign with.

**Strategy:** delete or demote to env-gated debug per the debug-instrumentation rule.

### Step 9: Render contract refresh

**Trigger:** dispatch `mc2-render-contract-synthesizer` agent after step 8.

**Action:** the synthesizer reads current state, updates `docs/render-contract.md` to reflect "GPU is sole projection authority for terrain quads in the indirect path; CPU has no per-frame vertex projection state."

---

## Verification strategy

- **Per-commit:** tier1 smoke (`scripts/run_smoke.py --tier tier1 --duration 30 --kill-existing`). All 5 missions must PASS.
- **Step 3 (picking):** manual UAT — cursor tracking, selection, placement. User-driven smoke at `mc2_01`, `mc2_10` (substrate-heavy), `mc2_17` (water-heavy).
- **Step 5 (cull cascade):** if Fix A-style ghost-mech / streak symptoms appear during smoke, treat as a CRITICAL regression. The cull cascade is the exact class Fix A patched.
- **Step 7 (loop deletion):** Tracy capture before + after. `vertexProjectLoop` zone disappears. Wolfman zoom CPU time drops by the measured loop cost (~475 µs).
- **Final:** seven-day soak after step 7 before step 8 (Fix A retirement). Soak surfaces the kind of cull-cascade edge cases that show up on rare mission layouts (e.g., mc2_24's reactor environment, mc2_17's water-only zones).

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

Expected findings density (based on Fix B precedent): 2-4 CRITICAL, 4-6 MAJOR, 5-8 MINOR. Plan v2 lands after review. Step 1 implementation does NOT start until plan v2 ships.
