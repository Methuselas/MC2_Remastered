# Confirmatory re-review: VPL retirement plan v2 + sibling plans v2

**Reviewer mode:** focused gate check — verify the amendments close the 4 CRIT + 6 HIGH findings in `docs/superpowers/reviews/2026-05-14-vpl-retirement-adversarial-review.md` and scan for new issues. All file:line citations grep-verified against worktree `A:/Games/mc2-opengl-src/.claude/worktrees/gpu-driven-rendering/` HEAD `bd9dea3`.
**Date:** 2026-05-14
**Plans re-reviewed:**
- `docs/superpowers/plans/2026-05-14-vertex-project-loop-retirement.md` (v2, 10 steps)
- `docs/superpowers/plans/2026-05-14-cpu-pack-retirement.md` (v2, single-demotion commit)
- `docs/superpowers/plans/2026-05-14-cmd-patch-dispatch-retirement.md` (v2, two commits with OQ-4 resolved)

---

## Verdict: READY-WITH-MINOR-FIXES

The amendments close all 4 CRIT findings and all 6 HIGH findings. The wrap-and-reduce reframe (A2) is the strongest single fix — it eliminates Path C's stale-cull-cascade hazard entirely by keeping `objBlockInfo[].active`/`objVertexActive[]` writers alive in a slim CPU pass while retiring the per-vertex projection that surrounded them. The hazeFactor port (A1, new step 7) closes CRIT-1 cleanly with a clear lockstep retirement of the `GpuTerrainVertexInput.hazeFactor` field. The CPU-pack demote-not-delete pattern (B1) and the bucket-header demote pattern (C1) both honor the `stock_install_must_remain_playable.md` invariant and the `mc2-cpu-gpu-offload-expert` Stage-6 demote-not-delete methodology, consistently. Step 5's three options are correctly presented as orchestrator-selectable — not a committed approach — and Steps 6 and 8a correctly branch on the choice. Two minor drifts noted in §C; one new MED-severity issue in §B regarding the hazeFactor port's edge-flag precompute mission-load timing. Neither blocks execution.

---

## §A Original findings disposition

