# Tool Preview Rendering Architecture — MC2 tooling previews

- Date: 2026-06-10
- Status: STRATEGY (design contract, no code in this doc)
- Scope: Asset Viewer, Editor preview panes, in-game UI previews (mechlab/logistics/mechbay)
- Principle: **No universal preview renderer. One correct backend per asset type, with explicit bridge seams.**

---

## 1. Backend Taxonomy

| Backend | What it is | Code anchor | Fidelity | Engine coupling |
|---|---|---|---|---|
| **B — Standalone Approximation** | Self-contained GL 3.3 Lambert viewer; inline shaders, single-color FBO, no engine state | `tools/asset_viewer/MeshPreview3D.{h,cpp}` | Geometry/UV/albedo truth only. Lighting NOT representative | Zero. Shares only `TglMeshLoader` + KTX2 decode |
| **A — Shader-Faithful Static** | Compiles real `shaders/static_prop.vert/.frag` unmodified, stub SSBO/UBO scene (`StandaloneSceneStubs`), reverse-Z MRT FBO | `tools/asset_viewer/ModelPreviewEngineShader.{h,cpp}`, contract: `docs/asset-viewer-backend-a-shader-contract.md` | Shading-faithful (legacy lane: no view-uniforms/coalesce/PBR-slots). Not pixel-exact (no mission lights/shadow/fog) | Shader files + UBO/SSBO contract. Fail-open to B |
| **C — In-Engine Context-Swap** | Full engine renders one object under a private camera: SimpleCamera swaps `eye`, own viewport/MVP, white key light, shadows/fog off, per-render `renderLists()` flush | `code/simplecamera.cpp:108-216`, `Mech3DAppearance::render` (`mclib/mech3d.cpp:2338+`) | Exact engine output for the configured context | Total — IS the engine. Only runs inside game/editor process |

Naming rule: a "preview backend" is defined by **who owns GL state and the camera**, not by which mesh loader feeds it. All three share `TglMeshLoader` output upstream of the render seam.

## 2. Asset-Type Matrix

| Asset type | Standalone tool (Asset Viewer) | In-process UI (game/editor) | Why |
|---|---|---|---|
| Static props (.tgl buildings/trees) | **Backend-A**, fail-open to B | Backend-A-style pane, or world placement | Static prop shading is stateless enough to stub (proven: A v2 ships). C is overkill |
| Mechs / vehicles / turrets (animated, skinned, paint schemes) | **Backend-B (geometry-truth only)** today; Backend-A-mech is v3 | **Backend-C (SimpleCamera + CPU draw path)** | Paint scheme, bone animation, damage state live in `Mech3DAppearance` + TGL pools — re-implementing standalone = guaranteed drift. C already correct in-engine; the GPU-batching steal is fixed by `MechPreviewRenderScope` (see §5) |
| Effects (gosFX) | Out of scope / thumbnail only | Backend-C (effects only render via engine FX pipeline) | gosFX has no standalone contract |
| Terrain chunks / tiles | Not previewed standalone | Editor world view (chunk path) | Terrain renderer is chunk/GPU-only post-8z; no per-asset preview makes sense |
| Textures / colormaps | Plain ImGui image (no 3D backend) | Same | Don't dress a texture view as a renderer |

Decision rule for new asset types: **if correct appearance depends on engine-resident mutable state (animation, paint, damage, FX), preview must be Backend-C; if appearance is a pure function of (mesh, materials, shader files), Backend-A; if you only need "is the geometry/UV sane", Backend-B.**

## 3. What Is Shared vs Never Shared

### Shared (the bridge seams)
1. **Asset decode layer**: `TglMeshLoader`, KTX2 decode, Stuff→GL transform (`x'=-x, y'=z, z'=y`), texture path resolution. One loader, all backends. Drift here is data corruption, not style.
2. **Shader source files**: Backend-A references `shaders/*.vert/.frag` + `*.hglsl` includes **by path, never copied** (hard constraint already in ModelPreviewEngineShader.cpp:3-4). `ShaderIncludeResolver` is the shared include walker.
3. **The scene-binding contract**: UBO binding 1 layout + SSBO bindings 0/1/2/3/20, documented in `asset-viewer-backend-a-shader-contract.md`. Engine changes to these bindings must update the contract doc — this is the drift tripwire (§4).
4. **`PreviewSurface.h` interface**: orbit camera params, resize, draw-to-ImGui. All standalone backends implement it; new backends must too.
5. **Smoke harness pattern**: `--smoke-backend-*` exit-code gates.

