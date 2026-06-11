# SHADOW-ARC-RECON-0 — Shadow System End-to-End Recon

**Date:** 2026-05-28  
**Status:** RECON COMPLETE — read-only, no code changes  
**Next slice:** SHADOW-VIEW-1

---

## 1. Shadow Authority Chain

### 1.1 Who owns what

All shadow state lives in `GameOS/gameos/gos_postprocess.cpp` — the `gosPostProcess` class.

| Resource | Owner | Member | Location |
|---|---|---|---|
| Static shadow FBO | `gosPostProcess` | `shadowFBO_` | gos_postprocess.cpp:1098 |
| Static shadow depth texture | `gosPostProcess` | `shadowDepthTex_` | gos_postprocess.cpp:1101 |
| Static shadow dummy color (AMD) | `gosPostProcess` | `shadowDummyColorTex_` | gos_postprocess.cpp:1116 |
| Dynamic shadow FBO | `gosPostProcess` | `dynShadowFBO_` | gos_postprocess.cpp:1367 |
| Dynamic shadow depth texture | `gosPostProcess` | `dynShadowDepthTex_` | gos_postprocess.cpp:1370 |
| Dynamic shadow dummy color (AMD) | `gosPostProcess` | `dynShadowDummyColorTex_` | gos_postprocess.cpp:1380 |
| `staticLightSpaceMatrix_[16]` | `gosPostProcess` | class member | gos_postprocess.h:173 |
| `dynamicLightSpaceMatrix_[16]` | `gosPostProcess` | class member | gos_postprocess.h:182 |

### 1.2 Texture allocation

**Static shadow map** (gos_postprocess.cpp:1098–1144):
- Size: 4096×4096 (`shadowMapSize_ = 4096`)
- Format: `GL_DEPTH_COMPONENT24`
- Filter: `GL_LINEAR` with `GL_COMPARE_REF_TO_TEXTURE` / `GL_LEQUAL` (PCF-ready sampler)
- Border: `1.0f` → out-of-bounds = fully lit
- AMD workaround: dummy `GL_R8` color attachment required for rasterization

**Dynamic shadow map** (gos_postprocess.cpp:1367–1412):
- Size: 4096×4096 (`dynShadowMapSize_ = 4096`, upgraded from 2048)
- Format: `GL_DEPTH_COMPONENT24`, same PCF sampler setup
- AMD workaround: same dummy color attachment

### 1.3 Light matrix computation

**`gosPostProcess::buildStaticLightMatrix()`** (gos_postprocess.cpp:1247–1358):
- Called once per mission (gated by `staticLightMatrixBuilt_` flag)
- World-fixed orthographic frustum centered at map origin
- Frustum radius: `mapHalfExtent * sqrt(2) * 1.05f` covers full-map diagonal
- Orthonormal basis from sun direction cross products
- `VP = ortho * view` (column-major, uploaded `GL_FALSE`)
- Debug probes: `MC2_DEBUG_SHADOW_FRUSTUM`, `MC2_DEBUG_SHADOW_ZRANGE`

**`gosPostProcess::buildDynamicLightMatrix()`** (gos_postprocess.cpp:1421–1533):
- Called every frame; camera-centered frustum fit
- Input: 8 camera frustum corners from caller (Stuff-space, NDC-unprojected)
- Frustum XY extent via corner projection onto light-space XY plane
- Power-of-2 anti-shimmer snapping
- Clamps to map bounds + elevation slab `[-512, 4096]`
- Same orthonormal basis construction as static
- Clip-Z: `[0,1]` via `GL_ZERO_TO_ONE` (forward-Z for shadow passes)

### 1.4 Shadow matrix upload (receiver side)

Both matrices uploaded as raw `glUniformMatrix4fv` calls from C++ — **not** in ViewUniforms UBO.
- `lightSpaceMatrix` uniform → all static shadow receivers
- `dynamicLightSpaceMatrix` uniform → all dynamic shadow receivers
- No EngineView/ViewUniforms integration yet

### 1.5 Texture binding for receivers

**Screen-space shadow pass** (`runScreenShadow()`, gos_postprocess.cpp:682–689):
- Static shadow depth → `GL_TEXTURE2` → `shadowMap` sampler
- Dynamic shadow depth → `GL_TEXTURE3` → `dynamicShadowMap` sampler

**Terrain inline shadow sampling** (bound from terrain shader setup):
- Static shadow depth → unit 9 → `shadowMap` sampler
- Dynamic shadow depth → unit 10 → `dynamicShadowMap` sampler