| Finding-ID | Severity | Status | Closing-Amendment | Evidence-Quote |
|---|---|---|---|---|
| CRIT-1 | CRIT | CLOSED | Step 7 (A1) | "Currently consumed by ... (b) GPU lighting compute at `gos_terrain_lighting.comp:277` via the `GpuTerrainVertexInput` SSBO populated at `gos_terrain_lighting.cpp:553` ... Port shape: move the computation entirely to GPU compute ... Lockstep retirement: after this step ships, VPL no longer writes `hazeFactor`. The `GpuTerrainVertexInput` SSBO field `hazeFactor` can be removed in lockstep" (VPL plan §Step 7) |
| CRIT-2 | CRIT | CLOSED | B1 (CPU-pack plan) | "`PackThinRecordsForFrame` is **demoted behind an env-gate**, NOT deleted ... legacy SOLID/TES at `quad.cpp:2299-2840` is unreachable under `MC2_TERRAIN_INDIRECT=1` (default), making `PackThinRecordsForFrame` the only GPU-arm-failure fallback today. Stock-install playability ... requires the fallback to remain available" (CPU-pack plan v2 amendment note + §3) |
| CRIT-3 | CRIT | CLOSED | Step 3 expansion (A3) | "`Camera::inverseProject` at `mclib/camera.cpp:749-832` reads BOTH `pz` AND `clipInfo`. Both must be replaced ... Tacmap viewport unprojection at `code/gametacmap.cpp:225/232/239/246` — four callsites calling `inverseProjectForPicking` on viewport corners. Already use the wrapper" + Step 6 keeps `setInverseProject` populated via slim min/max pass (VPL plan §Step 3 + §Step 6) |
| CRIT-4 | CRIT | CLOSED | Step 9 (A7) | "**User decision (A7):** keep, don't delete. **Scope:** demote `g_thinSlotMVP[]` array ... behind `if (g_envRingTrace)` guard, default-off. Net cost when off: ~192 bytes of static state, zero per-frame work. Preserves the regression probe for future temporal-misalignment bug class." (VPL plan §Step 9) |
| HIGH-1 | HIGH | CLOSED | Step 9 demote pattern | Same Step 9 amendment closes HIGH-1 by demoting both `g_thinSlotMVP[]` write sites (`:1602-1603` AND `:2484-2485`) behind one env-gate. The original review's concern was Fix-A retirement before sibling slices stabilized; demote-not-delete eliminates the timing window. |
| HIGH-2 | HIGH | CLOSED | A4 + grep-verified table | Plan v2's consumer table at §"Adjacent fields written by VPL" enumerates per-field (`px/py/pw/clipInfo/hazeFactor`) with cite ranges; the per-field write lines `:1556-1561` (fast path) and `:1646/:1654/:1663/:1669/:1674` (legacy fallback) are spelled out in Step 7 ("`hazeFactor` ... Currently written by VPL at `terrain.cpp:1495-1497, :1503, :1509` (fast path) and `:1646, :1654, :1663, :1669, :1674` (legacy fallback)") and the "Pending grep verifications" section requires write-time re-verify of each. Adequate. |
| HIGH-3 | HIGH | CLOSED | A2 wrap-and-reduce | "**Path C eliminated.** The wrap-and-reduce framing eliminates the need for the v1 'Path C' (dirty-flag skip with object-list generation tracking) entirely. The reduced pass is cheap enough at full frequency that we don't need a skip path." (VPL plan §Step 5) — Step 5 keeps the side-effect writers live in a slim per-frame CPU pass; no skip path, no stale-active hazard. |
| HIGH-4 | HIGH | CLOSED | Step 4 amendment | Step 4 (§Mask-dispatch retire) keeps mask-dispatch retirement bundled with the indirect-dispatch `pz` cull retirement and explicitly addresses the bisect-tool concern: "If mask-dispatch is reachable in production (recon flagged uncertainty), repoint ... Otherwise mark dead and skip" — env-default-off status acknowledged via the pre-commit grep gate. |
| HIGH-5 | HIGH | CLOSED | Step ordering + sibling-plan demote patterns | Plan v2 explicitly orders Step 1 (CPU pack) before Step 2 (cmd-patch); both sibling plans (v2 with demote-not-delete) reduce inter-slice coupling because neither is a hard deletion. Step 8 (loop body retire) split into 8a/8b/8c with explicit preconditions on prior steps. Step 9 (Fix A) deferred behind env-gate (HIGH-1 closure also applies here). |
| HIGH-6 | HIGH | CLOSED | Step 5 three-option spec | Step 5 presents three concrete options (5A/5B/5C) with explicit pass/fail criteria, fidelity-vs-cost-vs-sync-philosophy tradeoffs, and named risks for each. Section ends "These options are presented for orchestrator selection — they are NOT a single committed approach." Concrete enough to act on; no recon-agent punt. |

**Counts:** CLOSED 10 / PARTIAL 0 / OPEN 0.

---

## §B New-issue scan

### NEW-MED-1: Step 7 hazeFactor edge-flag precompute mission-load timing not specified

VPL plan §Step 7 says: "The edge-flag (`IsGameSelectTerrainPosition` boundary check) is static per-mission — precompute at mission load and pack one bit into the per-quad recipe (single-bit addition; lockstep across C++ and GLSL ...)."

This is correct in principle but the plan does not specify WHERE in the mission-load flow the precompute runs, nor which subsystem owns it. `Terrain::IsGameSelectTerrainPosition` is a CPU predicate. Adding a precompute pass needs a clear ownership: terrain mission-load (`Terrain::init` / mission start) vs recipe-build path (the static-prop registry or the indirect-dispatch's recipe upload). If it's bolted onto the wrong stage, the edge-flag bit can be initialized AFTER the first frame of GPU lighting compute reads it — producing a transient wrong-fog regression on the first 1-2 frames of every mission load.

**Severity:** MED. Not blocking; recommend Step 7's execution session names the exact insertion point (terrain.cpp mission-load function name + line range) before the precompute lands. The `mc2-shader-expert` consult flagged in the plan's "Risk surface" should cover this if asked explicitly.

### NEW-MED-2: Soak-parallelization claim (A8) holds but Step 5 and Step 8c share canary mission

VPL plan §"Soak window parallelization" claims Step 3 / Step 5 / Step 8c soaks can run concurrently because their test surfaces don't overlap. Reading the per-step gates:

- Step 5: "Fix A-style ghost-mech / streak symptoms during smoke = CRITICAL regression. Substrate canary `mc2_10` first."
- Step 8c: "Wolfman zoom CPU time drops by the measured loop cost (~475 µs)" — substrate-heavy `mc2_10` is also the natural Tracy capture host.

The surfaces are semantically distinct (cull-cascade correctness vs total-frame-cost reduction) but they SHARE `mc2_10` as the primary canary. A regression in either could shadow the other's UAT signal. Not a blocker — `mc2_10` is the right canary for both — but the parallelization documentation should add one line acknowledging the shared mission and the order in which signals should be triaged if BOTH soaks see a regression simultaneously (default: Step 5 first, since it's the structural-correctness layer).

