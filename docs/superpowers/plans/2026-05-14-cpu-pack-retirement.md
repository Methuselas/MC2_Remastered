# CPU Thin-Record Pack Retirement — Implementation Plan v2

> **Plan v2 amendment note (2026-05-14):** v2 lands amendment B1 from the orchestrator's Wave-0 grep-verified decision: `PackThinRecordsForFrame` is **demoted behind an env-gate**, NOT deleted. The Wave-0 grep walk established that legacy SOLID/TES at `quad.cpp:2299-2840` is unreachable under `MC2_TERRAIN_INDIRECT=1` (default), making `PackThinRecordsForFrame` the only GPU-arm-failure fallback today. Stock-install playability (per `memory/stock_install_must_remain_playable.md`) requires the fallback to remain available. Demotion behind `MC2_TERRAIN_INDIRECT_CPU_FALLBACK=1` (default-off) preserves the safety net while removing the per-frame cost. Commit sequence collapses from three commits to one — the soak-window-plus-deletion structure is replaced by single-commit demotion; the function declaration's eventual full removal is deferred to a future plan, contingent on telemetry showing the env-gate is never set in production. Other retirement targets in this plan (M2 thin emit `pz` block, parity infrastructure, `s_shadow` staging) still die in commit 1. This matches the demote-don't-delete pattern applied to Fix A (VPL plan step 9) and the bucket-header SSBO (cmd-patch plan OQ-4 resolution).

> **Foundation:** Raster-triangle Fix B (commit `005ebc7`, 2026-05-14) moved per-corner clip-space projection from `gos_terrain_thin.vert` into `shaders/gpu_driven_terrain_solid.comp` and stored `clipPos` directly in the thin record. The GPU compute shader is now the sole projection authority for every terrain quad in the production indirect-SOLID path. Every CPU-side path that walks visible quads, projects corners through `vertexProjectLoop`'s `pz` output, and writes thin records into the indirect-draw SSBO is now an orphan: the GPU re-derives the same data downstream and overwrites it. See `docs/superpowers/progress/2026-05-14-raster-triangle-handoff.md` ("Suggested fixes" / Fix B) for the rationale; this plan retires the orphaned CPU code Fix B made obsolete.
>
> **Relationship to VPL retirement:** This slice is step 1 of the 9-step `VertexProjectLoop` retirement sequence (`docs/superpowers/plans/2026-05-14-vertex-project-loop-retirement.md` §"Interleaved retirement sequence"). It ships independently because:
> 1. it is fully unblocked by Fix B alone — none of the cull-cascade audit, picking repoint, or shared-reduction audit work the VPL plan tracks is on this slice's critical path.
> 2. its blast radius (the CPU pack and its CPU-mirror pz cull) is structurally narrower than VPL's; it lands now and the soak clock starts now.
>
> This plan does **NOT** touch `vertexProjectLoop` itself or any of its other consumers. After this slice ships, VPL still runs every frame and writes `vertices[c]->pz`, `px`, `py`, `pw`, `clipInfo`, `hazeFactor` for non-pack consumers. The Tracy zone `vertexProjectLoop` MUST still appear with cost unchanged within noise — that is the load-bearing "no VPL coupling" sanity probe in §4.
>
> **Advisor framing:** `mc2-cpu-gpu-offload-expert`'s canonical staging shape (`.claude/agents/mc2-cpu-gpu-offload-expert.md` `<core_knowledge>`) has Stage 6 as **demote-not-delete**: gated-off CPU body stays as retirement telemetry. This plan honors that by landing the retirement in two commits separated by a soak window — first behind an opt-out env var, then deletion after soak — rather than a single irreversible deletion. `mc2-terrain-indirect-expert` explicitly scopes the legacy CPU thin-record packer out of its `<limits>` ("the compute path replaces it"), confirming the path is understood as terminal even by the subsystem expert.

---

## §1 Problem statement

The CPU-side "thin-record pack" code walks `land->getQuadList()` once per frame, applies the same skip set the GPU compute already enforces (pointer guards, blank-vertex `vertexNum < 0` sentinel, recipe coverage, `terrainHandle == 0/0xffffffffu`, and per-tri `pz` clip-range check), and writes a `TerrainQuadThinRecord` per surviving quad into the active ring slot of `SolidThinRecordSSBO`. Pre-Fix-B, this thin record's per-corner `clipPos[4]` had to be re-projected in `gos_terrain_thin.vert` from a recipe lookup, so the CPU pack's role was "pack the skinny structure; the VS re-derives projection."