**Terrain skips the screen-shadow pass** — it handles shadow inline in its fragment shader.  
All other objects (static props, mechs, buildings) defer to screen-shadow pass.

---

## 2. Shadow Bias (Polygon Offset + Per-Pixel)

**GL-level bias** (gos_postprocess.cpp:1181, 1204):
```cpp
glPolygonOffset(2.0f, 4.0f);  // factor, units
```

**Shader per-pixel slope bias** (shadow.hglsl):
```glsl
float bias = max(0.005 * (1.0 - NdotL), 0.002);
float currentDepth = projCoords.z - bias;
```
Range: 0.002 (lit face) to 0.005 (grazing angle).

**Reverse-Z / Forward-Z boundary** (critical, fragile):
- Main scene: reverse-Z (`glClearDepth(0)` = far plane)
- Shadow passes: forward-Z (`glClearDepth(1)` = far plane)
- Manual `glClearDepth()` swap at `beginShadowPass()` / `endShadowPass()`
- Missing call → silent depth test corruption

---

## 3. Frame Pass Ordering

**In `mclib/txmmgr.cpp:renderLists()` (~lines 1915–1990):**

```
[1] gos_BuildStaticLightMatrix()          — once per mission
[2] gos_BeginShadowPrePass(true)          — bind shadowFBO, glClearDepth(1)
[3] Terrain::renderStaticTerrainShadowFullMap()  — terrain depth into 4096×4096
[4] gos_EndShadowPrePass()                — restore FBO, glClearDepth(0)
[5] Unproject 8 NDC corners (Stuff→MC2 swizzle)
[6] gos_BuildDynamicLightMatrix(corners)  — per-frame frustum fit
[7] gos_BeginDynamicShadowPass()          — bind dynShadowFBO, glClearDepth(1)
[8] GpuStaticPropBatcher::flushShadow()   — static props shadow, non-indirect
[9] GpuMechBatcher::flushShadow()         — mechs shadow, ring-slot prev frame
[10] gos_EndDynamicShadowPass()           — restore FBO, glClearDepth(0)
[11] ... main scene render ...
[12] terrain.frag: inline shadow sampling (skips screen-shadow pass)
[13] static props / mechs render into GBuffer (normal.a = screen-shadow-eligible)
[14] runScreenShadow() fullscreen quad     — deferred shadow for non-terrain pixels
```

**Gate:** Dynamic shadow steps 6–10 gated on `MC2_SHADOW_ENABLE`.  
Static terrain shadow (steps 2–4) gated on `gos_IsTerrainTessellationActive()`.

---

## 4. View Model — Current vs. Proposed

### 4.1 Current state

| ViewId | Constant | Status |
|---|---|---|
| 0 | `kInvalidViewId` | sentinel |
| 1 | `kMainSceneViewId` | registered, active every frame |
| 2 | `kShadowDirectional0ViewId` | **reserved, NOT YET REGISTERED** |

`ViewKind` enum already has:
- `ViewKind::MainScene = 1`
- `ViewKind::ShadowStatic = 2`
- `ViewKind::ShadowDynamic = 3`

### 4.2 Proposed EngineView registrations

**SHADOW-VIEW-1 scope** — register both shadow views, no visual change:

```cpp
// Static shadow view (world-fixed, built once per mission)
EngineView shadowStaticView;
shadowStaticView.id               = kShadowDirectional0ViewId;     // 2
shadowStaticView.kind             = ViewKind::ShadowStatic;
shadowStaticView.viewport[2]      = shadowMapSize_;                // 4096
shadowStaticView.viewport[3]      = shadowMapSize_;
shadowStaticView.renderMask       = kRenderMaskShadowCasters;
shadowStaticView.debugName        = "ShadowDirectional0-Static";
shadowStaticView.viewUniforms     = {/* staticLightSpaceMatrix as worldToClipGL */};
```

```cpp
// Dynamic shadow view (per-frame camera-centered)
// Need a new ViewId: kShadowDynamicViewId = 3
EngineView shadowDynamicView;
shadowDynamicView.id              = kShadowDynamicViewId;           // 3 — new
shadowDynamicView.kind            = ViewKind::ShadowDynamic;
shadowDynamicView.viewport[2]     = dynShadowMapSize_;             // 4096
shadowDynamicView.viewport[3]     = dynShadowMapSize_;
shadowDynamicView.renderMask      = kRenderMaskShadowCasters;
shadowDynamicView.debugName       = "ShadowDynamic";
shadowDynamicView.viewUniforms    = {/* dynamicLightSpaceMatrix as worldToClipGL */};
```

