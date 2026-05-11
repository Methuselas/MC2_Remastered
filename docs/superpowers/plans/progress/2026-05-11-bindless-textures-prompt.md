# Renderer Modernization — Phase A: Bindless Textures

> **Model: opus.** Reason: foundational ABI change that touches every draw path (terrain, water, static props, mechs, particles, UI). Architectural decisions made here ripple into Phase B (pre-bake terrain) and Phase C (GPU-driven rendering). The texture-handle representation locked in by Phase A becomes the lingua franca for both follow-ons + the gpu-mech branch's eventual merge. Opus for spec + adversarial review; sonnet for mechanical execution stages after the design ships.

> **Required sub-skills:** `superpowers:using-git-worktrees`, `superpowers:writing-plans`, `adversarial-plan-review` (this slice qualifies — touches every draw path, introduces new SSBO schemas, retires the per-draw `glBindTexture` path that every shader currently depends on).

> **Sibling slices** (run in parallel AFTER this ships):
> - Phase B: `2026-05-11-pre-bake-terrain-renderer-prompt.md` — consumes bindless handles in static SSBO.
> - Phase C: `2026-05-11-gpu-driven-rendering-prompt.md` — consumes bindless handles in GPU-compute-generated indirect cmds.
>
> **Cross-branch coordination:** The gpu-mech branch ships GPU skinning (per user, complete). The bindless-handle ABI must be compatible with whatever the gpu-mech branch expects on merge. Stage 0 of this slice MUST grep the gpu-mech branch and document the merge contract.

---

## Worktree

Create a fresh worktree off `claude/nifty-mendeleev` (or whatever the current main dev branch is at execution time — verify at write-time). Do NOT branch off `claude/parallel-amdahl` — that's the Phase 1 terrain-lighting worktree.

```
.claude/worktrees/bindless-textures/  → branch claude/bindless-textures
```

Use the `using-git-worktrees` skill.

## Roadmap reference

This slice is **Phase A** of the renderer modernization arc (bindless → pre-bake terrain || GPU-driven rendering). Strategic context lives in this worktree at `docs/superpowers/plans/progress/2026-05-11-renderer-modernization-tri-slice-overview.md` (executor: if that file doesn't exist, this prompt + the two siblings ARE the overview — write a short overview if you want, but it's not load-bearing).

Parent CPU→GPU offload arc: `docs/superpowers/cpu-to-gpu-offload-orchestrator.md`.

Phase 1 (GPU terrain lighting) shipped on `claude/parallel-amdahl` — that's the precedent for SSBO pattern + parity gates + cost-split telemetry + staged default-on flip.

## Goal

Replace per-draw `glBindTexture` with bindless texture handles (`ARB_bindless_texture`) across all draw paths. Eliminate the hundreds of CPU-side bind calls per frame and the driver-side state validation each one triggers. Establish the bindless-handle ABI that Phase B and Phase C consume.

Expected cut: ~0.5-1 ms/frame in scattered "render *" zones (textureManager, objects, water, weather). Larger downstream effects when Phase B/C land and can issue draws without CPU intervention for state changes.

## What to read first (in order)

1. **`memory/mc2_texture_handle_is_live.md`** — MC2 already has a handle-not-pointer abstraction. The slot-index pattern is half-way to bindless. Understanding this is mandatory before touching `gosTextureHandle` semantics.
2. **`memory/mc2_argb_packing.md`** — texture sampling conventions (BGRA in memory). Bindless handle struct must preserve this.
3. **`memory/cpp_glsl_ubo_struct_lockstep.md`** — if the bindless handle changes shape, every shader's SSBO struct must update lockstep. This is the load-bearing rule.
4. **`docs/amd-driver-rules.md`** — AMD RX 7900 XTX (target hardware) ARB_bindless_texture quirks. Verify support, residency requirements, lifetime rules.
5. **`GameOS/gameos/gpu_cull_compute.cpp`** + **`GameOS/gameos/gpu_cull_readback.cpp`** — the modern persistent-mapped + fence-sync pattern from Track C. Bindless handle table lives in a similar SSBO pattern.
6. **Phase 1's design doc**: `docs/superpowers/specs/2026-05-10-quadsetuptextures-gpu-compute-port-design.md` — pattern reference for SSBO struct layout discipline, parity gate, soak window.
7. **gpu-mech branch survey** (cross-branch coordination): grep the gpu-mech branch (`claude/gpu-mech-batcher` or current name — verify) for `glBindTexture`, `gosTextureHandle`, `ARB_bindless`. Document what the gpu-mech branch expects for texture handle ABI so this slice ships a compatible representation.

## Scope

**In:**
- Vendor extension probing for `ARB_bindless_texture` (verify on AMD RDNA3 target).
- New `gos_bindless_texture.{h,cpp}` module that owns the bindless handle table (mapped from existing slot indices).
- All `glBindTexture` callsites in `GameOS/gameos/` and `mclib/` converted to write a handle into an SSBO instead.
- Shader updates: every fragment shader that currently uses `uniform sampler2D` becomes `sampler2D(handleArray[index])` or pulls handle directly from an SSBO.
- Killswitch `MC2_BINDLESS=0` opts out (falls back to legacy `glBindTexture` path); default-on after soak.
- Parity gate `MC2_BINDLESS_PARITY=1` — dual-run both paths, compare per-draw output via screenshot diff OR per-bucket draw-count + bound-handle checksum.
- Cost-split bucket `bindless_per_frame_ns` to confirm retirement of `glBindTexture` overhead post-flip.
- Bindless-handle struct definition that Phase B and Phase C consume (lockstep header per `cpp_glsl_ubo_struct_lockstep.md`).

