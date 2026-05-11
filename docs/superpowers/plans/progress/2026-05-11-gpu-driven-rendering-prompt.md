# Renderer Modernization — Phase C: GPU-Driven Indirect Command Generation

> **Model: opus.** Reason: moves indirect command generation from CPU to GPU compute shader. Architectural decision on the CPU↔GPU command pipeline boundary that AAA renderers center on (UE5 Nanite, Frostbite, DOOM 2016+). Sets the pattern for how per-frame draw lists are built in this codebase going forward. Spec + adversarial review under opus; sonnet for per-bucket mechanical implementation.

> **Required sub-skills:** `superpowers:using-git-worktrees`, `superpowers:writing-plans`, `adversarial-plan-review` (this slice qualifies — moves load-bearing draw list construction to GPU compute, introduces new compute shaders + SSBO schemas, retires the per-frame CPU per-bucket indirect-cmd build).

> **Phase A (bindless textures) is DEFERRED** (see `2026-05-11-bindless-textures-prompt.md` for the rationale: driver support too uneven for portable distribution). Phase C's compute shaders write `uint32` texture slot indices into per-bucket indirect-cmd metadata; the CPU MDI loop binds the bucket's texture once via `glBindTexture(slot)` before issuing `glMultiDrawArraysIndirect`. That's one bind per pipeline-bucket (not one per draw), so the cost is small but non-zero. The main Phase C win — eliminating per-frame CPU cmd-build work — is independent of bindless.
>
> **Sibling slice** (independent, can run in parallel): Phase B at `2026-05-11-pre-bake-terrain-renderer-prompt.md`. Phase B = static bake of map-stable data; Phase C = dynamic per-frame indirect-cmd generation. Both touch the terrain indirect-cmd SSBO — coordination point: Phase B writes templates at mission load, Phase C compute shader fills in per-frame deltas (visibility + cull) using bake as input. If Phase B doesn't ship, Phase C reads the per-frame CPU-built recipe data (slower but works).

---

## Worktree

Create a fresh worktree off `claude/nifty-mendeleev` HEAD (currently `93d3cbd` — the post-Phase-1-merge state).

```
.claude/worktrees/gpu-driven-rendering/  → branch claude/gpu-driven-rendering
```

## Roadmap reference

Phase C of the renderer modernization arc. Originally scoped as the third of three slices (bindless → pre-bake terrain || GPU-driven rendering); bindless is deferred so the arc is now just (pre-bake terrain || GPU-driven rendering).

Parent arc: `docs/superpowers/cpu-to-gpu-offload-orchestrator.md`.

This is the architectural endpoint of the Track C compute cull work — Track C already moved VISIBILITY decisions to GPU compute; Phase C completes the picture by moving the INDIRECT-COMMAND WRITES to GPU compute too. The CPU's role reduces to: kick off the compute pass, then kick off the MDI consuming its output.

Phase 1 (terrain lighting GPU compute) is the precedent for: GPU compute pattern, parity gate, SSBO ring buffers, perf-gate empirical anchoring.

## Goal

Replace per-frame CPU construction of indirect commands with GPU compute shader generation. The compute shader reads visibility + per-element state from existing SSBOs (Track C outputs, Phase B static bake, or per-frame CPU input as fallback) and writes indirect-command SSBO that `glMultiDrawArraysIndirect` consumes.

Expected cut: ~1.5-3 ms/frame in the combined `render terrain` + `render textureManager` + `render objects` + `render water` zones — wherever CPU currently builds indirect cmds. Win does NOT depend on camera stationarity (works under RTS panning).

The compounding architectural win: combined with Phase B (static bake), the CPU draw pipeline becomes: "submit one compute dispatch, bind per-bucket texture, submit one MDI per pipeline, done." Total per-frame draw prep on CPU drops toward sub-millisecond. (Phase A would have eliminated the per-bucket texture bind too — deferred for portability reasons, see Phase A prompt.)

## What to read first (in order)