**Notes:**
- `kShadowDynamicViewId = 3` needs to be added to EngineView.h
- ViewUniforms.worldToClipGL ← shadow light space VP matrix (no perspective, orthographic only)
- ViewUniforms.worldToViewGL ← shadow light view matrix (for receivers that need world→light-view)
- ViewUniforms.cameraWorldPos ← light source position (sun direction * -radius)
- Registration in `buildStaticLightMatrix()` / `buildDynamicLightMatrix()` respectively

### 4.3 Cascade-ready naming (future)

If CSM is ever needed, extend:
- `kShadowDirectional0Cascade0ViewId = 2` (near)
- `kShadowDirectional0Cascade1ViewId = 3` (mid)
- `kShadowDirectional0Cascade2ViewId = 4` (far)

Not needed now. Single shadow per type is the authorized scope.

---

## 5. Resource Model

### 5.1 Current registry state

`RenderCore/RenderResourceRegistry.h` — `RenderResourceId` enum:

| Id | Name | Status | Details |
|---|---|---|---|
| 1 | `MainColor` | defined | RGBA16F scene color |
| 2 | `MainDepth` | defined | Scene depth+stencil |
| 3 | `ShadowStaticMap` | **registered** | 4096×4096 Depth24, `shadowDepthTex_` |
| 4 | `TerrainHeightTexture` | registered | terrain height |
| 5 | `MaterialGpuBuffer` | defined | GPU material table |
| — | `ShadowDynamicMap` | **NOT in enum** | not registered |

### 5.2 ShadowStaticMap registration — CORRECT

Registration in `gos_postprocess.cpp` (inside `initShadows()`):
```cpp
RenderCore::RenderResourceDesc d;
d.id        = RenderCore::RenderResourceId::ShadowStaticMap;
d.kind      = RenderCore::RenderResourceKind::Texture2D;
d.format    = RenderCore::RenderResourceFormat::Depth24;
d.debugName = "ShadowStaticMap";
d.width     = static_cast<uint32_t>(shadowMapSize_);   // 4096
d.height    = static_cast<uint32_t>(shadowMapSize_);   // 4096
d.glName    = static_cast<uint32_t>(shadowDepthTex_);
d.sizeBytes = static_cast<uint64_t>(shadowMapSize_) * shadowMapSize_ * 4u;
d.valid     = true;
RenderCore::registerOrUpdateRenderResource(d);
```
Destruction: marked `valid=false` via `registerOrUpdateRenderResource`. **Correct.**

### 5.3 ShadowDynamicMap — MISSING from registry

`dynShadowDepthTex_` is private to `gosPostProcess`. No `RenderResourceId::ShadowDynamicMap` exists.

**SHADOW-RESOURCE-1 scope:**
1. Add `ShadowDynamicMap = 6` to `RenderResourceId` enum
2. Register in `initShadows()` same as static, using `dynShadowDepthTex_`
3. Update on resize if ever resizable (currently fixed 4096×4096)
4. Mark invalid on shutdown alongside static

### 5.4 Missing producer/consumer descriptors

`RenderResourceDesc` has `producerPassId` and `consumerMask` fields — currently zero for all resources. Future SHADOW-RESOURCE-1 should populate:
- ShadowStaticMap producer: shadow pre-pass
- ShadowStaticMap consumers: terrain inline + screen-shadow pass
- ShadowDynamicMap producer: dynamic shadow pass
- ShadowDynamicMap consumers: screen-shadow pass (non-terrain)

---

## 6. Shader Inventory

### 6.1 Shadow producer shaders (write depth)

| Shader | Target | Key uniforms | SSBO |
|---|---|---|---|
| `shadow_depth.vert` + `.frag` | Generic static geometry | `lightSpaceMatrix` | — |
| `shadow_terrain.vert/tesc/tese/frag` | Terrain (tessellated) | `lightSpaceMatrix`, `tessLevel`, `tessDisplace`, sampler2D tex1+matNormal2 | — |
| `shadow_object.vert` + `.frag` | Static objects (Stuff-space) | `lightSpaceMatrix`, `worldMatrix`, `lightOffset` | — |
| `shadow_mech.vert` | GPU-skinned mechs | `lightSpaceMatrix`, `u_instanceBase`, `u_skinningMode` | binding 0 (instances), binding 1 (bones) |
| `shadow_static_prop.vert` | GPU-instanced props | `lightSpaceMatrix` | binding 0 (Instance SSBO) |

