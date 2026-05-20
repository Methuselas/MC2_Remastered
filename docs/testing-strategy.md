# MC2 Automated Testing Strategy

Captured 2026-05-19 from a meta-discussion about test coverage during the
OpenGL -> Vulkan-prep arc. Smoke tests (`scripts/run_smoke.py`) are the
existing safety net; this document enumerates what else is worth wiring in,
ranked by leverage for MC2 specifically.

The frame of reference is "what do other C++ game-engine shops actually do":
golden-image perceptual diff (idTech, Frostbite, UE Automation, Unity GTF),
shader-validator gates (every Vulkan-bound codebase), sanitizer builds
(everyone), and a thin layer of unit tests on pure-logic code (math, parsers,
hash tables). MC2's twist is Vulkan-prep -- shader validation goes from
"nice" to "load-bearing" because GLSL-isms that the AMD/NVIDIA GL frontends
silently accept will hard-fail in SPIR-V.

## Tier 1 -- nearly free; do these first

### 1.1  Shader validator gate  (Vulkan-prep #1)

Single-shot CMake / Python target that walks `shaders/*.{vert,frag,tesc,tese,comp,geom}`,
prepends `#version 430` (matching `makeProgram()` discipline), resolves
`#include` against `shaders/include/`, and runs:

- `glslangValidator -V` (compile -> SPIR-V; catches GLSL syntax + Vulkan
  rules: explicit `layout` qualifiers, uniform vs SSBO, sampler decoration)
- `spirv-val` on the emitted SPIR-V (catches malformed module structure)
- Optional: `spirv-cross` round-trip back to GLSL as a portability smoke

Failure exits nonzero. Wire into pre-commit (cheap, <2s for all 64 shaders)
or as a separate CMake target the smoke gate depends on.

Why this is the single highest-leverage Vulkan-prep test: most
"works on GL, breaks on Vulkan" bugs are uninitialized varyings, implicit
type conversions, missing `layout(location=)`, non-uniform control flow in
sampler use, or `texture2D()`-style legacy calls. All of them show up
immediately under `glslangValidator -V`.

Skeleton: `scripts/validate_shaders.py` (sister to `run_smoke.py`).

### 1.2  KHR_debug callback as fail-on

Env-gate the existing `KHR_debug` callback (`MC2_GL_DEBUG_FATAL=1`) so smoke
runs `abort()` on `GL_DEBUG_SEVERITY_HIGH` instead of silently logging.

Catches the GL_BLEND / sampler / depth-state inheritance bugs already
documented in:
- `memory/blend_state_inheritance_in_post_process.md`
- `memory/sampler_state_inheritance_in_fast_paths.md`
- `memory/gpu_direct_depth_state_inheritance.md`

Zero new infrastructure -- the callback already exists; flip a switch.

### 1.3  Static analysis

`clang-tidy` with a narrow ruleset on `mclib/` and `GameOS/gameos/gpu_*`:
- `bugprone-*` (uninit, dangling refs)
- `cppcoreguidelines-init-variables`
- `readability-implicit-bool-conversion`
- `bugprone-narrowing-conversions` (ARGB pack/unpack gotchas)

PVS-Studio is the AAA standard but commercial; clang-tidy gets 80% free.
The `export_clang_paths.sh` infrastructure already in tree supports this.

## Tier 2 -- unit tests on the parts that ARE unit-testable

Drop `doctest.h` (single header, no build-system invasion) and a `mc2-tests`
CMake target. Write tests only where smoke gives zero feedback signal:

| Domain | Example unit |
| --- | --- |
| Hashing | `elfHash`, `FastFileFind` key normalization (slash-direction trap, `memory/fst_forward_slash_invariant.md`) |
| Bit packing | `gos_VERTEX` ARGB pack -> BGRA unpack round-trip (`memory/mc2_argb_packing.md`) |
| Projection math | `projectZ`, `inverseProjectZ` (the `clip.w` sign trap, `memory/clip_w_sign_trap.md`, was a textbook unit-testable bug) |
| Cull predicates | `inView` / `canBeSeen` against synthetic AABBs |
| Quad windowing | terrain `quadList` slot arithmetic (`memory/quadlist_is_camera_windowed.md`) |
| Asset scale | mip-chain integrity for upscaled-art codepath (`memory/amd_auto_lod_strict_fail.md`) |
| Pool accounting | TGL vertex/color/face pools: allocate-to-exhaustion, assert NULL return |

DO NOT try to unit-test rendering. That is Tier 3's job.

## Tier 3 -- visual-diff capture  (renderer's actual contract)

This is what idTech, Frostbite, UE, and Unity actually run. The Stage 2.E
visual-diff infrastructure (shipped 2026-05-04) is the canonical Tier 3
implementation:

