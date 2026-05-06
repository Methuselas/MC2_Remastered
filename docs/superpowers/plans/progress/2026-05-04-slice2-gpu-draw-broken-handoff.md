# Handoff — Slice 2 GPU object draw dispatch produces no visible buildings

**Date:** 2026-05-04
**Worktree:** `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`
**Branch:** `claude/nifty-mendeleev`
**HEAD when paused:** `fc53cf9` (other session's slice3-3a counter work)
**Deploy target:** `A:/Games/mc2-opengl/mc2-win64-v0.3/`

---

## Executive summary (read this first)

The slice 2 object-offload arc landed a GPU rendering path
(`g_useGpuObjects` flag, OBJBATCHER pipeline) that the user flipped
default-on in commit `61f6a66`. **The GPU path has never visibly
rendered buildings.** It runs the vertex/fragment shaders, hits 100%
GPU utilization, and reports correct submission counts in `[OBJBATCHER
v1] event=summary` lines — but `gl_Position` outputs land outside the
visible NDC cube `[-1,+1]`, so all triangles are clipped and no pixels
are written.

The bug was masked for the entire slice 2 development arc by an MSVC
**incremental-link silent staleness** issue: prior `mc2.exe` binaries
had `g_useGpuObjects=false` baked into their data segment from before
the default-on flip. With the stale flag, the engine ran the legacy
CPU path which DOES render. Stage 2.D parity passed because parity
tested only the lighting kernel math, not the draw call dispatch.

The previous session forced a full link, surfacing the regression.
Multiple shader-side fix attempts were made; none produced visible
buildings. **The legacy CPU path is also bit-rotted** — crashes at
`gameos_graphics.cpp:3963` (NULL `selectBasicRenderMaterial`) when
forced via `MC2_GPU_OBJECTS=0`. So there is no clean opt-out
short of code change.

This handoff is for a fresh session to take over diagnosis.

---

## Background — where this fits

- Stage 2.E (visual-diff harness) was paused 2026-05-04 due to mech
  idle animations being wall-clock-driven (determinism floor). See
  [docs/superpowers/plans/2026-05-04-stage2e-phase1-plan.md](../2026-05-04-stage2e-phase1-plan.md).
- Default-on flip landed as commit `61f6a66`. Justification was
  "Stage 2.D parity passed + power-user testing under
  `MC2_GPU_OBJECTS=1`." Both claims turned out to be misleading:
  parity didn't test the rendering pipeline, and "power-user
  testing" was running on stale-lib binaries with the flag baked off.
- Slice 3 (static-object update bypass) recon + brainstorm exist:
  [docs/superpowers/explorations/2026-05-04-slice3-static-update-bypass-recon.md](../../explorations/2026-05-04-slice3-static-update-bypass-recon.md)
  + [docs/superpowers/brainstorms/2026-05-04-slice3-static-update-bypass-brainstorm.md](../../brainstorms/2026-05-04-slice3-static-update-bypass-brainstorm.md).
  Other session has begun Stage 3.A counter infrastructure (commits
  `7ec9f6d` and `fc53cf9`).
- Slice 2 shipped behind-flag through Stages 2.A, 2.B, 2.C.1, 2.C.2,
  2.C.4, 2.D.1, 2.D.1.1, 2.D.2, 2.D.2.1, 2.D.3. The default-on flip
  is the broken commit.

---

## What the broken state looks like

With current binary (`gpu_objects=1` per startup banner):
- Mission loads. Terrain renders. Mechs render.
- **No buildings, trees, decorations, fences, civilian structures
  render.** Map looks like bare terrain.
- GPU utilization spikes during gameplay (vertex shaders running
  per-vertex per-instance for tens of thousands of instances).
- `[OBJBATCHER v1] event=summary` reports normal-looking submission
  counts: `submitted_instances=20975, gpu_drawn_instances=31502`,
  `submit_buildings=20975, submit_trees=11085`, low fallback rate.
- FPS is ~140 at 4K — high because no actual fragment work happens
  (all triangles clipped before rasterization).

Tracy zone `Render.GpuStaticProps` (added at `txmmgr.cpp:1469-1477`)
shows the flush firing per frame.

With `MC2_GPU_OBJECTS=0` env (forces legacy CPU path):
- **mc2.exe crashes at frame 0** with stack trace:
  ```
  gosRenderer::drawIndexedTris (gameos_graphics.cpp:3963)
  gos_RenderIndexedArray         (gameos_graphics.cpp:4867)
  UpdateRenderers                (mechcmd2.cpp:832)
  draw_screen                    (gameosmain.cpp:518)
  ```
- READ violation at 0x0 — `selectBasicRenderMaterial(curStates_)`
  returns NULL for some `curStates_` combo and the code dereferences
  it. `gosASSERT(mat)` is no-op in RelWithDebInfo.
- This crash **predates this session** — pre-existing rot from the
  slice 2 development period when nobody was running the legacy
  path (because of the stale-lib masking).

So both paths are broken in the current binary. Only fresh-link
binaries pre-dating slice 2 work, OR stale-lib binaries with the
flag baked false.

---

## What we've tried — RenderDoc diagnostic chain

Three captures across the session, each on a `glDrawElementsInstancedBaseVertex`
call from the `Render.GpuStaticProps` Tracy zone in mc2_06 / mc2_18.

### Capture 1 — original shader (before any fix)
- `VS Out gl_Position`: constant `(2.00, 2.00, 2.00, 1.00)` for every
  vertex.
- This is the **behind-camera guard** at `static_prop.vert:141-143`
  firing for every vertex (because `clip4.w < 0.1`).
- Pipeline state otherwise looks clean: framebuffer is the 3840×2160
  R16G16B16A16_FLOAT post-process FBO, depth Less Equal + write on,
  blend off, color mask RGBA, viewport 4K.

### Capture 2 — after changing modelMatrix to `M * v`
Edit at `static_prop.vert:118`:
```glsl
// was:  vec4 world = vec4(a_position, 1.0) * inst.modelMatrix;
// to:   vec4 world = inst.modelMatrix * vec4(a_position, 1.0);
```
- `VS Out gl_Position`: huge varying values, e.g. `(-8.83E+07,
  5.87E+07, 5.03E+07, 5.25E+07)`.
- After perspective divide: NDC ≈ `(-1.68, +1.12, +0.96)` — just
  outside visible cube.
- Some vertices still produced `(2,2,2,1)` (guard still firing).
- **CPU update count dropped from ~869 to ~119 objects per frame**
  in `GameLogic.Units.TerrainObjects` zone — because the modelMatrix
  change made world positions land at correct map locations, so cull
  correctly culled most as off-screen. (This was the only "real"
  improvement signal that the change did something.)

### Capture 3 — after adding axis swap on OUTPUT
Edit at `static_prop.vert:121` added:
```glsl
vec4 clip4_pre = u_worldToClip * world;
vec4 clip4 = vec4(-clip4_pre.x, clip4_pre.z, clip4_pre.y, clip4_pre.w);
```
- `VS Out gl_Position`: huge varying values, different from capture 2
  (e.g. `(2.96E+07, -1.54E+07, 4696746.50, 1.75E+07)`).
- After perspective divide: NDC ≈ `(1.69, -0.88, 0.27)` — still
  off-screen but in different direction.
- **Vertex 0 and vertex 1 produced IDENTICAL NDC** despite different
  `a_position` — gl_Position values were exact scalar multiples
  (~1.93x). This means the projection collapsed all building
  vertices onto the same camera ray. Geometrically degenerate.
- Hot regions across the screen showed all-uniform divergence — not
  concentrated.

### Final state (NOT visually tested)
After reverting modelMatrix change back to original `v * M` while
keeping the axis swap on output. User said "still no buildings"
but no fresh RenderDoc data was shared.

---

## What we've ruled out

1. **Hook position.** Verified `GpuStaticPropBatcher::flush()` is called
   at `txmmgr.cpp:1475` AFTER terrain solid render (line 1462) and
   BEFORE overlays — comment at line 1464-1468 explicitly addresses
   the `memory/render_order_post_renderlists_hook.md` trap. Not the bug.

2. **Depth/blend/cull state.** Explicitly set in flush at
   `gos_static_prop_batcher.cpp:1352-1357`: GL_DEPTH_TEST, GL_LEQUAL,
   GL_TRUE depth mask, GL_BLEND off, GL_CULL_FACE, GL_BACK. Capture
   confirms these reach the rasterizer.

3. **Framebuffer.** Capture confirms drawing to the post-process FBO
   (3840×2160 R16G16B16A16_FLOAT). Same FBO terrain renders to.

4. **Color mask.** Capture confirms RGBA all enabled.

5. **Viewport.** Capture confirms `0,0 3840 2160` matching window.

6. **Shader compile.** CPU update counts dropping from 869→119 between
   captures proves the shader IS recompiling on hot reload (cull system
   uses world position which only changes when shader's transform is
   different).

---

## Active hypothesis space (for next session)

The vertex shader produces `gl_Position` outputs that consistently
land outside `[-1,+1]` NDC. Either:

**(A) Matrix convention bug somewhere in the projection chain.**
Static-prop's chain has 3 matrices and an axis swap to coordinate:
- `inst.modelMatrix` (SSBO, std430 col-major read)
- `u_worldToClip` (uniform, GL_TRUE upload)
- `u_mvp` (uniform, GL_TRUE upload)
- Axis swap (MC2 world → camera frame: `(-x, z, y)`)

Stuff::LinearMatrix4D / AffineMatrix4D / Matrix4D have subtle
storage differences (12 vs 16 floats, conversion at `matrix.cpp:48-58`).
Each upload path has its own transpose/no-transpose convention. I
attempted multiple fixes but kept getting it wrong. Reference: the
SAME projection chain works for terrain (`shaders/terrain_overlay.vert`)
which uses `terrainMVP` (composed `axisSwap * worldToClip` baked in
CPU-side, uploaded GL_FALSE via explicit row-major rewrite at
`gamecam.cpp:150-178`).

**(B) The `s_worldToClip` matrix value differs from gamecam's
`worldToClip`** despite both using `worldToCameraMatrix * cameraToClip`.
Verified both are computed identically (`camera.cpp:2161` and
`tgl.cpp:1596`) but didn't verify they hold identical VALUES at
draw time. Could differ if one is updated stale from a prior frame.

**(C) Vertex stream binding mismatch.** SSBO 0 (Instances) shows
`Buffer 67440, 112 bytes at offset 959488 bytes` — verified in
RenderDoc. The 112-byte instance struct + offset look correct. But
maybe the per-vertex VBO attribute layout is reading from the wrong
columns, producing positions that look reasonable in isolation but
aren't what the C++ thinks it submitted.

**(D) GLSL shader output stage problem unique to this configuration.**
Some AMD RX 7900 XTX driver quirk per
`docs/amd-driver-rules.md` that we haven't checked.

The "vertex 0 and vertex 1 produce same NDC" observation from capture 3
is the load-bearing weird signal. For a normal projection, two distinct
vertices should map to different NDC even on the same camera ray. The
exact-scalar-multiple gl_Position outputs suggest the projection is
producing degenerate results — **collapsing 3D vertex inputs onto a
single camera-ray direction**. Either the matrix is singular, or
matrix multiplication is in the wrong order so that vertex info is
washing out into the homogeneous w coordinate.

---

## Recommended next-step diagnostic (one-line shader test)

Replace the entire vertex transform in `static_prop.vert` with:

```glsl
// TEMPORARY DIAGNOSTIC — force every vertex to screen center.
gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
```

(Comment out the existing `vec4 world = ...` through `gl_Position =
vec4(ndc.xyz * absW, absW);` block. Keep the rest.)

Deploy the shader. Launch the game. **If you see colored pixels
at the screen center**, the rasterizer is reachable and the entire
bug is in the vertex transform chain. Pursue (A)/(B). **If still
nothing visible**, there's a deeper issue (wrong framebuffer
attachment, fragment discard, stencil rejection, blend state) that
the RenderDoc captures didn't catch. Pursue (D) or escalate.

