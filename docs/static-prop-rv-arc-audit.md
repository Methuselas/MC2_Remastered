# StaticPropOpaque R-to-V Arc Audit

Date: 2026-05-28  
Worktree: `A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev`  
Branch: `claude/nifty-mendeleev`  
Mode: read-only audit plus this report. No code/shader/default/tuning changes.

## Verdict

YELLOW after remediation.

The default StaticPropOpaque lane still renders and remains coherent. The original audit found one HIGH contradiction in the PBR path plus several MEDIUM documentation/registry gaps; remediation commits below fixed the HIGH and required registry/docs issues. PBR remains visually experimental on flat legacy roofs, so the lane is a good R-to-V architecture template but not a final visual-quality template.

## Remediation

| Slice | Commit | Status |
|---|---|---|
| PBR-WINDOW-BYPASS-FIX | `15a3336c` | Fixed. Fragment PBR now reuses existing `v_flags` bit 1 and skips specular for window/hot-pink nodes. |
| STATICPROP-ENV-DOC-REGISTRY-FIX | `453d7fbf` | Fixed. `MC2_STATIC_PROP_IBL_SH_STRENGTH` is registered as an aux tune env; PBR/IBL/debug docs and smoke comments match current behavior. |
| IBL-REGISTRY-EXTENSION-SAFETY | `a0fc4d7f` | Fixed. Mission SH map lookup is counted and ignores null placeholders, so future mappings are reachable. |
| STATICPROP-ARC-AUDIT-UPDATE | pending in this report commit | Updates verdict and preserves original findings for traceability. |

Validation after remediation:

| Check | Result |
|---|---|
| Build `mc2` RelWithDebInfo | PASS after slices 1, 2, and 3 |
| Static-prop shader reflection | PASS for `static_prop.vert` and `static_prop.frag` variants after slice 1 |
| Tier1 default smoke | PASS 5/5 after slice 1 |
| `MC2_STATIC_PROP_PBR_V1=1` smoke | PASS `mc2_01` after slice 1 |
| `bash scripts/check-env-registry.sh` | PASS after slice 2 |
| `MC2_STATIC_PROP_IBL_SH=1` smoke | PASS `mc2_01` after slice 3 |

## Executive Summary

StaticPropOpaque is still the strongest R-to-V reference lane in the worktree: RenderWorld handles feed RenderSnapshot rows, snapshot-built DrawPacket/meta is default authority, PipelineDesc is registered and applied, ViewUniforms is default-on with a legacy kill switch, MaterialGpu layout is ABI-locked, and capture/debug surfaces exist.

After remediation, the core Track R architecture is sound enough to copy as a reference pattern. The remaining caution is visual: PBR is still experimental and should not be treated as final art direction for other lanes.

## Top Findings

| Severity | Finding | Evidence | Recommendation |
|---|---|---|---|
| HIGH | PBR no longer bypasses window/hot-pink nodes. Vertex lighting skips window nodes, but fragment PBR applies specular after texture/highlight when `u_pbrV1Strength > 0.0` and does not check `v_flags`. | `shaders/static_prop.vert:334-337`, `shaders/static_prop.frag:335-378` | Fixed in `15a3336c`. |
| MEDIUM | PBR registry/docs are stale after per-fragment PBR move. They still describe per-vertex PBR, roughness=1.0, constant F0, and deferred MaterialGpu lookup, while shader now does per-fragment PBR, roughness fallback 0.6, MaterialGpu metallic/roughness, and albedo-tinted F0. | `RenderCore/RendererFeatureRegistry.h:323-329`, `docs/tier1_env_vars.md:96-99`, `shaders/static_prop.frag:315-378` | Fixed in `453d7fbf`. |
| MEDIUM | `check-env-registry.sh` fails because `MC2_STATIC_PROP_IBL_SH_STRENGTH` is used and smoke-allowlisted but not registered or allowlisted in registry enforcement. | validator run; `GameOS/gameos/gos_static_prop_batcher.cpp:195-209`, `scripts/run_smoke.py:482-486` | Fixed in `453d7fbf`. |
| MEDIUM | `IblShRegistry.h` has the null sentinel as the first mission-map row, so future appended rows after it will never resolve. Current empty map is safe, but the extension pattern is a trap. | `RenderCore/IblShRegistry.h:50-52`, `RenderCore/IblShRegistry.h:86-98` | Fixed in `a0fc4d7f`. |
| LOW | Several debug/docs comments are stale: `ibl_sh_runtime.h` says IBL strength default 1.0, smoke comments still mention debug material modes 1..4, tier1 docs omit several live static-prop envs. | `GameOS/gameos/ibl_sh_runtime.h:5-12`, `scripts/run_smoke.py:474-486`, `docs/tier1_env_vars.md` | Required static-prop env docs/comments fixed in `453d7fbf`; remaining older docs may still mention pre-remediation state. |
| LOW | `shader_reflect` has unrelated terrain drift. Static-prop reflect variants passed. | validator run: `shaders/gos_terrain.frag` type name drift only | Track separately; not a StaticPropOpaque blocker. |