**Out:**
- Pre-baked terrain indirect commands — that's Phase B.
- GPU-driven indirect command generation — that's Phase C.
- Per-actor light-data dedup elimination — orthogonal slice.
- gpu-mech branch's draw path — coordination only; that branch owns its own update.
- Vulkan/D3D12 migration — out of arc.

## Plan shape (suggested — spec session owns final)

1. Stage 0: spec + design doc with full file structure, handle struct layout, gpu-mech-branch ABI compatibility audit. Adversarial review gate.
2. Stage 1: vendor extension probe + handle table scaffold (no behavior change). `MC2_BINDLESS_TRACE=1` prints handle table state.
3. Stage 2: convert ONE draw path (suggest: water — smallest surface area, one bucket, separate shader, already on its own SSBO ring per WaterStream pattern). Parity gate on water draw.
4. Stage 3: extend to remaining paths in dependency order — terrain, static props, mechs (or stub for gpu-mech-merge), particles, UI/HUD.
5. Stage 4: soak with `MC2_BINDLESS=1 MC2_BINDLESS_PARITY=1`.
6. Stage 5: default-on flip per Phase-1-Stage-5 precedent.
7. Stage 6: demote parity infrastructure (gated off, not deleted, per Debug Instrumentation Rule).

## Parity / Soak gates

- Per-draw bind-call count comparison: legacy path issues N `glBindTexture` calls per frame; bindless path issues 0 (or just 1-2 for fallback paths). Counter to verify retirement.
- Visual parity: screenshot diff at fixed camera state, byte-equal under `MC2_BINDLESS_PARITY=1`. NOT bit-fuzzy — bindless reads the same texels.
- Tracy zone deltas: `mcTextureManager->update()` should drop a measurable fraction (bind list maintenance retires).
- Tier1 5/5 + Carver5O + Magic + mc2_10 canary all PASS.
- 7-day soak (Track B precedent) under `MC2_BINDLESS=1` in normal gameplay.

## Killswitch + env vars

- `MC2_BINDLESS=0` — force legacy `glBindTexture` path. Default-on after Stage 5 flip.
- `MC2_BINDLESS_PARITY=1` — dual-run mode (both paths execute, comparator checks parity). Default off.
- `MC2_BINDLESS_TRACE=1` — handle table state + per-frame bind-call counter prints. Default off.

## Load-bearing constraints (per adversarial-plan-review skill step 6)

- **`mc2_texture_handle_is_live.md`**: handles mutate per-frame. Resolve at draw time, never cache. Bindless layer must preserve this — the bindless handle is the FINAL value, not the slot-index that gets resolved later. Resolution happens in the handle-table writer, not in the consumer.
- **`mc2_argb_packing.md`**: BGRA memory layout. Bindless sampling produces the same byte order.
- **`cpp_glsl_ubo_struct_lockstep.md`**: bindless-handle struct (likely `uvec2` per ARB_bindless_texture) must be defined in a shared header included by both C++ and GLSL.
- **AMD driver rules**: verify `glMakeTextureHandleResidentARB` lifetime — residency must persist for the frame, may need explicit non-residency on texture deletion.
- **Existing persistent-mapped buffer patterns** (gpu_cull_readback.cpp, etc.): handle table SSBO follows the same pattern — single persistent map, no per-frame reallocation.

## Adversarial review gate (mandatory)

Run `adversarial-plan-review` skill against the Stage 0 design doc before any code lands. This slice qualifies under the skill's Mandatory triggers:
- New SSBO schema (handle table).
- Retires load-bearing CPU API (`glBindTexture` callsites across the codebase).
- Touches every shader that samples textures (~10-20 shaders).
- Cross-branch ABI implication (gpu-mech merge).

Dispatch prompt MUST include "use the adversarial-plan-review skill in `.claude/skills/`" verbatim.

## Exit criteria

- All Parity/Soak gates pass.
- `MC2_BINDLESS=0` killswitch reproduces pre-slice behavior bit-for-bit.
- Bindless-handle struct definition + lockstep header committed; Phase B and Phase C can grep+consume.
- Memory file `bindless_handle_abi.md` captures the canonical handle representation + residency lifetime rules for future slices.
- gpu-mech-branch merge contract documented (memory file or design doc section).

## Stop conditions

- `glMakeTextureHandleResidentARB` returns 0 on AMD RDNA3 → STOP, debug driver state; this is the single-point-of-failure for the whole slice.
- Parity diff non-zero across 3 iteration rounds → STOP, surface findings (most likely sampler-state or residency-lifetime bug).
- Any tier1 mission FAIL under `MC2_BINDLESS=1` → STOP, revert flip to parity-only mode, bisect.
- gpu-mech branch ABI incompatibility discovered → STOP, coordinate with gpu-mech branch owner before locking in handle struct.

## Why opus

This slice:
- Sets the bindless-handle ABI for all downstream renderer work + gpu-mech merge.
- Touches every shader + every draw path (large architectural surface).
- Has cross-branch coordination implications (gpu-mech).
- AMD driver quirks for bindless need careful handling.

Opus for spec + adversarial review + cross-branch survey. Sonnet for mechanical per-path conversion in Stages 2-3.
