# Execution prompt — Track C: GPU Compute Cull + Async Readback (C0/C1/C2/C3)

**Paste this into a fresh session to execute.** Self-contained briefing.

---

You are executing **Track C** of the MC3 rendering modernization arc — the architectural endpoint of the CPU→GPU offload. Compute cull moves visibility computation to GPU; async readback feeds existing CPU lifecycle gates 1-frame-lagged. Splits into FOUR sequential slices C0/C1/C2/C3.

**Read this prompt fully before starting any slice.** Each of C0-C3 has its own gate and rollback. Don't collapse the staging.

## Worktree

`A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`. All paths below are relative.

## Required reading (in order)

1. `CLAUDE.md` — worktree rules. ⚠️ "Load-Bearing Cull Infrastructure" section is mandatory.
2. `docs/superpowers/plans/2026-05-06-track-c-compute-cull.md` — **this is your plan.** ~738 lines covering all four slices. C0 is fully implementable, C1 condensed, C2/C3 detailed sketches.
3. `docs/superpowers/specs/2026-05-06-track-abc-brainstorm-decisions.md` — Q7 (accept 1-frame AI artifact), Q8 (combined dispatch), Q9 (visible-ID list), Q10 (C0 dynamic actor records), Q11 (mesh-range bucket key), **Q12 (sync contracts — SSBO atomics + GL_SHADER_STORAGE_BARRIER_BIT + GL_COMMAND_BARRIER_BIT, NO GL_ATOMIC_COUNTER_BARRIER_BIT)**, Q13 (GL 4.6 OR ARB extensions), Q14 (split into 4 slices), **Q15 (sequential-with-overlap soak)**, Q17 (block-active rollup at C1), Q18 (lights preflight at C3).
4. `docs/superpowers/explorations/2026-05-06-track-c-compute-cull-recon.md` — recon. **Critical finding: Track C1 ships the FIRST compute path AND the first cull→indirect-draw barrier in the engine. Zero precedent to mirror; Q12 contracts are normative.**
5. `memory/cull_gates_are_load_bearing.md` — cascade hazard.
6. `memory/gpu_direct_renderer_bringup_checklist.md` — 9 traps every new fast path hits.
7. `docs/amd-driver-rules.md` — AMD RX 7900 XTX driver quirks.
8. `memory/camera_model_oblique_cinematic.md` — camera shape (relevant for HZB Q21 follow-up).

## Skill to invoke

**`superpowers:subagent-driven-development`** with **adversarial review at every slice boundary** (C0→C1, C1→C2, C2→C3). Track C is high-stakes architectural endpoint work; skill's adversarial-plan-review trigger fires on every slice.

## Critical prerequisites

**C0 is independent** (substrate-only, no compute, no readback, no gate handoff). Can start any time once HEAD builds clean. **Run C0 first.**

