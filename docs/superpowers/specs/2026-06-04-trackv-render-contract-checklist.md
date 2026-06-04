# Track V Render-Contract Compliance Checklist

**Companion to:** [2026-06-04-trackv-coherent-render-pipeline.md](2026-06-04-trackv-coherent-render-pipeline.md)
(the contract). This is the reviewable instrument: run it against any VL plan or
implementation to check contract compliance. Each item cites the contract clause
it enforces. A plan/impl is **compliant** only if every applicable box is checked
or explicitly N/A with reason.

**How to use:** for a plan review, check the boxes the plan *commits* to; flag any
unchecked applicable box as a gap. For an impl review, check the boxes the code
*proves* (grep/debug-view/capture). Items marked **[STOP]** are hard stops —
unchecked = halt-and-escalate.

---

## A. Color / output (VL-1)

- [ ] **A1** Albedo + emissive textures are sRGB-stored (hardware decode at
  sample) OR shader-decoded — exactly one path, never both. *(§3.2.1/2, INV-COLOR-1)*
- [ ] **A2 [STOP]** No double-decode: no shader applies `pow()`/manual decode to a
  texture that is also `GL_SRGB*`-stored. *(INV-COLOR-1)*
- [ ] **A3** Data textures (normal, ORM/metallic-roughness, AO, height, masks,
  lightmaps) are linear-stored and never decoded. *(§3.2.2, INV-COLOR-4)*
- [ ] **A4 [STOP]** Encode happens exactly once, at the single composite write —
  either `GL_FRAMEBUFFER_SRGB` on that blit OR an explicit `linearToSrgb` OETF,
  never both. *(§3.2.5, INV-COLOR-2)*
- [ ] **A5** No shader writing to location 0 (scene color) applies an OETF;
  `sceneFBO_`/`GBuffer1` are `RGBA16F` and hold linear. *(INV-COLOR-3a/3b)*
- [ ] **A6** Tonemap (ACES) runs once, on linear HDR, after SSAO/fog/bloom-extract,
  before grade and encode. *(§3.2.4, INV-POST-1)*
- [ ] **A7 [STOP]** UI/HUD composites *after* the scene encode, is treated as
  sRGB-display art, and is excluded from tonemap/bloom (no UI in `sceneFBO_`).
  *(§3.2.6, INV-COLOR-5, INV-POST-4)*
- [ ] **A8 [STOP]** Decode and encode land *together* as one atomic flip with a
  default-OFF byte-identical path; neither edge ships alone (single-edge flips
  darken or wash the whole image). *(§3.4)*