## Authority Chain

| Stage | Owning files | Current authority | Fallback path | Validation/log/debug | Duplicate/contradictory authority |
|---|---|---|---|---|---|
| Game static prop/building | `mclib/bdactor.cpp`, `mclib/gvactor.cpp`, `code/objmgr.cpp` | Actor/appearance registers recipes and marks visible through `GpuStaticPropRegistry`. | CPU draw/fallback when GPU static props off or submit fails. | population/fallback counters; gameplay pick logs. | Legacy visibility still exists beside RenderWorld registration. |
| RenderWorld handle/object record | `RenderWorld/RenderWorld.h`, `RenderWorld/RenderWorld.cpp`, `GameAdapters/StaticPropRenderAdapter.cpp` | `RenderWorld::upsertStaticProp` owns stable handle/generation/object id view. | `adoptStaticPropRecipe` for late spawn/legacy recipe adoption. | RenderWorld logs and inspector lookup. | Record `pipelineId` comments still describe some fields as documentary in older M1.5 language. |
| RenderSnapshot static prop row | `GameOS/gameos/render_snapshot.h`, `GameOS/gameos/render_snapshot.cpp` | `ExtractRenderSnapshot()` snapshots alive RenderWorld slots and registry material/type/light facts. | Missing optional facts become sentinels; validation failures skip row. | `[RENDER_SNAPSHOT]` log includes validation, material, cull, packet, build counters. | Snapshot rows are frame-lifetime; dispatch still keeps live-built arrays for compare/fallback. |
| DrawPacket/meta | `RenderCore/DrawPacket.h`, `GameOS/gameos/static_prop_dispatch_meta.h`, `GameOS/gameos/gos_static_prop_batcher.cpp` | v6 packet/meta arrays are default dispatch; snapshot-built arrays are default when compare passes. | `MC2_STATIC_PROP_LEGACY_DISPATCH=1` returns to legacy MDI; snapshot mismatch falls back to live-built arrays. | `[DRAW_PACKET_V6]`, snapshot build mismatch counters. | Live builder remains fallback and compare authority. This is intentional but must be described as dual-authority-with-fallback. |
| PipelineDesc/applyPipeline | `RenderCore/PipelineDesc.h`, `RenderCore/PipelineRegistry.cpp`, `GameOS/gameos/pipeline_binder.cpp` | `PipelineId` 1/2 maps to StaticPropOpaque/AlphaTest PipelineDesc; `applyPipeline()` owns GL state. | Invalid id returns null desc; legacy path can still bind direct program state. | Pipeline State inspector and registry static asserts. | `PipelineRegistry.cpp` comment says object-id false, but object-id variants exist via shader prefix; PipelineDesc does not yet model object-id variant dynamically. |
| MaterialGpu row | `RenderCore/MaterialGpu.h`, `shaders/include/material_gpu.hglsl`, `gos_static_prop_batcher.cpp` | C++ `MaterialGpu` is ABI authority; Material table default-on; albedo sampling default-on if table valid. | `MC2_MATERIAL_GPU=0` disables table; `MC2_MATERIAL_GPU_SAMPLE=0` or invalid sidecar falls back to `texArrayLayer`. | mirror script, shader_schema, reflect goldens, material inventory, `[MATERIAL_GPU v4]`. | Debug roughness/metallic modes read zeros when sampling off, which is safe but can mislead if user expects CPU MaterialGpu inventory values. |
| ViewUniforms/EngineView | `RenderCore/ViewUniforms.h`, `RenderCore/EngineView.h`, `GameOS/gameos/view_uniforms_gl.cpp`, `code/gamecam.cpp` | `gamecam.cpp` fills MainScene EngineView every frame; UBO binding 3 is default-on unless `MC2_VIEW_UNIFORMS=0`. | Shader prefix omits `MC2_USE_VIEW_UNIFORMS`; shader uses legacy `u_worldToClipGL`. | `[VIEW_UNIFORMS v1]`, schema validation, inspector view state. | `view_uniforms_gl.h/cpp` comments still say default OFF in places while actual static_prop prefix/upload path is default ON. |
| Vertex shader | `shaders/static_prop.vert`, `shaders/include/lighting.hglsl`, `shaders/include/view_uniforms.hglsl` | VS owns transform, calc_light, ambient/IBL, window lighting skip, PBR sun discovery varyings. | Legacy uniform matrix path when ViewUniforms off; coalesce vs non-coalesce instance indexing. | shader_reflect variants pass for static_prop.vert. | PBR moved to frag but several docs/registry still describe VS PBR. |
| Fragment shader | `shaders/static_prop.frag`, `shaders/include/material_gpu.hglsl` | FS owns albedo sampling, alpha test, material debug, PBR specular, object-id output variants. | `texArrayLayer` fallback; PBR block compiled out without ViewUniforms; debug off returns normal path. | shader_reflect variants pass for static_prop.frag. | Remediated: PBR now honors window skip via existing `v_flags`. |
| GL dispatch | `gos_static_prop_batcher.cpp`, `pipeline_binder.cpp` | v6 DrawPacket/meta default dispatch with PipelineDesc program state. | legacy MDI and v5 diagnostic loop still present. | v6/v5 summaries, GL error counters, MaterialGpu sample logs. | Legacy dispatch still uploads new visual uniforms; safe fallback. |
| Inspector/debug/capture | `GuiRuntime/EditorInspector.cpp`, `scripts/capture_baseline.py`, `scripts/run_smoke.py` | Inspector shows Render Spine, Pipeline State, visual controls, material inventory; capture records key envs. | Env gates remain process-start authority; sliders modulate only uploaded strength when gate active. | ImGui panels, capture metadata, smoke allowlist. | Docs and smoke comments lag modes/defaults; capture env list omits material debug mode. |

