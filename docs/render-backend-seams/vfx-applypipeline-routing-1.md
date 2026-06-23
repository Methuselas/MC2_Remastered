# VFX-APPLYPIPELINE-ROUTING-1

**Status:** SHIPPED as **ROUTED_BY_APPLYPIPELINE** with
`proofStatus: nondeterministic_visual_gate_pending`. Routes the 3 live VFX
per-item blend sites through `applyPipeline`. The real deliverable is the
**honest proof mechanic** for a nondeterministic visual pass — not a faked
byte-hash.

## What changed
At each of the 3 LIVE per-item blend sites, the hand-set `glBlendFunc` 2-liner is
replaced by `applyPipeline(getPipelineDesc(blendMode==1 ? VfxXAdditive : VfxXAlpha), "VfxX...")`:
- particle_billboard — `gos_particle_bridge.cpp:1127` (per draw-group)
- tube_ribbon (deferred) — `gos_particle_bridge.cpp:767` (per record)
- vfx_mesh — `gos_vfx_mesh_bridge.cpp:311` (per instance)

(+ the two bridges gained the `PipelineRegistry.h` / `pipeline_binder.h` includes.)
**Kept verbatim:** program bind (VFX `glProgramName==0` → applyPipeline SKIPs
program), flush-level depth/cull, `glDrawBuffers(1,COLOR0)` deferred-tube override,
samplers, VAO/SSBO, all uniforms. tube-immediate (dead) path left as-is.

## Why it is correct (state-equivalence)
Per VFX-APPLYPIPELINE-ROUTING-RECON-1 §4, `applyPipeline(VfxX)` produces the
**byte-identical** FF state the bridges set by hand (depth never writes, cull never
on, always GEQUAL, no frontFace, no polygon offset). The precise `BlendMode` rows
(`AdditiveSrcAlphaOne`=SRC_ALPHA/ONE, `AdditiveOneOne`=ONE/ONE, `AlphaBlend`),
`applyPipeline`'s switch, and `check-pipeline-key`'s `CANONICAL_BLEND` guard
together guarantee the routed `glBlendFunc` equals the old per-`blendMode` func.

## Proof — and why it is `*_pending`, not VISUAL_PROVEN
Layered gate attempted:
- ✅ full build green; ✅ smoke PASS (multiple runs, no crash); ✅ no GL errors;
  ✅ all 6 `VfxX` ids confirmed as `applyPipeline` routed-evidence (pass_coverage);
  ✅ state-equivalence (recon §4) + checker-guaranteed row→func mapping.
- ❌ runtime `[PIPELINE_BIND]` trace of the 6 rows + a VFX-bearing A/B could **not
  be landed in headless smoke this slice**: the GPU-particle bridges only draw
  when effects actually spawn (combat). Idle tier1 + the `MC2_FX_FORCE_SPAWN`
  fixture did not produce confirmed in-frame particles in the captured artifacts,
  and the trace did not surface. byte-hash is invalid anyway (nondeterministic
  spawn).

Per the slice directive, the routing is **not** marked VISUAL_PROVEN on an
unlanded gate. The ledger records `status: ROUTED_BY_APPLYPIPELINE`,
`proofStatus: nondeterministic_visual_gate_pending`, with the evidence + the
explicit upgrade condition (`next: VFX-VISUAL-GATE-1`).

## Ledger / checker mechanic (the durable deliverable)
- `pipeline-pass-coverage-ledger.json`: VFX `DESCRIPTIVE_REGISTERED →
  ROUTED_BY_APPLYPIPELINE` + `proofStatus` + `proofNote` + `next`.
- `check-pass-coverage.py`:
  - ROUTED+ evidence now accepts a `pipelineIds` **list** (VFX = 6 ids), not just
    a single `pipelineId`.
  - new `proofStatus` field: allowed set
    {`byte_identical`, `perceptual_ab`, `oracle_coverage`,
    `nondeterministic_visual_gate_pending`}; and **FAILs if a pass claims
    `VISUAL_PROVEN`/`SPIRV_ELIGIBLE` while `proofStatus` is pending** — you cannot
    fake "proven" on a gate you didn't land.

## Verification
- Build green (mc2 + launcher, isolated worktree).
- `pipeline_key` / `pipeline_desc` / `pass_coverage` PASS.
- Adversarial (all FAIL, restored): VISUAL_PROVEN-while-pending; invalid
  proofStatus value; planted blend factor/func mismatch (collapse) on a VfxX row.
- No GL errors; smoke PASS.

## Exclusions held
Routed only the 3 live sites; no ownership moved for program/drawBuffers/VAO/
SSBO/samplers/uniforms/sorting/instance-data/compute; dead immediate-tube path
untouched; no shader/SPIR-V change; no particle determinism rewrite.

## Next
**VFX-VISUAL-GATE-1** — land a VFX-bearing A/B (force confirmed in-frame particles
via a combat scenario / soak cheat / fixed-effect bookmark) + `[PIPELINE_BIND]`
trace of all 6 rows + oracle-coverage; then upgrade VFX → VISUAL_PROVEN. This is
the reusable "prove a nondeterministic visual pass" capability for future
particles / spray / smoke / vegetation.