- **Engine capture:** `GameOS/gameos/gos_visual_diff.h`/`.cpp`. Env-gated
  on `MC2_VISUAL_DIFF_CAPTURE`. Counts frames from
  `SmokeMode::missionHasStarted()`, snapshots the pre-HUD framebuffer at
  `MC2_VISUAL_DIFF_FRAME_N` via `gos::screenshot::writeTGA` to
  `MC2_VISUAL_DIFF_OUT`, exits with code 4 on timeout. Wired in
  `gameosmain.cpp` at the pre-HUD seam (between `pp->endScene()` and
  `projectz_overlay_render`). No `glReadPixels` or stb_image_write
  needed; engine TGA serializer is already in tree.
- **Python harness:** `tests/smoke/object_visual_diff.py`. Two modes:
  - `--measure-variance`: runs each mission TWICE at identical config;
    reports per-mission max channel delta, differing-pixel count, ratio,
    and an 8x8 hot-region grid. NOT a gate -- the architectural
    validation step.
  - `--gate`: applies the spec tolerance (max channel delta <= 2 LSB and
    differing-pixel ratio <= 0.5%); PASS/FAIL gate, exit 0/2.
- **Camera:** no teleport, no scripted-input pipeline. The mission's own
  intro pan + command-position settle gives the deterministic vantage;
  the harness just snapshots at a chosen frameN per mission. Per-mission
  frameN lives in `CAPTURE_FRAMES` in the harness (currently 400 for all
  tier1 missions).
- **Baselines:** intended location `tests/smoke/baselines/visual-diff/<mission>.tga`.

What this catches (no human inspection required):
- Stale-shader-cache class (`memory/stale_shader_cache_symptom.md`)
- Black-static-props / lighting regressions
  (`memory/shader_exe_deploy_lockstep.md`)
- LightsData UBO -> SSBO C++/GLSL skew (`memory/cpp_glsl_ubo_struct_lockstep.md`)
- Asset-scale OOB blits (`memory/amd_auto_lod_strict_fail.md`)
- Terrain decal regressions when `MC2_TERRAIN_INDIRECT_OVERLAY` drifts

### Tier 3 status (as of 2026-05-20)

The capture engine and Python harness are both implemented and shipped.
Tier1 alignment + `DEPLOY_DIR` realignment landed 2026-05-20 (TIER1 =
`[mc2_01, mc2_03, mc2_10, mc2_17, mc2_24]` matching the canonical smoke
tier1; deploy = `mc2-win64-v0.4`).

**Variance gate (the empirical pre-requisite for committing baselines) is
NOT green.** First measurement against HEAD on v0.4 (frameN=400, all
tier1 missions, gpu_objects=0):

| Mission | ratio | max delta | verdict | hot-region pattern |
| --- | --- | --- | --- | --- |
| mc2_01  | 0.2527 | 249 | EXCEEDS | broad, full-frame |
| mc2_03  | 0.6265 | 242 | EXCEEDS | broad, full-frame |
| mc2_10  | 0.0034 | 19  | OK      | localized bottom rows |
| mc2_17  | 0.2365 | 249 | EXCEEDS | broad, mid-rows heavy |
| mc2_24  | 0.0027 | 250 | OK      | 4 cells (mech-region signature) |

Outcome (C) per the dispatch playbook -- 3+ missions exceed and the
drift is NOT localized to mech regions, so this is not a simple
idle-anim suppression. Root cause hypothesis: ongoing intro pan, unit
movement, or particle systems still active at frame 400 for these
missions. Resolution paths (not yet attempted):

1. Per-mission `CAPTURE_FRAMES` tuning -- push frameN higher until
   intro pan + settle completes for mc2_01 / mc2_03 / mc2_17.
2. Engine-side suppression of any per-frame nondeterminism source
   (anim, particles, lighting flicker) gated on
   `VisualDiff::isCaptureEnabled()`.
3. Tighter `SmokeMode::missionHasStarted()` semantics (currently fires
   on first ready-tick; may need to wait for camera-driver settle).

DO NOT commit baselines until variance is green; a flapping baseline
defeats the gate. Suppression / frameN tuning is a separate follow-up
slice.

## Tier 4 -- sanitized smoke

The `.claude/worktrees/asan-mvp/` worktree is already 90% of this work.
Promote: one ASAN build of `mc2.exe`, one ASAN smoke run (`mc2_01`, 30s)
as a weekly or pre-merge gate.

Catches the use-after-free / pool-overrun / heap-write-OOB class. ASAN
overhead is ~2-3x runtime; tier1 (5 missions x 30s) is still tractable.

## Tier 5 -- RenderDoc CLI pipeline-state snapshot  (advanced; optional)

`renderdoccmd` can re-run a `.rdc` capture headless and dump the
GL/Vulkan pipeline state to JSON. Workflow:

1. Capture one reference frame per scene with RenderDoc, check in `.rdc`.
2. CI step: `renderdoccmd convert -i ref.rdc -o pipeline.json`,
   `diff pipeline.json baseline.json`.