Fix B changed two things at once:
- The compute shader (`shaders/gpu_driven_terrain_solid.comp`) now performs the same skip set the CPU pack performs, **and** computes `clipPos[c]` for each corner, **and** writes the same thin record layout (96 B/record, up from 32 B pre-Fix-B). See the comment chain at `gpu_driven_terrain_solid.comp:93` ("matches `PackThinRecordsForFrame()`"), `:159` ("Mirrors `PackThinRecordsForFrame` and `gos_terrain_thin.vert:176`"), `:270`, `:301`, `:315`, `:365`, `:379` — the compute shader was explicitly authored to byte-match the CPU pack's record layout.
- The bridge VS (`gos_terrain_thin.vert`) no longer touches `terrainMVP`. It reads pre-projected `clipPos` directly from the thin record. The CPU pack's per-corner pz feed (which fed projection on the VS side) is now read by nothing in the GPU path.

Concretely, after Fix B:
- When `MC2_GPU_DRIVEN_TERRAIN_SOLID=1` (default-on, set in `gpu_driven_common.cpp:13,64`), the compute dispatch arms at `gos_terrain_indirect.cpp:~1958-1974` and the CPU `PackThinRecordsForFrame()` call at `:1978` is dead. The fallback comment at `:1977` ("CPU path (legacy — demote-don't-delete per CLAUDE.md debug instrumentation rule)") already acknowledges its retirement-pending status.
- The M2-fast-path thin-emit block in `TerrainQuad::draw` (`mclib/quad.cpp:2087-2108`) reads `vertices[c]->pz` to decide tri1/tri2 culling, then emits to `mcTextureManager`/`PatchStream` via the legacy `gVertex[3]` path (`:2299-2840`). When the fast path is `isFastPathActive() && isThinRecordsActive() && isReady() && !isOverflowed()` (gated at `:2026-2035`), the per-tri pz check at `:2089-2094` still runs on the CPU as a duplicate of compute's gate. With Fix B's compute now sole authority on every quad it admits, this CPU pz check is producing a redundant cull decision the GPU is going to make again.
- The CPU mirror inside `PackThinRecordsForFrame` itself (`gos_terrain_indirect.cpp:1638-1656`) is the same logic copy-pasted into the indirect-pipeline-internal pack, which the bridge now ignores when `MC2_GPU_DRIVEN_TERRAIN_SOLID=1`.

**Perf claim.** From `docs/render-perf-snapshot.md`:

> | `Terrain::SolidComputeDispatch` (CPU submission) | ~0.95 mean / 0.59 median | post-Fix-B baseline; ~80-170 µs recoverable via cmd-patch retirement, ~500 µs floor target via bind-once + persistent-mapped + dirty-flag skip (sequenced in plan) |
> | CPU pack path retirement | QUEUED — prompt drafted; deletes `PackThinRecordsForFrame` + parity infrastructure | Fix B (shipped) | VPL retirement step 1, cmd-patch retirement |

The 95-170 µs range cited in the orchestrator's framing maps onto the snapshot's `~80-170 µs recoverable via cmd-patch retirement` line. The CPU pack and the cmd-patch dispatch retirement are sibling slices in the same recovery bucket; this slice claims the CPU-pack portion. Treat the exact µs split as bucket-level approximation, not a load-bearing per-slice number — `mc2-render-perf-expert` is the authority and the snapshot must be re-measured after this slice ships.

---

## §2 File:line target table (grep-verified 2026-05-14 against HEAD `bd9dea3`)