## Feature Gate Matrix

| Env var | Default | Disable/kill switch | Registry | Smoke allowlist | Docs/tier1 | ImGui relation | Slider bypass risk | Validation |
|---|---:|---|---|---|---|---|---|---|
| `MC2_VIEW_UNIFORMS` | ON | `=0` | feature row | yes | listed | no direct control; PBR interlock | no | shader_schema; reflect variants |
| `MC2_SNAPSHOT_STATIC_PROP_BUILD` | ON | `=0` | feature row | yes | listed | none | no | snapshot build counters |
| `MC2_STATIC_PROP_LEGACY_DISPATCH` | OFF as kill switch; packet path ON | `=1` forces legacy dispatch | feature row | yes | listed | feature state panel | no | v6 logs, snapshot compare |
| `MC2_MATERIAL_GPU` | ON | `=0` | feature row | yes | listed | material panels indicate inactive/empty | no | mirror, schema, MaterialGpu logs |
| `MC2_MATERIAL_GPU_SAMPLE` | ON | `=0` | feature row | yes | listed | no direct slider | no | sampleOn log |
| `MC2_STATIC_PROP_AMBIENT_V1` | OFF | unset/`=0` | feature row | yes | listed | text/debug status only | no | uniform strength 0/1 |
| `MC2_STATIC_PROP_IBL_SH` | ON | `=0` | feature row | yes | listed | strength slider and active set display | no; slider uploads 0 when env off | uniform strength gate |
| `MC2_STATIC_PROP_IBL_SH_STRENGTH` | 0.5 | knob only | aux row | yes | listed | initial `g_iblShStrength` | parent gate prevents bypass | env registry PASS |
| `MC2_STATIC_PROP_IBL_SH_SET` | unset -> registry/default | knob only | aux row | yes | not in tier1 | active set display | not a gate | no direct validator |
| `MC2_STATIC_PROP_PBR_V1` | OFF | unset/`=0`; also zeroed if ViewUniforms off | feature row | yes | listed | strength + roughness controls | no; slider hidden/disabled when env off | reflect/schema compile |
| `MC2_STATIC_PROP_PBR_V1_STRENGTH` | 1.0 | knob only | aux row | yes | listed | initial strength slider | parent gate prevents bypass | env registry pass |
| `MC2_STATIC_PROP_PBR_V1_DIAG_SUNFOUND` | OFF | unset/`=0` | aux row | yes | listed | no widget | no; requires PBR strength > 0 | shader path only |
| `MC2_STATIC_PROP_DEBUG_MATERIAL` | 0 | unset/`=0` | feature row | yes | listed | labels/status only | n/a | shader branch |

