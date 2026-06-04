# Track V — Coherent Render Pipeline Contract (TRACKV-COHERENT-RENDER-PIPELINE-1)

**Date:** 2026-06-04
**Status:** SPEC / contract reference. Design only — no production code, no shader
edits, no feature flips were made producing this document.
**Branch context:** `claude/nifty-mendeleev` (worktree `nifty-mendeleev`).
**Author session:** Session B (contract). Companion to Session A (VL roadmap →
VL-1 plan → maybe VL-4 plan).

## What this document is (and is NOT)

This is the **renderer constitution**: one explicit color / lighting / material /
depth / atmosphere output contract that every render path must obey. It is the
shared layer *beneath* the Track V visual-fidelity roadmap
([docs/superpowers/specs/2026-05-22-visual-fidelity-roadmap.md](docs/superpowers/specs/2026-05-22-visual-fidelity-roadmap.md)),
not a replacement for it.

- This is **not** another ranked visual roadmap.
- It does **not** rerank VL-1..VL-6, and it does **not** rerank the roadmap's
  V1..V11 feature items.
- It takes the VL roadmap as **input** and states the contract those lanes must
  implement, plus a mapping from each contract violation to the lane that fixes
  it.

Two numbering schemes are referenced throughout; keep them distinct:

- **VL-1..VL-6** — the contract-implementation lanes defined by this campaign
  (color/output, lighting, material, shadow/depth/cutout, atmosphere/post,
  PBR/normal polish). Session A plans these.
- **V1..V11** — the feature backlog items in the 2026-05-22 visual-fidelity
  roadmap (PBR material, IBL, CSM, HDR post, SSAO, decals, particles, LOD,
  terrain mat, object-ID, reactive surface). The VL lanes draw features from
  these.

Every `file:line` in this document was grep-verified at write time against the
`nifty-mendeleev` worktree. Symbols are stable; line numbers drift — re-grep
before relying on a number.

---

## 1. Executive verdict

The engine is **gamma-incoherent**, and the incoherence is not the one the recon
summary named. The recon said "terrain albedo is sRGB, static-prop albedo is
linear." Measured against source, that is wrong in its specifics and the true
state is worse-because-subtler:

**Measured color state (grep-verified):**

1. **No `GL_FRAMEBUFFER_SRGB` anywhere.** The token is defined in GLEW/SDL
   headers only; `glEnable(GL_FRAMEBUFFER_SRGB)` is never called. The engine's
   own code comments confirm it:
   `gos_static_prop_batcher.cpp:2530` — *"engine has no GL_FRAMEBUFFER_SRGB."*
2. **Terrain and static-prop albedo are BOTH uploaded linear**, not split sRGB
   vs linear:
   - Terrain colormap: `GL_RGBA8` — `gos_terrain_indirect.cpp:940`.
   - Static-prop albedo array: `GL_RGBA8` — `gos_static_prop_batcher.cpp:2578`,
     `:3196`; BC7 path `GL_COMPRESSED_RGBA_BPTC_UNORM` (UNORM = linear) —
     `:2531`, `:3119`.
3. **The sRGB-decode paths exist but are LATENT (default-OFF gates) — a loaded
   gun, not a current defect.** Three surfaces *can* request sRGB-decoded
   storage, all behind default-OFF env gates:
   - Mech (and shared) textures: `txmmgr.cpp:3746-3748` selects
     `GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM` for `vkFormat==146` KTX2 sidecars,
     `GL_COMPRESSED_RGBA_BPTC_UNORM` for `145` — but the entire sidecar path is
     gated by `MC2_TEXMGR_COMPRESSED_UPLOAD` (default OFF, `txmmgr.cpp:3692`;
     `RendererFeatureRegistry.h:533-540`). When OFF, mechs fall back to RGBA8
     linear via `gos_NewTextureFromMemory` (`txmmgr.cpp:3779`) — identical to
     terrain/props.
   - Terrain BC7 (`gos_terrain_indirect.cpp:876`) and static-prop BC7
     (`MC2_STATICPROP_BC7`) are likewise default-OFF.
   Reality check: **3,811 of the deployed `data/textures/*.ktx2` sidecars carry
   `vkFormat==146` (BC7 sRGB)** — the cook has been run (`batch_cook.py --bc7`),
   the assets are armed. So in the default shipping/smoke config every surface is
   uniformly RGBA8-linear-no-decode and the pipeline is internally consistent;
   flipping `MC2_TEXMGR_COMPRESSED_UPLOAD=1` instantly makes mechs the only
   sRGB-decoded surface. This is the central VL-1 hazard — a loaded asymmetry,
   not a visible defect today. (The recon's "terrain sRGB / prop linear" framing
   was wrong on both location and liveness.)
4. **No surface shader does a manual sRGB->linear decode.** `gos_terrain.frag:365`,
   `static_prop.frag:200`, `mech.frag:102` all sample with a plain `texture()` /
   `textureLod()` and no `pow(c, 2.2)`. So in the default config NOTHING is
   decoded; decode happens only through the latent hardware-sRGB-storage gates
   above.
5. **The composite never encodes.** `postprocess.frag:143` writes
   `FragColor = vec4(color, 1.0)` with no OETF; the in-file comment at
   `postprocess.frag:109` says *"Tonemapping (no gamma — pipeline is already
   sRGB)"* — this is **aspirational and false**: nothing upstream encoded to
   sRGB and `GL_FRAMEBUFFER_SRGB` is off, so the backbuffer write is whatever the
   tonemap/grade produced, untransformed.

**Net effect.** In the default config the pipeline is "accidentally coherent in
the wrong space": sRGB-encoded 8-bit texels are sampled as if linear (no decode),
lit with linear math (dot/ambient/SH) on nonlinear values, and written without
encode (no re-encode). For an **unlit** texel (light = 1.0) this is an *exact*
identity pass-through (sRGB in, same sRGB out), which is why the image "looks
fine." But every **lit** texel is already mis-shaded: `srgb * NdotL` is not
`encode(decode(srgb) * NdotL)`, so shading a perceptual value darkens midtones
differently than shading a linear value. The scene is not "shifted-but-
consistent" — it is correctly-consistent only where nothing is lit, and
silently-wrong everywhere light varies. That is precisely why ambient/specular
tuning today is luck. Three things turn the latent wrongness into a visible one:

- **The latent sRGB-decode gate** (point 3): flip `MC2_TEXMGR_COMPRESSED_UPLOAD=1`
  and mechs decode-but-never-re-encode → they read too dark next to props/terrain
  on the same screen. Armed in the asset tree, off by default.
- **ACES tonemap (`MC2_TONEMAP_ACES`, default OFF) expects linear HDR input.**
  Fed sRGB-encoded values, its knee/shoulder land in the wrong place — a large
  part of why "ACES exists but is off": turning it on today applies a filmic
  curve in the wrong space.
- **The scene buffer is `GL_RGBA16F`** (`gos_postprocess.cpp:544-547`) — it *can*
  hold linear HDR, but nothing is putting calibrated linear data in it.

**The lighting models are fragmented** — at least eight distinct ambient/diffuse
stories run today, several of them live by default: terrain hemisphere ambient
(default-ON), terrain-overlay's own copy of it (default-ON), grass's own diffuse,
static-prop SH-L2 IBL (default-ON @0.5), mech hemisphere ambient (default-ON
@0.15), water sky-tint, decal lighting, and unlit VFX — each with independent
gates and defaults (Section 4, Section 8). **Depth is already coherent**
(reverse-Z scene + forward-Z shadow, Section 6) — that is the one contract the
engine already keeps, and it is the model for how the others should be enforced.

