# Object-Offload Slice 2 — Substrate Gap Recon (Cause Map)

Date: 2026-05-03
Branch: `claude/nifty-mendeleev`
HEAD: `95baa44` (Stage 2.D.3 plumbing committed; substrate gate pending)
Author: subagent dispatched per Stage 2.D.3 substrate-gap-recon-prompt
Skill applied: `.claude/skills/adversarial-plan-review.md` (verification appendix in §8)

> **Purpose:** Code-grounded cause map for the 56-pair Stage 2.D.3 mismatch
> inventory. Maps each mismatch class to a concrete CPU operation that has
> no GPU equivalent (or a wrong/incomplete one). The controller will use
> this map to pick A1 (incremental) vs A2 (coordinated atomic) for the
> follow-up substrate fix slice. **Recon — no code changes.**

---

## §1. Executive summary

Walking the CPU per-vertex lighting kernel (`mclib/tgl.cpp:1657-2344`) line
against the GPU kernel (`shaders/static_prop.vert` + `shaders/include/lighting.hglsl`)
surfaces **four distinct CPU operations that have no GPU equivalent**, plus
one C++/CPU-state asymmetry. None of these are the 6 bugs already fixed in
`38ba240` + `014ceb8` — those were direction/swizzle/temporal/window-skip
fixes. The inventory's mismatches are a **superset**, exposing operations
that the prior parity passes (mc2_01 1200-frame, mc2_24 1800-frame) did not
exercise because they were dominated by Bldg/Tree populations whose worldLights
are correctly populated.

The four substrate gaps, in dominance order:

1. **GenericAppearance + `SetLightList(NULL,0)` → GPU returns vec3(1) (white-out).**
   `genactor.cpp:1201` zeroes `s_listOfLights`/`s_numLights` immediately
   before `genShape->CacheGpuLightData()` at `genactor.cpp:1219`. The
   cached `lightData_.numLights_` ends up `0`. CPU's vertex loop also
   skips lighting (line 1938) but produces the per-vertex tag color directly
   (typically `0xFF000000`). GPU's `calc_light` short-circuits at
   `lighting.hglsl:195` with `return vec3(1)`. Result: GPU emits
   `0xFFFFFFFF`, CPU emits `0xFF000000` (or near-black). Explains the
   single observed white-out at mc2_18 typeId=474, AND likely many of
   the colored-cpu-gray-gpu pairs where the GPU value is mid-gray
   (`0x2A` / `0x80`) that come from texture multiplication of the
   white in fragment-stage outside parity scope.

2. **CPU-only `lighteningLevel` write to redSpec/greenSpec/blueSpec, NOT
   carried through to GPU's lit ARGB.** `tgl.cpp:1798-1801` seeds
   `redSpec/greenSpec/blueSpec = lighteningLevel` when nonzero. These
   feed `frgb` (fog/spec channel) at `tgl.cpp:2295/2299/2304/2317`. The
   `listOfTriangles[j].fRGBLight` write at line 2544 then propagates them
   into the parity-comparison source's `frgb`. **GPU has no equivalent
   path** — `static_prop.frag` consumes only `v_argb` (tex \* v_argb +
   highlight + fog mix). Stock missions tier1 likely have
   `lighteningLevel == 0` (only `weather.cpp:371` writes it during
   lightning bolts), so this is unlikely to be the dominant cause but
   is a real gap.

3. **CPU's TG_LIGHT_TERRAIN pre-bake into `listOfColors[].redSpec/.greenSpec/.blueSpec`
   at `tgl.cpp:2146-2148` is silently dropped on GPU.** Per spec line 270,
   slice 2 ships with TG_LIGHT_TERRAIN GPU-side ignored
   (`lighting.hglsl:255-263`). CPU applies this to `redSpec/greenSpec/blueSpec`
   which then flow into `frgb` (the spec/fog channel). Like #2, GPU
   doesn't read this. The visual effect is small (specular-only) but
   accumulates across the per-vertex bake, especially when `useShadows`
   is true. Could contribute to grayscale-additive class.