Catches silent state regressions (blend mode flipped, sampler swapped,
wrong texture unit, depth bias clobbered) without needing pixel-perfect
golden images. Exactly the workflow `memory/reference_renderdoc_pipeline_export_first_step_for_render_regressions.md`
already describes -- this just automates it.

## Industry context (one sentence per shop)

| Shop | What they actually run |
| --- | --- |
| id Software / idTech | Heavy math/util unit tests, golden frames, in-engine asserts everywhere |
| Frostbite / EA | Per-platform image-diff farm per-commit on every renderer |
| Unreal | Automation Framework + Gauntlet; screenshot tests are first-class |
| Unity | Graphics Test Framework -- explicitly golden-image-diff with tolerance |
| Godot / indie | GUT + reference renders |
| Carmack / Lengyel school | Compile-time asserts everywhere; debugger-driven; minimal runtime test scaffolding |

The common thread: **renderers are tested by pixels, not by unit tests.**
Unit tests cover pure logic; everything else is screenshot diff or
sanitizer-augmented runtime.

## Recommended adoption order

1. **Tier 1.1 (shader validator)** -- highest Vulkan-prep leverage; one
   Python script + one CMake custom target.
2. **Tier 3 (golden-image diff)** -- highest renderer-regression leverage;
   the right permanent gate against the stale-shader / state-leak bug class
   that smoke alone cannot catch.
3. **Tier 1.2 (KHR_debug fatal)** -- free; flip a switch.
4. **Tier 4 (ASAN smoke)** -- promote the existing `asan-mvp` worktree.
5. **Tier 2 (doctest unit tests)** -- opportunistic; write one whenever
   fixing a math/parser/hash bug so the regression test exists forever.
6. **Tier 5 (RenderDoc pipeline JSON)** -- last; advanced; only if state
   leaks become a recurring pain point.

## Sketches

| Tier | File(s) | Purpose |
| --- | --- | --- |
| 1.1 | `scripts/validate_shaders.py` | Shader compile + SPIR-V emit gate |
| 1.2 | `docs/testing-strategy-engine-hooks.md` (Sketch A) | KHR_debug fatal env-gate |
| 1.3 | (already wired in tree) | clang-tidy via `export_clang_paths.sh` |
| 2   | `tests/unit/CMakeLists.txt`, `tests/unit/test_main.cpp`, `tests/unit/test_hashing.cpp`, `tests/unit/test_projection.cpp`, `tests/unit/test_argb_pack.cpp` | doctest harness + three example tests |
| 3   | `GameOS/gameos/gos_visual_diff.{h,cpp}`, `tests/smoke/object_visual_diff.py`, `tests/smoke/baselines/visual-diff/` (pending green variance) | Implemented; Sketch B SUPERSEDED -- see `docs/testing-strategy-engine-hooks.md` |
| 4   | `scripts/run_smoke_asan.py`, `scripts/asan_suppressions.txt` | ASAN build + single-mission smoke wrapper (sketches not yet in tree) |
| 5   | `scripts/renderdoc_capture.py`, `docs/testing-strategy-engine-hooks.md` (Sketch C) | In-process RenderDoc capture + pipeline-state JSON diff |

All sketches are structurally complete but not exercised. Each one calls
out its prerequisites explicitly in its header docstring -- the most
common are leaf-TU extraction (Tier 2), engine-side env hooks (Tiers 3
and 5; both in `docs/testing-strategy-engine-hooks.md`), and the
existence of one checked-in baseline (Tiers 3 and 5; `--regen` flag on
both harnesses).

### Prerequisite ordering (which sketch unblocks which)

```
1.1  validate_shaders.py        independent; ready to wire today
1.2  KHR_debug fatal env-gate   independent; ~15 lines in existing callback
1.3  clang-tidy                 already in tree

2    doctest tests              prerequisite: factor `elfHash`, `projectZ`,
                                  packing helpers into leaf TUs (one PR each)

3    visual-diff capture        IMPLEMENTED (engine + harness shipped
                                  2026-05-04); blocking next step is
                                  variance-green across tier1 -- as of
                                  2026-05-20 mc2_01 / mc2_03 / mc2_17
                                  drift broadly at frameN=400 (outcome C).
                                  See "Tier 3 status" above. Baseline
                                  commit is gated on variance green.

4    ASAN smoke                 prerequisite: clang_rt ASAN runtime DLL on PATH
                                  or copied next to mc2-asan.exe

5    RenderDoc capture          prerequisites:
                                  (a) engine-side RenderDoc API hook (Sketch C)
                                  (b) renderdoc_app.h vendored at
                                      3rdparty/renderdoc/
                                  (c) renderdoccmd on PATH
                                  (d) one regen-pass to seed baseline JSONs
```

Promote in adoption order from the section above
(1.1 -> 3 -> 1.2 -> 4 -> 2 -> 5). Each is independent at adoption time;
nothing forces a big-bang.