**Verdict:** Fix the **color space first** (VL-1). Until there is one defined
working space with a defined decode point and a defined encode point, every other
lane — lighting, material, tonemap, PBR — is tuning in an undefined space and its
"correct" values are luck. The color/output contract is the keystone; lighting
and material correctness are *downstream of it*, not parallel to it.

---

## 2. Desired render pipeline diagram

The target. Arrows are data; bracketed labels are the color space the data lives
in at that edge. The single rule: **lighting math happens in LINEAR, exactly
once; decode is at sample, encode is at the single final write.**

```
                            ASSET / TEXTURE
   albedo/emissive (sRGB-authored)      normal/MR/AO/mask (linear-authored)
            |                                       |
            | decode at sample                      | no decode (data textures)
            | (GL_SRGB* storage OR shader OETF^-1)   |
            v                                       v
        [LINEAR]                                 [LINEAR]
            \                                       /
             \                                     /
              v                                   v
        +-----------------------------------------------+
        |   SURFACE SHADING  (one lighting model)       |   <-- all in [LINEAR]
        |   sun + ambient(IBL/SH) + local + emissive    |
        |   * baseColor, * (1-metal) diffuse + spec     |
        +-----------------------------------------------+
                              |
                              | write [LINEAR HDR]
                              v
                  sceneFBO_  RGBA16F  (location 0)        <-- HDR scene color
                  GBuffer1   RGBA16F  (location 1: normal.xyz + shadow-mask.a)
                  ObjectId2  R32_UINT (location 2: pick id)
                  Depth24Stencil8     (reverse-Z, clear 0.0, GEQUAL)
                              |
        water / transparent / VFX draw INTO sceneFBO_     <-- still [LINEAR HDR]
                              |
                              v
        +-----------------------------------------------+
        |   POST CHAIN  (operates in [LINEAR HDR])       |
        |   SSAO (modulate before tonemap)               |
        |   depth fog / atmosphere                       |
        |   bloom extract+blur (threshold in linear)     |
        |   exposure                                     |
        |   ACES tonemap  [LINEAR HDR] -> [LINEAR LDR]    |
        |   color grade (lift/gamma/gain or LUT)         |
        +-----------------------------------------------+
                              |
                              | ENCODE ONCE: linear -> sRGB
                              | (GL_FRAMEBUFFER_SRGB on the final blit,
                              |  OR explicit OETF in the composite shader)
                              v
                      DEFAULT FRAMEBUFFER  [sRGB 8-bit]
                              |
                              | UI / HUD composited AFTER encode
                              | (drawn straight to FB0, sRGB-aware, NOT tonemapped)
                              v
                           DISPLAY
```

**Where today diverges from this diagram (all grep-verified):**

- Decode edge: **missing in the default config for all surfaces**; latent
  (default-OFF gate `MC2_TEXMGR_COMPRESSED_UPLOAD`) for mech/terrain/prop
  BC7-sRGB (`txmmgr.cpp:3747`).
- Surface shading "one lighting model": **violated** — five different models
  (Section 3).
- Post chain in linear HDR: the buffer is HDR (`RGBA16F`) but its contents are
  not calibrated linear, and the whole HDR/bloom/ACES sub-stack is **default
  OFF** (`gos_postprocess.cpp:252,261,273,325`). One grade *always* runs (the
  "sunset grade", `postprocess.frag:121-141`).
- Encode-once edge: **missing** (`postprocess.frag:143`, no OETF; no
  `GL_FRAMEBUFFER_SRGB`).
- UI-after-encode: **partially correct by sequencing** — HUD flushes to FB0
  after `endScene()` (`gameosmain.cpp:589-627`, `flushHUDBatch`
  `gameos_graphics.cpp:6344`), but because there is no encode step and the HDR
  sub-stack can apply to the shared framebuffer, the known issue "Bloom/FXAA/
  tonemapping apply to HUD" (`known_issues.md`) is the symptom of the missing
  exclusion boundary.

---

## 3. Color / output contract

This is the keystone contract. Everything else assumes it.

### 3.1 Definitions (the working spaces)

| Space | Meaning | Where it must live |
|---|---|---|
| **sRGB-encoded** | gamma-encoded 8-bit perceptual values | albedo/emissive texture *storage*; final framebuffer; UI textures |
| **Linear** | radiometric-linear values | everything between decode and encode: shading, ambient, fog, bloom threshold, tonemap input |
| **Linear HDR** | linear, range may exceed 1.0 | `sceneFBO_` (`RGBA16F`), post chain up to tonemap |

### 3.2 The five contract points

1. **Which textures are sRGB vs linear (by intent):**
   - **sRGB-authored (must decode on read):** albedo / baseColor, emissive, UI/
     HUD art, team-color art.
   - **Linear-authored (must NOT decode):** normal maps, metallic-roughness, AO,
     masks (wetness/damage/team-mask), data textures, depth, HDRI radiance
     (`GL_RGBA16F`, `gos_hdri.cpp:55`).
   - **Today:** in the default config albedo is stored linear *everywhere* (no
     surface decodes), so albedo is never decoded; the mech/terrain/prop BC7-sRGB
     decode paths are latent default-OFF gates (Section 1, point 3). This is the
     primary violation: there is no defined decode point, only a dormant
     inconsistent one. The fix is a per-texture "is this sRGB content?" flag
     honored at upload (choose `GL_SRGB8_ALPHA8` /
     `GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM` for albedo) **or** a shader-side decode
     — not both — applied to *all* albedo lanes together, not one gate at a time.

2. **Where decode happens:** exactly once, at **texture sample** in the surface
   shader, via sRGB texture storage (hardware) for albedo/emissive. Never a
   manual `pow()` *in addition* to sRGB storage (double-decode). Data textures
   are never decoded.

3. **Where lighting happens:** in **linear**, in the surface shader, after decode
   (Section 4). Per-vertex lit values that are interpolated (static props today,
   `static_prop.vert` Gouraud) must be computed from decoded-linear inputs.

4. **Where tonemap happens:** once, in the post chain, on **linear HDR** scene
   color, after SSAO/fog/bloom-extract, before grade and encode
   (`postprocess.frag` `tonemapSample()` at `:51`, currently gated by
   `enableTonemap`).

5. **Where final encode happens:** exactly once, at the **single composite write
   to the default framebuffer** — either by enabling `GL_FRAMEBUFFER_SRGB` for
   that blit or by an explicit linear->sRGB OETF in the composite shader (pick
   one; never both). Today this point does not exist
   (`postprocess.frag:143` writes untransformed; no `GL_FRAMEBUFFER_SRGB`).

6. **Where UI/HUD composites:** **after** the scene encode, drawn directly to
   FB0, treated as sRGB-encoded art, and **excluded from tonemap/bloom**. The
   sequencing already puts HUD after `endScene()` (`gameosmain.cpp:622`,
   `gameos_graphics.cpp:6393`). The contract adds: HUD must not be able to land
   in `sceneFBO_` (no HDR/bloom on HUD), and HUD art is sampled as sRGB.

### 3.3 Invariants (statically checkable / review-gated)

These are enforced by source inspection (grep/lint at review) plus the debug
views in Section 11, not by a runtime assert — except 3a, which the existing
`MC2_RENDER_CONTRACT_ASSERT` machinery (`render_contract.cpp`) can check.

- INV-COLOR-1: At most one decode per albedo sample. (No `pow()` decode in a
  shader that also binds a `GL_SRGB*` albedo.) *Check: grep each surface shader
  for `pow(`/manual decode against its bound texture's internalformat.*
