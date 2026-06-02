# MC2 Asset Viewer — Stage 2 Design (Lit PBR Material Preview)

**Date:** 2026-06-01
**Slice:** `MC2-ASSET-VIEWER-STAGE2-0`
**Base:** `claude/asset-viewer-stage2` off `claude/nifty-mendeleev` (which now contains
the @Methuselas merge + asset-viewer stage 1, landed 2026-06-01 at `29aebfe5`).
**Status:** SPEC ONLY — brainstormed, not yet planned/implemented. Next session
starts here (→ writing-plans).

## Goal

Render a PBR material on a 3D sphere with real-time lighting, as the second stage
of the MC2 Asset Viewer. Builds directly on stage 1's `PreviewSurface` seam: add a
new `MaterialPreviewPBR : PreviewSurface` alongside `TexturePreview2D`, and make the
sidebar's **Materials** entry live. The app and inspector continue to depend ONLY
on `PreviewSurface` — no rewire (this is exactly the "B architecture" stage 1's
seam was shaped for).

## Decisions locked in brainstorm (2026-06-01)

| Question | Decision |
|---|---|
| Render path / shader | **Spike first** (task 0). Try mc2's real `shader.pbr.orm` standalone; outcome picks Backend A (real shader) vs Backend B (viewer-local PBR). |
| Preview geometry | **UV sphere** (material ball) with tangents. |
| Material input | **Slot assignment** (baseColor/normal/orm/emissive) as the core, **+ a "Load .fit material" button** that populates the same slots via a minimal FIT material parser. |
| Lighting/camera | **Orbit camera + zoom + one rotatable directional light + small constant ambient.** |
| ORM packing | R = AO, G = Roughness, B = Metallic. |

## Task 0 — the spike (GATES the whole slice)

The one real unknown is **standalone RenderCore init**: can we render a PBR material
with mc2's real shader WITHOUT loading a `Mission`? Prior recon (stage-0 render
recon) found `RenderWorld::init()` has zero mission dependencies and `EditorBridge`
links rendercore/renderworld without game code — but a *lit material draw* needs
more (the shader program, its uniform/SSBO expectations, a light setup). The spike
must answer this with running code, not assumption.

**Spike (throwaway, timeboxed):** in the stage-2 worktree, attempt the minimum to
bind `shader.pbr.orm` (or its real GL program) against the existing GL context and
draw one triangle/sphere lit, with no Mission. Record what RenderCore/RenderWorld
calls are required and whether any pull in mission/global singletons.

**Outcome → backend choice (both hide behind `MaterialRenderBackend`):**
- **Tractable → Backend A (real mc2 PBR/ORM shader):** pixel-faithful "how MC2 draws
  it." `mc2_asset_viewer` links RenderCore (the one-line CMake add stage 1 deferred).
- **Swamp → Backend B (viewer-local Cook-Torrance):** a small self-contained PBR
  shader compiled by the viewer itself (no RenderCore link). Faithful-ish, fully
  isolated. Lower fidelity but unblocks the slice.

Do not pick A or B before the spike. The spec deliberately leaves it open.

## Architecture

New files under `tools/asset_viewer/` (all consumed via the existing seam):

| File | Responsibility |
|---|---|
| `MaterialRenderBackend.h` | Interface: `init()`, `setMaterial(slots)`, `render(camera, light, meshHandle)`. One impl chosen by the spike. |
| `RenderCoreMaterialBackend.{h,cpp}` *(Backend A, if spike passes)* | Binds mc2's real `shader.pbr.orm`; standalone RenderCore bring-up. |
| `LocalPbrMaterialBackend.{h,cpp}` *(Backend B, fallback)* | Self-contained Cook-Torrance GLSL over the 4 slots; no RenderCore. |
| `MaterialPreviewPBR.{h,cpp}` | The `PreviewSurface`. Owns slot textures + orbit camera + directional light state; drives the backend; `draw(availableSize)` renders into an FBO and blits to an `ImGui::Image`, or renders to the region. |
| `MaterialSlots.{h,cpp}` | UI for the 4 slots; each slot reuses the stage-1 `FileBrowser`/Browse picker to assign a file; loads via `UiEditorImageCache_Get`. |
| `FitMaterialLoader.{h,cpp}` | MINIMAL FIT parser: read ONE `Material{}` block → `{baseColor,normal,orm,emissive}` paths + `shader`, `ormPacking`, `alphaMode`. Not a general FIT parser. |
| `SphereMesh.{h,cpp}` | Generate a UV sphere: position, normal, **tangent**, uv. Tangents are required for normal mapping. |