This is a 5-minute test that conclusively narrows the search space.

---

## Recommended structural fix (option A from the prior session)

Eliminate the parallel projection chain entirely. Make static-prop
use the EXACT same `terrain_mvp_` matrix that terrain uses:

1. **Engine side:** in `gos_static_prop_batcher.cpp::flush()`, replace
   the upload of `s_worldToClip` (currently at line 1372 with GL_TRUE)
   with binding the SAME matrix value and SAME upload convention that
   terrain uses (`gameos_graphics.cpp:3572`, with GL_FALSE on the
   row-major-rewritten `terrain_mvp_`). Need to expose terrain_mvp_
   from gameos_graphics.cpp via accessor.
2. **Shader side:** rename `u_worldToClip` to match terrain's
   `terrainMVP` (or just bind both to the same uniform location).
   Drop the axis swap (it's now baked into terrain_mvp_ on the C++ side).

This eliminates 3 of 4 convention layers (modelMatrix is still its own
SSBO read, but that one was working in the original shader pre-flip).

---

## Files to read (in order, for context)

1. `.claude/worktrees/nifty-mendeleev/CLAUDE.md` — load-bearing project
   rules. Especially "Critical Rules", "Documentation Discipline",
   "Tier-1 Instrumentation Env Vars", "Smoke Gate command".
2. `~/.claude/projects/A--Games-mc2-opengl-src/memory/MEMORY.md` —
   index of all memory notes. Especially:
   - `parity_finds_gpu_substrate_bugs_visual_smoke_misses.md` — the
     parity-doesn't-test-rendering lesson that should have predicted
     this bug.
   - `msvc_incremental_link_silent_staleness.md` — written this
     session; explains how the bug stayed hidden.
   - `feedback_inherited_citations_must_regrep.md` — also written this
     session.
   - `terrain_mvp_gl_false.md` — terrain MVP convention reference.
   - `cpp_glsl_ubo_struct_lockstep.md`, `gpu_direct_depth_state_inheritance.md`,
     `render_order_post_renderlists_hook.md` — related convention traps.
3. `shaders/static_prop.vert` — the broken vertex shader (current state
   has my unsuccessful fixes applied; verify against original via
   `git diff`).
4. `shaders/terrain_overlay.vert` — the WORKING reference shader. Same
   projection chain shape, different uniform bindings.
5. `code/gamecam.cpp:150-178` — terrain MVP composition (axisSwap +
   row-major rewrite + `gos_SetTerrainMVP`).
6. `GameOS/gameos/gos_static_prop_batcher.cpp:1227-1450` — the
   broken `GpuStaticPropBatcher::flush()` body. Matrix uploads at
   1372/1382.
7. `GameOS/gameos/gos_static_prop_batcher.cpp:760-810` — `submit()`
   that packs SSBO instance records.
8. `GameOS/gameos/gameos_graphics.cpp:3572` — terrain's MVP upload
   site (`GL_FALSE` upload of `terrain_mvp_`).
9. `mclib/tgl.cpp:1589-1597` — `TG_Shape::SetCameraMatrices` that
   computes `s_worldToClip = s_worldToCamera * s_cameraToClip`.
10. `mclib/camera.cpp:2157-2161` — `Camera::worldToClip` computation
    that terrain uses.
11. `mclib/stuff/matrix.{hpp,cpp}` — Stuff::Matrix4D layout (col-major
    storage of row-vec convention; `entries[col*4+row]`).
12. `mclib/stuff/affinematrix.hpp` — AffineMatrix4D (12 entries,
    same access pattern, no col 3).
13. `mclib/stuff/linearmatrix.hpp` — LinearMatrix4D extends
    AffineMatrix4D.
14. `GameOS/gameos/gos_static_prop_batcher.cpp:1092-1093` — the
    LinearMatrix4D → Matrix4D conversion site that creates `xform`
    passed to `submit()`.

---

## Worktree state when handed off

```
git status:
  M code/missiongui.cpp                                 # other session WIP (pre-existing)
  M GameOS/gameos/gameos_graphics.cpp                   # other session WIP
  M GameOS/gameos/gameosmain.cpp                        # other session WIP
  M GameOS/gameos/gos_terrain_indirect.cpp              # other session WIP
  M GameOS/gameos/gos_terrain_indirect.h                # other session WIP
  M code/mechcmd2.cpp                                   # other session WIP
  M mclib/terrain.cpp                                   # other session WIP (defines g_terrainMaterialProfile)
  M mclib/terrain.h                                     # other session WIP
  M shaders/gos_terrain.frag                            # other session WIP
  M shaders/include/terrain_common.hglsl                # other session WIP
  M shaders/static_prop.vert                            # MY EDITS — modelMatrix reverted, axis swap on output added
  M scripts/build_release.sh                            # pre-existing
  M docs/superpowers/plans/progress/2026-05-01-...      # pre-existing dirty doc

git stash list:
  stash@{0}: Stage 2.E auto-skip on inMovieMode entry (paused 2026-05-04)
             # forceMovieToEnd auto-skip in code/missiongui.cpp; for Stage 2.E revival
```

**Do NOT clobber the other session's WIP** in working tree. They
include the `g_terrainMaterialProfile` definition needed by
`gameos_graphics.cpp:42`'s extern. If you need to switch HEAD or do
clean rebuilds, stash it first per the pattern from this session:
```bash
git stash push -m "session-handoff: WIP preservation" -- \
  GameOS/gameos/gameos_graphics.cpp GameOS/gameos/gameosmain.cpp \
  GameOS/gameos/gos_terrain_indirect.cpp GameOS/gameos/gos_terrain_indirect.h \
  code/mechcmd2.cpp mclib/terrain.cpp mclib/terrain.h \
  scripts/build_release.sh shaders/gos_terrain.frag \
  shaders/include/terrain_common.hglsl
```
Then `git stash pop` to restore when done.

---

## Build / deploy / verification recipe

```bash
# Build (per worktree CLAUDE.md — RelWithDebInfo mandatory):
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" \
  --build build64 --config RelWithDebInfo --target mc2

# CRITICAL: if you change a global initializer (like flipping a default
# bool), force a full link to avoid MSVC incremental-link silent
# staleness (see memory/msvc_incremental_link_silent_staleness.md):
rm -f build64/RelWithDebInfo/mc2.exe build64/RelWithDebInfo/mc2.lib \
      build64/RelWithDebInfo/mc2.exp \
      build64/out/GameOS/gameos/RelWithDebInfo/gameos.lib
# Then rebuild. Watch for "performing full link" in output.

# Deploy (per-file cp -f + diff -q, NEVER cp -r):
cp -f build64/RelWithDebInfo/mc2.exe "A:/Games/mc2-opengl/mc2-win64-v0.3/mc2.exe"
cp -f build64/RelWithDebInfo/mc2.pdb "A:/Games/mc2-opengl/mc2-win64-v0.3/mc2.pdb"
diff -q build64/RelWithDebInfo/mc2.exe "A:/Games/mc2-opengl/mc2-win64-v0.3/mc2.exe"

# Shader-only changes don't need rebuild — just deploy:
cp -f shaders/static_prop.vert "A:/Games/mc2-opengl/mc2-win64-v0.3/shaders/static_prop.vert"

# Verify the gpu_objects flag value at runtime (this would have caught the
# stale-lib problem earlier — runtime banner reads the live symbol):
cd "A:/Games/mc2-opengl/mc2-win64-v0.3" && \
  MC2_SMOKE_MODE=1 ./mc2.exe --mission mc2_06 --duration 6 2>&1 | \
  grep "INSTR v1"
# Expected output: ... gpu_objects=1 ...

# Confirm OBJBATCHER is exercising the GPU path:
cd "A:/Games/mc2-opengl/mc2-win64-v0.3" && \
  MC2_SMOKE_MODE=1 ./mc2.exe --mission mc2_06 --duration 6 2>&1 | \
  grep "OBJBATCHER"
# Expected: event=summary lines with non-zero gpu_drawn_instances.
# IMPORTANT: this only proves SUBMISSION, not VISIBLE RENDERING.
```

---

## RenderDoc capture recipe (since shader debugger errored on user's setup)

1. Open RenderDoc → File → Launch Application
2. Executable: `A:\Games\mc2-opengl\mc2-win64-v0.3\mc2.exe`
3. Working Directory: `A:\Games\mc2-opengl\mc2-win64-v0.3`
4. Defaults otherwise. Launch.
5. In-game: load mc2_06 (no intro, fastest to gameplay state).
6. Wait until command-position camera settles (~3-5s in-mission).
7. F12 to capture one frame.
8. Quit MC2 normally.
9. Open the capture in RenderDoc.

Useful diagnostics:
- **Event Browser** → look for events tagged `Render.GpuStaticProps`.
  Click any `glDrawElementsInstancedBaseVertex(...)` call.
- **Pipeline State** → click VS box. Bottom shows "Read/Write Bindings"
  (SSBO list). Click 🔗 next to "Buffer 67440" (Instances) to inspect
  raw bytes.
- **Mesh Viewer** → "VS Out" sub-tab shows post-vertex-shader
  `gl_Position` per vertex.
- **Pipeline State → Output Merger** → shows framebuffer, blend, depth
  state.

The shader debugger ("Debug this vertex" right-click) errors on user's
hardware. Don't rely on it.

---

## Two practical paths from here

### Path 1: One-line diagnostic before any more code

Replace `static_prop.vert` body with `gl_Position = vec4(0.0, 0.0,
0.0, 1.0);` for every vertex. Deploy, launch, see if anything renders
at screen center. This conclusively answers "is the rasterizer
reachable" and narrows the search space in 5 minutes. Strongly
recommend doing this FIRST.

### Path 2: Structural fix — bind terrain's `terrain_mvp_` directly

Change static-prop to use the SAME matrix terrain does (with the
axisSwap pre-baked CPU-side). Eliminates parallel convention chains.
~30 min of code change. Outline above in "Recommended structural
fix" section.

---

## Honest assessment of prior session's work

The prior session (this one) made multiple shader-side fix attempts
that did not converge:

1. Changed modelMatrix from `v * M` to `M * v` — produced varying
   `gl_Position` outputs but still off-screen. The CPU update count
   drop (869 → 119) proved the change had effect, but visually
   nothing better.
2. Added axis swap on input (`world` axis-swapped before
   `u_worldToClip * world`) — wrong order; matrix multiplication
   non-commutative. No improvement.
3. Moved axis swap to output (`clip4` axis-swapped after
   `u_worldToClip * world`) — mathematically equivalent to terrain's
   CPU-side axisSwap composition. Output changed but still off-screen.
4. Reverted modelMatrix back to `v * M` after deeper read of Stuff
   matrix conversion path showed `M * v` was wrong. Visual outcome
   "still no buildings" without fresh RenderDoc data.

The matrix convention analysis was **inconsistent across iterations**
— I (the prior session AI) flipped between `M * v` and `v * M`
verdicts based on each new piece of evidence. The current shader
state has `v * M` for modelMatrix + axis swap on output, which
matches my latest read of the conversion path but is unverified.

If you're tempted to "just try one more shader edit" without doing
the diagnostic test in Path 1, **don't**. The prior session's
iterations show that without ground-truth on whether the rasterizer
is even reachable, blind shader edits don't converge.

---

## Out-of-scope for next session

- Stage 2.E revival (paused arc; mech idle anim determinism is its own
  separate problem).
- Slice 3 (other session is already on it).
- Fixing the legacy CPU path crash at `gameos_graphics.cpp:3963`
  (separate bit-rot bug; only matters if you want a working
  `MC2_GPU_OBJECTS=0` opt-out).
- Reverting the default-on flip (commit `61f6a66`) — DO NOT do this
  without explicit user direction. Currently the default-on flip is
  in but the binary doesn't render correctly. User explicitly chose
  to debug rather than revert ("I don't need to ship anything today
  this isn't a scrum session").

---

## Memory notes written this session worth reading

- `~/.claude/projects/A--Games-mc2-opengl-src/memory/feedback_inherited_citations_must_regrep.md`
  — fired during plan reviews; planner copied citations from review
  subagent without re-grepping; failure mode that wasted multiple
  review rounds on Stage 2.E.
- `~/.claude/projects/A--Games-mc2-opengl-src/memory/msvc_incremental_link_silent_staleness.md`
  — fired discovering the slice 2 default-on regression; explains why
  global initializer flips can fail to reach the binary; includes
  prevention checklist.

---

## Communication style for next session

User has been collaborative through this session but ran low on
patience by the end (multiple iterations without convergence). Be
DIRECT. Do not catastrophize. Do not propose more shader edits
without first running the Path 1 diagnostic. If RenderDoc captures
are needed, give exact navigation steps (the shader debugger errors
on this hardware). When the user shares ambiguous data ("no change",
"still no buildings"), ask one specific clarifying question rather
than guessing.

The user explicitly chose "no need to ship anything today" — this is
a deep debug, not a sprint. Take time to verify each assumption
before changing code.
