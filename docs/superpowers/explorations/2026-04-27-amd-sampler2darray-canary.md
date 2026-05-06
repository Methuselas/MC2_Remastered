# AMD sampler2DArray Canary — Result Report

Date: 2026-04-27
Hardware: AMD RX 7900 XTX (per worktree CLAUDE.md target)
Driver: as installed at test time
Branch: `claude/nifty-mendeleev` (throwaway code; NOT merged into main)
Tester: ThranduilsRing
Build: `mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/RelWithDebInfo/mc2.exe`

---

## TL;DR

**The AMD `sampler2DArray` rule is REFUTED for the synthetic 4×4×4 RGBA8
case** — confirmed in **both menu state and mid-mission state** (runs 4
and 5). A clean per-layer pass across all four texture-array layers
returned exact (255,0,0,255), (0,255,0,255), (0,0,255,255),
(255,255,255,255) readbacks with `gl_err=0x0` and `max_error=0.0`,
identically under both states.

This **reopens** a major design degree of freedom for PatchStream:
the historical 1-draw-per-tile structure (§6.1 of
[2026-04-27-patchstream-shape-b-design.md](../specs/2026-04-27-patchstream-shape-b-design.md))
is no longer mandated by `docs/amd-driver-rules.md` line 13. Per-vertex
layer indexing into a single colormap array is a viable post-M0b
follow-up for collapsing terrain draws.

**Caveat:** only Canary A (synthetic, small RGBA8) was run.
Canary B (real-terrain-colormap-into-layer-0, identical rendering vs.
sampler2D path) is the conservative next step before re-architecting
PatchStream around it. Canary A is a strong signal, not a final proof.

---

## What was tested

### Canary A — synthetic 4x4x4 RGBA8 probe

- 4×4×4 `GL_TEXTURE_2D_ARRAY` (RGBA8), layers filled:
  - Layer 0: solid red (255,0,0,255)
  - Layer 1: solid green (0,255,0,255)
  - Layer 2: solid blue (0,0,255,255)
  - Layer 3: 1-pixel checker (alternating black / white, center pixel "on")