- INV-COLOR-2: At most one encode per frame, at the composite write. *Check: grep
  for OETF (`pow(.,1.0/2.2)` / sRGB encode) in any shader writing location 0, and
  for `glEnable(GL_FRAMEBUFFER_SRGB)`; exactly one path may exist.*
- INV-COLOR-3a: `sceneFBO_` and `GBuffer1` are `RGBA16F`
  (`gos_postprocess.cpp:544-568`). *Check: runtime FBO-format assert.*
- INV-COLOR-3b: No shader writing to location 0 applies an OETF (the buffer holds
  linear; encode is deferred to the composite). *Check: grep location-0 writers.*
  (The "holds linear values" property itself is not runtime-distinguishable in an
  `RGBA16F` texel — the linear-vs-encoded debug toggle in Section 11 is the human
  check.)
- INV-COLOR-4: Normal/MR/AO/mask textures are never sRGB-stored and never
  decoded.
- INV-COLOR-5: UI/HUD is composited after encode and is not tonemapped.

### 3.4 The migration hazard (why this is delicate)

Because the engine is currently "wrong-but-consistent," flipping any *single*
edge in isolation makes the image visibly worse, not better:

- Add albedo decode without adding final encode → whole scene darkens (you
  un-did the accidental pass-through).
- Add final encode without albedo decode → whole scene brightens/washes.
- Enable ACES without calibrated-linear input → midtones crush or wash.

Therefore VL-1 must land the decode edge and the encode edge **together**, as one
atomic contract flip behind one master gate, with a default-OFF byte-identical
path and a default-ON calibrated path, validated on the Section 9 scenes. This is
the single most important sequencing constraint in the whole campaign.

---

## 4. Lighting contract

### 4.1 Shared lighting inputs (the vocabulary)

| Input | Source today | Owner |
|---|---|---|
| **Sun (direction + color)** | map data, not a render knob: dir `mapdata.cpp:703` / `terrtxm2.cpp:762`; color packed `startLight` `txmmgr.cpp:1028-1031` | gameplay/map |
| **Ambient (hemisphere)** | per-lane ad-hoc (terrain V1, prop AMBIENT_V1, mech AMBIENT_V1) | fragmented |
| **IBL / SH** | static props only: SH-L2, `IblShCoeffs.h:35`, upload `gos_static_prop_batcher.cpp:5254-5259` | static-prop lane only |
| **Local lights (point/spot/infinite)** | `LightsData` SSBO binding=20, `ObjectLights` schema `lighting.hglsl:35-56`; 6 types `lighting.hglsl:17-22` | shared SSBO, prop/mech consume |
| **Static baked light** | per-vertex Gouraud / hot-color tags baked CPU-side (`tgl.cpp`); terrain pre-bakes `TG_LIGHT_TERRAIN` specular into colors (`lighting.hglsl:273-281`) | terrain + props |
| **Dynamic light** | the `LightsData` point/spot slots updated per frame | shared |

There is **no render-side override for the gameplay sun** (no env/profile/ImGui
for sun color/direction/intensity). The only renderer-owned "sun" is the
decorative skybox sun color, hardcoded `gos_postprocess.cpp:1237-1239` — unrelated
to surface lighting. (Confirmed: `trackv-lighting-consistency-opus-1.md` §1.)

### 4.2 Which system uses which TODAY (the fragmentation)

| Lane | Diffuse | Ambient | Specular | IBL | Per-vertex or per-frag | Gate / default |
|---|---|---|---|---|---|---|
| **Terrain** | sun dot, splat | hemisphere V1 (`MC2_TERRAIN_LIGHTING_V1`) + V2 shadow floor | snow-sparkle micro-glint only (no material specular) | none | per-frag (frag splat) | V1/V2 **default ON**, kill-switch `=0` (`gameos_graphics.cpp:5414-5418,5427-5438`) |
| **Static prop** | Gouraud sun dot per-vertex (modulate `static_prop.frag:325`) | hemisphere AMBIENT_V1 *and* SH-L2 IBL | PBR V1 Schlick (gated) | SH-L2 vertex-stage (upload `gos_static_prop_batcher.cpp:5254-5259`) | **per-vertex** (lit color interpolated; `.frag` only modulates) | IBL_SH default ON @0.5; AMBIENT_V1 OFF; PBR_V1 OFF |
| **Mech** | sun dot | hemisphere AMBIENT_V1 (`gos_mech_batcher.cpp:184-190`) | Blinn sheen V1 (`mech.frag:138-172`) | none | per-frag | AMBIENT_V1 ON @0.15; SPECULAR_V1 ON @0.05 |
| **Water** | sky tint + optional SH-sky reflection + optional RT mirror (`gameos_graphics.cpp:2305-2345`) | sky tint | reflection-as-specular | SH-sky (gated) | per-frag | sky tint 0.0; reflection HARD-gated OFF |
| **VFX** | unlit; brightness scalars (`gos_particle_bridge.cpp:100-113`) | none | none | none | per-frag | always on (brightness=1.0) |

Five *opaque object* lanes shown above, plus three more scene-writing lit paths
not in this table (terrain-overlay carries its own duplicate hemisphere ambient,
default-ON; grass carries its own diffuse; decals carry their own inline shadow/
fog — see Section 8). That is eight-plus independent ambient/diffuse stories,
three specular stories, IBL on exactly one lane, and three different ambient
terms (terrain, mech, prop-SH) **live by default**. This is the "lighting models
are fragmented" finding, grounded.

### 4.3 Target after VL-2 (the lighting contract)

One ambient model, one specular model, shared inputs, all in linear:

- **Sun:** single shared directional term from map data, consumed identically by
  terrain/prop/mech/water. (Optionally a render-side warm/cool *grade* knob, but
  the *direction/color* stays map-owned — adding a sun override is a new feature,
  out of contract scope; noted in `trackv-lighting-consistency-opus-1.md` §1.)
- **Ambient:** **IBL SH-L2 is the canonical ambient** for all opaque lanes. The
  per-lane hemisphere terms (terrain V1, prop AMBIENT_V1, mech AMBIENT_V1) are
  *approximations of the same thing* and must converge to the SH path; hemisphere
  becomes a fallback when no SH set is bound, never an additive second term
  (the "both" double-count is already flagged:
  `v-staticprop-visual-review-audit.md` §5.a). Energy: ambient must not stack.
- **Local lights:** the shared `LightsData` SSBO (binding=20) is the one local-
  light source; mech and prop already read it; terrain's `TG_LIGHT_TERRAIN`
  pre-bake is the legacy exception to retire toward the shared path.
- **Specular:** one Fresnel-Schlick + roughness lobe model (the static-prop PBR
  V1 and mech Blinn V1 are two encodings of the same intent and must converge),
  parameterized by the material contract's metallic/roughness (Section 5).
- **Emissive:** additive, post-ambient, pre-tonemap; from `MaterialGpu::emissiveTex`
  (declared, not yet sampled — Section 5).

### 4.4 Per-vertex vs per-fragment

- **Static props are per-vertex (Gouraud).** This is the biggest structural
  outlier: ambient/IBL/specular are computed in `static_prop.vert` and
  interpolated; the fragment only modulates albedo by the interpolated lit color
  (`static_prop.frag:190-201`). Per-vertex IBL/specular is acceptable for V1 but
  caps quality (no per-pixel normal-mapped specular). The contract: **specular
  and normal-mapped terms are per-fragment**; flat ambient may stay per-vertex
  as a perf tier, but it must be the *same model* evaluated at lower frequency,
  not a different model.