All producer frags write explicit `gl_FragDepth = gl_FragCoord.z`.  
All use forward-Z (shadow pass sets `glClearDepth(1)`).

### 6.2 Shadow receiver shaders (sample shadow maps)

| Shader | Shadow type | Sampling | Notes |
|---|---|---|---|
| `gos_terrain.frag` | Static + dynamic | Inline PCF, 16/8/4 + 8/4 taps | Skips screen-shadow pass via GBuffer flag |
| `shadow_screen.frag` | Static + dynamic | 8-tap + 4-tap Poisson PCF | Deferred fullscreen quad |
| `static_prop.frag` | Via screen-shadow | — | GBuffer eligible flag set |
| `mech.frag` | Via screen-shadow | — | GBuffer eligible flag set |
| `gos_vertex_lighted.frag` | Via screen-shadow | — | GBuffer eligible flag set |

### 6.3 Shadow sampling library — `include/shadow.hglsl`

**Key functions:**
- `calcShadow(worldPos, normal, lightDir, numTaps)` — static map PCF
- `calcDynamicShadow(worldPos, normal, lightDir, numTaps)` — dynamic map PCF
- Both use gradient-adaptive Poisson PCF:
  - Hardness: `clamp(depthGradient * 180.0, 0.0, 1.0)` — single magic number for MC2's bimodal terrain
  - Adaptive radius: `mix(3.2, 0.8, hardness)` — wide on flats, tight on cliffs
  - Per-pixel stratified rotation via position hash (breaks PCF banding)
  - Back-face guard: NdotL < 0.05 → return 1.0 (fully lit)
  - Out-of-bounds: return 1.0 (fully lit)
- Shadow factor: `mix(0.4, 1.0, shadow_ratio)` — minimum shadow darkness 0.4

**Global uniforms (all consumers via include):**
- `sampler2DShadow shadowMap`
- `mat4 lightSpaceMatrix`
- `int enableShadows`
- `float shadowSoftness` (default 2.5 texels)
- `sampler2DShadow dynamicShadowMap`
- `mat4 dynamicLightSpaceMatrix`
- `int enableDynamicShadows`

### 6.4 Debug visualizer

**`shadow_debug.frag`:**
- Magenta: depth ≥ 0.999 (unwritten / far plane)
- Red: depth ≤ 0.001 (near-plane clipping)
- Grayscale ramp (gamma 0.5): normal depth values

`gosPostProcess` has `showShadowDebug_` + `shadowDebugMode_` toggles (0=static, 1=dynamic).

---

## 7. Pass Graph (Descriptive)

```
MISSION LOAD
  └─► buildStaticLightMatrix()
        └─► staticLightSpaceMatrix_[16] (world-fixed ortho VP)

EVERY FRAME
  ├─► [once, gate: tessellation active]
  │     beginShadowPrePass → shadowFBO
  │     shadow_terrain shaders → static shadow depth 4096×4096
  │     endShadowPrePass
  │
  ├─► buildDynamicLightMatrix(8 frustum corners)
  │     └─► dynamicLightSpaceMatrix_[16] (camera-centered ortho VP)
  │
  ├─► beginDynamicShadowPass → dynShadowFBO
  │     shadow_static_prop.vert → static prop depth (non-indirect)
  │     shadow_mech.vert        → mech depth (prev-frame ring slot)
  │     endDynamicShadowPass
  │
  ├─► MAIN SCENE RENDER
  │     terrain.frag      ← samples both shadow maps inline → writes shadow to final color
  │     static_prop.frag  → writes GBuffer (shadow-eligible flag in normal.a)
  │     mech.frag         → writes GBuffer (shadow-eligible flag in normal.a)
  │     buildings.frag    → writes GBuffer (shadow-eligible flag)
  │
  └─► SCREEN SHADOW PASS (non-terrain pixels only)
        shadow_screen.frag (fullscreen quad)
          ← reads GBuffer depth + normal.a shadow-eligible flag
          ← samples shadowMap (unit 2) + dynamicShadowMap (unit 3)
          → multiplicative shadow factor → composited onto scene color
```

### 7.1 GBuffer shadow-eligibility sentinel

Non-terrain shaders write `rc_gbuffer1_screenShadowEligible(normal)` to GBuffer normal alpha.  
`shadow_screen.frag` reads this flag to skip pixels that handle their own shadow (terrain) or sky.

---

## 8. Debug Gaps

