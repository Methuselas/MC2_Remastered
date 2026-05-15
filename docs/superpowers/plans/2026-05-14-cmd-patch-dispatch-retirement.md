# Cmd-Patch Dispatch Retirement — Implementation Plan v2.1

> **Plan v2.1 amendment note (2026-05-14):** v2.1 adds one cross-plan tracking item closing NEW-LOW-1 from the VPL retirement re-review (`docs/superpowers/reviews/2026-05-14-vpl-retirement-rereview.md`): the water-stream path at `gos_terrain_water_stream.cpp:1372` carries the identical `GL_COMMAND_BARRIER_BIT` issue and will need a sibling fix when water cmd-patch is retired. This is documented in §"Related future work" as a tracked future slice (no plan doc yet) and as a load-bearing constraint on any water-stream barrier-touching change. No functional change to the SOLID-slice plan body.

> **Plan v2 amendment note (2026-05-14):** v2 lands amendments C1, C2, C3 from the orchestrator's Wave-0 review + advisor briefings. Key shifts: (C1) `g_solidBucketHeaderSsbo` is demoted behind `MC2_BUCKET_HEADER_TRACE=1` (default-off) rather than deleted or kept unconditionally — OQ-4 resolved with user decision option (c); (C2) makes explicit that the primary compute shader's `atomicAdd` into `cmd[bucket].count` MUST use `kVertsPerElement` (6u) as the addend (not 1u), the barrier between primary compute and `glMultiDrawElementsIndirect` MUST be `GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT`, and the per-frame init of `instanceCount`/`first`/`baseInstance` strategy is documented; (C3) the overflow-clamp at `min(visibleCount, u_maxThinRecords)` is preserved via the primary's bound check moved before the atomicAdd. Also flags the water-stream path (`gos_terrain_water_stream.cpp:1372`) as needing the same barrier change (out of scope for this plan but documented as related future change).

