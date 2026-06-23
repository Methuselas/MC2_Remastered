# WATER-ARMED-APPLYPIPELINE-ROUTING-1

**Status:** SHIPPED as **ROUTED_BY_APPLYPIPELINE** + `proofStatus:
byte_ab_capture_pending`. Routes the armed water fast-path FF state through
`applyPipeline(WaterArmed)`. Runtime trace **confirms correct state**; an
empirical byte-A/B is deferred (tooling would kill a concurrent mc2.exe).

## What changed (`gameos_graphics.cpp`, `renderWaterFastPath`)
Replaced the hand-set base FF block (`:3275-3279`: `glDisable(CULL)` +
`glEnable(DEPTH_TEST)` + `glDepthFunc(GEQUAL)` + `glEnable(BLEND)` +
`glBlendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA)`) with
`applyPipeline(getPipelineDesc(WaterArmed))`.

**Kept manual (not applyPipeline's job):**
- `glDepthMask(s_waterNoDepthWrite ? FALSE : TRUE)` (`:3288`) — runs AFTER and
  overrides applyPipeline's `depthWriteEnable=true`, **preserving the
  `MC2_WATER_NO_DEPTH_WRITE` A/B debug gate**.
- the save block + restore epilogue (water owns its own save/restore), the
  `glUseProgram(prog)` (applyPipeline SKIPs program since `glProgramName==0`),
  sampler, VAO, uniforms, FBO.

## Proof — and why `byte_ab_capture_pending`
- ✅ build green; ✅ smoke PASS 2/2 (mc2_01 + mc2_24); ✅ no GL errors.
- ✅ **RUNTIME-CONFIRMED**: with `MC2_RENDER_WATER_FASTPATH=1`, `[PIPELINE_BIND]
  WaterArmed` fired **3643× (mc2_01/clearwater) + 4194× (mc2_24)** = once/frame,
  with exactly `depth=GreaterEqual cull=None frontFace=Ccw polygonOffset=false`.
  Unlike VFX/terrain-overlay/decal, **water actually draws + traces** — the
  routing is empirically exercised and the state is the WaterArmed row.
- ✅ state-equivalence: applyPipeline(WaterArmed) == old hand-set (cull off, depth
  on, GEQUAL, blend SRC_ALPHA/1-SRC_ALPHA; +no-op frontFace/offset; depthMask kept
  manual).
- ❌ empirical **before/after byte-A/B not captured**: `run_visual_capture`
  unconditionally `taskkill /F /IM mc2.exe` (`_kill_existing_mc2`, :125) and a
  **concurrent foreign mc2.exe was running** — never-kill-concurrent rule. Water-
  fast is deterministic, so the byte-A/B is trivial to land when no concurrent
  instance is up.

Strongest pending case to date: *exercised + trace-confirmed*, deterministic —
distinct from VFX (nondeterministic) and terrain overlay/decal (not exercised).

## Proof taxonomy (the durable bit — feeds the future visual-gate harness)
`check-pass-coverage.py` PENDING proofStatus values now distinguish WHY a routed
pass isn't yet VISUAL_PROVEN:
- `nondeterministic_visual_gate_pending` — output nondeterministic (VFX spawn).
- `pass_not_exercised_in_smoke` — deterministic, content-dependent, didn't draw
  in tier1 (TerrainOverlay/TerrainDecal).
- `byte_ab_capture_pending` — deterministic, **DRAWS + trace-confirms correct
  state**, byte-A/B capture not yet run (Water).

The "no VISUAL_PROVEN/SPIRV_ELIGIBLE while pending" guard covers all three.

## Verification
Build green; `pipeline_key`/`pipeline_desc`/`pass_coverage` PASS; WaterArmed in
`applyPipeline` routed-evidence; smoke PASS; no GL errors; foreign WIP untouched.

## Exclusions held
Routed only the armed fast-path base FF block; legacy quad fallback + MDI
sub-variant untouched; depthMask debug gate + save/restore + program/sampler/
uniforms preserved; no shader/blend change; no SPIR-V.

## Next
**WATER-VISUAL-GATE** (or the shared visual-gate harness): `run_visual_capture
mc2_01 (clearwater)` armed before/after byte-hash when no concurrent mc2.exe →
VISUAL_PROVEN. Water is the easiest of the 3 pending passes to clear (it draws in
a standard armed run).