### 8.1 Current capabilities

| Capability | Status |
|---|---|
| Shadow map depth preview | `showShadowDebug_` + `shadow_debug.frag` — EXISTS |
| Shadow factor overlay | `screenShadowDebug_` mode — EXISTS (debug only) |
| Static vs dynamic toggle | `MC2_SHADOW_ENABLE` env var, `enableShadows`/`enableDynamicShadows` uniforms |
| `glPolygonOffset` visible in debugger | Not exposed to ImGui |
| `shadowSoftness` tunable | Not exposed to ImGui |
| ShadowStaticMap in RenderResource registry | YES |
| ShadowDynamicMap in RenderResource registry | NO |
| ShadowDirectional0 in EngineView registry | NO (reserved only) |
| ShadowDynamic in EngineView registry | NO (ViewKind defined, no ViewId) |
| Shadow map capture preset | NOT in capture presets |
| Shadow factor capture preset | NOT in capture presets |

### 8.2 Gaps for SHADOW-DEBUG-VIEWS-1

- No ImGui panel for shadow bias (`glPolygonOffset` factor/units)
- No ImGui slider for `shadowSoftness`
- `shadowDepthTex_` / `dynShadowDepthTex_` not accessible via RenderResource lookup (dynamic missing)
- No capture preset for shadow depth view
- No capture preset for shadow factor overlay
- `shadow_debug.frag` visualizer exists but not wired to engine debug menu (needs `MC2_SHADOW_DEBUG` gate or ImGui toggle)

---

## 9. Risks

### 9.1 Reverse-Z interaction (HIGH)
Shadow passes use forward-Z; main scene uses reverse-Z. Manual `glClearDepth()` swap is the boundary.  
Risk: any EngineView/ViewUniforms integration that auto-uploads projection matrices must NOT touch `glClearDepth` — shadow passes own that state explicitly.  
**Mitigation:** ViewUniforms upload is store-only (F1-4B decoupling); no auto-clear. Safe.

### 9.2 Bias/acne/peter-panning (MEDIUM)
Current bias: `glPolygonOffset(2.0, 4.0)` + per-pixel slope bias 0.002–0.005.  
Not tunable at runtime. Adding ImGui controls without an immediate visual reference will make it hard to validate.  
**Mitigation:** Wire SHADOW-DEBUG-VIEWS-1 before SHADOW-TUNING-1. Debug view is prerequisite.

### 9.3 Static vs dynamic shadow map disagreement (MEDIUM)
Static map is world-fixed (built once). Dynamic map is per-frame camera-centered.  
If sun direction changes mid-mission, static map becomes stale but is NOT rebuilt.  
Currently `staticLightMatrixBuilt_` is a one-shot flag — no invalidation path.  
**Mitigation:** Document this as known limitation. Any SHADOW-VIEW-1 work must NOT trigger a rebuild. Invalidation is a future slice.

### 9.4 Terrain displacement future desync (LOW now, HIGH later)
Shadow terrain tessellation uses same displacement as main terrain.  
When TERRAIN-DISPLACE-VISUAL-1 ships near-unit displacement, the shadow terrain pass MUST use identical displacement params, otherwise shadow-receiver geometry won't match shadow-caster geometry.  
**Mitigation:** Note in SHADOW-VIEW-1 plan that `tessDisplace` uniform must stay in sync between main and shadow terrain passes.

### 9.5 ViewUniforms / EngineView mismatch (LOW)
`ViewUniforms.worldToClipGL` is row-major, uploaded `GL_FALSE`.  
Shadow matrices `staticLightSpaceMatrix_` are also row-major, uploaded `GL_FALSE`.  
Bit-for-bit compatible. No mismatch.  
**Mitigation:** Verify in SHADOW-VIEW-1 implementation with `memcmp` or debug assert.

### 9.6 Resource lifetime / invalidation (MEDIUM)
`ShadowStaticMap` is marked invalid on shutdown. Correct.  
`ShadowDynamicMap` does not exist in registry — consumers can't query it, so no stale pointer risk.  
Risk: once registered, `dynShadowDepthTex_` could change if shadow map is recreated (no resize today, but theoretically possible).  
**Mitigation:** SHADOW-RESOURCE-1 must call `registerOrUpdateRenderResource` on any resize path.

### 9.7 GPU ring-slot lag (LOW)
Mech shadow pass reads previous-frame ring slot (fence-safe one-frame lag).  
Shadow view registration must NOT attempt to re-read current-frame data for mech shadows.  
**Mitigation:** No SSBO reads from SHADOW-VIEW-1 (view registration only).