1. **`memory/track_c_compute_cull.md`** — existing GPU compute cull pattern. Phase C's compute shaders mirror this shape (single-context GL, fence sync, persistent-mapped SSBO).
2. **`memory/water_ssbo_pattern.md`** — "CPU thin record + GPU consumes" precedent. Phase C inverts this: GPU compute WRITES the records, CPU just dispatches consumers.
3. **`memory/indirect_terrain_solid_endpoint.md`** — PR1 indirect-terrain implementation. CPU currently builds the indirect cmd SSBO; Phase C moves that build to compute shader.
4. **`memory/substrate_coalesce_sync_point_lesson.md`** — sync stall pattern to AVOID. Phase C's GPU-CPU coordination must use fences not readback.
5. **`GameOS/gameos/gpu_cull_compute.cpp`** — compute shader compile + dispatch pattern. Phase C reuses.
6. **`GameOS/gameos/gpu_cull_readback.cpp`** — 3-slot non-blocking ring pattern. Phase C may use for any CPU-side telemetry from the GPU pass.
7. **`memory/mc2_texture_handle_is_live.md`** — the existing `gosTextureHandle` uint32 slot-index API. Phase C compute shaders write slot indices into per-bucket indirect-cmd metadata; CPU MDI loop binds the bucket's texture before issuing the multidraw.
8. **Phase B's static SSBO struct** (if Phase B has shipped at execution time) — `terrain_prebake.md`. Phase C compute reads as input.
9. **`code/objmgr.cpp:1939-2050`** — per-object update loop. Phase C considers whether to move parts of this loop's emit work (cull-record writes) to GPU side.
10. **Phase 1 design doc**: `docs/superpowers/specs/2026-05-10-quadsetuptextures-gpu-compute-port-design.md` — pattern reference for compute-shader design discipline.

## Scope