- Terrain, mech, water, VFX are per-fragment already.

### 4.5 Baked vs dynamic

- **Baked:** SH-L2 ambient (loaded once at mission start, `IblShRegistry.h`);
  per-instance static light slots (permanent prefix, per the
  STATICPROP-PERMANENT-INSTANCE-LIGHTS work); terrain light pre-bake.
- **Dynamic:** `LightsData` point/spot slots; sun is map-static but applied each
  frame.
- Contract: baked terms are uploaded on a generation bump, not per frame; dynamic
  terms are the per-frame suffix. (This is already the discipline the perf track
  established for `LightsData` — the lighting contract inherits it.)

### 4.6 Lighting invariants

- INV-LIGHT-1: Ambient is single-source per pixel (SH OR hemisphere fallback,
  never summed).
- INV-LIGHT-2: All lanes consume the same sun term and the same `LightsData`
  SSBO schema (`lighting.hglsl:35-41` byte-lockstep with `TG_HWLightsData`).
- INV-LIGHT-3: Specular uses one BRDF, parameterized by material roughness/metallic.
- INV-LIGHT-4: Lighting math operates on **decoded-linear** albedo (depends on
  VL-1).

---

## 5. Material contract

### 5.1 Canonical material (target)

`MaterialGpu` (roadmap V1) is the one material struct every opaque surface flows
through. The roadmap's struct is the target; the contract pins the *meaning* of
each channel:

| Channel | Meaning | Space | Missing-map representation |
|---|---|---|---|
| **baseColor / albedo** | diffuse reflectance; under metallic, also the specular tint | **sRGB-stored, decoded to linear** | white `baseColorFactor` (1,1,1,1) |
| **roughness** | perceptual roughness (0=mirror, 1=rough); squared to α in BRDF | linear | scalar `roughnessFactor` (default 1.0 = fully rough/matte) |
| **metallic** | 0=dielectric, 1=conductor; selects F0 and kills diffuse | linear | scalar `metallicFactor` (default 0.0 = dielectric) |
| **normal** | tangent-space normal perturbation | linear (never sRGB) | flat (0,0,1) → use interpolated vertex normal |
| **emissive** | additive self-illumination, pre-tonemap | **sRGB-stored, decoded** | black (0,0,0) = no emission |
| **AO** | baked ambient occlusion, modulates ambient only | linear | white (1.0) = unoccluded |
| **team-color mask / damage mask** | gameplay overlays | linear (data) | zero = no overlay |

### 5.2 Which systems support which channels TODAY

Grounded against `static-prop-lighting-audit.md` §3.3 / §5 and the PBR-1 slices:

| Channel | Terrain | Static prop | Mech | Water | VFX |
|---|---|---|---|---|---|
| baseColor sample | yes (splat) | yes (`static_prop.frag:200`) | yes (`mech.frag:102`) | tint only | yes (billboard) |
| baseColor **decoded** | no | no | **only if `MC2_TEXMGR_COMPRESSED_UPLOAD=1`** (latent, `txmmgr.cpp:3747`) | no | no |
| roughness | n/a | **sampled under `MC2_STATICPROP_PBR_SLOTS` gate** (`static_prop.frag:242,370`) | partial (env roughness scalars `gos_mech_batcher.cpp:210-223`) | no | no |
| metallic | n/a | **sampled under `MC2_STATICPROP_PBR_SLOTS` gate** (same ORM sample) | scalar only | no | no |
| normal map | splat normals (`gameos_graphics.cpp:7874` `GL_RGBA8`) | **declared, NOT sampled** (vertex normal only) | no | flat-up stub | no |
| emissive | no | **declared, NOT sampled** | no | no | additive blend ≈ emissive |
| AO | no | no | no | no | no |

The recurring pattern: on static props the full `MaterialGpu` is *uploaded* but
only `albedoTex` (always) and `metallicRoughnessTex` (under the
`MC2_STATICPROP_PBR_SLOTS` compile gate, `static_prop.frag:242,370`) are
*consumed*; `normalTex` and `emissiveTex` remain dead payload (the
`static-prop-lighting-audit.md` §3.3 "albedo-only" finding was at an earlier HEAD
before the PBR-slots ORM sample landed). The material contract's job is to make
the shaders read what is already uploaded, and to do it on one schema across
lanes rather than one compile-gate per lane.

### 5.3 Which systems support them NEXT (order, not rerank)

The roadmap already fixes the order (V1 static-props/buildings → mechs → terrain
done → VFX separate; `2026-05-22-visual-fidelity-roadmap.md` V1). The contract
adds the *channel* sequence that minimizes risk on each lane:

1. **emissive** first (additive, cannot darken what works) — gated, samples the
   already-uploaded `emissiveTex`.
2. **metallic/roughness** next (drives the unified specular from Section 4.3).
3. **normal map** last on each lane (needs a TBN basis the static-prop vertex
   does not currently carry — `static-prop-lighting-audit.md` §6 step 4).

### 5.4 Material invariants

- INV-MAT-1: baseColor and emissive are sRGB-stored and decoded; all other
  channels are linear and never decoded (ties to INV-COLOR-4).
- INV-MAT-2: A missing map resolves to the neutral default in 5.1, never to an
  undefined sample.
- INV-MAT-3: One `MaterialGpu` schema across lanes; per-lane shaders may read a
  subset but never redefine a channel's meaning.

---

## 6. Depth / shadow / visibility contract

This is the one contract the engine **already keeps**. The job here is to write
it down and stop regressions, not to invent it.

### 6.1 Reverse-Z (LOCKED, runtime-true)

- Scene: `glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)` (`gameosmain.cpp:1065`),
  `glClearDepth(0.0)` (`gameosmain.cpp:529,577`), `glDepthFunc(GL_GEQUAL)` on
  every color pass (`gameos_graphics.cpp:3047,3248,3936,7617,8138,...`, each with
  the comment `// reverse-Z (U2): was GL_LEQUAL`). Near→1.0, far→0.0.
- Shadow FBOs: **forward-Z** — clear 1.0, `glDepthFunc(GL_LESS)`
  (`gameos_graphics.cpp:5099-5102,5246,5315-5320`; also
  `gos_postprocess.cpp:2163-2202`). Comment: *"scene set glClearDepth(0), so
  force 1.0f around this shadow clear."*
- HZB convention is **locked, no runtime yet**: MIN-reduce pyramid, ceil mip
  sizing, `glGenerateMipmap` prohibited, fail-closed
  ([docs/hzb-depth-convention.md](docs/hzb-depth-convention.md);
  `tests/unit/test_depth_hzb.cpp`). Owner `TRACKRV-HZB-VISIBILITY-OPUS-1`.

INV-DEPTH-1: scene is reverse-Z GEQUAL; shadow maps are forward-Z LESS; any new
pass picks the matching convention explicitly. This invariant is the template for
how all contracts in this document should be enforced (one documented convention,
asserted, with the "do not fix the wrong comment" discipline of
`critical_inline_rules.md`).

### 6.2 Depth prepass

- **There is no scene Z-prepass** (grepped `prepass`/`depthPrepass`/`zprepass`:
  only the shadow prepass exists, `beginShadowPrePass`
  `gameos_graphics.cpp:5072`). The contract does **not** mandate adding one; it
  records its absence so SSAO/decals/fog designs do not assume a populated depth
  texture before color. (SSAO consumes the post-color depth, which is fine.)