4. **CPU's redSpec/greenSpec/blueSpec accumulation from TG_LIGHT_POINT
   and TG_LIGHT_SPOT.** Lines 2093-2095, 2117-2119, 2196-2198 add
   per-light contributions to `redSpec/greenSpec/blueSpec` (NOT to
   `redFinal/greenFinal/blueFinal`). These end up in `frgb`, NOT `argb`.
   GPU's `calc_light` adds POINT and SPOT contributions to `final`
   (the diffuse color) at `lighting.hglsl:225-253`. **Direction of
   contribution differs**: CPU sends point/spot light into the spec
   channel (which `static_prop.frag` doesn't sample); GPU sends them
   into the diffuse channel (which `v_argb` carries to the parity
   buffer). For any actor near a point or spot light source, GPU's
   `v_argb` will have an additive term that CPU's `argb` won't.
   Possible cause of the colored-colored class (#3 in symptom map)
   when actor sits near base/spot lights.

5. **C++/CPU-state asymmetry with `BaseVertexColor`.** `BaseVertexColor`
   defaults to `0` (`code/logmain.cpp:77`, `code/mechcmd2.cpp:164`).
   Both CPU (`tgl.cpp:1892-1905`) and GPU (`lighting.hglsl:143`) add
   it to the per-vertex color when nonzero. The CPU uses 0..255 byte
   space, GPU uses 0..1 normalized. **Same algorithm but different
   precision: GPU rounds at `uint(clamp(x*255+0.5))` (line 52-55).**
   For nonzero `BaseVertexColor`, this can produce ±1 LSB drift that
   the parity tolerance `channelsWithin2` already absorbs. Not
   load-bearing for stock tier1 but flagged as a near-future cause if
   prefs change brightness.

The dominant cause is **#1 (GenericAppearance white-out)**: a single
substrate fix (don't call `CacheGpuLightData()` after `SetLightList(NULL,0)`,
or have `calc_light` accept a "use base_light only" flag) likely closes
the white-out anomaly AND a chunk of colored-cpu-gray-gpu pairs that arise
when the same population ships with default `0xFF000000` aRGBLight tags
and gets gray-textured. Causes #2/#3/#4 are smaller in stock missions.

**Confidence**: HIGH on #1 (code-grounded one-to-one map, including the
exact early-return). MEDIUM on #2/#3/#4 (depends on which actors hit the
sample, which can't be confirmed without runtime inspection). LOW on #5
(prefs-dependent).

**Recommendation**: A1 incremental — land #1 first, re-sample, then
decide whether #2-#4 need separate fixes or whether the inventory's
remaining classes are explained by a new fifth bug not surfaced here.
Detail in §6 + §8.

---

## §2. CPU lighting kernel — operation inventory

`MultiTransformShape` per-vertex loop (tgl.cpp:1745-2344). Listed operations
in load order, with file:line and inputs.

| # | Op | CPU file:line | What it does | Inputs | Output |
|---|---|---|---|---|---|
| C1 | Position transform (perspective) | tgl.cpp:1754-1768 | `xformCoords = pos * shapeToClip; rhw=1/w; screen.xy = xform.xy*rhw*viewMul + viewAdd` | `theShape->listOfTypeVertices[j].position`, `shapeToClip` | `listOfVertices[j].x/y/z/rhw` |
| C2 | Lightening level seed | tgl.cpp:1798-1801 | If `lighteningLevel > 0`, `redSpec=greenSpec=blueSpec=lighteningLevel` | `TG_Shape::lighteningLevel` static (set by `weather.cpp:371`) | `redSpec/greenSpec/blueSpec` (DWORD locals) |
| C3 | Magic Hot Pink → night windows | tgl.cpp:1804-1828 | If `aRGBLight==0xffff00ff`: night → `redFinal/greenFinal/blueFinal = hotPinkRGB`; daytime → `0x2F` gray | `theShape->listOfTypeVertices[j].aRGBLight`, `theShape->hotPinkRGB`, `isNight`, `nightFactor` | redFinal/greenFinal/blueFinal |
| C4 | Magic Hot Yellow → outside building | tgl.cpp:1829-1850 | If `aRGBLight==0xffffff00` AND `nightFactor >= 0.75`: `redFinal/greenFinal/blueFinal = hotYellowRGB` | aRGBLight, hotYellowRGB, nightFactor | redFinal/greenFinal/blueFinal |
| C5 | Magic Hot Green → base light | tgl.cpp:1851-1869 | If `aRGBLight==0xff00ff00`: night → `redFinal/greenFinal/blueFinal = hotGreenRGB`; daytime → 0 | aRGBLight, hotGreenRGB, isNight, nightFactor | redFinal/greenFinal/blueFinal |
| C6 | Magic Hot Red/Blue → no-op | tgl.cpp:1870-1877 | Empty bodies (placeholder for blink lights) | aRGBLight | none |
| C7 | Non-magic colored tag | tgl.cpp:1878-1886 | If `aRGBLight & 0x00ffffff != 0` AND `!lightsOut`: `redFinal/greenFinal/blueFinal = aRGBLight bytes` | aRGBLight, lightsOut | redFinal/greenFinal/blueFinal |
| C8 | HUD element fallback | tgl.cpp:1887-1890 | `redFinal=greenFinal=blueFinal=0xff` | isHudElement | redFinal/greenFinal/blueFinal |
| C9 | BaseVertexColor additive | tgl.cpp:1892-1905 | redFinal += R; greenFinal += G; blueFinal += B; clamp 255 | global `BaseVertexColor` | redFinal/greenFinal/blueFinal |
| C10 | Lighting gate | tgl.cpp:1934-1936 | Enter only if `useVertexLighting && Renderer != 3 && !isSpotlight && !isWindow` | useVertexLighting=true (terrain.cpp:166), Environment.Renderer=0 (logmain.cpp:786), isSpotlight, isWindow | gate |
| C11 | TG_LIGHT_AMBIENT | tgl.cpp:1944-1959 | `redAmb = R; greenAmb = G; blueAmb = B` (latches; not accumulated) | `s_listOfLights[i]->aRGB`, `s_listOfLights[i]->lightType` | redAmb/greenAmb/blueAmb |
| C12 | TG_LIGHT_INFINITE | tgl.cpp:1961-2030 | `cosine = lightDir · normal; if cosine < 0: redFinal += abs(cosine) * R_light; ...` | s_lightDir[i], s_listOfLights[i]->aRGB, theShape->listOfTypeVertices[j].normal | redFinal/greenFinal/blueFinal |
| C13 | TG_LIGHT_INFINITEWITHFALLOFF | tgl.cpp:2033-2061 | Falloff(length) * cosine * lightColor → redFinal/greenFinal/blueFinal | s_lightToShape[i], s_lightDir[i], pos, normal, lightColor | redFinal/greenFinal/blueFinal |
| C14 | **TG_LIGHT_POINT → spec channel** | tgl.cpp:2064-2122 | Falloff * cosine * lightColor → **redSpec/greenSpec/blueSpec** (NOT redFinal!). Branch only adds when `cosine < 0` (line 2082). | s_lightDir[i], lightColor, normal | **redSpec/greenSpec/blueSpec** |
| C15 | **TG_LIGHT_TERRAIN → listOfColors[]** | tgl.cpp:2125-2158 | If `useShadows`, writes `listOfColors[j].redSpec/.greenSpec/.blueSpec = falloff * lightColor` | useShadows, s_lightDir[i], lightColor, theShape->listOfTypeVertices[j].position | **listOfColors[j].redSpec/.greenSpec/.blueSpec** (NOT redFinal!) |
| C16 | **TG_LIGHT_SPOT → spec channel** | tgl.cpp:2160-2202 | Same as POINT but uses s_spotDir for cone → **redSpec/greenSpec/blueSpec** (NOT redFinal!) | s_spotDir[i], s_lightDir[i], lightColor, normal | **redSpec/greenSpec/blueSpec** |
| C17 | Ambient flat add | tgl.cpp:2207-2210 | `redFinal += redAmb; greenFinal += greenAmb; blueFinal += blueAmb` | redAmb/greenAmb/blueAmb | redFinal/greenFinal/blueFinal |
| C18 | redFinal/greenFinal/blueFinal clamp | tgl.cpp:2212-2219 | Clamp each to 255 | (locals) | (locals) |
| C19 | argb pack with hardcoded alpha | tgl.cpp:2232 | `listOfVertices[j].argb = (0xff<<24) + (redFinal<<16) + (greenFinal<<8) + blueFinal` | redFinal/greenFinal/blueFinal | listOfVertices[j].argb |
| C20 | Pre-baked terrain spec accum | tgl.cpp:2238-2240 | `redSpec += listOfColors[j].redSpec; ...` (combines this-frame + prior-frame terrain bake) | listOfColors[j].redSpec/.greenSpec/.blueSpec | redSpec/greenSpec/blueSpec |
| C21 | redSpec/greenSpec/blueSpec clamp | tgl.cpp:2242-2249 | Clamp to 255 | (locals) | (locals) |
| C22 | Else-branch (no vertex lighting) | tgl.cpp:2253 | `argb = (0xff<<24) + (redAmb<<16) + (greenAmb<<8) + blueAmb` | redAmb/greenAmb/blueAmb | listOfVertices[j].argb |
| C23 | Per-vertex elevation fog | tgl.cpp:2256-2294 | Computes `fogValue` from elevation; writes `frgb` field | listOfVertices[j].frgb (init from fogRGB at line 1789), useFog | listOfVertices[j].frgb |
| C24 | Frgb pack with spec channel | tgl.cpp:2295/2299/2304/2317 | `frgb = (fogValue<<24) + (redSpec<<16) + (greenSpec<<8) + blueSpec` | fogValue, redSpec/greenSpec/blueSpec | listOfVertices[j].frgb |
| C25 | Distance fog | tgl.cpp:2309-2318 | If `useFog && Camera::HazeFactor != 0`, recompute fogValue from haze | Camera::HazeFactor, useFog | fogValue, listOfVertices[j].frgb |
| C26 | aRGBHighlight additive | tgl.cpp:2320-2343 | If `aRGBHighlight != 0`: rFinal += R; gFinal += G; bFinal += B; clamp 255; rewrite argb | aRGBHighlight, listOfVertices[j].argb | listOfVertices[j].argb |
| C27 | Per-face flight loop entry | tgl.cpp:2349-2502 | Iterate all faces; for each, accumulate redFinal/greenFinal/blueFinal/redSpec/greenSpec/blueSpec from `useFaceLighting` (FALSE in stock) | tri->faceNormal, useFaceLighting=false | (mostly dead) |
| C28 | listOfTriangles[j].aRGBLight write | tgl.cpp:2504-2523 | For each corner: `argb = listOfVertices[V].argb`; rFinal += redFinal (face); clamp; `aRGBLight[i] = (alphaValue<<24) + (rFinal<<16) + (gFinal<<8) + bFinal` | listOfVertices[V].argb, redFinal/greenFinal/blueFinal (face), alphaValue | listOfTriangles[j].aRGBLight[i] |
| C29 | listOfTriangles[j].fRGBLight write | tgl.cpp:2525-2544 | `frgb = listOfVertices[V].frgb`; rFinal += (redSpec>>16) & 0xff (a no-op for redSpec≤255 — note CPU bug here); `fRGBLight[i] = ...` | listOfVertices[V].frgb, redSpec/greenSpec/blueSpec (face) | listOfTriangles[j].fRGBLight[i] |
| C30 | TG_TypeShape::init magic node parsing | tgl.cpp:259-260, 475-476 | `isSpotlight = strnicmp("SpotLight_",10)==0; isWindow = strnicmp("LitWin_",6)==0` | newShape->getNodeName() | isSpotlight, isWindow flags |

**The load-bearing per-vertex output for parity comparison is C19 + C26**:
`listOfVertices[j].argb` after the highlight additive. The Stage 2.D parity
harness reads this at `gos_static_prop_batcher.cpp:825`.

---

## §3. GPU lighting kernel — operation inventory

`shaders/static_prop.vert` + `shaders/include/lighting.hglsl`. Operations
in load order.

| # | Op | GPU file:line | What it does | Inputs | Output |
|---|---|---|---|---|---|
| G1 | Position transform | static_prop.vert:101-112 | D3D-style worldToClip, manual divide, then `gl_Position = ndc.xyz * absW` | a_position, inst.modelMatrix, u_worldToClip, u_terrainViewport, u_mvp | gl_Position |
| G2 | Behind-camera guard | static_prop.vert:124-126 | If clip4.w < 0.1: gl_Position = vec4(2,2,2,1) (cull) | clip4.w | gl_Position |
| G3 | a_aRGBLight unpack | static_prop.vert:137-141 | `perVertexARGB.x = (a_aRGBLight >> 0) & 0xff / 255` (B); .y=G; .z=R; .w=A | a_aRGBLight (uint, raw DWORD) | perVertexARGB (vec4 in BGRA-channel-order) |
| G4 | PerType SSBO lookup | static_prop.vert:145 | `ptd = perType_.t[inst.typeID]` | inst.typeID, per-type SSBO (filled at finalizeGeometry from TG_TypeShape::hot{Pink,Yellow,Green}RGB) | ptd.hotPinkRGB.rgb / .hotYellowRGB.rgb / .hotGreenRGB.rgb |
| G5 | get_base_light dispatch | static_prop.vert:152-157 | Calls `get_base_light(perVertexARGB, isNight=false, nightFactor=0, isHudElement=false, lightsOut=false, ...)` — **isNight/nightFactor/lightsOut all hardcoded false/0/false** | perVertexARGB, ptd.hot*RGB | base_light (vec3 in [0,1]) |
| G5a | Hot Pink magic | lighting.hglsl:66-80 | If `start_v_light == 0xffff00ff`: night → hotPinkRGB; daytime → vec3(0x2f/255) | startVLight, isNight, nightFactor, hotPinkRGB | final |
| G5b | Hot Yellow | lighting.hglsl:81-94 | Mirrors C4 | (same) | final |
| G5c | Hot Green | lighting.hglsl:95-105 | Mirrors C5 | (same) | final |
| G5d | Hot Red/Blue | lighting.hglsl:106-113 | Empty bodies (mirrors C6) | startVLight | none |
| G5e | Non-magic colored tag | lighting.hglsl:114-137 | If `(start_v_light & 0xffffff) != 0` AND `!lightsOut`: `final = startVLight.zyx` (BGR→RGB swizzle, gated by MC2_STATIC_PROP_LIGHTING) | startVLight, lightsOut, MC2_STATIC_PROP_LIGHTING define | final (vec3 R,G,B in [0,1]) |
| G5f | HUD fallback | lighting.hglsl:138-141 | `final = vec3(1)` if isHudElement | isHudElement | final |
| G5g | BaseVertexColor add | lighting.hglsl:143 | `final.xyz += g_scene.baseVertexColor.xyz` (sceneData uniform from txmmgr.cpp:1141, swizzled `.zyxw()` from BaseVertexColor DWORD) | g_scene.baseVertexColor | final |
| G5h | get_base_light clamp | lighting.hglsl:145 | `clamp(final.xyz, 0, 1)` | final | base_light |
| G6 | World-space normal/pos | static_prop.vert:172-173 | `worldNormal = a_normal * mat3(inst.modelMatrix); worldPos = world.xyz` | a_normal, inst.modelMatrix | worldNormal, worldPos |
| G7 | Window/spotlight skip gate | static_prop.vert:192-200 | If `(flags & (kFlagIsWindow \| kFlagIsSpotlight)) != 0`: `lit = base_light` (skip calc_light) | inst.flags | lit |
| G8 | calc_light dispatch (else branch) | static_prop.vert:199 | `lit = calc_light(int(inst.lightDataIndex), worldNormal, worldPos, base_light)` | inst.lightDataIndex, ObjectLights UBO[idx] | lit |
| G8a | calc_light early-return | lighting.hglsl:195-196 | **`if (numLights.x == 0) return vec3(1, 1, 1)`** — returns saturated white | ld.numLights.x | lit |
| G8b | TG_LIGHT_AMBIENT | lighting.hglsl:206-208 | `ambient += lcolor` (accumulated; added at end) | ld.light_color[i] | ambient |
| G8c | TG_LIGHT_INFINITE | lighting.hglsl:209-212 | `n_dot_l = clamp(dot(normal, -light_dir), 0, 1); final += n_dot_l * lcolor` | ld.light_dir[i], normal, lcolor | final |
| G8d | TG_LIGHT_INFINITEWITHFALLOFF | lighting.hglsl:213-224 | `dist = length(vert - lightPos); GetFalloff; final += n_dot_l * lcolor * falloff` | ld.light_to_world[i][3], vertex_world_pos, normal, ld.light_falloff | final |
| G8e | **TG_LIGHT_POINT → final (diffuse) channel** | lighting.hglsl:225-236 | `to_light = lightPos - vert; n_dot_l = clamp(dot(normal, normalize(to_light)), 0, 1); final += n_dot_l * lcolor * falloff` | ld.light_to_world[i][3], vertex_world_pos, normal | **final** (diffuse) |
| G8f | **TG_LIGHT_SPOT → final (diffuse) channel** | lighting.hglsl:237-254 | Same as POINT, uses light_dir.xyz as cone axis | (same) | **final** (diffuse) |
| G8g | **TG_LIGHT_TERRAIN → no-op** | lighting.hglsl:255-263 | Body is comment-only; explicitly does nothing per spec R2 | (none) | (none) |
| G8h | calc_light return | lighting.hglsl:266 | `return final + ambient` (no clamp at return; clamp happens at parity-pack and v_argb output) | final, ambient | lit |
| G9 | aRGBHighlight additive | static_prop.vert:211 | `lit = clamp(lit + inst.aRGBHighlight.rgb, 0, 1)` | inst.aRGBHighlight | lit |
| G10 | v_argb output | static_prop.vert:218 | `v_argb = vec4(lit, 1.0)` (alpha hardcoded 1.0) | lit | v_argb |
| G11 | Parity write | static_prop.vert:232-247 | `b8/g8/r8 = uint(clamp(lit*255,0,255)); a8=255; packed = b8\|(g8<<8)\|(r8<<16)\|(a8<<24); parityOut[idx] = packed` | lit, u_parityWrite, u_parityVertsPerType, u_parityBaseVertex | parityOut_[idx] |

**No GPU consumer of the spec/fog channel beyond fog mix.** `static_prop.frag:85`
does `c.rgb = mix(v_fog.rgb, c.rgb, u_fogValue)` where `v_fog.rgb` is the
unmodified instance fogRGB (from submit() line 776-779), NOT the per-vertex
`frgb` carrying spec contributions.

---

## §4. Gap analysis — operations missing on GPU

| Missing op | CPU origin (file:line) | What GPU has instead | Likely symptom class | Confidence |
|---|---|---|---|---|
| **#1 — `s_numLights==0` semantics divergence** | tgl.cpp:1938 (CPU's loop entry) + GenericAppearance::update at genactor.cpp:1201 setting `SetLightList(NULL,0)` BEFORE `CacheGpuLightData()` at line 1219 | calc_light early-returns `vec3(1)` at lighting.hglsl:195-196 — converting a "no lights configured" signal into "saturate to white." CPU loop simply doesn't add anything. | white-out anomaly (mc2_18 typeId=474 cpu=0x818181 gpu=0xFFFFFF directly), AND many colored-cpu-gray-gpu pairs where CPU vertex tag is non-zero and GPU goes white | **HIGH** |
| **#2 — `lighteningLevel` seed of redSpec/greenSpec/blueSpec** | tgl.cpp:1798-1801 | None. GPU has no concept of TG_Shape::lighteningLevel. | grayscale-additive (uniform RGB delta) | LOW (only fires during weather lightning bolts; stock tier1 likely 0) |
| **#3 — TG_LIGHT_TERRAIN pre-bake into listOfColors** | tgl.cpp:2125-2158, listOfColors consumed at tgl.cpp:2238-2240 | Comment-only no-op at lighting.hglsl:255-263. GPU's CPU-prebaked-listOfColors is NOT consumed. | grayscale-additive (when terrain light is uniform/dim) | MEDIUM (`useShadows` must be true; not always; but when true, it adds a small spec contribution that GPU misses) |
| **#4 — TG_LIGHT_POINT/SPOT routed to redSpec/greenSpec/blueSpec on CPU; routed to `final` on GPU** | tgl.cpp:2093-2095 (POINT), 2196-2198 (SPOT) write redSpec/greenSpec/blueSpec | lighting.hglsl:225-236 (POINT), 237-254 (SPOT) `final += ...` | colored-colored (both colored, non-uniform deltas; GPU has POINT/SPOT additive that CPU shoves into spec) | MEDIUM (depends on whether sample actors are near point/spot lights — mc2_05 typeId=210 with a yellow CPU value and gray GPU could be this if a yellow point light is nearby and CPU spec-routed it while GPU didn't) |
| #5 — BaseVertexColor precision asymmetry | tgl.cpp:1892-1905 (0..255 byte arithmetic) | lighting.hglsl:143 (0..1 normalized arithmetic with `+0.5` round at decode) | (latent — within ±2 LSB tolerance) | LOW (no-op when BaseVertexColor=0; absorbed by `channelsWithin2` tolerance otherwise) |
| #6 — TG_LIGHT_INFINITE alpha-byte from light's aRGB | tgl.cpp:1980 reads `(startLight>>24) & 0xff` is NOT used — CPU only reads bytes 0-23 from aRGB | GPU likewise ignores alpha. Match. | (no gap) | n/a |
| #7 — Cosine sign convention | tgl.cpp:1977 `if (cosine < 0.0f)` then `cos = fabs(cosine)`; GPU uses `clamp(dot(normal, -light_dir), 0, 1)` | Mathematically equivalent: `dot(normal, -ld) > 0 ⟺ dot(normal, ld) < 0`, both use the same magnitude. | (no gap) | n/a |

**Negative-claim verifications** (per CLAUDE.md "Negative claims need
opposite-direction grep"):

- "GPU has no `lighteningLevel` consumer": grep'd
  `lighteningLevel` across `shaders/include/*.hglsl`, `shaders/static_prop.{vert,frag}` —
  zero hits. Confirmed.
- "GPU `static_prop.frag` doesn't read frgb spec channel": grep'd
  `frgb`, `redSpec`, `greenSpec`, `blueSpec`, `v_fog` consumption in
  `static_prop.frag` — only `v_fog.rgb` is read (line 85 `mix(v_fog.rgb, ...)`),
  and that's the per-instance fogRGB (set at submit() lines 776-779), NOT
  per-vertex frgb. The per-vertex frgb path that CPU writes to is dropped
  entirely.
- "GPU `calc_light` does not handle TG_LIGHT_TERRAIN": grep'd
  `TG_LIGHT_TERRAIN` in lighting.hglsl — fires the comment-only branch at
  line 255-263. Confirmed no actual computation.

---

## §5. Mismatch class → cause hypothesis

### A. white-out anomaly (1 pair)

**mc2_18 typeId=474: cpu=`0xFF818181` gpu=`0xFFFFFFFF`**

- **Best-supported hypothesis (HIGH)**: Cause #1. Actor goes through
  GenericAppearance::update path, hits `genShape->SetLightList(NULL, 0)` at
  genactor.cpp:1201 immediately followed by `genShape->CacheGpuLightData()`
  at line 1219. `lightData_.numLights_ = 0`. GPU's `calc_light` short-circuits
  at lighting.hglsl:195 and returns `vec3(1)`. lit = (1,1,1) → 0xFFFFFFFF.
  CPU's vertex loop doesn't enter (line 1938 `s_numLights==0`); so
  `redFinal/greenFinal/blueFinal = 0` from the magic-tag default
  (`0xFF000000`). But CPU outputs `0x81` (gray ~129)? That suggests the
  per-vertex aRGBLight tag for typeId=474 is NOT default — maybe `0xFF818181`
  (a non-magic colored tag) or similar. Then CPU's "Some other kind of light"
  branch at tgl.cpp:1878 fires → redFinal=greenFinal=blueFinal=0x81. Output
  argb = 0xFF818181. Matches CPU side. GPU's get_base_light on the same
  tag: start_v_light = 0xFF818181, the non-magic branch at lighting.hglsl:114
  fires, final = startVLight.zyx = (0x81, 0x81, 0x81)/255 = (0.506, 0.506, 0.506).
  Then `calc_light` is called — but with numLights=0 it returns `vec3(1)`!
  **lit = (1,1,1)** — completely overrides base_light. Output gpu = 0xFFFFFFFF.
- **Test**: if cause #1 is right, every GenericAppearance vertex with
  numLights_=0 in lightData_ should produce GPU lit=vec3(1) regardless of
  base_light. If we also see other GenericAppearance typeIds in the
  inventory consistently producing gpu values that depend on the per-vertex
  tag clamped UP to (1,1,1), this hypothesis dominates.
- **Alternative**: Could be a corrupted SSBO read (lightDataIndex
  out-of-bounds). Less likely since static_prop.vert:199 uses
  `int(inst.lightDataIndex)` and the C++ side always returns from
  `addLightDataStructure` a valid offset.
- **Distinguishing evidence**: dump `lightData_.numLights_` for the
  CacheGpuLightData call from genactor.cpp:1219; if observed `numLights_==0`
  consistently for GenericAppearance, hypothesis confirmed.

### B. colored-cpu-gray-gpu (28 pairs, 51%)

Examples: mc2_10 t=84 cpu=`0xFFAFAFAF` gpu=`0xFF3F3F3F`; mc2_05 t=210
cpu=`0xFFF6F690` gpu=`0xFF2A2A2A`; mc2_05 t=133 cpu=`0xFFFF2A2A`
gpu=`0xFF2A2A2A`; mc2_19 t=626 cpu=`0xFF80FF80` gpu=`0xFF808080`;
mc2_15 t=287 cpu=`0xFF1B1B15` gpu=`0xFF2F2F2F`.

- **Best-supported hypothesis (split MEDIUM)**: this class is heterogeneous;
  the gray on the GPU side comes from at least two sources:

  - **Sub-class B1 (3-channel colored CPU, achromatic GPU)**: Cause #1 +
    cause #4 combined for actors near point lights. CPU adds the colored
    contribution to redSpec/greenSpec/blueSpec (which goes to frgb, not
    argb), so its `argb` keeps the per-vertex tag's color. GPU adds the
    colored contribution to `final` directly, **but if the actor sits in a
    region where AMBIENT alone is small + INFINITE light is dim/perpendicular**,
    GPU's diffuse may end up slightly elevated but uniform (gray). Net:
    CPU shows magic-tag colors more, GPU shows ambient-dominated dim color.
    Most-likely-fitting: mc2_05 t=210 cpu=0xF6F690 gpu=0x2A2A2A — yellow
    actor near a point light: CPU's tag-derived color survives plus AMBIENT,
    GPU's calc_light starts from a base derived from a tag whose magic
    branch may fire DIFFERENTLY on GPU than CPU.

  - **Sub-class B2 (single-channel colored CPU, gray-equal GPU)**: e.g.
    cpu=`0xFFFF2A2A` gpu=`0xFF2A2A2A`. CPU has R=255, G=B=42; GPU has
    R=G=B=42. **Strongly suggests CPU's redFinal got R-light contribution
    and GPU's didn't, OR CPU's per-vertex tag is `0xFFFF0000` (Hot Red)
    which is empty-bodied on CPU (tgl.cpp:1870-1873) and lighting.hglsl:106-109
    on GPU**: both empty bodies. Net both should produce 0 base_light. Then
    light loop: CPU adds R from the same light direction as GPU. They should
    agree. Unless `s_lightDir[i]` is per-actor stale on CPU (but Bug 5 fixed
    that for Bldg/Tree). Probable cause: cause #4 (POINT routing into spec
    on CPU vs final on GPU) where the dominant light is a yellow point light:
    CPU's redFinal stays 255 (from tag) + ambient ~42; GPU's redFinal also
    starts non-zero but POINT contribution on GPU adds a yellow tint — but
    the spec sample shows GPU = 0x2A2A2A (no yellow). So this isn't quite
    cause #4. **More likely cause #1 if these actors are also Generic.**

  - **Sub-class B3 (mc2_15 t=287 negative-delta — CPU darker than GPU)**:
    cpu=0x1B1B15, gpu=0x2F2F2F. The GPU value `0x2F` is the daytime Hot Pink
    fallback color at lighting.hglsl:78 (`vec3(0x2f/255.0)`). **GPU fired
    Hot Pink magic where CPU did not.** That's strange because the magic
    comparison `start_v_light == 0xffff00ff` is identical on both. Most
    plausible: the per-vertex aRGBLight is `0xffff00ff` (Hot Pink), and
    the **CPU's `nightFactor > Stuff::SMALL` branch fired (tgl.cpp:1812)
    multiplying hotPinkRGB by nightFactor**, producing the dim 0x1B1B15
    value. GPU's branch lighting.hglsl:72-75 has the same nightFactor
    multiplier — but GPU's `nightFactor` is **hardcoded to 0** at
    static_prop.vert:154! So GPU falls into the `else` (line 76-79) and
    outputs `vec3(0x2f/255.0)` directly. **CPU has a real nightFactor
    available; GPU passes 0.** This is a load-bearing incompleteness in
    static_prop.vert's get_base_light call.

  - **Sub-class B4 (the rest)**: probably cause #1 white-out clamped down
    by texture mul to something like `tex.rgb * 1` = `tex.rgb`, but the
    parity SSBO captures pre-tex `lit` so this would still be 1.0. Reject.

- **Tests for this class**:
  1. mc2_15 t=287: dump aRGBLight tag and confirm it's `0xffff00ff`.
     Check eye->getNightFactor() at the time of update. If both confirm,
     B3 is the cause for this pair.
  2. mc2_05 t=210: dump aRGBLight tag and lightDataIndex content. If
     numLights_=0 (Generic) → cause #1. If numLights_>0 with point light
     → cause #4.
  3. mc2_19 t=626: ditto. cpu=0x80FF80 gpu=0x808080. If aRGBLight tag is
     `0xff80ff80`, CPU's "Some other kind of light" branch produces
     final=(0x80,0xff,0x80). GPU: same branch produces (0.502, 1.0, 0.502).
     With numLights=0 (early return) → vec3(1). With numLights>0, INFINITE
     adds — and the result clamps to all 1.0 → 0xFFFFFF, not 0x808080.
     **Only cause #1 (numLights=0 → vec3(1)) doesn't fit either.** This
     pair specifically has GPU = 0x80 across all channels. **Most plausible:
     the GPU-decoded `start_v_light` is reading the wrong tag** (alpha
     mismatch in DWORD reload, see §7) and producing only the BLUE half-byte
     of the original — reduced uniformly to 0x80. This is a HIGH-confidence
     UNANSWERED case.

### C. grayscale-additive (19 pairs, 35%)

Example: mc2_10 t=84 cpu=0xFFAFAFAF gpu=0xFF3F3F3F. Uniform RGB delta = `+0x70`.

- **Best-supported hypothesis (MEDIUM)**: cause #2 (lighteningLevel) OR
  cause #3 (TG_LIGHT_TERRAIN spec dropped) OR cause #4 (POINT/SPOT routing
  difference, but only if the light is white). For typeId=84 with
  cpu=0xAFAFAF gpu=0x3F3F3F: CPU has uniform 0xAF, GPU has uniform 0x3F.
  Diff is ~0x70 (112). That's a substantial uniform additive that GPU is
  missing. **Most plausibly cause #3**: TG_LIGHT_TERRAIN bake adds
  uniform white-ish spec contribution on CPU (when useShadows=true), GPU
  does nothing. Magnitude 0x70 is a single, fairly bright terrain-light
  contribution.