- 4×4 RGBA8 FBO target.
- Vert + frag shader pair, GLSL 4.30 (`#version 430` prefix, matching the
  worktree's standard for SSBO/std430 contexts).
- Frag samples `texture(arrayTex, vec3(uv, float(layer)))` — exact path
  PatchStream B-array would use.
- Per-layer: `glClear(0,0,0,0)`, `glDrawArrays(GL_TRIANGLE_STRIP, 0, 4)`
  fullscreen quad, `glReadPixels(2,2,1,1, GL_RGBA, GL_UNSIGNED_BYTE)`.
- Compare readback to expected, max-abs-error across all channels and layers.

### Where the canary fires

`gosRenderer::beginFrame()` after the global VAO bind, gated by
`MC2_AMD_TEXARRAY_CANARY=1` env var. Frame number controllable via
`MC2_AMD_TEXARRAY_FIRE_FRAME` (default 120). One-shot per process.

### State save/restore

Saved and restored: GL_DRAW/READ_FRAMEBUFFER bindings, current program,
VAO, VBO, active texture, `GL_TEXTURE_BINDING_2D_ARRAY` on unit 0,
viewport, `GL_COLOR_WRITEMASK`, `GL_DEPTH_WRITEMASK`. The canary forces
colorMask all-on, depthMask on, `glDrawBuffers([GL_COLOR_ATTACHMENT0])`,
disables depth-test/blend/cull during its draw. State save/restore
follows the static-prop-batcher precedent at
`gos_static_prop_batcher.cpp:692–712, 817–834`.

---

## Test runs and results

### Run 1 — `init()`-time hookpoint (later moved)

- Canary fired inside `gosRenderer::init()`, before any normal frame.
- Result: `pass=N max_error=1.0000 L*=(0,0,0,0) gl_err=0x502`.
- Diagnosis: `GL_INVALID_OPERATION` was a stale error from earlier in
  init, not a canary failure. Moved hook to `beginFrame()` (frame N gate)
  and added per-step `checkErr` logging.

### Run 2 — frame-60 hook, post-init, sanity-mode shader

- Frag temporarily bypassed texture sample, output a constant per-layer
  color.
- Result: `pass=N L*=(0,0,0,255) gl_err=0x0`.
- Diagnosis: with constant-color output, RGB *still* read 0 — but alpha
  read 255. That signature is `glColorMask(FALSE,FALSE,FALSE,TRUE)`-
  inherited. Added colorMask save/restore + force-all-on, plus explicit
  `glDrawBuffers`.

### Run 3 — frame-60, colorMask fix in place, real texture-sample shader

- Result: `pass=N L*=(0,0,0,255) gl_err=0x0`,
  `inherited colorMask=(1,1,1,1) depthMask=1`.
- Diagnosis: colorMask was already all-on, ruling that out. The persisting
  (0,0,0,255) signature had to be reading from somewhere with black RGB
  + alpha-1 — which is the **window default framebuffer**. The canary
  bound only `GL_DRAW_FRAMEBUFFER`; `glReadPixels` reads from
  `GL_READ_FRAMEBUFFER`, still default. **Bug in canary, not the driver.**

### Run 4 — frame-60 (menu state), FBO bound to GL_FRAMEBUFFER (both targets)

- Result:

  ```
  [AMD_TEXARRAY v1] event=inherited_state colorMask=(1,1,1,1) depthMask=1
  [AMD_TEXARRAY v1] event=program_info shp=118 locTex=0 locLayer=1
  [AMD_TEXARRAY v1] event=result pass=Y max_error=0.0000
    note=L0=(255,0,0,255) L1=(0,255,0,255) L2=(0,0,255,255) L3=(255,255,255,255)
    gl_err=0x0
  ```

- Verdict: PASS. All four layers sampled and read back exactly.

### Run 5 — frame-600 (mid-mission), independent confirmation

User direct-launched with `MC2_AMD_TEXARRAY_FIRE_FRAME=600` and skipped
through to a mission. Canary fired once `beginFrame` reached frame 600
— deep enough into gameplay that the engine had already loaded the
mission, run shadow passes, compiled additional shaders, and built up
its full GL state.

- Result:

  ```
  [AMD_TEXARRAY v1] event=inherited_state colorMask=(1,1,1,1) depthMask=1
  [AMD_TEXARRAY v1] event=program_info shp=121 locTex=0 locLayer=1
  [AMD_TEXARRAY v1] event=result pass=Y max_error=0.0000
    note=L0=(255,0,0,255) L1=(0,255,0,255) L2=(0,0,255,255) L3=(255,255,255,255)
    gl_err=0x0
  ```

- `shp=121` (vs run 4's `shp=118`) confirms additional shader
  programs had been compiled by frame 600 — i.e. the canary really
  did fire during gameplay, not during a static menu.
- Inherited colorMask is `(1,1,1,1)` even mid-mission — the engine's
  per-frame state hygiene leaves a clean mask at `beginFrame` time,
  so the canary's colorMask save/force logic is defensive scaffolding
  rather than load-bearing.
- Verdict: PASS in mission state, identical to menu-state result.

The menu-vs-mission question raised by the reviewer is answered:
sampler2DArray works correctly under both states. Canary A is closed.

---

## Acceptance criteria

| Criterion | Required | Observed |
|---|---|---|
| Synthetic array probe exact (or near-exact) | ≤ 1 LSB error | 0 LSB error |
| No GL errors during draw / readback | `gl_err == 0` | `gl_err = 0x0` |
| Layer-correct sampling | distinct per layer | 4 distinct exact matches |
| State left clean for caller | save/restore complete | colorMask, FBO both targets, VAO, program restored |
| One-shot, env-gated, default-off | yes | `MC2_AMD_TEXARRAY_CANARY=1`, fires once on frame N then never again |

All Canary A acceptance criteria met.

Canary B (terrain-equivalence) **not yet run.** Recommended scope:
copy one real terrain colormap (a 256×256 or 512×512 DXT1/DXT5 or RGBA8
tile) into layer 0 of a `GL_TEXTURE_2D_ARRAY`, render the same terrain
bucket once via `sampler2D`-on-tex1 (legacy) and once via
`sampler2DArray`-on-layer-0 (test), screenshot diff. If pass, the
B-array PatchStream variant is fully unblocked.

---

## Implications for PatchStream

The PatchStream spec (`docs/superpowers/specs/2026-04-27-patchstream-shape-b-design.md`)
treats `sampler2DArray` as banned in:

- §6.1 ("Shape B cannot collapse this... `sampler2DArray` is explicitly
  banned by `docs/amd-driver-rules.md` line 13").
- §6.2 ("Material change ... since the AMD-banned `sampler2DArray`
  rule ... forces individual `sampler2D` per-material binds").
- §6.4 ("No `sampler2DArray`. Per-material colormaps on individual units").
- §10 BR1 (mentions ban in mitigation framing).
- §12.5 #4 ("speculative follow-on... if a future driver/regression-test
  cycle confirms `sampler2DArray` has been fixed").

**§12.5 #4 is now the active path.** Canary A passing means a B-array
follow-up to PatchStream is plausibly buildable. M0b should still ship
as scoped (no shader changes, persistent-mapped VBO, ~6–10 draws/frame)
because changing the M0b mechanism this late would re-open all the
buffer/shader/contract questions the previous audits closed. The B-array
variant can land on top of M0b as a separate slice once Canary B
confirms terrain-tile-equivalence.

The PatchStream spec should be updated to:
1. §12.5 #4 — promote from "speculative" to "viable, gated on Canary B".
2. §6.1 — note that the structural ~6–10 draws/frame is M0b's choice,
   not an absolute structural ceiling.
3. Add a forward-compat row to §3 "Becomes modern" — the per-vertex
   record could include a 1-byte material slot now (in `frgb`'s spare
   byte, or as a future attribute extension) to ease the B-array
   follow-up. Not required for M0b.
4. Update `docs/amd-driver-rules.md` line 13 — qualify or remove the
   ban once Canary B confirms. The synthetic-only canary (this report)
   is enough to soften the rule from "NEVER use" to "verified working
   on small RGBA8; needs terrain-equivalence verification before
   production use".

---

## Files added (throwaway, do not merge to main)

- `shaders/amd_texarray_canary.vert` (~10 lines)
- `shaders/amd_texarray_canary.frag` (~12 lines)
- `GameOS/gameos/gameos_graphics.cpp`:
  - File-local `static void runAmdTexArrayCanary()` (above
    `gosRenderer::init`, ~180 lines)
  - One-shot env-gated call in `gosRenderer::beginFrame()` (~15 lines)

All changes are env-gated; default-off. Zero behavior change without
`MC2_AMD_TEXARRAY_CANARY=1`. Easy to revert in a single commit.

---

## Recommendation

1. **PatchStream spec stays as-is for M0b.** The mechanism (persistent-
   mapped VBO, no shader changes, ~6–10 draws) is correct and
   independent of this finding.
2. **Spec §12.5 #4 promoted from speculative to "viable, gated on
   Canary B"** — see proposed updates above.
3. **Canary B is the next step.** Owner-time-budget: ~half-day. Same
   harness, real terrain colormap source, comparison rendering. If pass:
   B-array PatchStream variant goes onto the roadmap as a follow-on.
4. **Throwaway branch retained for re-test.** Keep this canary code in
   the worktree as an artifact; do not delete. If/when AMD driver
   updates land, re-running with `MC2_AMD_TEXARRAY_CANARY=1` is one
   command. Do not merge to main.
5. **Update `docs/amd-driver-rules.md` line 13** to reflect Canary A
   pass — soften wording but keep the warning until Canary B passes.