### 6.3 Alpha cutout

- Static props: `ALPHA_TEST_BIT = 1` (`static_prop.frag:141`), discard at
  `tex_color.a < 0.5` (`static_prop.frag:215-216`).
- Grass/foliage: discard at `bladeMask < 0.3` / `GrassAlpha < 0.01`
  (`gos_grass.frag:40-41`).
- Contract: alpha cutout threshold is **0.5** for cards/props; cutout is a
  material flag (`materialFlags` bit 0), not per-shader magic.

### 6.4 Shadow casters / receivers

- Per-object caster flag: `TG_Shape::noShadow` (`tgl.h:811`; early-return
  `tgl.cpp:3285`). Dynamic-shadow eligibility predicate excludes spotlights,
  windows, HUD, clamped, alpha-test (trees), and non-opaque alpha
  (`tgl.cpp:3114-3116`).
- Receiver contract is the `GBuffer1.a` screen-space-shadow mask:
  `a > 0.5` → pixel handled its own shadow, post-pass skips it;
  `a <= 0.5` → post-pass applies shadow (`render_contract.hglsl:6-8,75-77`).
  Static props/mechs emit `a=0` (eligible) via
  `rc_gbuffer1_screenShadowEligible`; terrain/grass emit `a=1` (self-handled).
- Kill-switches: `MC2_SHADOW_ENABLE` (`gos_static_prop_batcher.cpp:6620-6622`,
  default OFF), `MC2_SHADOW_DYNAMIC_PROP_CASTERS` (default ON under enable).

INV-SHADOW-1: receiver policy is expressed *only* through `GBuffer1.a`; no lane
invents a second shadow channel. The continuous-alpha escape hatch
(`rc_gbuffer1_legacyTerrainMaterialAlpha`, `render_contract.hglsl:48-64`) is a
**known contract violation** to retire — and it lives in the **legacy terrain
CPU-shader** water/shoreline path (`gos_terrain.frag:812,834,870`), *not* in the
modern MDI water shader. The MDI water path is contract-clean: it emits a binary
`.a=0` via `rc_gbuffer1_screenShadowEligible` (`gos_terrain_water_mdi.frag:256`).
So the violation is the old terrain-shader water path, which the water lane
should be migrated off of.

### 6.5 Object-ID / picking

- Object-ID target: `R32_UINT` at location 2
  (`render_contract.h:61` `ObjectId2_R32UI = 2`). Written by static props
  (`static_prop.frag:138`) and mechs (`mech.frag:85`). **Not** written by terrain
  (`gos_terrain.frag` declares locations 0/1 only), water, UI, shadow, or VFX
  (`render_contract.cpp:291` `writesLocation2 = false`; AlphaObject also false
  `:266`).
- Picking is **CPU**, not GPU readback: `Camera::inverseProject`
  (`camera.cpp:666,872`; comment `:884` "both pure CPU, no GPU readback"). The
  object-ID buffer is therefore an **art/debug inspection** target today (roadmap
  V10), not the picking authority.

INV-ID-1: object-ID is opaque-geometry-only (props + mechs); alpha and VFX never
write it (so picking through smoke/leaves is undefined-by-design). MLR-rendered
mechs do not write IDs (`renderworld_arc_status` residual) — empirically
`mlr_mech_draws=0` across tier1.

### 6.6 Foliage depth / shadow

- Tree impostors **skip shadow rendering**: `TreeAppearance::renderShadows` is a
  no-op (`bdactor.cpp:4151-4153`); trees call `SetUseShadow(false)`
  (`bdactor.cpp:4246`); buildings similarly (`bdactor.cpp:2167`).
- Tree cards are excluded from the GPU dynamic-caster path via `alphaTestOn`
  (`tgl.cpp:3109-3116`).
- The depth-only shadow fragment shader is **empty** — `void main() {}`,
  no alpha discard (`shadow_instanced.frag:1-2`). So any foliage that *does* cast
  casts a solid quad (SHADOW-FOLIAGE-ALPHA-DISCARD-1, deferred, `known_issues.md`).

INV-FOLIAGE-1: foliage depth uses alpha cutout in the color pass; foliage shadow
either does not cast (impostor) or must apply the same cutout in the shadow
program — never a solid card. (This is the contract VL-4 must satisfy.)

### 6.7 GBuffer layout (the MRT contract)

| Location | Name | Format | Carries |
|---|---|---|---|
| 0 | `FragColor` | `RGBA16F` | linear HDR scene color |
| 1 | `GBuffer1` | `RGBA16F` | `rgb` = world normal `*0.5+0.5`; `a` = shadow mask (>0.5 self-handled) |
| 2 | `ObjectId2` | `R32_UINT` | object handle (opaque props+mechs only) |
| depth | — | `DEPTH24_STENCIL8` | reverse-Z scene depth |

(`render_contract.h:61`, `gos_postprocess.cpp:544-568`, `static_prop.frag:133-138`,
`mech.frag:75-85`, `gos_terrain.frag:29,31`.)

---

## 7. Atmosphere / post contract

### 7.1 Pass order (target, all in linear HDR until encode)

```
sceneFBO_ (linear HDR, opaque + water + VFX already composited)
  -> SSAO            (modulate ambient term; before tonemap)
  -> depth fog       (lerp toward fog color in linear; before tonemap)
  -> bloom extract   (threshold in linear HDR) -> blur -> (composite at end)
  -> exposure        (scalar multiply)
  -> ACES tonemap    (linear HDR -> linear LDR)
  -> color grade     (lift/gamma/gain or LUT)
  -> bloom composite (add bloom)
  -> ENCODE linear->sRGB   (the single encode point, Section 3.2.5)
  -> [FB0 sRGB]
  -> UI/HUD          (drawn after, sRGB art, not tonemapped)
```

### 7.2 What exists today (grounded)

| Feature | Gate | Default | Site |
|---|---|---|---|
| HDR master | `MC2_HDR_POST` | OFF | `gos_postprocess.cpp:279-280` |
| ACES tonemap | `MC2_TONEMAP_ACES` (needs master) | OFF | `:300-302`; curve in `postprocess.frag` `tonemapSample()` `:46-53` |
| Bloom | `MC2_BLOOM` (needs master) | OFF | `:288-290` |
| SSAO | `MC2_SSAO` (independent) | OFF | `:352-353` |
| **Sunset grade** | none | **ALWAYS ON** | `postprocess.frag:121-141` (warm push, lum grade, vignette, top glow) |
| HDRI skybox | `MC2_HDRI_SKY` | ON | `gos_postprocess.cpp:181-208,981` (background only; not sampled by surfaces) |
| Final encode | — | **MISSING** | `postprocess.frag:143` (no OETF) |

Notes:
- Water and VFX draw into `sceneFBO_` **before** post, so both are tonemap- and
  bloom-eligible (`trackv-lighting-consistency-opus-1.md` §4). VFX default to
  alpha blend (`GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA`, `gos_particle_bridge.cpp:508`);
  per-group `blendMode==1` switches to additive (`GL_SRC_ALPHA, GL_ONE`,
  `gos_particle_bridge.cpp:602-607`), which accumulates past 1.0 → designed to
  bloom when the stack is on.
- **Fog is per-shader, and itself fragmented across lanes** — there is no post
  depth-fog pass. Static props use `mix(v_fog.rgb, c.rgb, u_fogValue)` (vertex
  fog, `static-prop-lighting-audit.md` §4.4); terrain uses its *own* per-fragment
  height-exponential fog with a hardcoded fog color `vec3(0.58,0.65,0.75)`
  (`gos_terrain.frag:840-849`) — a different mechanism. Fog is thus another
  fragmented contract. The contract slots a unified depth-fog post pass in 7.1,
  but that is a VL-5 *addition*, not a current state.