## Shader Path Matrix

| Combination | Compile? | Render? | Expected behavior | Notes |
|---|---|---|---|---|
| ViewUniforms ON default | yes | yes | Uses binding 3 `ViewUniformsBlock`; PBR block available. | Default prefix defines `MC2_USE_VIEW_UNIFORMS`. |
| `MC2_VIEW_UNIFORMS=0` | yes | yes | Uses legacy `u_worldToClipGL`; PBR block compiled out and CPU force-zeros PBR strength/roughness. | Safe rollback. |
| MaterialGpu/sample ON default | yes | yes | Samples `materials[materialIdx].albedoTex`; invariant should match `texArrayLayer`. | Default visual should match fallback while invariant holds. |
| `MC2_MATERIAL_GPU=0` or sample off | yes | yes | Falls back to `texArrayLayer`; Material debug modes 5/6 show 0 in shader. | Safe but debug can mislead. |
| Snapshot dispatch ON default | yes | yes | Snapshot-built packet/meta dispatches when compare clean; live arrays are fallback. | Inspector should say snapshot-owned only with fallback caveat. |
| `MC2_STATIC_PROP_LEGACY_DISPATCH=1` | yes | yes | Legacy MDI path; new visual uniforms still uploaded in per-type loop. | DrawPacket authority disabled. |
| Ambient OFF | yes | yes | Uploads 0; no visual change. | Default off. |
| Ambient ON | yes | yes | Adds hemisphere term in VS, skips window branch. | Experimental but simple. |
| IBL ON default, strength 0.5 | yes | yes | Adds SH-L2 diffuse ambient in VS. | Default-on after flip. |
| IBL `=0` or strength 0 | yes | yes | Strength 0 short-circuits eval; visual baseline for IBL-off. | Env `=0` differs from unset by design. |
| PBR OFF default | yes | yes | `u_pbrV1Strength=0`; no PBR, no `u_cameraWorldPos` read. | Safe. |
| PBR ON + ViewUniforms ON | yes | yes | Per-fragment specular with MaterialGpu/literal roughness; window nodes bypass specular. | YELLOW for visual quality on flat legacy roofs. |
| PBR ON + ViewUniforms OFF | yes | yes | PBR compiled out and force-zeroed. | Safe. |
| PBR ON + no sun found | yes | yes | No specular; diag mode paints magenta. | Sun scan accepts both infinite types. |
| PBR roughness override off | yes | yes | Uses MaterialGpu roughness when sample on, else literal 0.6. | Required docs updated in `453d7fbf`. |
| PBR roughness override on | yes | yes | ImGui value replaces roughness if ViewUniforms not disabled. | Parent PBR gate still controls strength. |
| Debug material modes 1..6 | yes | yes | 1 albedo, 2 material palette, 3 normal, 4 tex layer, 5 roughness, 6 metallic. | Values >6 are clamped on CPU, so shader hot-pink unknown branch is not reachable through env. |
| Window/hot-pink branch + PBR ON | yes | yes | Vertex lighting skips and fragment PBR now skips via `v_flags` bit 1. | Fixed in `15a3336c`. |
| Shadow static prop shaders | yes | excluded | Separate shadow path, not part of visual lane. | No visual feature audit here. |

## PBR Status

Classification: YELLOW.

The gate mechanics are good: `MC2_STATIC_PROP_PBR_V1` is default off, strength upload is zero when off, ViewUniforms off compiles the block out and force-zeros the runtime upload, `u_cameraWorldPos` is only used inside `#if defined(MC2_USE_VIEW_UNIFORMS)`, and sun detection matches both `TG_LIGHT_INFINITE` and `TG_LIGHT_INFINITEWITHFALLOFF`.