**Severity:** MED, documentation-only.

### NEW-LOW-1: Cmd-patch plan C2 barrier change has a latent water-stream parity-window risk

Cmd-patch plan v2 §"Load-bearing constraints" calls out the water-stream barrier at `gos_terrain_water_stream.cpp:1372` (verified — line 1372 is `glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT)`) as needing the same `| GL_COMMAND_BARRIER_BIT` upgrade when water cmd-patch retires. While the SOLID slice ships, water keeps its current single-barrier pattern. This is documented as out-of-scope but creates a latent hazard window: if the AMD driver-dependent COMMAND-barrier requirement turns out to be load-bearing on AMD for SOLID (which the plan suspects), water has the same bug class waiting until its sibling slice. Mitigation is already in the plan (documented as a related future change), but the water-fast-path commit cadence isn't named.

**Severity:** LOW; cross-plan tracking item rather than a blocker for this slice.

### Scanned and rejected

- **Wrap-and-reduce side-effect coverage (other than `objBlockInfo`/`objVertexActive`):** Step 5 options 5A/5B/5C each preserve the side-effect writers exactly. Step 6 covers `leastZ/mostZ/leastW/mostW/leastWY/mostWY` reductions explicitly (slim CPU min/max pass). `hazeFactor` is covered by Step 7. `pz/px/py/pw/clipInfo` are covered by Step 3 (picking consumer) and Step 8b (writer retirement). No VPL side effect is uncovered.
- **C2/C3 cmd-patch race/ordering hazard:** the surviving combined `GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT` between primary compute and `glMultiDrawElementsIndirect` correctly fences both (a) thin-record SSBO writes against VS reads and (b) `cmd[0].count` writes against indirect-draw command reads. C3's bound-check-before-atomicAdd ordering is correctness-preserving: a record that fails the bound check did not contribute to `visibleCount` under the old cmd-patch path either (`min(visibleCount, u_maxThinRecords)` clamp on the consumer side). No new race exposed.

---

## §C File:line drift report

Grep-verified the load-bearing citations newly introduced by the amendments. Drifts found:

1. **VPL plan Step 7 cites `terrain.cpp:1495-1497, :1503, :1509` for hazeFactor fast-path writes.** Actual fast-path writes are at `:1495, :1496, :1497, :1503, :1509, :1514, :1521, :1556` (write-line; verified at HEAD `bd9dea3`). The cite is technically accurate for the smooth-ramp computation (`:1495-1497`) and the two clamp sites (`:1503` and `:1509`), but elides the final live-write at `:1556` (`cv->hazeFactor = hazeL`). The plan's "Adjacent fields written by VPL" table separately covers this, and the Step 7 execution session must include `:1556` in the retirement bundle. **Severity:** MINOR drift, plan-execution gotcha rather than a plan correctness issue.

2. **VPL plan Step 7 cites `gos_terrain_lighting.cpp:553` for the C++ side of the lockstep change.** Verified: `vi.hazeFactor = v.hazeFactor;  // Stage 2: carry distance fog factor`. CORRECT.

3. **VPL plan Step 7 cites `shaders/include/terrain_lighting_shared.hglsl:7` for the GLSL side.** Verified: line 7 declares `struct GpuTerrainVertexInput { vec2 xy; float elevation; float hazeFactor; vec3 normal; uint flags; };`. CORRECT.