- The "sunset grade" runs unconditionally and is the de-facto baseline look.
  Because it runs in the wrong space (no decode upstream, no encode downstream),
  it is tuned to compensate for the missing color pipeline. **When VL-1 lands the
  decode/encode edges, the sunset grade must be re-tuned or it will double up.**
  This is a named stop-condition (Section 11).

### 7.3 UI/HUD exclusion

- HUD flushes to FB0 after `endScene()` (`gameosmain.cpp:589-627`,
  `flushHUDBatch` `gameos_graphics.cpp:6344,6393`). Contract: HUD is never in
  `sceneFBO_`, never tonemapped, never bloomed. The current "Bloom/FXAA/
  tonemapping apply to HUD" issue (`known_issues.md`) is the symptom of this
  boundary not being enforced when the HDR stack is on — VL-5 must make the
  exclusion structural (separate UI framebuffer or explicit gate), not
  sequencing luck.

### 7.4 Atmosphere invariants

- INV-POST-1: tonemap input is linear HDR; tonemap happens once, before encode.
- INV-POST-2: bloom threshold is evaluated in linear HDR (not post-encode).
- INV-POST-3: SSAO and fog modulate before tonemap.
- INV-POST-4: encode is the last scene operation; UI is after encode and excluded.
- INV-POST-5: exactly one always-on grade; if VL-1 changes the working space, the
  grade is re-tuned in the same change (no silently-stacked grades).

---

## 8. Subsystem conformance table

`CP` = color/output, `LT` = lighting, `MT` = material, `DS` = depth/shadow,
`AP` = atmosphere/post.

| Subsystem | Current path | Mismatches vs contract | Target path | Required changes | Risk |
|---|---|---|---|---|---|
| **Terrain** | PBR splat, per-frag; albedo `GL_RGBA8` linear (`gos_terrain_indirect.cpp:940`); hemisphere V1/V2 **default-ON** (`gameos_graphics.cpp:5414-5438`); self-shadow in-frag (`GBuffer1.a` self-handled); own height-exp fog; no objectId | CP: albedo not decoded. LT: own hemisphere ambient (live by default); no material specular (snow-sparkle only). MT: no MR/normal-map material struct. | decode albedo; SH ambient as canonical; unified specular; keep self-shadow | sRGB-stored colormap OR shader decode; converge ambient to SH; expose MR | **med** — terrain is the visual mass; re-tuning splat after decode is large |
| **Terrain-overlay** (cement/road/runway) | `terrain_overlay.frag`; **own** hemisphere V1+V2 ambient + own fog + own inline shadow; `GBuffer1.a` self-handled; **default-ON** via `MC2_TERRAIN_INDIRECT_OVERLAY` | CP: not decoded. LT: **duplicate** of terrain's hemisphere ambient — the fragmentation this doc catalogs, live by default. | decode albedo; share terrain's ambient, not a copy | fold ambient/fog into the terrain lane's model | **med** — default-ON; any terrain ambient change must touch both or they diverge |
| **Static props** | Gouraud **per-vertex**; albedo linear; SH-L2 IBL ON@0.5; AMBIENT_V1/PBR_V1 OFF; `MaterialGpu` albedo + gated MR sampled (`static_prop.frag:242,370`), normal/emissive dead; objectId written; `GBuffer1.a=0` | CP: albedo not decoded. LT: per-vertex specular; ambient can double (IBL+hemi). MT: normal/emissive dead; MR only under a compile gate. | decode albedo; per-frag specular; sample MR/emissive/normal uniformly | sRGB albedo; move spec to frag (needs TBN); read dead channels; pick single ambient | **med** — reference lane, has full closure; per-vertex→per-frag is structural |
| **Trees / foliage** | alpha-test cards (`static_prop.frag:215`); impostor far-LOD; shadow **skipped** (`bdactor.cpp:4151`); empty shadow frag | CP: same albedo-decode gap. DS: foliage shadow is solid-card if ever cast (`shadow_instanced.frag:1-2`). | decode albedo; cutout in shadow program; impostor matches lit card | sRGB albedo; alpha discard in shadow depth shader (SHADOW-FOLIAGE-ALPHA-DISCARD-1) | **low-med** — cutout in shadow is contained; impostor color match is the art risk |
| **Grass** | `gos_grass.frag`; **own** diffuse (`bladeNormal`, NdotL, `:51-56`); own shadow; alpha cutout (`:40-41`); `GBuffer1.a` self-handled | CP: not decoded. LT: yet another independent diffuse term. | decode; converge diffuse/ambient with terrain | fold into terrain lighting model | **low-med** — small screen area, but an independent lighting story |
| **Mechs** | per-frag; albedo **latently** sRGB-decodable (gated `MC2_TEXMGR_COMPRESSED_UPLOAD`, `txmmgr.cpp:3747`); hemisphere AMBIENT_V1 ON@0.15; Blinn spec V1 ON@0.05; objectId written; `GBuffer1.a=0` | CP: decode is a loaded gun — flip the gate and mechs alone decode → read too dark. LT: own ambient + own specular model. MT: scalar MR only. | one decode policy (all lanes together); SH ambient; unified BRDF; MR maps | settle the global decode decision so mechs match neighbors on or off; converge ambient/spec | **med-high** — hero asset; the one lane already half-armed, easiest to over/under-shoot |
| **Vehicles** | shares mech engine path (`mech3d.cpp` engine-side); same shaders | same as mechs | same as mechs | folds into the mech lane changes | **low** (rides mech lane) |
| **Water** | MDI fast path into `sceneFBO_` before post (`gameos_graphics.cpp:2402`); depth-driven color, no albedo texture; sky tint 0.0; SH-sky + RT reflection hard-gated OFF (`gameos_graphics.cpp:2305-2345`); MDI `.a=0` contract-clean (`gos_terrain_water_mdi.frag:256`) | CP: tonemapped when stack on; base luma below ACES knee → reads dark. LT: own reflection-as-spec. DS: the `.a` escape is the *terrain-shader* water path (`gos_terrain.frag:870`), not MDI. | raise exposure (not cut); reflection as IBL specular; migrate legacy terrain-water path off the `.a` escape | tune water exposure; route reflection through specular contract | **high** — water has z-fight + GL-state-cache fragility (`known_issues.md`); touch via GlStateGuard |
| **Particles / VFX** | billboards into `sceneFBO_`; default alpha blend (`:508`), per-group additive `SRC_ALPHA,ONE` (`gos_particle_bridge.cpp:602-607`); brightness scalars; lit/soft gated default-OFF; no objectId | CP: brightness tuned for no-encode pipeline. LT: unlit by default (lit/soft exist gated, V7). AP: additive blows out under bloom by design. | optionally lit (V7); brightness re-tuned for linear; soft-particle depth fade | re-tune brightness after VL-1; lit/soft is V7 (separate) | **med** — additive in linear HDR behaves very differently; re-tune required |
| **Decals** (scorch/footfall) | `decal.frag`; blends into scene; own cloud+static+dynamic shadow + own fog; `GBuffer1.a=1`; decal-bake shipped (`MC2_TERRAIN_INDIRECT_OVERLAY`) | CP: not decoded. AP: own fog/shadow copies. (Roadmap V6 grows this into a real system.) | decode; share fog/shadow; ride V6 | render path exists now; full contract deferred to V6 | **low** — small area; flagged so reviewers don't treat it as future-only |
| **Sky** (procedural gradient + HDRI) | `skybox.frag` writes location 0 (`zenith/horizon/sun` colors); HDRI skybox background-only, not sampled by surfaces (`gos_postprocess.cpp:981`) | CP: sky color authored in display space, lands in the same untransformed backbuffer — must ride the single encode like everything else | author sky in the chosen working space; ride the one encode | confirm sky color space vs the encode decision | **low** — easy to forget; sky is a location-0 writer too |
| **UI / HUD** | flushed to FB0 after `endScene()` (`gameos_graphics.cpp:6393`); textured art `GL_RGBA8`; HUD text is mask-multiply (`gos_text.frag`), not textured art | CP: no defined sRGB handling; can be tonemapped when stack on (`known_issues.md`). AP: exclusion is sequencing-only. | sRGB art; composited after encode; hard exclusion from tonemap/bloom | structural UI/scene separation; sRGB UI sampling (textured art); text-mask path needs no decode | **med** — readability regressions are immediately user-visible |