- [ ] **A9** The always-on grade (sunset/vignette, `postprocess.frag:121-141`) is
  re-tuned in the same change that flips the working space; no silently-stacked
  grades. *(INV-POST-5, stop #4)*
- [ ] **A10** Every albedo lane is covered in the same milestone (terrain colormap
  + BC7 atlas, cement, mine, static-prop, mech, legacy color TGA, terrain-overlay)
  — no opaque color surface left undecoded to read as "the one wrong thing".
  *(§8 conformance; the terrain-BC7-145 trap)*

## B. Lighting (VL-2)

- [ ] **B1 [STOP]** Ambient is single-source per pixel (SH/IBL canonical, or
  hemisphere fallback) — never summed. *(§4.3, INV-LIGHT-1)*
- [ ] **B2** All opaque lanes consume the same sun term and the same `LightsData`
  SSBO schema (binding=20, byte-lockstep with `TG_HWLightsData`). *(INV-LIGHT-2)*
- [ ] **B3** Specular uses one BRDF, parameterized by material roughness/metallic
  (the per-lane PBR-V1 / Blinn-V1 variants converge). *(§4.3, INV-LIGHT-3)*
- [ ] **B4** Lighting math runs on decoded-linear albedo (depends on A1). *(INV-LIGHT-4)*
- [ ] **B5** No lane invents a second ambient/diffuse model (watch terrain-overlay,
  grass, decals — each currently carries its own). *(§4.2, §8)*
- [ ] **B6** Specular/normal-mapped terms are per-fragment; flat ambient may be
  per-vertex only if it is the *same* model at lower frequency. *(§4.4)*

## C. Material (VL-3)

- [ ] **C1** `baseColor` and `emissive` are sRGB-stored+decoded; `roughness`,
  `metallic`, `normal`, `AO`, masks are linear and never decoded. *(INV-MAT-1)*
- [ ] **C2** A missing map resolves to the neutral default (white baseColor, rough
  1.0, metal 0.0, flat normal, black emissive, white AO) — never an undefined
  sample. *(§5.1, INV-MAT-2)*
- [ ] **C3** One `MaterialGpu` schema across lanes; per-lane shaders may read a
  subset but never redefine a channel's meaning or gate it per-lane with a
  different compile flag. *(INV-MAT-3)*
- [ ] **C4** Channel rollout order is emissive → metallic/roughness → normal
  (normal last; needs a TBN basis static props do not yet carry). *(§5.3)*

## D. Depth / shadow / visibility / cutout (VL-4)

- [ ] **D1 [STOP]** Reverse-Z untouched: scene is `glClipControl ZERO_TO_ONE` +
  `glDepthFunc GEQUAL` + clear 0.0; shadow FBOs are forward-Z `GL_LESS` + clear
  1.0. No "fixing" a depth func, clear value, or the `// reverse-Z (U2)` comments
  without an explicit depth-contract review. *(§6.1, INV-DEPTH-1, stop #7)*
- [ ] **D2** Any new pass picks the matching depth convention explicitly (scene
  reverse-Z vs shadow forward-Z); HZB obeys the MIN-reduce locked contract
  (`hzb-depth-convention.md`, `glGenerateMipmap` prohibited). *(§6.1)*
- [ ] **D3** Alpha cutout is a material flag (threshold 0.5 for cards/props), not
  per-shader magic. *(§6.3)*
- [ ] **D4 [STOP]** Receiver policy is expressed *only* through `GBuffer1.a`
  (`>0.5` self-handled, `<=0.5` post-pass applies); no lane invents a second
  shadow channel. The continuous-alpha escape
  (`rc_gbuffer1_legacyTerrainMaterialAlpha`) is the legacy terrain-water path
  (`gos_terrain.frag:870`) to retire — MDI water is already clean (`.a=0`).
  *(§6.4, INV-SHADOW-1)*
- [ ] **D5** Foliage: cutout in the *color* pass; foliage shadow either does not
  cast (impostor) or applies the same cutout in the shadow program — never a solid
  card (`shadow_instanced.frag` is empty today, SHADOW-FOLIAGE-ALPHA-DISCARD-1).
  *(§6.6, INV-FOLIAGE-1)*
- [ ] **D6** Object-ID (`R32_UINT` location 2) is opaque-geometry-only (props +
  mechs); alpha and VFX never write it; picking is CPU `Camera::inverseProject`,
  not GPU readback. *(§6.5, INV-ID-1)*
- [ ] **D7** MRT layout intact: location 0 linear HDR color, location 1 normal +
  shadow-mask.a, location 2 objectId; a color/post change must not corrupt
  COLOR1/COLOR2. *(§6.7)*

## E. Atmosphere / post (VL-5)

- [ ] **E1** Tonemap input is linear HDR; bloom threshold evaluated in linear HDR.
  *(INV-POST-1/2)*
- [ ] **E2** SSAO and fog modulate before tonemap. *(INV-POST-3)*
- [ ] **E3** Fog is unified (the per-lane fog fragmentation — static-prop
  `u_fogValue` vs terrain's own height-exp fog — converges, or is explicitly
  scoped as a VL-5 addition). *(§7.2)*
- [ ] **E4** Encode is the last scene operation; UI after, excluded. *(INV-POST-4)*

## F. Cross-cutting (every VL lane)

- [ ] **F1 [STOP]** Default-OFF path is byte-identical to shipped (frame hash /
  pixel compare). *(stop #2)*
- [ ] **F2 [STOP]** No GPU-direct pass touched by the change skips the GlStateGuard
  discipline (save/restore touched slots + `gos_InvalidateRenderStateCache()`) —
  applies to static-prop/mech batchers, terrain bridges, water fast path, particle
  bridge, post-process, not water alone. *(stop #8)*
- [ ] **F3** New uniform uploads taking a `GLuint program` use the
  `glProgramUniform*` family (not `glUniform*`); direct row-major matrices use
  `GL_FALSE`, material cache uses `GL_TRUE`. *(stop #9)*
- [ ] **F4 [STOP]** Every contract edge changed ships with its debug view — a flip
  without its view is not reviewable. *(stop #10, §11)*
- [ ] **F5** Shaders deploy in lockstep with the exe; build `mclib` before `mc2`.
  *(critical_inline_rules)*

## G. Required debug views (minimum reviewability bar)

- [ ] **G1** Albedo-only, all opaque lanes (terrain/overlay/prop/mech) — for the
  same-albedo-family test. *(§11)*
- [ ] **G2** Linear-vs-encoded toggle — see the working space directly; catch
  double/missing decode-encode. *(§11, VL-1 minimum bar)*
- [ ] **G3** Ambient-only, all opaque lanes — proves single ambient (B1
  unverifiable without it). *(§11)*
- [ ] **G4** Material channel view (albedo/normal/MR/AO/emissive), extended beyond
  static props to mech/terrain. *(§11)*
- [ ] **G5** GBuffer1.a (shadow-mask) view — proves receiver policy. *(§11)*

## H. Validation scenes (capture both default-OFF and contract-ON)

- [ ] **H1** S1 terrain + static prop, same albedo family. **[VL-1 acceptance]**
- [ ] **H2** S2 mech standing on terrain (no decode asymmetry). **[VL-1 acceptance]**
- [ ] **H3** S3 water + terrain + props (not crushed under ACES).
- [ ] **H4** S4 foliage near/mid/far (impostor color match; shadow cutout).
- [ ] **H5** S5 UI/HUD overlay (excluded from tonemap/bloom).
- [ ] **H6** S6 night/dark scene (shadowed sides not crushed; emissive windows).
- [ ] **H7** S7 shadow scene (reverse-Z stable; receiver mask correct).
- [ ] **H8** S8 fog scene (fog in linear; no banding after encode).
- [ ] **H9** Use in-engine framebuffer-hash probes for byte-identity claims, not OS
  screenshots (PNG sha ≠ pixel-identity). *(§10)*

---

## Per-VL-lane minimum gate (which sections are mandatory)

| Lane | Mandatory sections | Acceptance scenes |
|---|---|---|
| VL-1 color/output | A (all), F1/F2/F4, G1/G2, H1/H2/H5 | S1, S2 must pass |
| VL-2 lighting | B (all), A4 holds, F1/F4, G3, H6 | ambient-family consistent |
| VL-3 material | C (all), B3, F1/F3/F4, G4 | material channels correct |
| VL-4 shadow/depth/cutout | D (all), F1/F4, G5, H4/H7 | reverse-Z stable, foliage cutout |
| VL-5 atmosphere/post | E (all), A4/A6/A9 hold, F1/F4, H3/H8 | no UI leak, fog/water sane |
| VL-6 PBR/normal polish | B6, C1/C4, F1/F4, G4 | per-frag specular, normal maps |
