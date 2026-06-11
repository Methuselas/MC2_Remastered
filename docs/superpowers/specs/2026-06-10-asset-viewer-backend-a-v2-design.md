# Asset Viewer Backend-A v2 — Engine-Shader Static-Prop Preview — Design

**Date:** 2026-06-10
**Worktree:** `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev`
**Tool:** `tools/asset_viewer/` (`mc2_asset_viewer`)
**Predecessor:** Asset Viewer S5 polish (shipped, nifty `7e4abed9`). MVP + mod-workbench before that.

## North star

> **Backend-A v2 = shader-faithful static-prop preview, NOT pixel-exact in-mission preview.**

Render a single static prop with the *real* engine static-prop shaders + real material
albedo + a representative standalone light rig, behind a UI toggle vs the existing
Backend-B approximation. The fidelity anchor is the actual `shaders/static_prop.{vert,frag}`
loaded from the repo — if they change and the preview breaks, that is a *useful signal*,
not a regression.

Explicitly out of scope for v2 (these are **v3 seams, documented, not bugs**): per-mission
`ObjectLights` rig, shadow cascade, fog matching, hot-window magic-color lights, any
mission-load context. The asset viewer must NOT need a loaded mission to preview a prop.

## Naming

The `PreviewSurface.h` comment reserved the name `ModelPreviewRenderCore`. We use
**`ModelPreviewEngineShader`** instead — it is shader/pipeline-state-faithful, not a
RenderCore-pipeline-registry/batcher consumer, and the honest name prevents future
confusion. Update the `PreviewSurface.h` comment accordingly with the note:

> "RenderCore-faithful here means shader + pipeline-state faithful (real `static_prop`
> shaders, matching depth/blend/cull), NOT full engine scene render (no batcher, no
> mission lights, no shadow cascade)."

## Architecture (the locked call)

**Reference the engine shaders; stub the scene-coupled bindings; do NOT link RenderCore or the batcher.**

Verified by recon against the in-game path:
- The engine static-prop draw is `gos_static_prop_batcher.cpp:5347` (`glDrawElementsInstancedBaseVertexBaseInstance`) using `shaders/static_prop.vert` + `shaders/static_prop.frag` via `PipelineId::StaticPropOpaque`.
- Those shaders are **self-contained** for: vertex pos/normal/uv transform, material-array albedo sampling, alpha-test, and a directional+ambient light.
- They are **scene-coupled** for: `ObjectLights` SSBO (binding 20, CPU-baked per-mission via `GatherLightsParameters`/`txmmgr.cpp::renderLists`), shadow map + `lightSpaceMatrix` (`gos_postprocess.cpp`), fog, hot-window lights. None exist in a standalone tool.
- `pipeline_binder::applyPipeline(PipelineDesc(StaticPropOpaque))` only sets depth/blend/cull GL state (`PipelineDesc.h:50-71`: depth GREATER_EQUAL reverse-Z, write on, cull back, opaque blend). Trivially replicated with a few `glEnable`/`glDepthFunc` calls — no need to link RenderCore.