The original contract drift after V-MATERIAL-PBR-3 is fixed: fragment PBR now checks existing `v_flags` bit 1 and skips specular for window/hot-pink nodes, matching the vertex lighting bypass.

Visual status: PBR remains YELLOW/experimental. The current model is a gated dielectric/specular approximation on legacy flat roofs without material masks, so broad or blown roof highlights are expected until roughness/metallic/material authoring is more real.

## IBL Status

IBL is default ON: `MC2_STATIC_PROP_IBL_SH` unset enables SH-L2, `=0` kills it. Strength defaults to 0.5, can be set by `MC2_STATIC_PROP_IBL_SH_STRENGTH`, and is modulated by ImGui. Env remains authoritative because upload is `s_iblShEnabled ? g_iblShStrength : 0.0f`.

Coefficient convention is well documented in `RenderCore/IblShCoeffs.h` and `shaders/static_prop.vert`: raw SH projection, c1..c5 diffuse convolution in shader, Y-up Stuff-space normals, order L00/L1-1/L10/L11/L2-2/L2-1/L20/L21/L22.

Per-mission selection exists but has no real mission mappings today. Unknown/null missions fall back to `"default"`. The mission-map extension trap is fixed in `a0fc4d7f`: lookup uses a counted loop and ignores null placeholders, so future rows remain reachable. No selected-set log line was found; inspector displays active set.

## Material/Layout/Binding Status

MaterialGpu layout is sound.

| Item | Status |
|---|---|
| C++ layout | `MaterialGpu` is 32 bytes; offsets assert albedo 0, flags 16, metallic 24, roughness 28. |
| GLSL mirror | `shaders/include/material_gpu.hglsl` matches field order and stride. |
| Mirror validator | PASS via Git Bash: `OK: MaterialGpu field order matches (8 fields)`. |
| shader_schema | PASS: MaterialGpu 32, ViewUniforms 144, GpuMechInstance 64. |
| shader_reflect | Static prop variants PASS; unrelated `gos_terrain.frag` drift. |
| Binding 5 | MaterialGpu table in static_prop coalesce frag; guarded by `u_materialGpuSample`. |
| ViewUniforms binding 3 | Correct and separate from cull UBO binding 2. |
| CULL binding 2 | No collision with ViewUniforms because UBO/SSBO namespaces differ and comments reserve it. |
| IBL/PBR uniforms | Uniform names are stage-agnostic and looked up for both legacy/coalesce programs; no binding collision. |

Missing/danger areas: `StaticPropMaterialInventoryEntry` has its own static asserts, good. No duplicated `MaterialGpu` struct drift found. Required PBR registry/tier1 docs were updated in `453d7fbf`.

## Debug/Inspector/Capture Status

Render Spine remains useful and prefers DrawPacket/render-spine authority for pipeline/material facts. Pipeline State reflects PipelineDesc, but PipelineDesc does not fully model shader object-id variants yet. Material inventory shows albedo/roughness/metallic rows, but shader debug modes 5/6 return zero when `u_materialGpuSample=0`, while the inspector can still show inventory data.

Debug material labels in shader/registry/ImGui support modes 1..6. `scripts/run_smoke.py` comments were updated in `453d7fbf`.

ImGui sliders do not bypass env gates: IBL slider uploads zero when `MC2_STATIC_PROP_IBL_SH=0`; PBR strength/roughness uploads zero/sentinel when PBR/ViewUniforms gates are off. PBR diagnostic has no widget and is env-only.

Capture harness records commit, mission, preset/camera-ish data, resolution, IBL/PBR envs and strengths. It does not include `MC2_STATIC_PROP_DEBUG_MATERIAL` in `CAPTURE_ENV_KEYS`, so material debug captures are less reproducible than IBL/PBR captures.

## Rollback Status