**Out of contract by design (named so reviewers do not assume coverage):** the
legacy MLR fragment path (`gos_vertex_lighted.frag` / `object_tex.frag`) still
exists as a fallback for non-batched mechs / dev-override gosFX, but is gate-dead
in the shipping path (`mlr_mech_draws=0` empirically across tier1,
`renderworld_arc_status`). It is not covered by this contract; do not count it as
conformant or non-conformant.

---

## 9. Implementation lanes — which VL lane satisfies which contract violation

This section maps contract violations to the lanes that fix them. **VL-1..VL-6
ordering is taken as given from the campaign brief and is NOT reranked here.** The
V1..V11 column shows which roadmap feature(s) each lane draws from.

| VL lane | Contract clause(s) it satisfies | Violations it closes | Roadmap items it draws from |
|---|---|---|---|
| **VL-1 color/output** | §3 all (INV-COLOR-1..5) | no decode edge; no encode edge; no `GL_FRAMEBUFFER_SRGB`; false "already sRGB" comment (`postprocess.frag:109`); latent mech-vs-rest decode gate | V4's **gamma/encode sub-step** (roadmap V4 line "Gamma correction (linear to sRGB)") — VL-1 makes the linear space the rest of V4 assumes real |
| **VL-2 lighting** | §4 all (INV-LIGHT-1..4) | eight+ ambient/diffuse models; ambient double-count; fragmented specular; IBL on one lane | V2 (IBL Phase 0), V1† (shared lighting via MaterialGpu) |
| **VL-3 material** | §5 all (INV-MAT-1..3) | dead normal/emissive on props; per-channel meaning undefined; missing-map undefined | V1† (PBR MaterialGpu contract), V9 (terrain material) |
| **VL-4 shadows/depth/cutout** | §6.3, §6.4, §6.6 (INV-SHADOW-1, INV-FOLIAGE-1) | foliage solid-card shadow; legacy terrain-water `GBuffer1.a` escape; receiver-policy drift | V3 (stable CSM — incl. its "per-object shadow flags" + "far-building LOD shadow policy") |
| **VL-5 atmosphere/post** | §7 all (INV-POST-1..5) | unconditional grade in wrong space; UI tonemap leak; no unified depth fog; fragmented per-lane fog; tonemap/bloom default-OFF and mis-spaced | V4 (bloom/tonemap), V5 (SSAO grounding) |
| **VL-6 PBR/normal polish** | §4.4 (per-frag specular), §5.3 step 3 (normal maps) | per-vertex prop specular; no normal-mapped surfaces; no energy-conserving BRDF | V1† (PBR depth), V2 Phase 1 (BRDF LUT), V11 (reactive surface rides PBR) |

† V1 spans VL-2, VL-3, and VL-6: it is the roadmap's PBR-material item, which
carries both the shared lighting model (VL-2) and the material schema (VL-3), and
its per-fragment depth is VL-6. One roadmap item, three contract lanes.

**Sequencing — a substrate dependency, NOT a roadmap rerank.** This contract does
**not** reorder the roadmap's feature delivery: the roadmap may ship V1 PBR before
V4 HDR exactly as it sequences (roadmap §"Sequencing"). What the contract asserts
is narrower and is about *color space*, not feature order: **the color-space edges
(decode at sample, encode at composite — V4's gamma sub-step, surfaced here as
VL-1) must be defined before any lane's color values are *finally tuned*.** If V1
PBR ships first (roadmap order), nothing is blocked — but its material/specular
values are then tuned in the undefined space and **must be re-tuned when VL-1
lands**. The contract makes that re-tuning cost explicit; it does not move V1. So:
VL-1 is a *prerequisite substrate* for the final tuning of VL-2/VL-3/VL-5, not a
re-prioritization of which roadmap feature ships first. VL-4 (depth/shadow/cutout)
is color-independent — it operates on geometry/coverage — so it proceeds in
parallel with VL-1 (why Session A's "VL-1 plan → maybe VL-4 plan" split is sound).
VL-6 polishes what VL-1..VL-3 establish.

---

## 10. Validation scenes (canonical)

Each scene is a fixed mission + camera preset, captured at three configs:
(A) **default-OFF** (byte-identical regression baseline),
(B) **contract-ON calibrated**, (C) **contract-ON + per-channel debug view**.
Capture via `scripts/quick_shot.py <MISSION> <WAIT> <LABEL>` (parent-shell env
reaches the engine; `trackv-lighting-consistency-opus-1.md` §4) or direct launch.
PNG sha is not pixel-proof — for byte-identity claims use the in-engine
framebuffer-hash smoke probes, not OS screenshots
(`v-staticprop-visual-review-audit.md` §6).

| # | Scene | Mission(s) | What it proves | Pass criterion |
|---|---|---|---|---|
| S1 | **Terrain + static prop, same albedo family** | mc2_03, mc2_17 | terrain and props read as one material family after decode | a grey building on grey ground has matching mid-grey; no lane is darker than the other |
| S2 | **Mech standing on terrain** | mc2_24 | the mech-decode asymmetry is resolved | mech albedo no longer reads darker than the ground it stands on |
| S3 | **Water + terrain + props** | mc2_24, mc2_03 | water sits in the same space; not crushed under ACES | shoreline reads bright not black; no double-tonemap halo at water edge |
| S4 | **Foliage near / mid / far** | mc2_24 (tree-heavy) | LOD/impostor + cutout coherent across distance; shadow not solid-card | impostor color matches near card; foliage shadow (if cast) shows cutout, not a quad |
| S5 | **UI / HUD overlay** | any | UI excluded from tonemap/bloom; sRGB-correct | HUD text/bars identical with stack ON vs OFF; no bloom on HUD |
| S6 | **Night / dark scene** | a low-light map | decode/encode correct in the dark; lit-window glow | shadowed sides not crushed to black; emissive windows glow (needs isNight wiring, `static_prop.vert:172-175`) |
| S7 | **Shadow scene** | mc2_17 | reverse-Z scene + forward-Z shadow stable; receiver mask correct | no acne/peter-pan; props receive, terrain self-handles; no shadow on HUD |
| S8 | **Fog scene** | a fog map | fog reads in linear; no banding after encode | fog lerp smooth; distant objects fade to fog color, not to grey-washed |

S1 and S2 are the **primary VL-1 acceptance scenes** — if the same-albedo-family
test fails, the color contract is not satisfied regardless of any other result.

---

## 11. Debug views needed

The roadmap's per-feature debug table (`2026-05-22-visual-fidelity-roadmap.md`,
"Debug mode requirement") is the precedent. The contract additionally requires
these **cross-lane** views, because the violations are about *consistency across
lanes* and today no single vocabulary spans all lanes
(`trackv-lighting-consistency-opus-1.md` §3: `kDebugViewMask_Terrain = 0`,
terrain is outside the canonical `RenderDebugView` registry).