> **Foundation:** Fix B (commit `005ebc7`, 2026-05-14) moved per-corner projection into `gpu_driven_terrain_solid.comp` and made the indirect SOLID path the sole GPU projection authority for terrain quads. With Fix B landed, the second compute dispatch ("cmd-patch") that reads `GpuDrivenBucketHeader.visibleCount` and writes `DrawElementsIndirectCommand.count` is structurally redundant: the primary cull/pack dispatch already knows when a record is admitted, and can `atomicAdd` directly into the indirect-draw cmd buffer's `count` field at admission time.
>
> **Architectural unlock:** the bucket-header indirection (`g_solidBucketHeaderSsbo`) exists only because cmd-patch needs a single 32-bit visibleCount to multiply by `u_vertsPerElement=6`. Replacing `atomicAdd(hdr.visibleCount, 1u)` with `atomicAdd(cmds[0].count, 6u)` collapses the producer-consumer pair into a single producer dispatch. One `glDispatchCompute(1,1,1)` + its `GL_SHADER_STORAGE_BARRIER_BIT` go away; `GL_COMMAND_BARRIER_BIT` stays (still need to fence compute->indirect-draw).
>
> **Recon report:** grep walk done 2026-05-14 (this plan's author session). File:line table in §2. Sibling plan structure mirrored from `docs/superpowers/plans/2026-05-14-vertex-project-loop-retirement.md`.

**Goal:** Retire the second `glDispatchCompute` for the SOLID terrain indirect path. Replace `atomicAdd(hdr.visibleCount, 1u)` + cmd-patch with a direct `atomicAdd(cmds[0].count, kVertsPerQuad)` in the cull/pack dispatch. Per `docs/render-perf-snapshot.md` line 30: "~80-170 µs recoverable" from `Terrain::SolidComputeDispatch` (CPU submission). Saves one compute dispatch + one `GL_SHADER_STORAGE_BARRIER_BIT` per frame.

**Scope:** SOLID terrain indirect path only (`gos_terrain_indirect.cpp`). The water fast path (`gos_terrain_water_stream.cpp`) shares the same `gpu_driven_cmd_patch.comp` shader binary but has its own SSBO state (`g_waterBucketHeaderSsbo`, `g_waterIndirectCmdBuffer`) and an independent dispatch site. Water cmd-patch retirement is a sibling slice and **explicitly out of scope** for this plan — see §1.

---

## Out of scope

- **Water fast-path cmd-patch retirement.** Same shader, same pattern, but distinct buffers and a distinct dispatch site at `GameOS/gameos/gos_terrain_water_stream.cpp:1371-1391`. The water path also patches `u_cmdCount=2` indirect cmds (vs SOLID's 1), and the `WaterThinRecord` flags semantics differ. Out-of-scope-on-purpose: ship SOLID first, validate parity, sibling slice retires water after soak. **Related future change (per C2):** the water-stream barrier at `gos_terrain_water_stream.cpp:1372` (currently `GL_SHADER_STORAGE_BARRIER_BIT` alone) MUST become `GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT` when its cmd-patch is retired; documented here so the water sibling slice doesn't lose this latent driver-dependent fix.
- **Deletion of the `gpu_driven_cmd_patch.comp` source file.** As long as the water path still uses it, the file stays on disk. Plan removes only the SOLID consumer (program + dispatch + bucket-header SSBO + CPU readback probes). File deletion ships with the water sibling.
- **Probe / diagnostic retirement.** `MC2_RING_TRACE` (`gos_terrain_indirect.cpp:2518-2548`) currently reads `cmd[0].count` after cmd-patch barrier to detect race. After retirement, the same probe must read the same field after the primary-dispatch barrier; the read site moves but the probe stays. Probe-6/7 readback of `hdr.pad1_/pad2_` (`gos_terrain_indirect.cpp:2204-2244`) is a separate diagnostic that depends on the bucket header SSBO; see §2 row "header SSBO" for the conditional retirement.
- **`pad1_` / `pad2_` near-clip / spread counters.** These are clip-w diagnostic atomic-add counters in `gpu_driven_terrain_solid.comp:239/342`. If we delete `g_solidBucketHeaderSsbo` entirely, we orphan these. Decision in §7 OQ-4 — likely keep a minimal 16 B counter SSBO bound for diagnostics, drop only the `visibleCount` slot.

---

## Architectural references (read before opening any phase)

1. `docs/superpowers/plans/2026-05-14-vertex-project-loop-retirement.md` — sibling plan; "step 2: cmd-patch retirement (already queued)" is this slice. Match its tone, structure, soak discipline.
2. `docs/render-perf-snapshot.md:30,47` — perf claim: `~80-170 µs recoverable via cmd-patch retirement`. Quoted verbatim; do not invent a tighter number until measured.
3. `memory/substrate_coalesce_sync_point_lesson.md` — canonical lesson: `glGetBufferSubData` after a GPU write causes implicit sync stall (substrate 2x regression 2026-05-11). Cmd-patch retirement aligns with this rule's design philosophy: counter work stays on GPU. Cited again in §7 OQ for the probe readback path.
4. `memory/ring_slot_state_must_travel_with_slot.md` — Fix A / Fix B pattern. Any uniform a GPU producer uses to write a ring slot must be stashed with the slot. Applies here because the cmd buffer is single-slot, not ringed; the primary dispatch's `atomicAdd` lands directly in the slot the bridge will draw from after `GL_COMMAND_BARRIER_BIT`.
5. `memory/cpp_glsl_ubo_struct_lockstep.md` — `DrawArraysIndirectCommand` is a 16 B std430 struct. The GLSL side already declares it (`gpu_driven_cmd_patch.comp:47-52`). After retirement the primary compute shader must declare it; lockstep with C++ host-side allocation at `gos_terrain_indirect.cpp:1513-1521`.
6. `.claude/agents/mc2-terrain-indirect-expert.md:82-100` — `<known_pitfalls>`. Specifically: "Ring-slot fence missed" (line 94), "Struct-lockstep silent break" (line 92), "Compute cull and indirect terrain are entangled" (line 96). Re-read before each commit.
7. `.claude/agents/mc2-shader-expert.md` — AMD-driver shader rules. `atomicAdd` on SSBO offsets: confirm contention model. See §7 OQ-3.
8. `.claude/agents/mc2-cpu-gpu-offload-expert.md:69` — "Sync stall hazard from readback after GPU write." Retirement removes the `pad1_/pad2_` readback (`gos_terrain_indirect.cpp:2204-2244`) only if `g_solidBucketHeaderSsbo` is retired wholesale; otherwise readback path is unchanged.

---

## Load-bearing constraints

- **`GL_COMMAND_BARRIER_BIT` stays.** Removing the second dispatch removes one `GL_SHADER_STORAGE_BARRIER_BIT` (between primary and cmd-patch). The barrier AFTER the surviving dispatch must still be `GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT` because the GPU still needs to fence compute-write of `cmd[0].count` against the subsequent `glDrawElementsIndirect` (or `glMultiDrawArraysIndirect`) consumer. Today's barrier at `gos_terrain_indirect.cpp:2504` already has both bits; that line survives, just moves one statement up.
- **Pre-init `instanceCount / first / baseInstance`.** Today cmd-patch writes all four fields of `DrawArraysIndirectCommand` every frame (`gpu_driven_cmd_patch.comp:78-81`). After retirement, the primary compute only touches `cmd[0].count`. The other three fields must be initialized **once at allocation time** (`gos_terrain_indirect.cpp:1513-1521`) with `instanceCount=1, first=0, baseInstance=0` and never written again. The per-frame `count` reset becomes a `glClearBufferSubData` of just the 4-byte count field at offset 0, before the primary dispatch.
- **`atomicAdd` increment is `kVertsPerElement` (6u), not 1u.** The visibleCount→count translation is `count = visibleCount * 6` (six vertices per emitted quad: two triangles, three verts each). Direct path: `atomicAdd(cmds[bucket].count, kVertsPerElement)` per admitted record (where `kVertsPerElement == 6u`). The cmd-patch shader's current multiplication at `shaders/gpu_driven_cmd_patch.comp:74` (`count = vis * vertsPerElement`) collapses into the atomic add by adding 6 per record rather than 1. Increment value is hardcoded in `g_locCmdVPE` upload at `gos_terrain_indirect.cpp:2063` (currently `glUniform1i(g_locCmdVPE, 6)`); move the literal `6u` into the primary compute shader (or expose as `u_vertsPerElement` uniform on the primary). **Multi-bucket note:** if the primary emits records into multiple buckets (one indirect cmd per bucket), each admitted record does `atomicAdd(cmds[bucket].count, 6u)` against its own bucket's count field.
- **Barrier semantics after cmd-patch dies (C2).** Today's barriers split the work: a `GL_SHADER_STORAGE_BARRIER_BIT` between primary and cmd-patch (`:2490`) and a combined `GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT` between cmd-patch and the indirect draw (`:2504`). After cmd-patch dies, the barrier between primary compute and `glMultiDrawElementsIndirect` **MUST** be `GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT` — the COMMAND half moves up from the post-cmd-patch site to the post-primary site. The `SHADER_STORAGE` half is for the VS reads of `thin[]`/`recipe[]`; the `COMMAND` half is for the indirect-draw read of `cmd[].count`. Dropping COMMAND is a latent driver-dependent corruption (NVIDIA tolerant, AMD failing).
- **Water-stream path needs the identical barrier change (out of scope).** `GameOS/gameos/gos_terrain_water_stream.cpp:1372` currently uses `GL_SHADER_STORAGE_BARRIER_BIT` alone after the water primary dispatch; this must become `GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT` when the water cmd-patch retirement sibling slice lands. Documented here as a related future change; **not in scope for this plan**.
- **Per-frame init of `instanceCount`/`first`/`baseInstance` (C2).** Today cmd-patch writes all four fields of `DrawElementsIndirectCommand` every frame (`shaders/gpu_driven_cmd_patch.comp:78-81`). When cmd-patch dies, these MUST be either (a) initialized once at buffer creation AND `count` cleared per frame via `glClearBufferSubData(GL_DRAW_INDIRECT_BUFFER, offset_of_count, 4, &zero)` before primary dispatch, or (b) written by the primary compute via a single-invocation init step (one workgroup-zero invocation writes the constants, then the main work proceeds). **Plan picks (a):** allocate-time initialization for `instanceCount=1`, `first=0`, `baseInstance=0` set once at `gos_terrain_indirect.cpp:1513-1521`; per-frame `glClearBufferSubData` of the 4-byte count field before primary dispatch. This is the simpler approach and avoids divergent control flow in the primary compute.
- **Clamp logic preserved (C3).** Cmd-patch today clamps `vis = min(hdr.visibleCount, uint(u_maxThinRecords))` at `gpu_driven_cmd_patch.comp:73` before multiplying — this enforces the thin-ring overflow bound. Primary compute already has the overflow guard at `shaders/gpu_driven_terrain_solid.comp:350` (`if (outSlot >= u_maxThinRecords) return;`) on the consumer side, but `hdr.visibleCount` will overshoot when overflow occurs because the atomicAdd happens before the bound check today. **Plan picks:** move the atomicAdd after the bound check in primary compute. Concretely: do the bound check first (`if (outSlot >= u_maxThinRecords) return;`), then `atomicAdd(cmds[bucket].count, 6u)`. The bound check uses a pre-incremented per-bucket counter as today; the atomicAdd into `cmd[].count` only fires when the record is actually admitted. This eliminates the need for a separate `min(...)` clamp at the indirect-cmd write site.
- **No env-gated dead code.** Same rule from sibling plan §"Load-bearing constraints." Either cmd-patch dies cleanly (Path A complete) or it stays alive as primary (Path A blocked). Soak-window env-gate is acceptable for the parity-validation commit only, must be ripped out in the default-on commit. Per `memory/mc_texture_manager_dual_queue_legacy_retirement_debt.md`.
- **AMD `atomicAdd` contention.** `kMaxThinRecords` is the upper bound on simultaneously-active workgroup invocations contending on `cmd[0].count`. Same contention shape as the existing `atomicAdd(hdr.visibleCount, 1u)` (`gpu_driven_terrain_solid.comp:349`) — every admitted record already does one atomicAdd on a shared 32-bit slot. Replacing the target offset by 4 bytes (from `hdr.visibleCount` at offset 0 of the header SSBO to `cmds[0].count` at offset 0 of the indirect-cmd SSBO) does not change contention pressure; both atomics serialize on a single uint. See §7 OQ-3 for AMD driver confirmation.

---

## §2 File:line target table (grep-verified 2026-05-14)

All file:line citations re-grep at write-time per worktree CLAUDE.md "Documentation discipline."

### Shader sites

| Path | Current symbol | Current line range | Action |
|---|---|---|---|
| `shaders/gpu_driven_terrain_solid.comp` | `atomicAdd(hdr.visibleCount, 1u)` admission | `349` | **Modify** — replace with `atomicAdd(cmds[0].count, 6u)` (or `atomicAdd(cmds[0].count, uint(u_vertsPerElement))` if uniform); add binding for `DrawArraysIndirectCommand` SSBO (new binding slot, propose `binding = 8`); declare `struct DrawArraysIndirectCommand` lockstep with `gpu_driven_cmd_patch.comp:47-52` and C++ host-side type at `gos_terrain_indirect.cpp:1513-1521`. |
| `shaders/gpu_driven_terrain_solid.comp` | `atomicAdd(hdr.pad1_, 1u)` (near-clip-w probe) | `342` | **Conditional** — see §7 OQ-4. If `g_solidBucketHeaderSsbo` is retained for diagnostics, keep. If retired, retarget or remove. |
| `shaders/gpu_driven_terrain_solid.comp` | `atomicAdd(hdr.pad2_, 1u)` (recipe-spread probe) | `239` | **Conditional** — same as `pad1_`. |
| `shaders/gpu_driven_cmd_patch.comp` | entire file | full | **Keep on disk** — water path still uses it. Delete only when water sibling slice ships. SOLID program handle (`g_solidCmdPatchProgram`) goes away regardless. |

### C++ dispatch / allocation sites — `gos_terrain_indirect.cpp`

| Symbol | Current line range | Action |
|---|---|---|
| `static GLuint g_indirectCmdBuffer` decl | `1411` | **Keep** — survives as primary's atomicAdd target. |
| `g_indirectCmdBuffer` allocation (`glGenBuffers` + initial fill) | `1513-1521` | **Modify** — initial fill must set `{count=0, instanceCount=1, first=0, baseInstance=0}` once at allocation; per-frame only `count` is zeroed. |
| `static GLuint g_solidCmdPatchProgram` decl | `1435` | **Delete** in flip commit. |
| `static GLuint g_solidBucketHeaderSsbo` decl | `1436` | **Conditional** — §7 OQ-4. |
| Solid cmd-patch program compile (`BuildComputeProgramFromFile`) | `2050-2068` | **Delete** in flip commit. Uniform-location caches `g_locCmdVPE`, `g_locCmdCC` follow. |
| `g_solidBucketHeaderSsbo` lazy-alloc | `2070-2075` | **Conditional** — §7 OQ-4. |
| `glGetBufferSubData` on bucket header (probe 4/5 readback) | `2204-2244` | **Conditional** — §7 OQ-4. If bucket header retired, this probe block goes too; if retained, unchanged. Note this is a CPU readback after GPU write; `memory/substrate_coalesce_sync_point_lesson.md` flags such reads — comment at `:2201-2203` claims "fence wait above guarantees" no stall, but stall risk on AMD remains. Sibling concern, not blocker. |
| `glClearBufferSubData` of bucket header (per-frame zero) | `2421-2428` | **Modify or delete** — replace with `glClearBufferSubData` of `g_indirectCmdBuffer` first 4 bytes (count only). If `g_solidBucketHeaderSsbo` retained for diagnostics, also clear that. |
| Binding bucket header to slot 6 (`glBindBufferBase`) | `2442` | **Conditional** — §7 OQ-4. If retired, delete binding; if retained for diagnostics, keep. New binding `glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, g_indirectCmdBuffer)` added at same site (or chosen slot). |
| **DISPATCH 2 block** (cmd-patch dispatch + barrier between) | `2492-2504` | **Delete** entirely. The barrier currently at `:2490` (`glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT)` between primary and cmd-patch) is also deleted; the surviving barrier at `:2504` (`glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT)`) moves up to immediately after the DISPATCH 1 `glDispatchCompute`. Net: one barrier total, both bits, between primary dispatch and `glDrawElementsIndirect`. |
| `MC2_RING_TRACE` cmd-count readback probe | `2518-2548` | **Modify** — site moves up by ~25 lines (now reads cmd[0].count after the combined `STORAGE | COMMAND` barrier, same correctness). Logic unchanged. The dependent `vis` readback at `:2531-2534` is conditional on §7 OQ-4. |
| Teardown: `glDeleteProgram(g_solidCmdPatchProgram)` | grep at write-time | **Delete** in flip commit (along with the decl). |
| Teardown: `glDeleteBuffers(1, &g_solidBucketHeaderSsbo)` | grep at write-time | **Conditional** — §7 OQ-4. |

### Untouched (do not modify)

| Path | Reason |
|---|---|
| `GameOS/gameos/gos_terrain_water_stream.cpp:1371-1391` | Water cmd-patch dispatch; out of scope. |
| `GameOS/gameos/gos_terrain_water_stream.cpp:104, 1196-1201, 1206-1207, 1297, 1378-1391, 1617-1619, 1626-1628` | All water-side `g_cmdPatchProgram` / `g_waterBucketHeaderSsbo` sites. |
| `shaders/gpu_driven_water.comp:256` | Water primary `atomicAdd(hdr.visibleCount, 1u)` — water sibling slice retires this; not this plan. |

---

## §3 Execution sequence

Two commits. Mirrors the Fix-B pattern (`005ebc7`): parity-validate behind env var first, flip default and rip out the legacy path second. Soak window between commits per `track_b_widen_static_prop_registry.md` precedent (7 days minimum).

### Commit 1: Direct-atomicAdd path armed behind `MC2_SOLID_CMD_PATCH_RETIRE=1`

**Adds:**
- New SSBO binding in `gpu_driven_terrain_solid.comp` for `DrawArraysIndirectCommand cmds[]` (binding 8 proposed).
- Lockstep `struct DrawArraysIndirectCommand` in the primary compute shader matching the existing declaration in `gpu_driven_cmd_patch.comp:47-52`.
- Env-gated branch in the primary compute: when env is set, `atomicAdd(cmds[0].count, 6u)` runs alongside `atomicAdd(hdr.visibleCount, 1u)`. Both run; no field skipped. The cmd-patch dispatch still fires unconditionally to keep visibleCount→cmd.count translation authoritative under env=0.
- Pre-init of `g_indirectCmdBuffer` at allocation time (`gos_terrain_indirect.cpp:1513-1521`) with `{count=0, instanceCount=1, first=0, baseInstance=0}`. (No-op behavioral change under env=0 because cmd-patch overwrites all four fields every frame; sets up env=1 correctness.)

**Validates (parity probe):**
- A new MC2_SOLID_CMD_PATCH_PARITY probe reads `cmds[0].count` after the surviving barrier (`gos_terrain_indirect.cpp:2518-2548` extended). Under env=0, expected value: `min(visibleCount, kMaxThinRecords) * 6` (cmd-patch path). Under env=1, expected value: same, derived from primary's direct atomicAdd.
- Byte-for-byte comparison: dual-pass parity. Run primary with BOTH env=0 and env=1 simulated (the cmd-patch dispatch still runs; we now ALSO read `cmds[0].count` after primary dispatch but BEFORE cmd-patch dispatch — if those values match the post-cmd-patch value, the direct path is correct).
- Probe emits `[CMD_PATCH_PARITY v1] event=match` per frame for 600 frames, `event=mismatch frame=N pre=X post=Y` on any divergence. Tier1 must show zero mismatches across all 5 missions before flip.

**Verification:** tier1 smoke `--tier tier1 --duration 30 --kill-existing` (no `--with-menu-canary`). Pass = zero mismatches. Visual smoke must also pass (no triangles, no missing geometry).

**Rollback:** `unset MC2_SOLID_CMD_PATCH_RETIRE` (env defaults to off; commit is additive only).

### Commit 2: Default-on flip + cmd-patch dispatch deletion

**Precondition:** commit 1 has soaked 7 days with zero parity mismatches across user-driven smoke at `mc2_01`, `mc2_10`, `mc2_17` minimum (substrate-heavy `mc2_10` is the canary — see `memory/track_c_substrate_regression.md`).

**Deletes:**
- `gos_terrain_indirect.cpp:2492-2504` (DISPATCH 2 block + intermediate barrier).
- `gos_terrain_indirect.cpp:2050-2068` (cmd-patch program compile + uniform caches).
- `gos_terrain_indirect.cpp:1435` (`g_solidCmdPatchProgram` decl) + teardown site.
- Env-gate wrapping the atomicAdd in `gpu_driven_terrain_solid.comp` (path becomes unconditional).
- The pre-cmd-patch parity probe added in commit 1; the post-barrier `MC2_RING_TRACE` probe stays (its read site moves up since cmd-patch is gone).

**Demotions behind `MC2_BUCKET_HEADER_TRACE=1` (OQ-4 RESOLVED — option (c)):**
- `g_solidBucketHeaderSsbo` decl, alloc, binding, teardown, per-frame clear — all gated.
- `shaders/gpu_driven_terrain_solid.comp:239,342` (`pad1_` / `pad2_` writes) — gated.
- `gos_terrain_indirect.cpp:2204-2244` (probe 4/5 CPU readback of header) — gated.

**Per C2: per-frame init strategy for `g_indirectCmdBuffer`:**
- `instanceCount=1, first=0, baseInstance=0` set ONCE at allocation (`:1513-1521`).
- Per-frame `glClearBufferSubData(GL_DRAW_INDIRECT_BUFFER, count_offset, 4, &zero)` of the 4-byte count field before primary dispatch.

**Per C3: overflow-clamp ordering in primary compute:**
- Move bound check (`if (outSlot >= u_maxThinRecords) return;`) BEFORE the `atomicAdd(cmds[bucket].count, 6u)` so the indirect-cmd count never overshoots when the thin ring overflows.

**Verification:** tier1 smoke + 7-day soak + Tracy capture before/after on wolfman zoom. Expected savings per `docs/render-perf-snapshot.md:30`: `~80-170 µs` recoverable from `Terrain::SolidComputeDispatch` (CPU submission). Validate by Tracy capture — the `Terrain::SolidComputeDispatch` zone should drop by the measured amount; no new mismatch counters; no visual regression.

**Memory updates:**
- Update `memory/ring_slot_state_must_travel_with_slot.md` if cmd-patch retirement supersedes any of its claims about the second dispatch.
- New memory file: `memory/cmd_patch_dispatch_retired.md` documenting the pattern (atomicAdd directly into indirect-cmd buffer) so the water sibling slice can cite it.
- Update `docs/render-perf-snapshot.md` row 30 + row 47 to reflect SHIPPED state.

---

## §4 Verification gates

### Per-commit gate (both commits)

```
py -3 scripts/run_smoke.py --tier tier1 --duration 30 --kill-existing
```

Per `memory/feedback_smoke_gate_no_menu_canary.md`: NO `--with-menu-canary`. All 5 tier1 missions must PASS.

### Parity gate (commit 1)

The critical correctness invariant: **the byte contents of `g_indirectCmdBuffer[0..16]` after the compute pipeline must be identical between the cmd-patch path and the direct-atomicAdd path.**

**Probe scaffolding** (added in commit 1, removed in commit 2):

1. After primary dispatch + intra-pipeline barrier, BEFORE cmd-patch dispatch: read `cmds[0]` via `glGetBufferSubData(GL_DRAW_INDIRECT_BUFFER, 0, 16, buf)`. Stash as `pre_patch`.
2. After cmd-patch dispatch + final barrier (current `MC2_RING_TRACE` site): read `cmds[0]` again. Stash as `post_patch`.
3. Compare: under MC2_SOLID_CMD_PATCH_RETIRE=1, `pre_patch.count == post_patch.count` must hold (primary already wrote final count). Under env=0, `pre_patch.count == 0` (only cmd-patch writes it). The probe emits one line per frame for 600 frames, then summary.
4. Tier1 across mc2_01, mc2_03, mc2_10 (substrate-heavy canary), mc2_17 (water-heavy — water cmd-patch unaffected, but co-residence test), mc2_24. Zero mismatches required.

**Note on readback cost:** the parity probe itself adds two 16-byte `glGetBufferSubData` reads per frame. Per `memory/substrate_coalesce_sync_point_lesson.md`, this risks implicit sync on AMD. Mitigation: the probe is env-gated (`MC2_CMD_PATCH_PARITY=1`) and the parity comparison only runs during the soak window; flip commit removes it.

### Tracy gate (commit 2)

User-driven Tracy capture at wolfman zoom on `mc2_10` (substrate-on baseline) before commit 2 and after. The `Terrain::SolidComputeDispatch` Tracy zone should drop by `~80-170 µs` per the perf snapshot's claim. Smoke FPS not a valid signal here (see `docs/render-perf-snapshot.md:16`).

### Cull-cascade safety (out of scope but check)

This slice does not touch `inView` / `canBeSeen` / `objBlockInfo` / `objVertexActive`. Per `memory/cull_gates_are_load_bearing.md` it shouldn't be a risk class, but the substrate canary mission (`mc2_10`) is the integration check — if substrate-on rendering breaks after this slice, that's a sign the indirect-cmd ordering interacts with the static-prop submission path.

---

## §5 Rollback

### Commit 1 rollback

Single command: `unset MC2_SOLID_CMD_PATCH_RETIRE`. The branch is env-gated; default-off. No source-code revert needed for commit 1 in isolation.

### Commit 2 rollback (post-flip)

Single-commit `git revert`. The commit deletes the cmd-patch program compile, the dispatch block, and the bucket-header SSBO conditionally. All deletions live in one commit so revert is mechanical.

**Soak-window rollback:** during the 7-day soak between commit 1 and commit 2, any visual regression triggers immediate `unset MC2_SOLID_CMD_PATCH_RETIRE` and bug-tracker entry. Do not proceed to commit 2 until the regression is reproduced, isolated, and either fixed in commit 1 amendment or escalated.

**Hard rollback path (post-soak, post-flip, regression discovered in production):** the cmd-patch shader binary stays on disk (water path keeps it). Resurrecting the SOLID consumer is mechanical: re-add the program handle, re-add the dispatch block, re-add the bucket-header alloc + binding, re-add the per-frame clear. The shader source is preserved through the entire arc.

---

## Related future work (NEW-LOW-1 cross-plan tracking)

The water fast-path at `GameOS/gameos/gos_terrain_water_stream.cpp:1372` carries the **identical** barrier-must-include-`GL_COMMAND_BARRIER_BIT` issue documented for the SOLID slice (§"Load-bearing constraints" — "Water-stream path needs the identical barrier change"). Today the water primary dispatch is followed by `glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT)` alone; when the water cmd-patch sibling slice retires (no plan doc yet), that barrier MUST become `GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT` for the same driver-dependent correctness reason (NVIDIA tolerant, AMD failing).

**Tracking status:**
- No plan document yet for water cmd-patch retirement; the SOLID slice ships first and soaks for 7 days minimum before water sibling planning begins. Per VPL retirement re-review NEW-LOW-1, this is a LOW-severity cross-plan tracking item — captured here so it does not slip when water sibling planning starts.
- **Load-bearing constraint on water-stream changes (any cadence):** any perf, correctness, or barrier-sequencing change touching `gos_terrain_water_stream.cpp` around `:1371-1391` MUST include the `GL_COMMAND_BARRIER_BIT` upgrade in the same commit, even if the change is not the cmd-patch retirement itself. This is the only safe way to prevent a latent AMD-driver-dependent corruption window from being introduced by an unrelated water-side edit.
- **Cadence:** unspecified — the SOLID slice ships, soaks; if AMD-driver behavior on the SOLID barrier change validates the concern, water sibling planning is upgraded in priority; if the SOLID change shows no AMD-specific issue, water sibling can be folded into the water cmd-patch retirement slice's broader scope without an emergency sibling fix.

## §6 Cross-references

- **Sibling plan (this slice is step 2 of):** `docs/superpowers/plans/2026-05-14-vertex-project-loop-retirement.md`
- **Perf claim source:** `docs/render-perf-snapshot.md:30, 47`
- **Sync-stall lesson (philosophy):** `memory/substrate_coalesce_sync_point_lesson.md` — "do counter work on GPU, never read back" applies directly.
- **Ring-slot state pattern:** `memory/ring_slot_state_must_travel_with_slot.md` — `cmd[0].count` is the surviving ring-slot state after retirement; primary dispatch writes it directly.
- **Struct lockstep rule:** `memory/cpp_glsl_ubo_struct_lockstep.md` — `DrawArraysIndirectCommand` 16 B std430 lockstep between C++ host allocation and GLSL declaration.
- **No env-gated dead code:** `memory/mc_texture_manager_dual_queue_legacy_retirement_debt.md` — env gate exists for soak only, dies in flip commit.
- **Soak precedent:** `memory/track_b_widen_static_prop_registry.md` — 7-day minimum between retirement commits.
- **Substrate canary:** `memory/track_c_substrate_regression.md` — `mc2_10` substrate-on is the integration check for indirect-cmd-related slices.
- **Advisor briefs (re-read before commit):** `.claude/agents/mc2-terrain-indirect-expert.md` (especially `<known_pitfalls>` line 92 "Struct-lockstep silent break" and 94 "Ring-slot fence missed"); `.claude/agents/mc2-shader-expert.md` (AMD atomicAdd rules); `.claude/agents/mc2-cpu-gpu-offload-expert.md` (counter-work-on-GPU methodology).

---

## §7 Open questions for adversarial review

### OQ-1: Does removing the intermediate `glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT)` change correctness?

Today's sequence at `gos_terrain_indirect.cpp:2488-2504`:

```
glDispatchCompute(groups,1,1);             // primary (writes visibleCount, thin records)
glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);   // <-- removed by retirement
... bind bucket header + indirect cmd ...
glDispatchCompute(1,1,1);                  // cmd-patch (reads visibleCount, writes cmd.count)
glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);  // survives
```

After retirement:

```
glDispatchCompute(groups,1,1);             // primary (writes thin records AND cmd.count directly)
glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);   // single barrier, both bits
```

**Review question:** does the surviving combined barrier correctly fence (a) thin-record SSBO writes against the subsequent VS read at draw time, and (b) `cmd[0].count` writes against the subsequent `glDrawElementsIndirect` command read? Per the GL spec, `GL_SHADER_STORAGE_BARRIER_BIT` covers (a) and `GL_COMMAND_BARRIER_BIT` covers (b). Both bits already set today. **Suspected answer: yes, correct, no change.** Confirm.

### OQ-2: Does the AMD driver have known issues with `atomicAdd` on a `DRAW_INDIRECT_BUFFER`-backed SSBO?

`g_indirectCmdBuffer` is created with `glBindBuffer(GL_DRAW_INDIRECT_BUFFER, ...)` initially (`gos_terrain_indirect.cpp:1515`). After retirement it is also bound as an SSBO (binding 8 in primary compute) and written via `atomicAdd`. Per GL spec, a buffer object can be bound to multiple targets and SSBO atomics on it are well-defined. But the AMD shader advisor (`.claude/agents/mc2-shader-expert.md`) may know of driver-specific behavior where DRAW_INDIRECT-flagged buffers have stricter coherence requirements.

**Review question:** does the AMD shader advisor or the AMD driver rules doc (`docs/amd-driver-rules.md`) flag any restriction on SSBO atomics targeting a buffer also used as `GL_DRAW_INDIRECT_BUFFER`? **Reviewer must spawn `mc2-shader-expert` agent and ask explicitly.**

### OQ-3: AMD atomicAdd contention on a single 32-bit slot at `kMaxThinRecords` workgroup invocations

Today: `atomicAdd(hdr.visibleCount, 1u)` per admitted record. Up to `kMaxThinRecords` (grep at write-time — current value at `gos_terrain_indirect.cpp` `g_locMTR` upload). Same contention shape applies to the proposed `atomicAdd(cmds[0].count, 6u)`. Replacing one atomic with another on a different SSBO is a wash for contention pressure.

**But:** if reviewer flags this as a hotspot, consider workgroup-local reduction: each workgroup accumulates a local count, one invocation per workgroup does the final atomicAdd. Pattern is well-known but adds shader complexity. **Suspected answer: not needed at current `kMaxThinRecords`, but flagged for review.**

### OQ-4: Should `g_solidBucketHeaderSsbo` be retired wholesale, or retained for `pad1_/pad2_` diagnostic counters?

**RESOLVED (v2, amendment C1):** option **(c) demote behind env-gate**, env-var name **`MC2_BUCKET_HEADER_TRACE`**, default-off.

**Scope of the demotion:**
- Wrap `g_solidBucketHeaderSsbo` allocation (`gos_terrain_indirect.cpp:2070-2075`), binding (`:2442`), per-frame clear (`:2421-2428`), and teardown in `if (g_envBucketHeaderTrace) { ... }`.
- Wrap the two diagnostic atomicAdd sites at `shaders/gpu_driven_terrain_solid.comp:239,342` (`pad1_/pad2_` counter writes) behind a compile-time or uniform-driven branch keyed off `MC2_BUCKET_HEADER_TRACE`.
- Wrap the `glGetBufferSubData` probe block at `gos_terrain_indirect.cpp:2204-2244` in the same env-gate.

**Net cost when off:** zero allocation, zero per-frame work, zero binding. Effectively retired.

**Net cost when on:** today's diagnostic capability preserved for regression probing.

**Commit 2 sequence (final):**
1. Delete cmd-patch shader (`shaders/gpu_driven_cmd_patch.comp`) — water path keeps it; deletion deferred to water sibling slice. The SOLID consumer is the only deletion in this slice.
2. Delete the second `glDispatchCompute` at `gos_terrain_indirect.cpp:2492-2502` plus the intermediate `GL_SHADER_STORAGE_BARRIER_BIT` at `:2490`.
3. Demote `g_solidBucketHeaderSsbo` behind `MC2_BUCKET_HEADER_TRACE=1` (alloc, bind, clear, readback, atomicAdd writes) per the C1 scope above.
4. Move COMMAND barrier bit from the post-cmd-patch site to the post-primary site (the survivor at `:2504` is repointed up by ~25 lines).
5. Move overflow-clamp atomicAdd ordering per C3 (bound check before atomicAdd).
6. Initialize `instanceCount=1, first=0, baseInstance=0` once at allocation per C2 (a).

### OQ-5: Are there ANY readers of `g_solidBucketHeaderSsbo.visibleCount` besides cmd-patch and the probe?

Grep walk done 2026-05-14:
- Compute writer: `shaders/gpu_driven_terrain_solid.comp:349`
- Compute reader (cmd-patch): `shaders/gpu_driven_cmd_patch.comp:73`
- CPU reader (probe 4/5): `gos_terrain_indirect.cpp:2204-2244`
- CPU reader (MC2_RING_TRACE expected-value calc): `gos_terrain_indirect.cpp:2531-2534`

**No other readers found.** Per `memory/feedback_data_flow_audit_asymmetry.md`, this is a "negative claim" that requires opposite-direction grep: I greppped `g_solidBucketHeaderSsbo`, `visibleCount`, `hdr.visibleCount` across the worktree. Reviewer must independently grep before commit 2 (the flip commit). Any additional reader = blocker.

### OQ-6: The water path shares the cmd-patch shader binary — does retiring the SOLID consumer affect water's program-cache?

`gpu_driven_cmd_patch.comp` is compiled twice: once as `g_cmdPatchProgram` (water, `gos_terrain_water_stream.cpp:1196-1197`) and once as `g_solidCmdPatchProgram` (solid, `gos_terrain_indirect.cpp:2051-2052`). The two compiled programs are independent GL objects. Deleting the SOLID program handle does not affect the water handle.

**Suspected answer: no interaction.** Confirm via post-commit-2 smoke at `mc2_17` (water-heavy mission).

### OQ-7: Net barrier accounting — is the saved barrier the savings, or is the dispatch CPU-submit cost the savings?

The perf snapshot claim is "~80-170 µs recoverable" on the CPU side (`Terrain::SolidComputeDispatch` CPU submission zone). Two sources of cost go away:

1. **CPU side:** one `glUseProgram` + uniform setup (already cached, near-zero) + one `glDispatchCompute(1,1,1)` submission + one `glMemoryBarrier` API call.
2. **GPU side:** the actual cmd-patch shader execution (single invocation, trivial work) + the implicit barrier work.

The claimed savings are CPU-side. GPU savings are likely small but unmeasured. Adversarial review should not invent a tighter number; the snapshot range is the right one to cite.

**Suspected answer: snapshot's range is correct; ratify, do not refine.**

---

## Adversarial review trigger

Per worktree CLAUDE.md "Review Discipline," this plan hits one high-stakes trigger:

1. **Architectural endpoint** — removing the second compute dispatch collapses the GPU producer-consumer pair into a single producer, shifting the indirect-cmd-buffer-write authority from cmd-patch to the primary cull/pack dispatch.

Adversarial review is REQUIRED before commit 1 implementation. Dispatch with the verbatim incantation "use the adversarial-plan-review skill" per the worktree rule.

Expected findings density: 1-2 CRITICAL (likely AMD-driver atomicAdd-on-DRAW_INDIRECT_BUFFER question, OQ-4 scope decision), 2-4 MAJOR (probe scaffolding, struct lockstep, barrier accounting), 3-5 MINOR. Plan v2 lands after review. Commit 1 does NOT start until plan v2 ships.
