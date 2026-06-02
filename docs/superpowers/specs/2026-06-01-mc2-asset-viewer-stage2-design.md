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
  shader compiled by the viewer itself (no RenderCore link). Fully isolated,
  unblocks the slice. **If Backend B is chosen, the preview MUST be labeled
  approximate** in the UI and README — e.g. a persistent line
  `Preview mode: Local PBR approximation, not exact MC2 shader.` — because the
  tool's promise is "how MC2 draws it" and modders must not trust a non-matching
  preview as game-accurate.

Do not pick A or B before the spike. The spec deliberately leaves it open.

**Stage 2 does NOT depend on RenderCore to ship value.** Whichever backend the
spike selects, the final deliverable remains: Materials sidebar live + manual slot
assignment + lit sphere + known/correct color-space behavior. If RenderCore
standalone init is a swamp, Backend B (labeled approximate) still ships the slice.

## Architecture

New files under `tools/asset_viewer/` (all consumed via the existing seam):

| File | Responsibility |
|---|---|
| `MaterialRenderBackend.h` | Interface: `init()`, `setMaterial(slots)`, `render(camera, light, meshHandle)`. One impl chosen by the spike. |
| `RenderCoreMaterialBackend.{h,cpp}` *(Backend A, if spike passes)* | Binds mc2's real `shader.pbr.orm`; standalone RenderCore bring-up. |
| `LocalPbrMaterialBackend.{h,cpp}` *(Backend B, fallback)* | Self-contained Cook-Torrance GLSL over the 4 slots; no RenderCore. |
| `MaterialPreviewPBR.{h,cpp}` | The `PreviewSurface`. Owns slot textures + orbit camera + directional light state; drives the backend; `draw(availableSize)` renders into an FBO and blits to an `ImGui::Image`. Owns FBO lifecycle + GL-state containment (see "FBO / GL-state ownership"). |
| `MaterialTextureLoader.{h,cpp}` | **Slot-aware** texture upload (see "Material texture upload policy"). Decodes via the existing backend (may reuse `UiEditorImageCache`'s decode path) but OWNS the GL upload + per-slot internalformat: baseColor/emissive → sRGB, normal/ORM → linear. This is NOT `UiEditorImageCache_Get` used blindly. |
| `MaterialSlots.{h,cpp}` | UI for the 4 slots; each slot reuses the stage-1 `FileBrowser`/Browse picker to assign a file; uploads via `MaterialTextureLoader` (slot-aware), NOT the UI image cache. |
| `FitMaterialLoader.{h,cpp}` | MINIMAL FIT parser: read ONE `Material{}` block → `{baseColor,normal,orm,emissive}` paths + `shader`, `ormPacking`, `alphaMode`. Not a general FIT parser. **Sequenced AFTER manual slots + sphere preview work** (does not block the MVP). |
| `SphereMesh.{h,cpp}` | Generate a UV sphere: position, normal, **tangent**, uv. Tangents are required for normal mapping; handedness must pass the validation fixture (see "Tangent validation"). |

**Seam check:** `MaterialPreviewPBR` implements `PreviewSurface` (`setSource`,
`draw(const ImVec2&)`, `label()`). `setSource` accepts a FIT material path (via
`FitMaterialLoader`) OR is bypassed when slots are assigned directly in the UI.
The app's existing `PreviewSurface*` dispatch picks `MaterialPreviewPBR` when the
sidebar's active type is Materials. No change to `AssetViewerApp`'s seam contract.

## Render flow

```
slots (4 textures, each via MaterialTextureLoader → GL texture w/ slot-aware internalformat)
  + SphereMesh (pos/normal/tangent/uv)
  + camera (orbit/zoom) + directional light (rotatable) + ambient
  → MaterialRenderBackend.render(...) into an offscreen FBO (state-contained)
  → ImGui::Image(fbo color tex) in the preview region
```
ORM sampled as R=AO, G=Roughness, B=Metallic. Emissive added post-lighting.
Normal map applied in tangent space (hence sphere tangents).

## Material texture upload policy (must-fix from review)

Stage 2 cannot blindly reuse `UiEditorImageCache_Get` for all material slots — it
is a UI image cache that uploads display images, not material textures with a
correct color-space policy. PBR preview requires color-space correctness:

- **baseColor:** sRGB internal format
- **emissive:** sRGB internal format
- **normal:** linear internal format
- **ORM:** linear internal format, packed R=AO, G=Roughness, B=Metallic

**Preferred implementation:** `MaterialTextureLoader` reuses the existing decode
code where possible but OWNS the GL upload + per-slot internalformat. The 4 slots
do NOT all go through `UiEditorImageCache_Get`.

**Fallback:** if Stage 2 ships using `UiEditorImageCache_Get` unchanged, the
preview MUST be labeled approximate and color-space correctness is explicitly
deferred — do not silently use one UI cache path for all PBR slots.

## FBO / GL-state ownership (must-fix from review)

`MaterialPreviewPBR` renders to an offscreen FBO then shows it via `ImGui::Image`.
The implementation plan MUST require GL-state containment (not left implicit):

- save + restore the previously-bound framebuffer and viewport around the render;
- do not leak depth / blend / cull state into ImGui's draw (ImGui expects its own
  state — push/pop or reset what you change);
