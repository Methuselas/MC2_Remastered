# Orchestrator prompt — GLSTATE-GUARD-ADOPTION-1 (paste into fresh opus session)

> Caveman register. Subagent-heavy. Adopt-not-port. Paste body below into new session.

---

You orchestrator. Opus. Subagent-heavy — you delegate recon + edits + verify, you synthesize + commit. Do not do big reads yourself; spawn agents, keep conclusions not file-dumps.

## Context (read first, do not re-derive)
RENDER-BACKEND-SEAMS arc recon DONE. This slice = first IMPLEMENTATION off it. Read these before anything:
- `docs/render-backend-seams/vulkan-readiness-audit-1.md` — the audit. KEY VERDICT: renderer already Vulkan-shaped in many places; dominant gap = **strictness tooling BUILT but NOT ADOPTED**. Cheapest prep = adopt what exists.
- `docs/render-backend-seams/opengl-correctness-ledger-1.md` — campaign ledger (queue clear).
- `GameOS/gameos/gl_state_guard.h` — the RAII guards (`GlScopedCapability`, `GlScopedDepthState`, `GlScopedTextureUnit`, `GlScopedSsboBinding`, `GlScopedClipControl`). Defined + proven. Deployed in ~1 site only. THIS SLICE = wire them everywhere they belong.
- memory: [[render-backend-seams-arc]], `known_issues.md` "GL state / render-state cache (META-FIX DEBT)".

## Ruling (locked — do not relitigate)
This is **GLSTATE-GUARD-ADOPTION-1**. Adopt existing `gl_state_guard` RAII in GPU-direct passes. That all.

### DO
- Wrap GPU-direct passes that mutate global GL state (blend/depth/cull/viewport/sampler/SSBO bind) in the EXISTING `gl_state_guard` RAII scopes.
- One pass family per slice. One commit per family. Smoke each.
- Reuse existing guard types. No new guard type unless a pass needs state no guard covers — and then justify in commit + flag to user first.
- Behavior-neutral. Guard snapshots state on ctor, restores + `gos_InvalidateRenderStateCache()` on dtor. Replaces the hand-rolled save/restore already there. Output identical.

### DO NOT (hard stop — these are NOT this slice)
- No SPIR-V / shader-permutation seam.
- No pipeline-state objects / kill `stateCacheValid_`.
- No RenderPassContract full pass-graph wiring.
- No `gl_utils.cpp` rewrite/delete.
- No GpuBuffer mass adoption.
- No Vulkan abstraction layer / no Vulkan code.
- No `GpuBindingSlots` registry (that next slice, separate).

### Order (least weird → most weird; shadows LAST)
1. post-process (`gos_postprocess.cpp` endScene chain)
2. water fast path (`renderWaterFastPath`)
3. particles / decals (particle bridge, decal/overlay batch draws)
4. static-prop / mech batchers (`gos_static_prop_batcher.cpp`, `gos_mech_batcher.cpp`)
5. shadows LAST (most historical weirdness — clip-control/state repair already landed; do not disturb blind)

## Workflow per pass family (repeat 1→5)
1. **Preflight** (mc2-repo-intel `preflight`, expect branch `claude/nifty-mendeleev`, expect worktree root). branch_ok/root_ok false → STOP, report. Foreign dirty `mclib/mech3d.cpp`+`txmmgr.h` = NOT yours, never touch/stage.
2. **Recon subagent** (1 per family): map every raw GL state mutation in that family's passes + the hand-rolled save/restore it already does + which `gl_state_guard` type covers each. Output: per-site table (file:line, state touched, current save/restore, guard to use, any state NO guard covers). NO edits.
3. **Edit** (you or 1 subagent): replace hand-rolled save/restore with the RAII guard at each site. Keep `gos_InvalidateRenderStateCache()` semantics. Minimal diff.
4. **Build**: `/mc2-build` (VS cmake path, `--build build64 --config RelWithDebInfo --target mc2`). Must link.
5. **Verify subagent**: deploy v0.4c (`build-dir build64/RelWithDebInfo`; if locked by transient mc2.exe WAIT, never `--kill-existing`) + smoke tier1 canonical (5/5) + grep logs for GL errors / state-leak symptoms (`MC2_GL_DEBUG_FATAL=1` clean run = positive proof no new HIGH GL msg). Behavior-neutral = no visual delta. Report.
6. **Commit** that family alone (stage only its files). Update ledger row. Next family.

## Acceptance (per family + overall)
- build links · tier1 5/5 · `MC2_GL_DEBUG_FATAL=1` no new HIGH GL msg · no visual delta · hand-rolled save/restore replaced by RAII · `gos_InvalidateRenderStateCache` call-list shrinks (the gap footprint).
- Done when all 5 families wrapped + the ~8-site `gos_InvalidateRenderStateCache` manual list collapsed to guard-driven.

## After this slice (queue, do NOT start now)
- **GPU-BINDING-SLOTS-REGISTRY-1** — one C++ enum source-of-truth for binding slots + emit/check GLSL binding constants + per-pass occupancy table. NO descriptor abstraction. Highest-ROI Vk-prep after guards. Design already in `gpu-buffer-wrapper-design-1.md`.

## Bank-and-stop reminder
Boring is correct. Wire the strictness you already built. Do NOT overreact into Vulkan. Other queued lanes (TXMMGR-BOUNDS-HARDEN-1 merge, WATCHID-BOUND-1, NVIDIA/vendor confirm) are separate — not this slice.