**C1 is GATED on:**
- Track B substrate ready in tier1 default-on (or at minimum, B's persistent-instance buffer shipped + Q16 `firstColorOffset` decision committed).
- Q16 closed (committed in HEAD's brainstorm-decisions doc).
- Q17 closed — block-active rollup path chosen (GPU compute aggregation OR CPU-side conservative walk). Picked at C1 plan-time per Task C1-RB.

**C2 is GATED on:** C1 ship + ≥1 week soak.

**C3 is GATED on:** C1+C2 ship + soak + **Q18 lights preflight** (Task C3-0 grep for `lightAppearance->inView` consumers; lights join C3 routing if any lifecycle gate reads them).

If C1's prerequisites aren't met: ship C0 alone, hand back to user, wait for B + Q16/Q17 to clear.

## Scope summary

### C0 — Dynamic actor visibility records (~3-5 days, executable now)

Per-frame CPU upload of compact `GpuActorRecord` (64B std430-aligned) for every dynamic actor (mechs, GVs, gates, turrets, generics). Common cull-input schema with Track B's static records — compute shader doesn't distinguish. NO compute cull yet. Validates schema + upload path; AABB parity vs legacy `recalcBounds()`.

Schema is **LOCKED** at the plan level — see plan §"C0 schema (LOCKED)" for byte offsets. `static_assert`s on size + every field offset; lockstep rule applies.

Exit: substrate buffer fills each frame, AABB parity 0 mismatches, tier1 5/5 PASS, AMD canary clean.

### C1 — Compute cull for render only (~1 week, gated on B + Q16/Q17)

Single combined compute dispatch over Track B static records + C0 dynamic records. Bucket-keyed scatter-write via SSBO `atomicAdd` (NOT ACBOs, per Q12). Indirect draw consumes via `gl_BaseInstance`. **GPU→GPU only — no CPU readback in C1; CPU lifecycle gates still use legacy `inView`.**

**Q12 sync contracts mandatory:**
- Counter representation: SSBO `uint` + shader `atomicAdd`.
- Counter reset: `glClearNamedBufferSubData`.
- Capacity overflow: `[GPU_CULL v1] event=overflow` log; never silent drop.
- Memory barriers: `GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT`. **DO NOT add `GL_ATOMIC_COUNTER_BARRIER_BIT`** — that's for OpenGL atomic-counter buffer objects which we explicitly chose not to use. Cargo-culting it is a known anti-pattern this plan exists to prevent.
- **Q17 block-active rollup** (Task C1-RB): pick GPU compute aggregation OR CPU-side walk; document and implement.

Bucket key per Q11: mesh-range + shader + texture-set + VAO + index-type. NOT material-only.

Exit: indirect draw output visually matches legacy CPU-cull at all zooms, `[GPU_CULL v1] overflow=0` across tier1, `[DESTROY v1]` parity (gates untouched at C1), AMD canary verifies barriers (this is THE first such verification in the engine — RenderDoc/apitrace pre/post barrier capture).

### C2 — Async readback into non-lifecycle consumers (~3-5 days)

3-frame readback ring + per-frame `glFenceSync` + three-tier fallback (frame N-1 / frame N-2 / conservative-visible). Feeds visibility into ONE non-lifecycle consumer first (smallest blast — Tracy plot or `[GPU_CULL v1]` summary).

Validates the readback infrastructure end-to-end before C3 routes lifecycle gates. Test hook `MC2_GPU_CULL_FORCE_FENCE_NOT_READY=1` exercises tier-2/tier-3 fallback paths; selftest hard-fail per advisor sharpening #2 if any tier doesn't fire.

`glInvalidateBufferSubData` after slot consumption (per Q12 step 4). Prevents driver ghosting on re-map.

Exit: readback ring proven non-stalling, all three fallback tiers exercised, non-lifecycle consumer produces sensible output for ≥30s gameplay.

### C3 — Gate handoff + Camera::UpdateRenderers stub (~1 week)

The actual perf payoff. Routes lifecycle gates to GPU-derived (1-frame-lagged) visibility:
- `objmgr::update`, `Mech3DAppearance::update`, `GVAppearance::update` consume frame-N-1 GPU bit.
- AI gate `code/mech.cpp:6497` and weapon-spawn-node queries (`mclib/mech3d.cpp:721,759,795,833`, `mclib/gvactor.cpp:445,500,533`) consume same.
- `Camera::UpdateRenderers` becomes a stub (Tracy verifies).

**Q18 lights preflight (Task C3-0)** — REQUIRED before any rewiring:
```bash
grep -rn "->inView\|lightAppearance.*inView\|isInView" code/ mclib/ \
  | grep -v "^code/light.cpp" | grep -v "^mclib/.*Appearance"
```
Audit each match. If any C3-routed lifecycle gate transitively reads `lightAppearance->inView`, lights JOIN the C3 routing list.

**Q7 1-frame artifact accepted as gameplay-tolerance tradeoff** — not "below human-perception floor" (that overclaim was caught in advisor pass). Validate via explicit canaries: zoom-transition, camera-jump, first-contact, weapon-spawn before flip.

Exit: `Camera::UpdateRenderers` is a stub, FPS gate ≥30% improvement at wolfman zoom, `[DESTROY v1]` count + identity parity, tier1 5/5 PASS.

## Gates

- **Per-task build:** clean.
- **Per-slice exit gates:** plan documents each slice's hard exit criteria explicitly. Read those.
- **Cross-slice:** Q15 sequential-with-overlap soak — each slice soaks under "previous-slice-default-on" config.
- **AMD canary** at C1 is load-bearing (no engine precedent; Q12 contracts are normative).

## When blocked

- **Track B not ready and you want to start C1:** STOP. C1's compute reads from B's persistent instance buffer; without B, C1's reads are undefined. Run C0 instead, hand back.
- **Q16/Q17/Q18 not closed:** flagged in plan; STOP for the affected slice. C0 is unaffected.
- **AMD barrier verification fails at C1:** investigate via RenderDoc/apitrace capture. The Q12 contract is correct (verified at brainstorm + advisor pass) — failure is likely a code bug, not a contract bug. Cite the failed capture in handback.
- **Q18 lights preflight finds lifecycle consumers** (light visibility feeding into a non-light lifecycle gate): scope expansion. Add lights to C3 routing list, document, proceed.
- **C3 visual canaries show observable AI 1-frame artifact** that's gameplay-affecting (not just visible — actually hurting play): per Q7 reversibility hedge, add `gpu_cull::isVisibleSync(actorId)` API as a separate engine-side primitive that DOES block on the fence at specific call sites. Pay the cost only there as content-layer concern. Document and ship.
- **`Camera::UpdateRenderers` Tracy zone fails to drop to ≤0.3ms after C3 stub** (plan target: ~6ms → ≤0.3ms): incomplete routing. Some legacy `inView` consumer wasn't rewired. Re-grep, find the holdout, route it.

## Soak windows (cumulative)

- C0: ≥3-5 days.
- C1: ≥1 week (first compute path in engine — extra discipline).
- C2: ≥3-5 days.
- C3: ≥1 week.

Total Track C: ~3-4 weeks of code + ≥3 weeks soak. Calendar time ~6-8 weeks dominated by sequential-soak.

## Deliverable

Four feature branches (or four sets of commits on one branch — your call) covering C0/C1/C2/C3 in sequence. Memory file per slice. `Camera::UpdateRenderers` reduced to a stub at end. CPU "should this be culled?" cost moved to ~50-100µs GPU compute dispatch.

After C3 ships and soaks: **Track G/C4 (HZB occlusion culling)** becomes the natural follow-up per Q21 — recon-first, plan after.

Track A1/A2/B may already have shipped or be in flight in parallel. Track C is the sequential capstone.