- recreate the FBO + color/depth attachments on preview-size change;
- check FBO completeness (`glCheckFramebufferStatus == GL_FRAMEBUFFER_COMPLETE`);
- delete all GL resources (FBO, attachments, textures, mesh buffers) on shutdown.

## Tangent validation (must-fix from review)

Sphere tangents are easy to get subtly wrong, so the plan MUST include concrete
checks (not just "verify it looks right"):

- a **flat-blue normal map** (`(128,128,255)` → tangent-space +Z) must produce the
  SAME shading as no normal map (within broad tolerance);
- a **known directional normal** must perturb shading in the EXPECTED direction
  (e.g. a normal tilted +U brightens the +light-facing side as predicted);
- the **UV seam** (where the sphere wraps) must not "explode" (no black/garbage band)
  — check tangent continuity/handedness across the seam.

## Smoke / verification (must-fix from review)

Avoid brittle exact-pixel goldens (they vary across drivers). The offscreen-render
smoke should instead:

- render a flat baseColor material on the sphere, read back the FBO, assert the
  center region is **non-black / non-empty** and that **`glGetError()` is clean**;
- optionally checksum the FBO for a stable-within-driver regression signal;
- for the flat-blue-normal-vs-no-normal case, compare the two renders within a
  **broad tolerance** (they should match), not an exact hash.
Exact pixel goldens are only acceptable if a backend is proven bit-deterministic.

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

Run `superpowers:writing-plans` against this spec. Plan task order (FIT loader moved
LAST so it never blocks the material-preview MVP):

0. **Spike** — standalone RenderCore PBR bring-up; decide Backend A vs B. If B, add
   the "approximate" label requirement.
1. **SphereMesh** + the tangent validation fixture (flat-blue == no-normal; known
   normal perturbs as expected; no seam explosion).
2. **MaterialRenderBackend** interface + the chosen impl (A or B).
3. **MaterialTextureLoader** — slot-aware GL upload (sRGB baseColor/emissive, linear
   normal/ORM). NOT blind `UiEditorImageCache_Get`.
4. **MaterialPreviewPBR** — FBO render + GL-state containment + resource cleanup.
5. **MaterialSlots** UI — manual slot assignment via the stage-1 browser/picker.
6. **Wire Materials mode** into the app → **MVP playable: manual slots → lit sphere**
   with correct color space. (This is the shippable deliverable, RenderCore or not.)
7. **Smoke** — offscreen render, assert non-black + clean `glGetError()`, flat-normal
   within tolerance (no brittle exact-pixel goldens).
8. **FitMaterialLoader** (LAST) — "Load .fit material" button populates the slots.

Apply the same adversarial + greybeard review before building, given the
RenderCore-init risk.

## Credit

Continues reuse of @Methuselas's image cache. Credit @Methuselas on any commit
touching/reusing his code (see memory `credit-methuselas-imgui-fit`).