| Feature | Disable | Disabled path compiles? | Validated/evidence | Fallback safety |
|---|---|---|---|---|
| ViewUniforms | `MC2_VIEW_UNIFORMS=0` | yes | reflect variants; runtime prefix path | legacy `u_worldToClipGL` |
| Snapshot dispatch | `MC2_SNAPSHOT_STATIC_PROP_BUILD=0` | yes | snapshot counters | live builder arrays |
| DrawPacket dispatch | `MC2_STATIC_PROP_LEGACY_DISPATCH=1` | yes | v6 gate + legacy branch | legacy MDI |
| MaterialGpu upload | `MC2_MATERIAL_GPU=0` | yes | mirror/schema; sampleOn reason | table unbound, no shader access when sample off |
| MaterialGpu sampling | `MC2_MATERIAL_GPU_SAMPLE=0` | yes | sampleOn log | `texArrayLayer` |
| Ambient | unset/`=0` | yes | strength upload 0 | no-op add |
| IBL | `MC2_STATIC_PROP_IBL_SH=0` or strength 0 | yes | strength upload 0 | eval short-circuit |
| PBR | unset/`=0` | yes | strength upload 0 | PBR short-circuit |
| PBR + ViewUniforms off | `MC2_VIEW_UNIFORMS=0` | yes | compile guard + force-zero | PBR block removed |
| Material debug | unset/`=0` | yes | debug uniform 0 | debug block skipped |

## Evidence Run

Commands run from `A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev`.

| Check | Result |
|---|---|
| `git status --short` | clean in nifty worktree before report creation |
| `python tools\shader_schema\validate.py` | PASS: MaterialGpu, GpuMechInstance, ViewUniforms |
| `python tools\shader_reflect\reflect.py` | FAIL due unrelated `shaders/gos_terrain.frag` type-name drift; all `static_prop.vert/frag` variants PASS |
| `C:\Program Files\Git\bin\bash.exe scripts/check-material-gpu-mirror.sh` | PASS |
| `bash scripts/check-env-registry.sh` | Original audit FAIL: unregistered `MC2_STATIC_PROP_IBL_SH_STRENGTH`; remediation PASS in `453d7fbf` |
| `bash scripts/check-visibility-log-schema.sh` | OK |

Original audit smoke/capture was not run because the HIGH issue was clear from code inspection. Remediation validation is listed above.

## Recommended Fixes

Must fix before continuing static prop visuals:

1. Restore PBR bypass for window/hot-pink nodes in fragment PBR. Fixed in `15a3336c`.
2. Register or intentionally allowlist `MC2_STATIC_PROP_IBL_SH_STRENGTH` so `check-env-registry.sh` passes. Fixed in `453d7fbf`.
3. Update PBR docs/registry to V-MATERIAL-PBR-3 reality: per-fragment, MaterialGpu roughness/metallic, albedo-tinted F0, roughness fallback 0.6, sun type matching both infinite types. Fixed in `453d7fbf`.

Should fix before copying template to terrain/mech/shadow/VFX:

1. Make `IblShRegistry.h` mission map extension-safe before real per-mission rows are appended. Fixed in `a0fc4d7f`.
2. Add selected SH set log line, including fallback/override reason. Still recommended.
3. Add `MC2_STATIC_PROP_DEBUG_MATERIAL` to capture metadata. Still recommended.
4. Update `docs/tier1_env_vars.md` for ambient, IBL, IBL strength, PBR diag, and material debug. Fixed in `453d7fbf`.
5. Fix stale comments in `ibl_sh_runtime.h`, `view_uniforms_gl.*`, and `scripts/run_smoke.py`. Static-prop-required comments fixed in `453d7fbf`; older ViewUniforms wording can be cleaned separately.

Optional polish:

1. Make material debug modes 5/6 label the "sample off -> shader zero" condition in the inspector.
2. Decide whether CPU should clamp `MC2_STATIC_PROP_DEBUG_MATERIAL > 6` or allow shader hot-pink unknown-mode branch for diagnostics.
3. Extend PipelineDesc/inspector language for object-id shader variants.

## Reusability Verdict

Yes for architecture, with a visual caveat. StaticPropOpaque can now be used as the R-to-V template for the next lane's authority chain, gates, validators, rollback story, and inspector/capture surfaces.

Do not copy PBR visual tuning as final art direction. PBR remains a gated experimental feature because broad/blown highlights on flat legacy roofs are expected until material masks and roughness data improve.

Reusable template: stable RenderWorld handle -> RenderSnapshot row -> DrawPacket/meta dispatch with live fallback -> PipelineDesc state -> MaterialGpu ABI -> ViewUniforms ABI -> shader feature gates -> inspector/capture/validator coverage.