**Seam check:** `MaterialPreviewPBR` implements `PreviewSurface` (`setSource`,
`draw(const ImVec2&)`, `label()`). `setSource` accepts a FIT material path (via
`FitMaterialLoader`) OR is bypassed when slots are assigned directly in the UI.
The app's existing `PreviewSurface*` dispatch picks `MaterialPreviewPBR` when the
sidebar's active type is Materials. No change to `AssetViewerApp`'s seam contract.

## Render flow

```
slots (4 textures, each via UiEditorImageCache_Get → GL texture)
  + SphereMesh (pos/normal/tangent/uv)
  + camera (orbit/zoom) + directional light (rotatable) + ambient
  → MaterialRenderBackend.render(...) into an offscreen FBO
  → ImGui::Image(fbo color tex) in the preview region
```
ORM sampled as R=AO, G=Roughness, B=Metallic. Emissive added post-lighting.
Normal map applied in tangent space (hence sphere tangents).

## UI

Sidebar: **Materials** now selectable (alongside Textures). When active, the
center/inspector shows: the 4 slot pickers (`MaterialSlots`) + a "Load .fit
material" button (→ `FitMaterialLoader` populates the slots) + the lit sphere
preview with orbit/zoom and a light-direction control (drag or sliders).

## Reuses (already in tree on this base)

- `UiEditorImageCache_Get` — loads slot textures (PNG/JPG/BMP/TGA) → GL. (@Methuselas)
- `FileBrowser` + the Win32 Browse picker — slot file assignment (stage 1).
- The `PreviewSurface` seam + `AssetViewerApp` shell (stage 1).

## Deferred

- IBL / environment lighting (this MVP is one directional + ambient).
- KTX2 / BC7 slot textures.
- Multiple materials / material list; animation; turntable capture.
- A general FIT parser or the JSON-vs-FIT manifest reconcile — `FitMaterialLoader`
  is read-only and minimal; it does NOT commit the format decision.
- Stage-1.5 `MC2AppShell` extraction (still open debt; not part of stage 2).

## Risks

1. **Standalone RenderCore init feasibility** — the whole reason task 0 is a spike.
   If Backend A is a swamp, Backend B keeps the slice deliverable.
2. **Tangent-space correctness** on the generated sphere — normal maps look wrong if
   tangents/handedness are off. Verify with a known normal map (flat-blue = no
   perturbation should look identical to no normal map).
3. **FBO/sRGB correctness** — baseColor/emissive are sRGB, normal/orm are linear;
   getting the color-space + framebuffer right is the usual PBR footgun.
4. **Offscreen render → ImGui::Image** lifetime/format (the preview is an FBO color
   attachment shown as an ImTextureID).

## Recommended next step (next session)

Run `superpowers:writing-plans` against this spec. Plan task order:
**Task 0 = the spike** (decide Backend A/B) → then SphereMesh → MaterialRenderBackend
+ chosen impl → MaterialPreviewPBR → MaterialSlots UI → FitMaterialLoader →
wire Materials mode into the app → smoke (offscreen render of a known material,
assert non-empty/expected output). Apply the same adversarial + greybeard review
before building, given the RenderCore-init risk.

## Credit

Continues reuse of @Methuselas's image cache. Credit @Methuselas on any commit
touching/reusing his code (see memory `credit-methuselas-imgui-fit`).