### Never shared (anti-coupling rules)
1. **GL state ownership.** Each backend sets ALL state it depends on (depth func, depth mask, blend, cull) and restores it. Lesson already paid for twice: terrain chunk transparency saga (inherited glDepthMask=FALSE) and the batcher steal. No backend may assume inherited state.
2. **Cameras/MVP.** Backend-A reverse-Z infinite-far; Backend-B standard-Z; Backend-C engine `eye` swap. Never route one backend's matrices through another's pipeline. (The GPU-batcher bug WAS exactly this: preview submissions flushed under world MVP.)
3. **Draw accumulation queues.** Standalone backends draw immediately into their own FBO. Backend-C flushes its own `renderLists()` within the camera scope. No preview draw may land in a deferred queue that flushes under a different camera.
4. **Engine globals into standalone tools.** Asset Viewer must never link `eye`, `mcTextureManager`, TGL pools, or the mech batcher. Stubs only, via the contract.
5. **Shader forks.** Never a "preview variant" copy of an engine shader. If a shader can't compile standalone, fix the resolver or add a stub binding — fail open to B, never fork.

## 4. Avoiding Renderer Drift

Drift = preview shows something the engine wouldn't. Mechanisms, cheapest first:

1. **Reference-don't-copy** (already enforced): Backend-A compiles the live shader files every launch. An engine shader change that breaks the standalone contract fails A's compile → fail-open to B → smoke `--smoke-backend-a-compile` goes red. The smoke IS the drift detector; keep it in tier gates.
2. **Contract doc as single source**: uniforms/SSBO list in `asset-viewer-backend-a-shader-contract.md`. Rule: any engine commit touching `scene.hglsl` / `lighting.hglsl` / binding indices updates the contract doc or the smoke fails.
3. **Golden-frame parity smokes (v3)**: render a fixture prop in Backend-A and in-engine with matched camera/light config; compare downsampled luminance (tolerance band, not pixel-exact). Catches semantic drift compile checks can't (UV-V check was the prototype; it returned inconclusive — needs higher-contrast fixture).
4. **No fidelity claims beyond tier**: each backend declares its fidelity tier in UI (§6). "Approximate" labeled approximate cannot drift — it never promised parity.
5. **Backend-C drifts by construction never** — it is the engine. Its risk is the opposite: engine pipeline changes (batching) silently breaking the preview *context*. Mitigation: a preview smoke that renders a mech via SimpleCamera and asserts non-empty viewport pixels + `[MECH_PREVIEW v1] gpu_submit=0 cpu_draw=1` trace line (`MC2_MECH_PREVIEW_TRACE`).

## 5. GPU Batching in UI Previews — the rule

Root cause on record: `GpuMechBatcher` accumulates `submitActor()` into `s_pendingSubmits` and flushes inside `MC_TextureManager::renderLists()` (`mclib/txmmgr.cpp:2721`) under the **world MVP snapshot**. A preview submission therefore rasterizes off the UI viewport.

Shipped fix pattern (keep, generalize):

```cpp
// gos_mech_killswitch.h:44-48
extern int g_mechPreviewRenderDepth;
struct MechPreviewRenderScope { ctor++ / dtor-- };
// mech3d.cpp:2683-2684
previewContext = (g_mechPreviewRenderDepth > 0);
if (g_useGpuMechs && ... && !previewContext) submitActor(...);  // else CPU TG_Shape::Render fallback
```

**The rule (binding for all future batchers — static props, vehicles, anything):**
> Any deferred/batched draw path MUST check preview depth at *submit* time and divert to an immediate path that honors the currently active camera. Flush-time fixes are forbidden — by flush time the camera context is gone.