| Debug view | Purpose | Gap today |
|---|---|---|
| **Albedo-only, all lanes** | confirm same-family albedo (S1/S2) | terrain albedo view exists but outside canonical registry; needs `kDebugViewMask_Terrain` flipped non-zero + a `TerrainViewToShaderMode` map (no shader edit) |
| **Linear-vs-encoded toggle** | see the working space directly; catch double-decode/encode | does not exist — new, VL-1 |
| **Ambient-only (IBL/SH), all opaque lanes** | confirm single ambient model | no IBL-only view on any opaque lane (`trackv-lighting-consistency-opus-1.md` §3: prop IBL is vertex-stage, needs new frag branch) |
| **Specular-only, all opaque lanes** | confirm one BRDF | none exists on any lane |
| **AO-only** | SSAO grounding | exists (`MC2_SSAO_DEBUG`, `ssao_apply.frag`) |
| **Material channel view (albedo/normal/MR/AO/emissive)** | confirm channel meaning (VL-3) | exists on static props (`MC2_STATIC_PROP_DEBUG_MATERIAL` modes 1-6); needs extending to mech/terrain |
| **GBuffer1.a (shadow-mask) view** | confirm receiver policy | not a dedicated view; derive from existing shadow debug |
| **Object-ID readback view** | art inspection (roadmap V10) | exists as `ObjectIdDebug` ViewMode; props+mechs only |
| **Overdraw / fill view (foliage, VFX)** | catch coverage blowups | VFX overdraw exists (`particle_billboard.frag` mode 4); foliage none |

Minimum bar for VL-1 to be reviewable: the **linear-vs-encoded toggle** and the
**albedo-only all-lanes** view must exist, or the color contract cannot be
visually verified.

---

## 12. Stop conditions

Hard stops. If any trips, halt the lane and review before proceeding.

1. **S1/S2 albedo-family test fails after VL-1.** If terrain and props (or mech
   and ground) do not read as one family with the contract ON, the decode/encode
   edges are wrong — do not proceed to VL-2/VL-3. (This is the whole point of
   VL-1.)
2. **Default-OFF path is not byte-identical.** Every VL lane must keep a
   default-OFF path that is byte-identical to shipped (the established Track V
   discipline: `trackv-post-grounding-soak-1.md` "Default behavior declaration";
   `ibl-plan.md` §8.1). A non-identical OFF path is a regression — stop.
3. **Double decode or double encode.** Any pixel that is sRGB-decoded twice or
   encoded twice (e.g. sRGB texture storage **and** a shader `pow()`; or
   `GL_FRAMEBUFFER_SRGB` **and** a composite OETF). INV-COLOR-1/2. Stop and pick
   one.
4. **The sunset grade was not re-tuned with VL-1.** The always-on grade
   (`postprocess.frag:121-141`) is tuned for the wrong space; landing VL-1 without
   re-tuning it stacks two corrections. INV-POST-5. Stop.
5. **UI/HUD enters `sceneFBO_` or gets tonemapped.** INV-POST-4 / INV-COLOR-5.
   The known issue must get *better*, never worse, under the contract. Stop.
6. **A lane invents a second ambient, specular, shadow, or color channel** rather
   than converging to the shared one. The entire campaign exists to remove
   fragmentation; adding a parallel model is the anti-goal. INV-LIGHT-1/3,
   INV-SHADOW-1, INV-MAT-3. Stop.
7. **Reverse-Z / shadow-Z convention is altered.** The depth contract (§6.1) is
   already correct and load-bearing; "fixing" a depth func or a clear value, or
   correcting the `// reverse-Z (U2)` comments, is forbidden without an explicit
   depth-contract review (`critical_inline_rules.md` "GL_FALSE for terrainMVP"
   precedent — do not fix comments that look wrong but are load-bearing).
8. **Any GPU-direct pass touched by a color/decode change without a GL-state
   guard.** The GL-state-cache META-FIX DEBT (`known_issues.md`: GlStateGuard)
   covers **every** GPU-direct pass VL-1 will touch — static-prop and mech
   batchers, terrain bridges, water fast path, particle bridge, post-process —
   not water alone. Any of them changed for decode/encode must respect the manual
   save/restore + `gos_InvalidateRenderStateCache()` discipline or it will
   flicker. Stop and route through the guard.
9. **New MaterialGpu uniform upload that ignores the explicit-program / transpose
   rules.** VL-3 makes shaders read more `MaterialGpu` channels, which adds uniform
   uploads. Per `critical_inline_rules.md`: an upload that takes a `GLuint program`
   MUST use the `glProgramUniform*` family (not `glUniform*`, which writes the
   bound program — silent wrong-shader bug), and direct-uploaded row-major
   matrices use `GL_FALSE` while the material cache uses `GL_TRUE`. A new upload
   that violates either is a stop.
10. **No debug view for the edge being changed.** Per Track V discipline, a
    contract edge without its debug view (§11) is not reviewable — do not ship the
    edge first and the view later. (INV-LIGHT-1 specifically is unverifiable until
    the ambient-only all-lanes view in §11 exists.)

---

## 13. Cross-references (inputs this contract sits beneath)

- [docs/superpowers/specs/2026-05-22-visual-fidelity-roadmap.md](docs/superpowers/specs/2026-05-22-visual-fidelity-roadmap.md)
  — the VL feature roadmap (V1..V11). **Input. Not replaced.**
- [docs/trackv-status.md](docs/trackv-status.md) — Track V status ledger
  (shipped post/grounding, lighting-consistency, VFX payoff; all gates default-OFF).
- [docs/trackv-lighting-consistency-opus-1.md](docs/trackv-lighting-consistency-opus-1.md)
  — per-lane lighting/tunable inventory + debug-view coverage (Section 3, 4, 7
  here lean on it).
- [docs/trackv-post-grounding-soak-1.md](docs/trackv-post-grounding-soak-1.md)
  — HDR/bloom/ACES/SSAO post stack, default-OFF declaration, capture matrix.
- [docs/static-prop-lighting-audit.md](docs/static-prop-lighting-audit.md)
  — the per-vertex Gouraud model + dead MaterialGpu channels (Section 4, 5 here).
- [docs/ibl-plan.md](docs/ibl-plan.md) — SH-L2 IBL choice, cooker/loader cubemap
  gap (Section 4, 5 here).
- [docs/v-staticprop-visual-review-audit.md](docs/v-staticprop-visual-review-audit.md)
  — gate-state matrix, ambient double-count finding, IBL flip recommendation.
- [docs/hzb-depth-convention.md](docs/hzb-depth-convention.md) — locked reverse-Z
  / MIN-reduce depth contract (Section 6.1).
- [docs/critical_inline_rules.md](docs/critical_inline_rules.md) — grounding,
  shader/GL, change-discipline rules (the enforcement model for these invariants).
- [docs/known_issues.md](docs/known_issues.md) — GlStateGuard debt, foliage
  shadow alpha-discard, HUD-tonemap issue, water z-fight (stop conditions 5, 8).
- `mclib/render_contract.h` / `shaders/include/render_contract.hglsl` — the
  existing GBuffer/shadow-mask contract this document extends.
```