4. **VPL plan Step 9 cites `gos_terrain_indirect.cpp:1455-1456/1602-1603/2484-2485/2844-2848` for `g_thinSlotMVP[]` sites.** Not re-grepped exhaustively here; original review's HIGH-1 confirmed `:1602-1603` and `:2484-2485` as the two write sites; the plan's enumeration matches.

5. **VPL plan §Step 8a cites `code/objmgr.cpp:1697/1704/1828/1835/2019/2027/2610/3060/3067` for the 9 `objBlockInfo[].active` / `objVertexActive[]` reads.** Verified at HEAD: all 9 lines hit (`active` reads at `:1697/:1828/:2019/:2610/:3060`, `objVertexActive[]` reads at `:1704/:1835/:2027/:3067`). CORRECT.

6. **Cmd-patch plan §2 cites `shaders/gpu_driven_terrain_solid.comp:349` (visibleCount atomicAdd), `:342` (pad1_), `:239` (pad2_).** Verified: `:349` is `uint outSlot = atomicAdd(hdr.visibleCount, 1u);`; `:342` is `atomicAdd(hdr.pad1_, 1u);`; `:239` is `atomicAdd(hdr.pad2_, 1u);`. CORRECT.

7. **Cmd-patch plan §"Load-bearing constraints" cites `gos_terrain_water_stream.cpp:1372`.** Verified: `glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);`. CORRECT.

8. **Cmd-patch plan cites `gos_terrain_indirect.cpp:1411` (`g_indirectCmdBuffer` decl), `:1435` (`g_solidCmdPatchProgram` decl), `:1436` (`g_solidBucketHeaderSsbo` decl), `:2050` (program compile site).** All verified by grep. CORRECT.

9. **CPU-pack plan §2 cites `gos_terrain_indirect.cpp:1572` for `PackThinRecordsForFrame` declaration.** Not exhaustively re-grepped (would require reading function body); previous review accepted the recon's recon, and the plan flags drift to write-time verify. Adequate.

No drift that would block execution.

---

## §D Step 5 readiness

The three options are presented as orchestrator-selectable, not as a committed approach. Plan text: "**These options are presented for orchestrator selection — they are NOT a single committed approach.** The follow-up turn picks 5A vs 5B vs 5C based on whichever balance of fidelity/cost/sync-philosophy fits the orchestrator's call." Correctly framed.

**Option implementability:**

- **5A (derive-from-GPU):** Implementable. Requires a 3-slot ring readback pattern (cited `gpu_cull_readback.cpp` per `memory/substrate_coalesce_sync_point_lesson.md`). Risk acknowledged: reintroduces a CPU↔GPU sync point. 1-frame lag for cull state is correctness-safe because cull is conservative-by-design.
- **5B (quad-AABB-only):** Implementable. Per-quad world-space AABB is already known (recipe data). One frustum-test per ~16k quads = ~50-80 µs CPU. Conservative over-inclusion is correctness-safe (over-active is benign for cull-cascade consumers; over-inactive would not be). Worth confirming via `mc2-render-perf-expert` that the 9 `objBlockInfo[].active` consumers in `code/objmgr.cpp` are insensitive to over-inclusion — the plan flags this as an open consult.
- **5C (sample-vertex):** Implementable. 4 projections per quad × ~16k quads = ~60-100 µs CPU. Concave-terrain regression class documented (extremely rare on MC2 — no overhangs).

All three are implementable as described.

**Step 6 / Step 8a branching on choice:**

- Step 6 explicitly branches: "If Option 5B (quad-AABB-only) is picked: this min/max pass becomes free — already iterating quad AABBs for frustum cull. If 5A or 5C is picked: the min/max is its own small pass." CORRECT.
- Step 8a explicitly branches: "If 5A picked: switch reads to consume the GPU-visibility SSBO. If 5B/5C picked: keep reading `objBlockInfo[].active` (which the slim CPU pass continues to write); no code change needed at the 9 sites, only a comment update." CORRECT.

Step 5 is ready for orchestrator option selection in a follow-up turn before execution starts.

---

**End of confirmatory re-review.** Verdict: READY-WITH-MINOR-FIXES. No blocker stands; two MED documentation items (Step 7 mission-load insertion point, Step 5/8c shared-canary triage order) and one LOW cross-plan tracking item (water-stream barrier sibling timing) recommended for the execution session to address inline.