- **Alternative**: cause #2 if lighteningLevel was nonzero at the time
  (weather event). Less likely for stock tier1 but possible during a
  scripted lightning bolt frame.
- **Distinguishing test**: dump `useShadows`, `s_listOfLights[i]->lightType`
  for any TG_LIGHT_TERRAIN entry, and `listOfColors[j].redSpec/.greenSpec/.blueSpec`
  for the affected vertices. If non-zero, cause #3 confirmed.

### D. colored-colored (9 pairs, 16%)

Both colored, non-uniform deltas. Example shape: e.g., a vertex where
CPU has (180, 100, 200) and GPU has (190, 130, 175).

- **Best-supported hypothesis (MEDIUM)**: cause #4 (POINT/SPOT routing
  difference) plus precision drift. CPU's colored point light adds to spec
  channel (lost), GPU's colored point light adds to diffuse. Result: GPU's
  argb has the point-light color additive, CPU's argb does not.
- **Alternative**: cause #5 (BaseVertexColor precision drift) — but only
  if BaseVertexColor != 0 in the affected mission. Unlikely for stock.
- **Distinguishing test**: pin the test to actors near known point or
  spot lights. If colored-colored pairs concentrate on those actors,
  cause #4 is dominant.

---

## §6. Recommended fix order

1. **Fix N1 — calc_light early-return semantics + GenericAppearance temporal pattern.**
   - **Coverage estimate**: 1 (the white-out anomaly) directly, plus an
     unknown but probably-large fraction of colored-cpu-gray-gpu (Sub-class
     B4 + part of B1) IF GenericAppearance is the affected population.
   - **Code change**: at GPU `lighting.hglsl:195-196`, change the early
     return to `return base_light` (preserve passed-in base) instead of
     `return vec3(1, 1, 1)`. Rationale: the early-return was historically
     intended for shaders where `base_light` is implicitly `vec3(1)`
     (mech/vehicle/particle path). For static_prop.vert, base_light has
     already been computed from the per-vertex tag, and "no lights
     configured" should mean "use the unaltered base color," NOT "saturate
     to white."
   - **Risk**: This file is included by `gos_tex_vertex_lighted.{vert,frag}`
     too. The legacy shaders pass `VertexLight` (mech body color) as
     base_light. If the legacy CPU path also hits `numLights==0` AND
     relies on the saturate-to-white behavior, this change would
     darken legacy mech rendering. **Mitigation**: gate the change behind
     `MC2_STATIC_PROP_LIGHTING` define same as the BGR/RGB swizzle in C1.
     `static_prop.vert` defines the symbol; the legacy shaders don't.
   - **Estimated lines**: 4 GLSL lines (#ifdef gate + new return + #else
     + old return + #endif). Plus a thoughtful comment.
   - **CPU reference**: tgl.cpp:1934-1938 — when CPU's `s_numLights==0`,
     the lighting block doesn't execute at all, leaving redFinal/greenFinal/blueFinal
     as set by the magic-tag decode + BaseVertexColor (which is the same
     content as base_light on GPU after #5).

2. **Fix N2 — pass real `nightFactor` to `get_base_light()`.**
   - **Coverage estimate**: ≥1 pair (mc2_15 t=287, possibly more night-window/base
     light tagged actors at twilight). Could explain a slice of B3
     sub-class.
   - **Code change**: `static_prop.vert:154` currently passes hardcoded
     `false, 0.0, false, false` for `isNight, nightFactor, isHudElement, lightsOut`.
     `nightFactor` is `eye->getNightFactor()`. C++ side already publishes it
     to a uniform somewhere (see `gos_tex_vertex_lighted.vert:75` — which
     hardcodes the same way per the comment). Adding nightFactor +
     isNight requires a new uniform in static_prop.vert OR a SceneData
     UBO field. SceneData has cap room.
   - **Risk**: low. Read-only addition. Only affects vertices with
     Hot Pink/Yellow/Green tags AND non-zero nightFactor.
   - **Estimated lines**: 3-5 (1 uniform decl, 1 sceneData field if going
     that route, 1 line at static_prop.vert:154).
   - **CPU reference**: tgl.cpp:1691-1697 — eye state read.

3. **Fix N3 — TG_LIGHT_TERRAIN consumption (or accept divergence).**
   - **Coverage estimate**: ~19 pairs of grayscale-additive class IF
     terrain-light is the dominant gray cause. Confirms via test in §5.C.
   - **Code change**: requires either (a) per-vertex SSBO field carrying
     CPU-prebaked redSpec/greenSpec/blueSpec from tgl.cpp:2146-2148,
     consumed by static_prop.vert and added to `lit`, OR (b) re-computing
     terrain-light on GPU (more involved). Option (a) is feasible but
     adds vertex-stride growth (12 bytes per vertex if a vec3, OR pack
     into a uint).
   - **Risk**: medium. Requires careful per-vertex SSBO synchronization
     with the registerType path. The bake at tgl.cpp:2146-2148 only
     happens when `useShadows=true` AND TG_LIGHT_TERRAIN is in s_listOfLights;
     for actors that don't fire this, the field stays 0 and no contribution.
   - **Estimated lines**: 30-50 (vertex stride growth + SSBO upload + VAO
     + shader read).
   - **Architecture decision**: do we just **accept** the ~+0x70 grayscale
     drift as "matches spec R2's stated tradeoff" (small specular
     under-count) and tighten the parity tolerance? That's a smaller but
     less-clean alternative.

4. **Fix N4 — TG_LIGHT_POINT/SPOT routing alignment.**
   - **Coverage estimate**: ~9 pairs of colored-colored, possibly more.
   - **Code change**: harder than fixes 1-3. Either (a) move CPU's POINT/SPOT
     contribution from spec channel to diffuse (changes the fallback CPU
     path's `argb` for non-eligible actors — touches the legacy CPU
     baseline, potentially regressing Bldg/Tree visuals not on GPU path),
     OR (b) move GPU's POINT/SPOT from `final` to a separate spec-like
     accumulator that doesn't end up in v_argb (mirrors CPU's spec channel
     drop), OR (c) accept the divergence.
   - **Risk**: HIGH for option (a) — touches CPU baseline. MEDIUM for (b)
     — adds complexity but matches CPU. LOW for (c) — accept and document.
   - **Estimated lines**: 5-10 in shader (option b); 15-30 in tgl.cpp (option a).
   - **Recommendation**: option (b) — drop POINT/SPOT contribution from
     GPU's `final`. Mirrors CPU's stock behavior.

5. **Fix N5 (optional) — `lighteningLevel` propagation.**
   - **Coverage estimate**: latent only (stock missions don't fire it
     consistently). Probably not load-bearing for tier1.
   - **Code change**: add a uniform for `TG_Shape::lighteningLevel`,
     consume in vertex shader. Trivial.
   - **Risk**: LOW.
   - **Estimated lines**: 5.

**Recommendation to controller**: **A1 (incremental, fix-and-verify)**.

Rationale: the single-line N1 fix is high-leverage with low risk (gated by
MC2_STATIC_PROP_LIGHTING) and addresses the highest-confidence hypothesis.
After landing N1, re-sample parity and check whether colored-cpu-gray-gpu
collapses to MUCH fewer pairs. If yes, problem is overwhelmingly cause #1
and N2/N3/N4 can be deferred. If colored-cpu-gray-gpu stays at ~28 pairs,
then N2/N3/N4 are needed and we have **fresh, post-N1 data** to scope each.

A2 (atomic) is risky here because N3/N4 each have architectural decisions
(accept-vs-fix) that benefit from the post-N1 evidence base. Atomic landing
forces those decisions before we have data.

---

## §7. Open questions / what the recon couldn't answer

1. **Are the inventory's typeIds Bldg, Tree, or Generic populations?**
   The inventory reports typeIds (the GpuStaticPropBatcher index from
   `s_typeIndex`). Mapping each typeId to its source TG_TypeShape node
   name and population class would CONFIRM hypothesis #1's coverage of
   the colored-cpu-gray-gpu class. **A smoke run with one extra log line
   per first sample of each typeId would resolve this.** Add to
   `gos_object_parity::ParityPrintMismatchByIndex` (line 632 of
   gos_object_parity.cpp): include `typeShape->name` and the population
   tag from the bucket.

2. **Does the inventory's typeId=474 in mc2_18 correspond to a
   GenericAppearance population?** Direct test of #1.

3. **For mc2_19 typeId=626 (cpu=0x80FF80 gpu=0x808080)**: is the
   per-vertex aRGBLight tag actually `0xff80ff80` or something else? The
   uniform 0x80 GPU output doesn't fit ANY of the four hypotheses cleanly.
   Possibility: the per-vertex aRGBLight DWORD reload via
   `glVertexAttribIPointer(4, 1, GL_UNSIGNED_INT, ..., (void*)36)` reads
   the DWORD as little-endian uint, but the buffer was written via
   `memcpy(vert+36, &src.aRGBLight, 4)`. Both should round-trip identity.
   **Edge case**: if the C++ DWORD value is read into a vec4 via
   `GL_UNSIGNED_BYTE` somewhere (which is NOT what Stage 2.C.2 does per
   line 663-669), the byte order would differ. Worth verifying with a
   one-shot trace of `a_aRGBLight` raw uint value at the suspect vertex.

4. **Is `useShadows` true at the time of the inventory sampling?** The
   TG_LIGHT_TERRAIN pre-bake (tgl.cpp:2127) only fires when `useShadows`
   is true. Without confirming, we can't quantify cause #3's coverage.

5. **Is `lightingLevel` ever non-zero in tier1 stock missions?** Probably
   not (only `weather.cpp:371` writes it during lightning bolts, which
   tier1 missions may or may not have), but worth confirming.

6. **For mc2_15 typeId=287 (cpu=0x1B1B15 gpu=0x2F2F2F)**: what's
   `eye->getNightFactor()` at sample time? Confirm if it's mid-twilight
   (causing CPU's `nightFactor * hotPinkRGB` to produce ~0x1B). If yes,
   N2 closes this pair.

---

## §8. Adversarial review checklist

Symbols cited by this recon and verified against current source:

| Symbol | Verified at | Match |
|---|---|---|
| `MultiTransformShape` per-vertex loop | tgl.cpp:1745-2344 | ✓ (re-grep'd against HEAD 95baa44) |
| `s_numLights == 0` short-circuit (CPU) | tgl.cpp:1938 (gate at `useVertexLighting && Renderer != 3 && !isSpotlight && !isWindow`) → loop body | ✓ |
| `numLights.x == 0 → return vec3(1,1,1)` (GPU) | lighting.hglsl:195-196 | ✓ |
| TG_TypeShape `hot{Pink,Yellow,Green}RGB` fields | tgl.h:575-577 | ✓ |
| `GpuStaticPropInstance::lightDataIndex` at offset 76 | gos_static_prop_batcher.h:18+33 (static_assert) | ✓ |
| Per-type SSBO upload at finalizeGeometry | gos_static_prop_batcher.cpp:670-715 | ✓ |
| `submit()` aRGBHighlight decode (R,G,B,A floats) | gos_static_prop_batcher.cpp:772-775 | ✓ |
| `BaseVertexColor` extern + writers | code/logmain.cpp:77, code/mechcmd2.cpp:164, code/prefs.cpp:91 | ✓ |
| `BaseVertexColor` upload to SceneData | mclib/txmmgr.cpp:1141 (with `.zyxw()` swizzle) | ✓ |
| `g_scene.baseVertexColor` consumption | shaders/include/lighting.hglsl:143 | ✓ |
| `useVertexLighting=true`, `useFaceLighting=false` | mclib/terrain.cpp:166-167 | ✓ |
| `Environment.Renderer = 0` | code/logmain.cpp:786, code/mechcmd2.cpp:2807 | ✓ |
| `lighteningLevel` static field | mclib/tgl.h:796, tgl.cpp:85 | ✓ |
| `lighteningLevel` writer | code/weather.cpp:371 | ✓ |
| `SetLightList(NULL,0)` in GenericAppearance::update | mclib/genactor.cpp:1201 | ✓ |
| `genShape->CacheGpuLightData()` after SetLightList | mclib/genactor.cpp:1219 | ✓ |
| `bldgShape->CacheGpuLightData()` site | mclib/bdactor.cpp:2247 | ✓ |
| `treeShape->CacheGpuLightData()` site | mclib/bdactor.cpp:4404 | ✓ |
| `CacheGpuLightData()` body (calls GatherGpuObjectLightDataOnly) | mclib/msl.cpp:1765-1785 | ✓ |
| `GatherGpuObjectLightDataOnly` body (calls GatherLightsParameters) | mclib/tgl.cpp:2848-2852 | ✓ |
| `GatherLightsParameters` reads `s_listOfLights/s_numLights` | mclib/txmmgr.cpp:954-955 | ✓ |
| `numLights_` ivec4 alignment | tgl.h:309-310 (int + pad[3]) ↔ lighting.hglsl:36 (ivec4) | ✓ |
| TG_LIGHT_TERRAIN special case (CPU writes listOfColors) | tgl.cpp:2125-2158 | ✓ |
| TG_LIGHT_TERRAIN GPU = no-op | lighting.hglsl:255-263 | ✓ |
| TG_LIGHT_POINT writes redSpec/greenSpec/blueSpec (CPU) | tgl.cpp:2093-2095 | ✓ |
| TG_LIGHT_POINT writes final (GPU) | lighting.hglsl:225-236 | ✓ |
| `static_prop.frag` consumes only v_argb (no per-vertex spec) | shaders/static_prop.frag:21-89 | ✓ |
| `static_prop.vert:154` hardcodes `false, 0.0, false, false` | shaders/static_prop.vert:154 | ✓ |
| Hot Pink daytime gray = `vec3(0x2f/255)` | lighting.hglsl:78 | ✓ |
| `MC2_STATIC_PROP_LIGHTING` define + .zyx vs .xyz swizzle | shaders/static_prop.vert:20 + lighting.hglsl:131-135 | ✓ |
| `aRGBLight` per-vertex tag default `0xff000000` | mclib/tgl.cpp:939 | ✓ |
| Parity write encoding (alpha=255 hardcoded) | shaders/static_prop.vert:232-247 | ✓ |
| Parity comparison source (CPU side) | gos_static_prop_batcher.cpp:825 | ✓ |
| Parity comparison reads listOfVertices[v].argb (NOT listOfTriangles) | gos_static_prop_batcher.cpp:791-797 (comment clarifies) + 825 (read) | ✓ |
| Magic comparisons `0xffff00ff` etc. | tgl.cpp:1804/1829/1851/1870/1874 ↔ lighting.hglsl:66/81/95/106/110 | ✓ identical |

Negative claims verified by opposite-direction grep (per CLAUDE.md
"Negative claims need opposite-direction grep"):

- "GPU has no `lighteningLevel` consumer": grep'd `lighteningLevel` against
  shaders/*.{vert,frag} and shaders/include/*.hglsl — zero hits. **Confirmed.**
- "GPU `static_prop.frag` doesn't read per-vertex spec channel": grep'd
  `frgb`, `redSpec`, `greenSpec`, `blueSpec` in shaders/static_prop.frag —
  zero hits. **Confirmed.**
- "GPU `calc_light` does not handle TG_LIGHT_TERRAIN": grep'd
  `TG_LIGHT_TERRAIN` in lighting.hglsl, walked the body at line 255-263 —
  comment-only no-op. **Confirmed.**
- "static_prop.vert hardcodes nightFactor=0": grep'd `nightFactor` and
  `getNightFactor` in shaders/static_prop.vert — zero hits beyond the
  hardcoded `0.0` literal at line 154. **Confirmed.**

Load-bearing constraints cross-referenced:

- ⭐ `cpp_glsl_ubo_struct_lockstep.md` — TG_HWLightsData ↔ ObjectLights
  layout match verified at tgl.h:304-309 ↔ lighting.hglsl:31-37. Falloff
  fields present on both sides (lightFalloff[16][4] ↔ light_falloff[16]).
- ⭐ `mc2_argb_packing.md` — confirmed swizzle behavior in
  static_prop.vert:137-141 (BGRA decode of DWORD via uint32 attribute) +
  lighting.hglsl:52-58 (re-pack to original DWORD for magic comparison).
  This recon's analysis is consistent with the documented pattern.
- ⭐ `cull_gates_are_load_bearing.md` — recon does not propose any cull
  changes. NA.
- ⭐ `feedback_offload_scope_stock_only.md` — recon scope is tier1+tier2
  stock, matches the inventory's scope.
- `feedback_data_flow_audit_asymmetry.md` — applied (negative-claim greps
  in both directions for §4 missing-op claims).

**Recommendation to controller**: **A1 (incremental, fix-and-verify)**.

Land N1 first (single-shader-line, MC2_STATIC_PROP_LIGHTING-gated change to
calc_light early-return). Re-sample parity at tier1 stock. Use the resulting
delta in mismatch class distribution as evidence base for whether N2/N3/N4
are needed and in what order. The single-fix-then-resample cadence mirrors
the stage-2.D.2 → 2.D.2.1 corrective pattern that already proved itself.

If post-N1 the inventory's mismatch count drops to <0.1% AND the white-out
anomaly is resolved, declare slice 2 substrate validated and proceed to
2.D.3 default-on flip prep. If post-N1 colored-cpu-gray-gpu and
colored-colored remain prominent, dispatch a second recon focused on
those classes with N1 confirmed-removed from the variable list.