---

## 10. Recommended Implementation Slices

### SHADOW-VIEW-1 — Register shadow EngineViews, no visual change

**Scope:**
1. Add `kShadowDynamicViewId = 3` to `EngineView.h`
2. In `buildStaticLightMatrix()`: after matrix is built, construct + register EngineView with `id=kShadowDirectional0ViewId`, `kind=ShadowStatic`, fill `viewUniforms.worldToClipGL` from `staticLightSpaceMatrix_`
3. In `buildDynamicLightMatrix()`: same for dynamic view
4. No shader changes. No uniform upload path changes. View registration is observer-only.
5. Verify via `resolveView(kShadowDirectional0ViewId)` returns non-null after mission load
6. Add entry to mc2-render-state debug state dump

**Risk:** zero visual. EngineView registration is additive.

---

### SHADOW-RESOURCE-1 — Register ShadowDynamicMap

**Scope:**
1. Add `ShadowDynamicMap = 6` to `RenderResourceId` enum
2. Register `dynShadowDepthTex_` in `initShadows()` with same descriptor pattern as static
3. Populate `producerPassId` for both ShadowStaticMap and ShadowDynamicMap
4. Update `valid=false` in shutdown for both
5. Validate JSON via resource inspector

**Risk:** zero visual. Registry is read-only by consumers.

---

### SHADOW-DEBUG-VIEWS-1 — Shadow depth/factor preview

**Scope:**
1. ImGui panel: shadow map preview (static + dynamic), toggle via `showShadowDebug_`
2. Expose `shadowSoftness` slider (0.5–8.0 texels)
3. Expose `glPolygonOffset` factor/units inputs (with reset-to-default button)
4. Add capture preset for shadow depth view
5. Add capture preset for shadow factor overlay
6. Wire `shadow_debug.frag` to debug menu (env gate or ImGui toggle)

**Risk:** debug-only paths. No production behavior change.

---

### SHADOW-TUNING-1 — Bias/PCF/shadow strength controls

**Scope:**
1. After SHADOW-DEBUG-VIEWS-1 provides reference view
2. Expose minimum shadow darkness (currently hardcoded `mix(0.4, 1.0, ...)`)
3. Expose per-pixel bias range (currently hardcoded 0.002–0.005)
4. Validate via visual capture compare
5. Write tuning observations to docs/observations/

**Risk:** low if debug view exists first. Without reference view, bias changes are unvalidatable.

---

## 11. File Reference Map

| File | Role |
|---|---|
| `GameOS/gameos/gos_postprocess.cpp` | Shadow FBO creation, light matrix building, begin/end passes |
| `GameOS/gameos/gos_postprocess.h` | Class members, matrix storage |
| `mclib/txmmgr.cpp` | Frame render loop, static + dynamic shadow pass orchestration |
| `GameOS/gameos/gos_static_prop_batcher.cpp:5419` | Static prop shadow draw call |
| `GameOS/gameos/gos_mech_batcher.cpp:522` | Mech shadow draw call |
| `RenderCore/EngineView.h` | ViewId/ViewKind enums, EngineView struct, registry API |
| `RenderCore/RenderResourceRegistry.h` | RenderResourceId enum, RenderResourceDesc, registry API |
| `RenderCore/RenderResourceRegistry.cpp` | Fixed-array registry implementation |
| `shaders/include/shadow.hglsl` | PCF sampling library, gradient-adaptive Poisson |
| `shaders/shadow_depth.vert/frag` | Generic static depth writer |
| `shaders/shadow_terrain.vert/tesc/tese/frag` | Tessellated terrain depth writer |
| `shaders/shadow_object.vert/frag` | Stuff-space static object depth writer |
| `shaders/shadow_mech.vert` | GPU-skinned mech depth writer |
| `shaders/shadow_static_prop.vert` | GPU-instanced prop depth writer |
| `shaders/shadow_screen.frag` | Deferred screen-space shadow applicator |
| `shaders/shadow_debug.frag` | Depth visualizer |
| `docs/plans/shadow-tess-design.md` | Tessellated terrain shadow design |
| `docs/plans/static-terrain-shadow-architecture.md` | Three-layer shadow model |
| `docs/observations/2026-05-25-shadow-postprocess-pipeline-map.md` | Full pipeline dataflow map |
| `docs/superpowers/specs/2026-05-16-gpu-driven-dynamic-sun-shadow-design.md` | GPU batcher shadow feed design |