**In:**
- One compute shader per draw bucket (terrain SOLID, terrain detail/overlay, terrain mines, water, static props, etc.). Each compute shader:
  - Reads per-element state (from Phase B's static SSBO if available, else from per-frame CPU input).
  - Reads visibility (from Track C compute cull output).
  - Writes one indirect-command struct (`DrawArraysIndirectCommand` or `DrawElementsIndirectCommand`) per element that passes cull.
  - Increments per-bucket draw count via `atomicAdd`.
- Per-bucket indirect-cmd SSBO sized to worst-case (`realVerticesMapSide²` for terrain, `maxStaticProps` for props, etc.).
- CPU side per-frame: `glDispatchCompute(cmd_gen_shader)` + `glMemoryBarrier(GL_COMMAND_BARRIER_BIT)` + `glMultiDrawArraysIndirect`. No per-element CPU work.
- Killswitch `MC2_GPU_DRIVEN=0` opts out per-bucket (falls back to per-bucket CPU cmd build); default-on after soak.
- Parity gate `MC2_GPU_DRIVEN_PARITY=1` — runs both paths, comparator checks indirect-cmd SSBO byte-equality per frame.
- Cost-split bucket retirement for whatever zones the win lives in (e.g. `render textureManager` cmd-build portion).

**Out:**
- Bindless textures (Phase A; deferred indefinitely — driver support concerns).
- Pre-bake terrain (Phase B; can ship before, after, or in parallel).
- Per-vertex skinning compute (already done on gpu-mech branch).
- Particle simulation on GPU (could be a follow-on slice).
- Vulkan/D3D12 secondary command buffers (out of GL 4.3 arc).

## Plan shape (suggested — spec session owns final)

1. Stage 0: spec + design doc. Enumerate every existing CPU-side per-frame indirect-cmd build site. Decide which buckets ship in Phase C and which defer. Define compute shader inputs (Phase B static SSBO if available, else CPU per-frame ring). Adversarial review gate.
2. Stage 1: ship the simplest bucket first (suggest: water — already has full thin-record pipeline; just move the per-frame thin-record CPU build into a compute shader). Parity gate proves the pattern.
3. Stage 2: extend to terrain SOLID bucket (largest single CPU saver).
4. Stage 3: extend to remaining terrain buckets (detail, overlay, mines).
5. Stage 4: extend to static-prop bucket (coordination with gpu-mech branch).
6. Stage 5: soak window (7-day per Track B precedent).
7. Stage 6: default-on flip per bucket (rolling — buckets can flip independently as each one passes parity + soak).
8. Stage 7: demote per-bucket CPU cmd-build paths (gated off, not deleted).

## Parity / Soak gates

- Per-frame indirect-cmd SSBO byte-equal under `MC2_GPU_DRIVEN_PARITY=1` per bucket, across tier1 5/5 + mc2_10 wolfman + mc2_01 water-heavy.
- Tracy: target zones drop measurably (specific zones depend on which buckets ship; track `render terrain` + `render water` + `render objects` deltas).
- Sum of per-bucket `gpu_driven_dispatch_us_per_frame` ≤ 500 µs (compute dispatch + barrier overhead budget).
- Tier1 5/5 PASS both env states + visual identical via screenshot diff.
- No new GL_INVALID_* lines.
- No sync stalls (`glClientWaitSync` timeout=0 always on hot path per `substrate_coalesce_sync_point_lesson.md`).

## Killswitch + env vars

- `MC2_GPU_DRIVEN=0` — force legacy per-bucket CPU cmd build. Default-on after Stage 6 per-bucket flips.
- `MC2_GPU_DRIVEN_<BUCKET>=0` — per-bucket killswitch (e.g. `MC2_GPU_DRIVEN_WATER=0` keeps water on CPU even when overall enabled). Allows bisection by bucket.
- `MC2_GPU_DRIVEN_PARITY=1` — dual-run + per-bucket comparator. Default off.
- `MC2_GPU_DRIVEN_TRACE=1` — per-bucket dispatch counters + draw count diagnostic prints. Default off.

## Load-bearing constraints (per adversarial-plan-review skill step 6)

- **GL 4.3 single-context constraint** (`2026-05-08-job-system-parallel-for-scope.md` Q5): all GL calls (compute dispatch + MDI + barriers) on render thread.
- **`memory/substrate_coalesce_sync_point_lesson.md`**: no `glGetBufferSubData` / `glMapBuffer(GL_MAP_READ_BIT)` on hot path. Use fence-sync ring (gpu_cull_readback.cpp pattern) if any CPU-side telemetry needed.
- **`memory/cpp_glsl_ubo_struct_lockstep.md`**: indirect-cmd struct + per-element state structs in shared headers.
- **`memory/mc2_texture_handle_is_live.md`**: compute shader writes `uint32` texture slot indices (NOT bindless `uvec2`). CPU MDI loop binds the bucket's texture once via `glBindTexture(slot)` before each multidraw call. Bindless deferred (see Phase A prompt).
- **Atomicity**: `atomicAdd` on per-bucket draw count; correct std430 alignment on the counter slot.
- **`memory/track_c_compute_cull.md`** lifecycle gates: Phase C must respect the same arming/disarming gate pattern (e.g. `IsFrameArmed()`-style preflight) so dispatches only fire when input data is ready.
- **`GL_COMMAND_BARRIER_BIT` ordering**: after compute writes indirect-cmd SSBO, the barrier MUST sync compute→MDI. Wrong barrier flag = MDI reads stale data.

## Adversarial review gate (mandatory)

Run `adversarial-plan-review` skill against Stage 0 design doc before code lands. Triggers:
- Multiple new compute shaders + SSBO schemas.
- Retires CPU per-frame cmd-build infrastructure across multiple draw paths.
- Cross-cutting with Phase B (static SSBO); Phase A bindless deferred.
- Sync-pattern hazards (the substrate-coalesce lesson is the precedent that almost caught us before).
- Perf gate ≥1.5 ms.

Dispatch prompt MUST include "use the adversarial-plan-review skill in `.claude/skills/`" verbatim.

## Exit criteria

- All Parity/Soak gates pass for each bucket that ships in this slice.
- `MC2_GPU_DRIVEN=0` reproduces pre-slice behavior bit-for-bit per-bucket.
- Memory file `gpu_driven_indirect_cmds.md` captures compute shader pattern + per-bucket invalidation contract + sync-stall avoidance pattern.
- Phase B (if ships after Phase C) can layer static SSBO under the compute shader's input read cleanly.
- gpu-mech branch's compute pipeline for mechs documented compatibility with Phase C's pattern (separate memory file or design doc section).

## Stop conditions

- Per-bucket parity diff non-zero after 3 iteration rounds → STOP, surface findings (most likely compute-shader math vs CPU math precision drift, or barrier ordering).
- Per-bucket Tracy delta < 200 µs → STOP that bucket, surface to user. Other buckets may still ship.
- Sync stall surfaces in profiling (Tracy GPU timeline shows CPU wait for GPU completion) → STOP, switch to non-blocking ring + skip-frame fallback per `gpu_cull_readback.cpp` precedent.
- Any tier1 mission FAIL under `MC2_GPU_DRIVEN=1` → STOP, revert flipped buckets to parity-only mode, bisect.
- AMD driver compute-dispatch-before-MDI ordering bug surfaces → STOP, surface to user; may need explicit fence between dispatch and draw.

## Why opus

This slice:
- Architectural endpoint of the CPU→GPU draw pipeline arc.
- Sets the pattern for GPU-driven rendering across the codebase (terrain, water, objects).
- Sync-pattern hazards have precedent (substrate-coalesce stall) — careful design needed.
- Per-bucket roll-out coordination across multiple shipping stages.

Opus for spec + adversarial review + per-bucket roll-out strategy. Sonnet for individual bucket implementation in Stages 1-4.