Corollaries:
- New batcher checklist item: "preview-scope guard present + trace event" before default-on.
- Do NOT try to make the batcher preview-aware (per-submission MVP capture, separate preview bucket). That is v4 territory (§8) and only worth it if CPU fallback becomes a measured preview-perf problem (it isn't: one mech, UI framerate).
- `MechPreviewRenderScope` should be hoisted into a generic `PreviewRenderScope` once a second batcher needs it — same counter, one semantic.

## 6. Preview Fidelity Levels for Modders

Expose as a visible, honest tier label in every preview pane + tool docs:

| Tier | Label in UI | Backend | Promise |
|---|---|---|---|
| F0 | "Geometry" | B | Mesh/UV/albedo correct; lighting illustrative only |
| F1 | "Engine shading" | A | Real shaders, neutral scene (no mission light/fog/shadow) |
| F2 | "Engine context" | C (in-game/editor) | What the engine draws under the preview camera config |
| F3 | "In-world" | place asset in a test mission/editor world | Ground truth |

Mechanics:
- Asset Viewer already has the A/B radio — rename options to the tier labels, persist choice, show fail-open transitions explicitly ("Engine shading unavailable: shader compile failed → Geometry tier", with log path).
- Modder docs state per tier what feedback is actionable: F0 = winding/UV/scale; F1 = material/texture/shading bugs; F2+ = paint scheme, animation, damage states.
- Never let a lower tier silently impersonate a higher one — that is the drift complaint generator.

## 7. Anti-Goals

- **No universal renderer.** Unifying B/A/C into one abstraction recreates the engine inside the tool, badly.
- **No shader forks or copies** for preview use.
- **No engine-global linkage in standalone tools** (no `eye`, no texture manager, no pools, no batcher).
- **No pixel-exactness promises** below F2. F1 is shading-faithful, not scene-faithful.
- **No preview-aware batcher complexity** until CPU fallback measurably hurts.
- **No effects/terrain standalone preview** — engine-only by nature; don't approximate.
- **No inherited GL state** — every backend sets and restores its full state block.

## 8. Risks

| Risk | Likelihood | Mitigation |
|---|---|---|
| Engine shader lane evolution (view-uniforms/coalesce/PBR-slots default-on) leaves Backend-A's "legacy lane" config orphaned | High over time | Contract doc tracks omitted defines; v3 task = compile A under the modern define set with extended stubs; smoke goes red when legacy lane is deleted |
| New batchers (static props GPU path, vehicles) reintroduce preview-steal | Medium | §5 rule + checklist + generic `PreviewRenderScope`; preview pixel smoke |
| Golden-frame parity smokes flaky (driver/AA differences) | Medium | Luminance-band compare on high-contrast fixtures, not pixel diff; UV-V check showed low-contrast fixtures don't gate |
| Fail-open masks regressions (A silently → B, user sees "preview works") | Medium | Explicit UI banner on fallback + smoke fails in CI even when UI fails open |
| Mech standalone preview (v3 Backend-A-mech) underestimates engine state (bones, paint, damage) | High | Scope v3 to bind-pose + paint-scheme only; animation stays F2 forever |
| SimpleCamera depends on legacy CPU draw (`TG_Shape::Render`); if 8z-style retirement ever hits the mech CPU path, F2 previews lose their renderer | Low now, real later | Preview smoke pins the dependency; if CPU path retires, that is the trigger for v4 preview-aware batching — not before |

## 9. Roadmap — v2 / v3 / v4

**v2 (now / near):**
- Tier labels + explicit fail-open banner in Asset Viewer (§6).
- Hoist `MechPreviewRenderScope` → generic `PreviewRenderScope`; add batcher checklist rule.
- Backend-C preview pixel smoke (SimpleCamera render → non-empty viewport + trace assert).
- High-contrast UV-V fixture to close the inconclusive orientation check.

**v3:**
- Backend-A "modern lane": compile static_prop under `MC2_USE_VIEW_UNIFORMS`/`MC2_COALESCE`/`MC2_STATICPROP_PBR_SLOTS` with extended stubs; keeps A alive as engine lanes migrate.
- Golden-frame parity smoke (A vs in-engine, matched neutral scene, luminance band).
- Backend-A-mech: bind-pose mech mesh through real mech shader with stub bone palette (identity bones) + paint-scheme colors. F1 for mechs in the standalone tool. No animation.
- Editor: embed an F1 prop pane (Backend-A is already in-process-compatible — it owns its FBO/state).

**v4 (only on demonstrated need):**
- Preview-aware GPU batching: per-submission camera capture or dedicated preview bucket flushed inside the preview scope. Trigger conditions: (a) mech CPU draw path scheduled for retirement, or (b) multi-mech preview scenes (formation viewer) where CPU path measurably hurts.
- Scene-config presets for F1 (mission ambient/fog profiles loaded into the stub UBO) — moves F1 toward F2 without engine linkage.
- Shared headless render service: game exe `--render-asset` mode emitting thumbnails for tool galleries (true F2 outside interactive session).

## 10. Next Implementation Slices (ordered, each independently shippable)

1. **S1 — Fidelity labels + fallback banner** (asset_viewer UI only; no engine risk).
2. **S2 — `PreviewRenderScope` generalization** (rename + move; mech batcher adopts; doc the batcher checklist in gos_mech_batcher.h header comment).
3. **S3 — F2 preview smoke** (SimpleCamera mech render, pixel + trace assert; wire into tier gates).
4. **S4 — UV-V high-contrast fixture** (close `asset-viewer-backend-a-uvv-check.md` open thread).
5. **S5 — Backend-A modern-lane compile smoke** (defines on, extended stubs; red flag for lane migration).
6. **S6 — Backend-A-mech bind-pose spike** (timeboxed; proves/kills v3 mech F1).