| Path | Current symbol | Current line range | Action |
|---|---|---|---|
| `GameOS/gameos/gos_terrain_indirect.cpp` | `static int PackThinRecordsForFrame()` declaration + body | `1572-1807` (decl at `:1572`; first ZoneScoped at `:1573`; ring-slot advance `:1587-1594`; Fix-A MVP snapshot `:1596-1604`; per-tri pz mirror block `:1638-1656`; record store loop ends ~`:1790`; `glBufferSubData` ~`:1789`) | **Demote** (v2) — body gated behind `MC2_TERRAIN_INDIRECT_CPU_FALLBACK=1` (default-off) in commit 1; declaration deletion deferred to a future plan contingent on telemetry showing the env-gate is unset in production |
| `GameOS/gameos/gos_terrain_indirect.cpp` | `static uint32_t s_packParityMask[kParityMaskWords]` + `kParityMaskWords` declaration | declared `:430` (comment block `:424-429`) | **Delete with the pack** — written only at `:1580` (memset reset) and `:1777` (the per-quad set inside the pack). External readers via `s_packParityMask` accessor at `:2870` must be re-greped (see §7 OQ-2). |
| `GameOS/gameos/gos_terrain_indirect.cpp` | `void ComputeDispatchParity_Check()` | declared `:2634`; second pack call `:2667`; comment block `:2622-2633` | **Delete** — parity infrastructure used only by `MC2_GPU_DRIVEN_PARITY` (development gate); makes no sense once CPU pack is gone, and Fix B retired the parity gate's purpose |
| `GameOS/gameos/gos_terrain_indirect.cpp` | `static int BuildIndirectCommands(int thinCount)` | declared `:1812`; call site `:1985` | **Retire alongside `PackThinRecordsForFrame`** — its single caller (`:1985`) lives in the CPU-fallback branch at `:1977-2002`. With the CPU branch gone, `BuildIndirectCommands` has no live caller. (Note: sibling plan `2026-05-11-gpu-driven-indirect-cmd-gen-plan.md` Task 6.2 already framed this as a paired demotion.) |
| `GameOS/gameos/gos_terrain_indirect.cpp` | `static TerrainQuadThinRecord s_shadow[kMaxThinRecords]` | declared `:1608`; written `:1674`; uploaded via `glBufferSubData` `:1789` | **Delete** — function-local static; dies with the function |
| `GameOS/gameos/gos_terrain_indirect.cpp` | CPU-fallback branch in `ComputePreflight` (or its current name — grep at write-time) | `:1977-2002` (comment `:1977`; thinCount call `:1978`; cmdCount call `:1985`; arming `:1992-2004`) | **Replace** — when GPU SOLID is unarmed (env=0 or compute path can't admit), instead of falling through to the CPU pack, emit a one-shot `[TERRAIN_INDIRECT v1] event=cpu_pack_retired path=stock_solid` log and return `false` to disarm the indirect pipeline for the frame. The stock-install rule (`memory/stock_install_must_remain_playable.md`) is satisfied by the legacy SOLID/TES path in `quad.cpp:2299-2840` which is the original pre-indirect terrain renderer — see §7 OQ-1 for the unresolved interaction |
| `mclib/quad.cpp` | M2 fast-path **thin-record emit scope only** in `TerrainQuad::draw` | thin-emit scope `:2161-2182` (`recipeIdx`/`ensureRecipeForQuad` work `:2161-2162`; `TerrainQuadThinRecord tr; ... appendThinRecordDirect(tr);` block `:2166-2182`); flag-bit sink `:2169-2171`; pz/pzTri compute `:2089-2106`; both-culled cull `:2108`; M2d overlay block `:2220-~2276` | **Gate-split — narrowed to thin-emit only.** `pzTri1`/`pzTri2` (computed `:2097-2106` from the per-corner pz check at `:2089-2095`) have **TWO sinks**, not one: (1) the thin-record flag bits at `:2169-2171` (`tr.flags \|= pzTri1?2:0 \| pzTri2?4:0`) — orphaned by Fix B because the GPU compute re-derives them; (2) the `gos_PushTerrainOverlay()` calls gated at `:2244/2251/2260/2267` (calls at `:2249/2256/2265/2272`) — the **LIVE world-space overlay producer** (cement-transition tiles, runway/road decals), **NOT orphaned by Fix B**. Fix B only moved SOLID base-quad clip projection to compute; the M2d overlay emit is a separate decal pipeline. Therefore the gate-split MUST wrap **ONLY the thin-emit scope** (approximately `:2161-2182`: the `recipeIdx`/`ensureRecipeForQuad` work at `:2161-2162` plus the `TerrainQuadThinRecord tr; ... appendThinRecordDirect(tr);` block at `:2166-2182`), gated `if (!IsFrameSolidArmed()) { <thin-emit> }`. The `pz`/`pzTri` computation at `:2089-2106`, the `if (!pzTri1 && !pzTri2) return;` cull at `:2108`, and the M2d overlay block at `:2220-~2276` MUST keep running **unconditionally** — they feed the live decal producer. |
| `mclib/quad.cpp` | Legacy SOLID/TES `gVertex[3]` emit block | `:2299-2840` (gVertex[0] init `:2299-2307`; gVertex[1] `:2309-2317`; gVertex[2] `:2319-2327`; tri2 `:2822-2840`; per-vertex `pz` reads at `:2301`, `:2311`, `:2321`, `:2824`) | **Keep** for this slice — this is the legacy pre-indirect terrain renderer, reached when `TerrainPatchStream::isFastPathActive() == false` or when the M2 fast path falls back. It still depends on `vertices[c]->pz`/`px`/`py`/`pw` from VPL. Its retirement is the VPL plan's separate step, NOT this slice. **Verify at write-time** that `mc2-terrain-indirect-expert` agrees this path is unreachable when `MC2_TERRAIN_INDIRECT=1` (the default) — see §7 OQ-3. |
| `GameOS/gameos/gos_terrain_mask_dispatch.cpp` | Mask-dispatch per-tri pz cull (`:208-223`) | declared `:208` (comment); pz read `:211`; tri composition `:215-222` | **Out of scope** — gated by `MC2_TERRAIN_MASK_DISPATCH` (verified at `gos_terrain_mask_dispatch.cpp:56-57`); separate retirement step per VPL plan step 4. This slice does not touch it. |

> **WARNING (HIGH — advisor-corrected mis-scope, terrain-indirect advisor 2026-05-14):** The original §2 framing prescribed gate-splitting roughly `quad.cpp:2110-2218` against `IsFrameSolidArmed()`. That is a **mis-scope that would kill all cement-transition / runway / road decals under default config.** GPU SOLID is default-armed (`MC2_GPU_DRIVEN_TERRAIN_SOLID=1`), so `IsFrameSolidArmed()` (defined `gos_terrain_indirect.cpp:1929-1930` = `s_frameSolidArmed && !s_processArmingDisabled`, true by default) returns true on every production frame. Gating the `:2110-2218` span (which the orchestrator's range overlaps with the M2d overlay producer) would skip `gos_PushTerrainOverlay()` for every quad — the live decal producer goes dark. This is the **`9964d5a` conflated-overlay-paths regression class** (reverted by `4dd2c89`). The corrected scope gates ONLY the thin-emit block (`:2161-2182`); the pz/pzTri compute (`:2089-2106`), the `:2108` cull, and the M2d overlay block (`:2220-~2276`) stay unconditional. Any decal/cement-transition pixel diff under default config after the gate-split is a CRITICAL indicating this mis-scope was re-introduced.

**Drift from orchestrator-cited line numbers:** The orchestrator's prompt cited `quad.cpp:2089-2094` (M2 thin-emit pz block) and `gos_terrain_indirect.cpp:1639-1656` (indirect-dispatch pz cull CPU mirror) and `quad.cpp:2301-2824` (legacy SOLID/TES emit). All three citations are accurate to within ±2 lines at HEAD `bd9dea3` and match the sibling VPL plan's table. The actual `PackThinRecordsForFrame()` declaration is at `:1572` (not `:1377` as cited in `2026-05-11-gpu-driven-indirect-cmd-gen-design.md:227,970`); citation drift between commits has shifted it forward by ~200 lines. The orphan-CPU-mirror block the orchestrator described as "the indirect dispatch pz cull at `gos_terrain_indirect.cpp:1639-1656`" exists as the per-tri pz check inside `PackThinRecordsForFrame` at `:1638-1656` — one line earlier than the sibling cite, lossless overlap.

---

## §3 Execution sequence (v2 — single demotion commit)

**Plan v2 collapses the three-commit sequence to one commit.** The v1 design was: commit 1 gates body, commit 2 soaks 7 days, commit 3 deletes. Per the orchestrator's Wave-0 decision (amendment B1), the function body is permanently demoted behind a default-off env-gate rather than deleted, because `PackThinRecordsForFrame` is today's only GPU-arm-failure fallback and stock-install playability (per `memory/stock_install_must_remain_playable.md`) requires it to remain reachable when GPU SOLID can't arm. The eventual deletion of the function declaration itself is deferred to a future plan, contingent on production telemetry showing `MC2_TERRAIN_INDIRECT_CPU_FALLBACK` is never set in real play sessions.

The retired-with-the-pack items (M2 thin emit `pz` block, parity infrastructure, `s_shadow` staging) still die in this single commit; their consumers are confirmed dead by Fix B + the grep-verified file:line table in §2.

### Commit 1 — Demote `PackThinRecordsForFrame` body behind `MC2_TERRAIN_INDIRECT_CPU_FALLBACK=1` (default-off); retire M2 emit pz block + parity infrastructure

**Scope:**
- Add env-var read at module-init in `gos_terrain_indirect.cpp` (mirror existing `MC2_GPU_DRIVEN_TERRAIN_SOLID` style at `gpu_driven_common.cpp:64`). Cache as `static bool s_cpuFallback` at first use; env-var name **`MC2_TERRAIN_INDIRECT_CPU_FALLBACK`**, default-off.
- Wrap the body of `PackThinRecordsForFrame()` (`:1572-1807`) in `if (!s_cpuFallback) { return 0; /* no records this frame; indirect pipeline disarms for this frame */ }` at function entry so the indirect path's "no records this frame" semantics are preserved. The legacy SOLID/TES path in `quad.cpp:2299-2840` is the safety net under that disarm.
- Wrap the CPU-fallback branch in `ComputePreflight` (lines `:1977-2002`) similarly — when `s_cpuFallback` is false, emit a one-shot `[TERRAIN_INDIRECT v1] event=cpu_pack_demoted path=stock_solid` log and `return false` to disarm.
- Gate-split ONLY the M2 thin-record emit scope at `quad.cpp:2161-2182` (the `recipeIdx`/`ensureRecipeForQuad` work + the `TerrainQuadThinRecord tr; ... appendThinRecordDirect(tr);` block), wrapped `if (!IsFrameSolidArmed()) { <thin-emit> }` (compute already produces the authoritative thin record + re-derives the `:2169-2171` flag bits). The pz/pzTri compute at `:2089-2106`, the `:2108` `!pzTri1 && !pzTri2` cull, and the M2d overlay block at `:2220-~2276` (the live `gos_PushTerrainOverlay` decal producer) MUST remain unconditional — see the §2 WARNING (the `:2110-2218` framing was a HIGH mis-scope, `9964d5a`/`4dd2c89` regression class).
- **Demote (NOT delete) the parity infrastructure** per OQ-2 resolution: `s_packParityMask`, `kParityMaskWords`, `ComputeDispatchParity_Check()`, the `MC2_GPU_DRIVEN_PARITY` env-var read, accessor `gos_terrain_indirect_getPackParityMask` (`gos_terrain_indirect.cpp:2951`). These self-gate (zero stock cost) and `gos_terrain_mask_dispatch.cpp:309` holds a real env-gated dependency on the accessor — full delete is deferred to VPL-plan Step 4 (mask-dispatch retire/repoint). See §7 OQ-2 and the Step 1b commit breakdown below.
- Retire the function-local `s_shadow` staging array (`gos_terrain_indirect.cpp:1608`) — dies inside the demoted body since it's function-local and only used during pack.
- Emit one-shot info-line at first-frame `cpu_pack_demoted` from each gate site so the smoke log captures the demotion.

**Demoted, NOT deleted:**
- `PackThinRecordsForFrame()` declaration + body remains on disk, gated. Future plan retires the declaration itself once telemetry confirms the env-gate is unset in production.
- `BuildIndirectCommands()` (`:1812+`) remains on disk if it has a live caller after demotion; if its only caller is inside the demoted `PackThinRecordsForFrame` body, gate it identically.
- The Fix-A MVP snapshot at `gos_terrain_indirect.cpp:1596-1604` continues to write under both code paths (GPU-armed always, CPU-fallback when env-gate set). VPL plan step 9 demotes Fix-A snapshot behind `MC2_RING_TRACE=1` separately.

**What stays building:**
- The legacy SOLID/TES path in `quad.cpp:2299-2840` is **untouched**. Reachable when `TerrainPatchStream::isFastPathActive() == false`.
- VPL runs unchanged. `vertexProjectLoop` Tracy zone still appears with unchanged cost.

**Tier1 smoke must show:**
- `--tier tier1 --duration 30 --kill-existing` (no `--with-menu-canary`): all 5 missions PASS.
- `[TERRAIN_INDIRECT v1] event=cpu_pack_demoted` fires once per mission in the artifact log under default env.
- `MC2_TERRAIN_INDIRECT_CPU_FALLBACK=1` re-runs of mc2_01 + mc2_10 PASS visually (env-gate exercise verified).
- Frame-perfect byte-compare on a 30s mc2_01 capture between pre-commit baseline and post-commit (default config): byte-identical because the GPU path already wrote authoritative thin records and the CPU pack was being overwritten or ignored. Any diff is a CRITICAL — CPU pack was contributing to a path we mis-classified as orphan.

### Step 1b commit breakdown (deferred-remainder split — advisor-corrected 2026-05-14)

The deferred Step 1b remainder (the quad.cpp gate-split + parity-infra demotion, distinct from the `PackThinRecordsForFrame` body demotion above) lands as **two commits**:

**Commit 1b-1 — quad.cpp narrowed thin-emit gate-split (per §2 A1 corrected scope).**
- Wrap ONLY `quad.cpp:2161-2182` (thin-emit scope) in `if (!IsFrameSolidArmed()) { ... }`. pz/pzTri compute (`:2089-2106`), the `:2108` cull, and the M2d overlay block (`:2220-~2276`) stay unconditional.
- Verification: tier1 smoke `--tier tier1 --duration 30 --kill-existing` (no menu canary) + **frame-perfect byte-compare on mc2_01**. Any decal/cement-transition pixel diff = **CRITICAL** = overlay producer wrongly gated (the §2 WARNING `9964d5a` regression class re-introduced).
- Recommend adding a `gos_push_overlay_calls` parity-summary assertion to the smoke gate: tens of thousands per 600-frame window on a live mission; **0 = producer killed** (the load-bearing structural canary the byte-compare backstops).

**Commit 1b-2 — thin telemetry / demotion commit.**
- Add a one-shot `[TERRAIN_INDIRECT v1] event=parity_infra_demoted` log at the `IsParityEnabled()` early-return (`gos_terrain_indirect.cpp:2718`); update the perf-snapshot row to "DEMOTED".
- **Do NOT delete** `s_packParityMask`, `gos_terrain_indirect_getPackParityMask`, or `ComputeDispatchParity_Check` — they self-gate (zero stock cost); full delete is the deferred follow-up gated on (a) mask-dispatch retirement (VPL Step 4) + (b) telemetry showing `MC2_GPU_DRIVEN_PARITY` / `MC2_TERRAIN_MASK_DISPATCH_PARITY` are never set in production.
- **Pre-existing MED to document in the demotion log:** since commit `18a4c36` demoted `PackThinRecordsForFrame`, `s_packParityMask` is already stale-zero under default env. Note this in the `parity_infra_demoted` log so a future debugger does not chase a phantom 100%-mismatch when running `MC2_TERRAIN_MASK_DISPATCH_PARITY=1` **without** `MC2_TERRAIN_INDIRECT_CPU_FALLBACK=1`.

### Future commit (deferred) — full removal of `PackThinRecordsForFrame` declaration

Deferred to a follow-up plan, contingent on a soak window proving `MC2_TERRAIN_INDIRECT_CPU_FALLBACK=1` is never set in production telemetry. When that lands:
- Delete `PackThinRecordsForFrame()` declaration + body (`:1572-1807`).
- Delete `BuildIndirectCommands()` (`:1812-1859`-ish — verify at write time).
- Delete `MC2_TERRAIN_INDIRECT_CPU_FALLBACK` env-var read and remaining gate code.
- Delete the CPU-fallback branch in `ComputePreflight` entirely.
- Update `docs/render-perf-snapshot.md`: re-measure `Terrain::SolidComputeDispatch` bucket; mark "CPU pack path retirement" row as FULLY DELETED.

This plan ships only the demotion commit. The deferred deletion is a follow-up.

---

## §4 Verification gates

**Per-commit smoke (the single demotion commit):**

```
py -3 A:/Games/mc2-opengl-src/.claude/worktrees/gpu-driven-rendering/scripts/run_smoke.py --tier tier1 --duration 30 --kill-existing
```

No `--with-menu-canary` (project default; per `memory/feedback_smoke_gate_no_menu_canary.md`).

**Parity probe — frame-perfect screen byte-compare on a 30s mc2_01 capture.** Two captures:
1. baseline (HEAD before the demotion commit)
2. post-commit at default env (`MC2_TERRAIN_INDIRECT_CPU_FALLBACK` unset; CPU pack demoted)

Both captures must be byte-identical screen-by-screen across the 30-second mc2_01 run. A third capture with `MC2_TERRAIN_INDIRECT_CPU_FALLBACK=1` is a separate sanity check that the env-gate path still produces correct output (it should also be byte-identical to baseline, since the CPU pack and the GPU path were both producing equivalent records). Discipline: the smoke harness sends no input; cam is static; output is deterministic; any frame diff is a CRITICAL. `memory/parity_finds_gpu_substrate_bugs_visual_smoke_misses.md` is the warning here: visual smoke alone passes while data flow is silently wrong; the byte-compare is the second gate.

**"VPL still alive" sanity probe.** User-driven Tracy at wolfman zoom in mc2_10 (substrate-heavy). The zone `vertexProjectLoop` MUST still appear in the captured trace, with mean cost within ±10% of the pre-slice baseline `~0.475 ms` cited in `docs/render-perf-snapshot.md`. If VPL disappears or its cost moves outside noise band, this slice has accidentally coupled to a non-CPU-pack VPL consumer — abort and investigate.

**Negative-claim probe.** Before the demotion commit, grep the worktree for any remaining consumer of `vertices[c]->pz`, `s_packParityMask`, `TerrainQuadThinRecord`, and `g_thinSlotMVP` outside the indirect-SOLID compute path and the legacy SOLID/TES emit in `quad.cpp:2299-2840`. Any unexpected consumer is an open question for adversarial review per `memory/feedback_data_flow_audit_asymmetry.md` (grep the candidate consumer, not the obvious-named one).

**Adversarial review trigger.** This plan is a legacy retirement and an architectural-endpoint shift (sole projection authority confirmed by deletion of the CPU mirror). Per the worktree CLAUDE.md Review Discipline section, that is two high-stakes triggers. Dispatch with the verbatim incantation "use the adversarial-plan-review skill" before commit 1 ships. Expect 2-4 CRITICAL + 4-6 MAJOR + 5-8 MINOR per the Fix-B precedent.

---

## §5 Rollback

**Runtime rollback (post-commit, regression discovered):**
- Set `MC2_TERRAIN_INDIRECT_CPU_FALLBACK=1` in the runtime env. The CPU pack body re-engages; the GPU path remains armed; the `cpu_pack_demoted` log is silenced. No source-code revert required.

**Source-code rollback:**
- Single-commit `git revert <demotion-sha>`. Restores the un-gated CPU pack body and re-introduces the M2 emit pz block + parity infrastructure. Mechanical.

**Stock-install safety:** because the demotion preserves the CPU pack body behind an env-gate rather than deleting it, the `memory/stock_install_must_remain_playable.md` invariant is satisfied without depending on the legacy SOLID/TES path being reachable. Any user who hits a GPU-arm failure can be advised to set the env-gate as an immediate workaround while the regression is investigated.

**Stock-install invariant** (`memory/stock_install_must_remain_playable.md`): the legacy SOLID/TES path in `quad.cpp:2299-2840` remains unmodified by this slice and is reached when `TerrainPatchStream::isFastPathActive() == false`. A user with `MC2_GPU_DRIVEN_TERRAIN_SOLID=0` plus `MC2_TERRAIN_INDIRECT=0` plus all other indirect-pipeline envs unset MUST still render correct terrain after commit 3. The §7 OQ-1 question about CPU-fallback reachability is the load-bearing open question on this guarantee.

---

## §6 Cross-references

- **Sibling VPL plan:** `docs/superpowers/plans/2026-05-14-vertex-project-loop-retirement.md` — 9-step interleaved sequence. This plan is step 1. The VPL plan's §"Adjacent fields written by VPL" table classifies `vertex->pz` consumers, and the entries marked "(a) Chopping block" at `quad.cpp:2089-2094` and `gos_terrain_indirect.cpp:1639-1656` are exactly the consumers this slice retires.
- **Raster-triangle handoff:** `docs/superpowers/progress/2026-05-14-raster-triangle-handoff.md` — Fix B description (lines 105-110 in that doc, "Fix B (better visual): clip-space positions in thin records / VS skips projection entirely / No MVP at all in the bridge"). The CPU pack was already irrelevant pre-Fix-B for the GPU-armed path; Fix B made the orphan total.
- **Perf snapshot:** `docs/render-perf-snapshot.md` — bucket map and the "CPU pack path retirement" row in the in-flight slices table. Refresh after the demotion commit (mark "DEMOTED" rather than "SHIPPED-DELETED"); refresh again after the deferred declaration-deletion commit.
- **Memory rules (load-bearing):**
  - `memory/ring_slot_state_must_travel_with_slot.md` — the Fix A pattern this slice's retirement makes inert. Once the CPU pack is gone, the per-slot MVP snapshot in `gos_terrain_indirect.cpp:1596-1604` becomes pure defense-in-depth (no CPU writer can produce mismatched data). VPL plan step 8 is the eventual Fix-A retirement, AFTER this slice.
  - `memory/stock_install_must_remain_playable.md` — load-bearing on §5 rollback's stock-install invariant. The legacy SOLID/TES path in `quad.cpp` is the safety net.
  - `memory/mc_texture_manager_dual_queue_legacy_retirement_debt.md` — sets the "do NOT keep dead code env-gated" rule. Path C in §3 commit 2 is the last-resort drift toward this anti-pattern; the orchestrator must approve before the slice closes there.
  - `memory/parity_finds_gpu_substrate_bugs_visual_smoke_misses.md` — load-bearing on §4 frame-perfect byte-compare requirement.
  - `memory/feedback_data_flow_audit_asymmetry.md` — load-bearing on §4 negative-claim probe.
  - `memory/feedback_smoke_gate_no_menu_canary.md` — load-bearing on §4 smoke-gate command.
- **Advisor briefs:**
  - `.claude/agents/mc2-terrain-indirect-expert.md` — `<limits>:152` ("the legacy CPU thin-record packer's `PackThinRecordsForFrame` past what the bridge code shows — the compute path replaces it") is the explicit advisor sign-off that the CPU pack is terminal.
  - `.claude/agents/mc2-cpu-gpu-offload-expert.md` — Stage 6 (`<core_knowledge>:49`) "demote-not-delete (gated-off CPU body stays as retirement telemetry per `memory/debug_instrumentation_rule.md`)" is honored by v2's single-demotion-commit structure; the gated body remains the GPU-arm-failure fallback, and the full declaration-removal is deferred to a future plan once production telemetry proves the env-gate is unset.

---

## §7 Open questions for adversarial review

**OQ-1 — CPU-fallback reachability when `MC2_GPU_DRIVEN_TERRAIN_SOLID=0`.** v2 RESOLVED: the Wave-0 grep walk established legacy SOLID/TES at `quad.cpp:2299-2840` is unreachable under `MC2_TERRAIN_INDIRECT=1` (default). v2 mitigates by demoting (not deleting) `PackThinRecordsForFrame`: under the demotion commit at default env, the CPU-fallback branch returns no records and the indirect pipeline disarms for the frame; under `MC2_TERRAIN_INDIRECT_CPU_FALLBACK=1` (the safety-valve env-gate), the CPU pack runs as before. Stock-install invariant is preserved by the env-gate, not by the legacy SOLID/TES path. The original v1 question about legacy-SOLID-reachability becomes a question only for the deferred declaration-deletion commit, not for this slice.

**OQ-2 — External readers of `s_packParityMask` / parity infrastructure. RESOLVED 2026-05-14 (terrain-indirect advisor).** 3-consumer reachability table (grep-verified at HEAD `63c9ee7`):

| Consumer | Site | Stock-reachable? | Why |
|---|---|---|---|
| `gos_terrain_indirect::ComputeDispatchParity_Check()` | call `txmmgr.cpp:1668` | NO (net effect) | Call site is stock-reachable (inside `if (gos_terrain_indirect::IsFrameSolidArmed())` at `txmmgr.cpp:1659`) but the callee self-gates: `gos_terrain_indirect.cpp:2718` `if (!gpu_driven::IsParityEnabled()) return;`. `IsParityEnabled` (`gpu_driven_common.cpp:36-42`) = `getenv("MC2_GPU_DRIVEN_PARITY")`, default-off. **No stock work.** |
| `gos_terrain_indirect_getPackParityMask(&packWords)` | `gos_terrain_mask_dispatch.cpp:309` | NO | Two default-off env gates: `MC2_TERRAIN_MASK_DISPATCH` (via `MasterEnabled()`, `gos_terrain_mask_dispatch.cpp:78-85`) AND `MC2_TERRAIN_MASK_DISPATCH_PARITY` (`:306-307`). |
| `gos_terrain_indirect.h:349` | forward decl only | n/a | Sole stock caller is the txmmgr row above. |

NOTE: there is a SEPARATE `WaterStream::ComputeDispatchParity_Check()` (`gos_terrain_water_stream.cpp:1418`, called from `gameos_graphics.cpp:2340`) — different namespace, **do NOT conflate**; Step 1b touches only the `gos_terrain_indirect::` symbol.

**Resolution verdict:** parity infra is dev-gated (`MC2_GPU_DRIVEN_PARITY` / `MC2_TERRAIN_MASK_DISPATCH_PARITY`, both default-off), **not stock-reachable in effect**. It **cannot be deleted in Step 1b** because `gos_terrain_mask_dispatch.cpp:309` holds a real (env-gated) dependency and the txmmgr call site is link-live. **Decision: demote-don't-delete** (consistent with Fix A / bucket-header / `PackThinRecordsForFrame`). Full deletion is deferred until VPL-plan Step 4 (mask-dispatch retire/repoint) lands. OQ-2 is closed; no longer blocks the demotion commit.

**OQ-3 — Legacy SOLID/TES reachability when M2 fast path is active.** `quad.cpp:2299-2840` is the legacy `gVertex[3]` path. Eligibility flags at `:2026-2035` show that the M2 fast path takes over when `isFastPathActive() && isThinRecordsActive() && isReady() && !isOverflowed() && (g_handle || !g_noWater || !g_noOverlay)`. When fast-path-eligible, the M2 thin emit block at `:2087-2218` runs and the legacy SOLID/TES block at `:2299+` is presumably skipped. **Open:** does the M2 block `return` or `continue` cleanly before the legacy block, or does the legacy block also run for some quad subset (e.g., overlay-only quads where M2d emits overlay but legacy block also emits base)? If both run, the demotion commit's repoint (skip M2 pz when GPU-armed) could leave the legacy block reading `pz` that is now stale relative to compute's gate. **Requires:** read of `quad.cpp:2110-2299` (the M2 block's full body) to confirm exit semantics.

**OQ-4 — `g_thinSlotMVP` / Fix-A defense-in-depth interaction.** The demotion commit's gate skips the Fix-A MVP snapshot at `gos_terrain_indirect.cpp:1596-1604` inside the demoted `PackThinRecordsForFrame` body. The snapshot at `:1601-1604` is **also** written by the GPU-armed path (the comment at `:1596-1600` says "Stash anyway to keep the 'every slot has a valid MVP' invariant uniform across CPU and GPU paths"). **Open:** is the snapshot's GPU-path write still happening after this commit's CPU pack gate? Grep `g_thinSlotMVP` writes; verify the GPU-armed branch at `:1958-1974` still hits a `memcpy(g_thinSlotMVP[g_thinRingSlot], ...)`. If not, Fix A is silently disabled and the raster-triangle bug class is reopened. The VPL plan's step 9 demotes Fix-A scaffolding behind `MC2_RING_TRACE=1` — Fix A must remain armed through this slice (i.e., the GPU-armed-branch snapshot write must remain unconditional regardless of `MC2_RING_TRACE`).

**OQ-5 — Tracy zone `Terrain::ThinRecordPack` becomes silent under default env.** Under v2 the zone is not deleted (function body is gated, not removed), but it stops emitting at default env. Anyone with a saved Tracy capture comparison harness keyed on this zone name will see it go quiet rather than disappear. **Open:** does `scripts/run_smoke.py` or any artifact-analysis script parse the Tracy zone name `Terrain::ThinRecordPack`? Grep before the demotion commit; document the default-env silence in the slice's commit message.

**OQ-6 — `BuildIndirectCommands` is invoked from anywhere outside the CPU-fallback branch?** Sibling plan `2026-05-11-gpu-driven-indirect-cmd-gen-plan.md` Task 6.2 framed this as a paired demotion, but didn't grep all callers. Confirm `BuildIndirectCommands` has exactly one call site at `:1985` before deleting; if a second caller exists, that caller defines a different consumer chain.

---

**End of plan v2.** Next action: implement the demotion commit per §3. The adversarial-review pass on v1 has been incorporated (amendment B1 from orchestrator's Wave-0 decision). The deferred declaration-deletion commit ships in a follow-up plan once production telemetry proves `MC2_TERRAIN_INDIRECT_CPU_FALLBACK` is never set.