`ModelPreviewEngineShader` therefore:
1. **Loads the actual `shaders/static_prop.{vert,frag}`** (not a copy) at runtime from a shader root (deploy `shaders/` if present, else repo `shaders/`). They cannot drift from the engine.
2. **Compiles them in a MINIMAL config** (preprocessor `#define`s chosen to take the lightest paths): legacy `u_worldToClipGL` uniform path (NOT `MC2_USE_VIEW_UNIFORMS` UBO), single `sampler2D u_tex` (NOT `sampler2DArray`/coalesce), material-GPU sampling off, PBR-specular `u_pbrV1Strength=0`. The exact define set is captured by the shader-contract report (below) and pinned in the plan after an empirical compile.
3. **Satisfies the scene bindings with stubs (the v3 seam):** a 1-entry `ObjectLights` SSBO at binding 20 = ambient + one infinite directional; a 1×1 white `sampler2DShadow`; `u_fogValue=1.0`; hot-window tags fed as zero. These are isolated in a `StandaloneSceneStubs` unit so v3 can swap them for real mission data without touching the render path.
4. **Reshapes existing `MeshGpu` geometry** to the `static_prop.vert` layout (loc0 pos3, loc1 normal3, loc2 uv2, loc3 `a_localVertexID`, loc4 `a_aRGBLight`). The two integer tags are fed as constants (0) — they drive hot-window/magic-color paths that are stubbed off. Reuses `TglMeshLoader` + `MeshGpu` (no second loader).
5. **Renders into the same FBO → `ImGui::Image` path as Backend-B** (`MeshPreview3D`'s plumbing pattern). Same orbit/zoom controls. NO second preview UI stack.

### GLSL include resolution

`static_prop.{vert,frag}` `#include` shared files (`include/view_uniforms.hglsl`,
`include/shadow.hglsl`, `include/lighting.hglsl`, `render_contract.hglsl`, etc.). The
asset viewer currently compiles only inline shader strings (no include support). v2 adds
a **minimal GLSL include preprocessor** (`ShaderIncludeResolver`) rooted at the real
`shaders/` directory — recursively inlines `#include "..."` relative to the shader root.
**Do NOT copy includes into `tools/asset_viewer/`** — resolve against the engine's real
`shaders/` tree so they stay in sync. If the engine already exposes a reusable include
resolver that links cleanly into the tool, prefer it; otherwise the tool-local resolver
is a ~50-line recursive text-substitution pass (cycle-guarded).

## Components (new units, clear boundaries)

| Unit | Responsibility | Depends on |
|------|----------------|-----------|
| `ModelPreviewEngineShader.{h,cpp}` | `PreviewSurface` impl: compile engine shaders, bind stubs + material + geometry, render to FBO | `MeshGpu`, `ShaderIncludeResolver`, `StandaloneSceneStubs`, `ShaderContractReport` |
| `ShaderIncludeResolver.{h,cpp}` | Recursively inline `#include` from the `shaders/` root; cycle-guarded; report unresolved includes | stdlib/filesystem |
| `StandaloneSceneStubs.{h,cpp}` | Build the 1-entry ObjectLights SSBO, 1×1 white shadow tex, fog/hot-window constants — the v3 seam | GL |
| `ShaderContractReport` (struct + a draw fn) | Capture + display: shader paths, compile/link status + logs, active defines, declared-but-unset uniforms/bindings, texture mode, which stubs are active | ImGui |
| `MeshPreview3D` (existing) | unchanged Backend-B; the toggle picks between the two `PreviewSurface`s | — |
| Static-prop panel (`ModelBrowser` host) | adds a backend toggle + contract report; default Backend-B | both previews |

`ModelPreviewEngineShader.cpp` is the largest; if it exceeds ~250 lines split the
shader-build half into `ModelPreviewEngineShader_compile.cpp`.

## Data flow

`select prop → TglMeshLoader → MeshGpu (reshaped to static_prop layout) →`
`ModelPreviewEngineShader.render(): bind program (engine shaders, minimal defines) +`
`u_worldToClipGL (orbit cam) + u_tex (prop albedo) + ObjectLights stub SSBO(b20) +`
`1×1 shadow + u_fogValue=1 → draw into FBO → ImGui::Image`. The backend toggle swaps
which `PreviewSurface` the panel draws; geometry/camera state is shared.

## Error handling — FAIL OPEN to Backend-B

Backend-A must never crash the viewer. On any failure:
- **Shader file missing / include unresolved / compile or link error** → capture the log
  into the contract report, show it in the UI, and **fall back to rendering Backend-B**
  for that frame. The toggle stays on "Backend-A (error)" so the user sees the report.
- **Missing/failed albedo texture** → render with a flat placeholder (Backend-A still
  draws; texture-missing is surfaced in the report), never crash.
- All GL objects RAII/guarded; `glGetError` drained and reported, not asserted.

## UI

- Backend toggle, **default Backend-B**:
  `Preview backend: ( ) Backend-B: Approximate  ( ) Backend-A: Engine Shader`
- A caption under Backend-A (no pixel-parity claims):
  `"Engine shader preview: real static_prop shader with standalone lighting stubs. Not mission-lighting exact."`
- A collapsible **Shader Contract** report panel (the fields above).

## UV-V convention verification

The MVP left an open question: the engine importer (`mclib/assimp_importer.cpp`) does a
manual `1-v` flip; the workbench glTF loader does not. With a textured engine-shader
preview now available, v2 **includes** an empirical UV-V check: load one known-textured
prop, confirm the albedo lands right-side-up under `static_prop.frag`'s UV usage; if the
V is flipped vs in-game, document the convention and fix the tool's UV handling. Only
defer if the textured preview cannot be made to render at all (then say so explicitly).

## Testing — GL-aware where needed

Backend-A inherently needs a GL context, so its smoke uses the existing offscreen-FBO
test-hook pattern (`MeshPreview3D::renderToPixels` already does headless FBO render).

```
--smoke-backend-a-compile <shaderRoot>
   resolve + compile + link the real static_prop shaders in the minimal config;
   assert program != 0, link OK; print active define set + any unresolved includes.
--smoke-backend-a-render <deploy>
   headless: load one known prop, render Backend-A into an FBO, assert the frame is
   non-empty (not all-clear-color) and GL-clean.
--smoke-backend-a-fallback
   point at a bad/nonexistent shader path; assert compile fails gracefully, the
   contract report records the error, and the fallback path (Backend-B) renders — no crash.
```
If a CI box lacks a GL context, these are gated like the existing GL smokes
(`--smoke-render`, `--smoke-mesh-render`) which already run against the deploy dir.

## Acceptance criteria

1. Asset viewer launches; Backend-B preview unchanged (default).
2. Backend-A can be toggled on.
3. Backend-A compiles the real `static_prop.{vert,frag}` from the repo/deploy shader dir (via the include resolver).
4. Backend-A renders at least one known textured static prop, non-empty frame.
5. Missing shader/texture → visible error in the contract report + fallback to Backend-B, no crash.
6. Existing 26/26 smokes remain green (no regression).
7. New Backend-A smokes pass: compile/link succeeds; FBO render non-empty; fallback works on a bad shader path.
8. UV-V verification done (or explicitly deferred only if the textured render is impossible).

## v2 / v3 split (documented)

```
v2 (this spec): Shader-faithful standalone preview
  - real static_prop.vert/frag (referenced, not copied)
  - real material albedo where feasible
  - representative ambient + directional light
  - NO mission context, NO shadow cascade, NO fog matching, NO hot-window lights
  - NO pixel-exact promise

v3 (future): Mission-context preview
  - real ObjectLights, real shadow/fog context, optional mission/load context
  - closer to in-game screenshot parity
  - swaps StandaloneSceneStubs for real feeders (the seam v2 leaves)
```

## Build / run

Unchanged toolchain (VS2022 `build64`, exe `build64/out/tools/asset_viewer/RelWithDebInfo/`).
New `.cpp`s added to the `mc2_asset_viewer` target. The tool gains a runtime dependency
on a readable `shaders/` root (deploy `shaders/` or the repo `shaders/`) — surfaced in the
contract report if absent. No new link against RenderCore/engine TUs (shaders are loaded
as text, not linked).

## Out of scope

- Linking RenderCore PipelineRegistry/`pipeline_binder`/`gos_static_prop_batcher` (state replicated, not linked).
- Mechs/vehicles/effects previews (static props only for v2).
- Shadow/fog/mission-light fidelity (v3).

## Non-regression

S5 + MVP smokes (26/26) stay green. Backend-B is the default and untouched; Backend-A is
purely additive behind the toggle.
