# HUD Rendering Pipeline Map (2026-05-25)

## Three-Zone Overview

```
┌──────────────────────────────────────────────────────────────────┐
│ GAME DATA SIDE (code/gamecam.cpp, code/missiongui.cpp, mclib/)   │
├──────────────────────────────────────────────────────────────────┤
│                                                                   │
│  • Actor::render() → Appearance::drawBars() for selected units   │
│  • Health/armor bars emit gos_VERTEX [4] with HUD_DEPTH=0.9999f │
│  • PolygonQuadElement.init(vertices) stores quad geometry       │
│  • MissionInterfaceManager::render() draws drag-select box      │
│  • gos_SetRenderState(gos_State_IsHUD, 1) marks for buffering   │
│  • gos_DrawQuads(), gos_DrawLines(), gos_DrawTris() call driver │
│                                                                   │
└──────────────────────────────────────────────────────────────────┘
         ↓ (gos_State_IsHUD=1 signals buffering)
┌──────────────────────────────────────────────────────────────────┐
│ API LAYER (GameOS/gameos/gameos_graphics.cpp)                    │
├──────────────────────────────────────────────────────────────────┤
│                                                                   │
│  gos_DrawQuads(vertices) / gos_DrawLines() / gos_DrawTris()     │
│    ├─ Check: is gos_State_IsHUD set?                             │
│    ├─ YES → Push to hudBatch_ vector (buffered, not flushed)    │
│    └─ Vertex z-depth already HUD_DEPTH (0.9999f) set by caller  │
│                                                                   │
│  HUD render state saved per-batch:                              │
│    • gos_Alpha_AlphaInvAlpha (transparency blend)               │
│    • gos_State_Perspective = 1 (for rhw)                        │
│    • gos_State_ZCompare = 1, gos_State_ZWrite = 1              │
│    • Clipping, AlphaTest, Texture states per-element           │
│                                                                   │
└──────────────────────────────────────────────────────────────────┘
         ↓ (Frame proceeds: terrain → objects → shadows → particles)
┌──────────────────────────────────────────────────────────────────┐
│ ENGINE SIDE (GameOS/gameos/gameos_graphics.cpp line 5840+)      │
├──────────────────────────────────────────────────────────────────┤
│                                                                   │
│  Frame sequence (gameosmain.cpp):                                │
│    1. gos_RendererBeginFrame() — clear hudBatch_                │
│    2. Environment.UpdateRenderers() — full 3D pipeline          │
│       • Terrain, objects, shadows, particles render to FBO      │
│    3. gos_RendererEndFrame() — terrain GPU state cleanup        │
│    4. pp->endScene() — composite post-process to FB 0           │
│    5. projectz_overlay_render() — debug overlay (if enabled)    │
│    6. gos_RendererFlushHUDBatch() — REPLAY buffered HUD        │
│       • Traverse hudBatch_ vector                                │
│       • For each call: restore saved render state + projection  │
│       • Call drawQuads/drawLines/drawTris with gos_State_IsHUD=0│
│       • Vertices already in clip space (x,y,HUD_DEPTH,rhw)      │
│       • Shader: gos_vertex.vert/frag (simple 2D + color)       │
│    7. GuiRuntime::Render() — ImGui (if enabled)                 │
│                                                                   │
│  HUD Shader chain:                                               │
│    • gos_vertex.vert: gl_Position = (mvp * pos) / pos.w         │
│      (mvp is identity for screen-space quads)                   │
│    • gos_vertex.frag: FragColor = color; GBuffer1 = flatUp()    │
│      (rc_gbuffer1_shadowHandled_flatUp() → a=1.0)               │
│      (signals shadow_screen.frag to skip post-shadow)           │
│                                                                   │
│  Depth behavior (reverse-Z):                                     │
│    • Scene renders depth 1.0 (far) to 0.0 (near)                │
│    • HUD writes z=HUD_DEPTH=0.9999f (very far, passes >scene)  │
│    • gos_State_ZCompare=1: depth test passes (0.9999 > scene)  │
│    • gos_State_ZWrite=1: HUD_DEPTH written to depth buffer      │
│    • Post-shadow shadow_screen.frag SKIPS HUD (GBuffer1.a=1)   │
│                                                                   │
└──────────────────────────────────────────────────────────────────┘
```

---

## Full Detail

### GAME DATA SIDE

#### Actor/Appearance Drawing

- **Entry point:** `code/actor.cpp:402` calls `drawBars()`
- **Implementation:** `mclib/appear.cpp:472` `Appearance::drawBars()`
  - Calculates bar geometry based on actor health status and screen position
  - Populates 4 `gos_VERTEX` structs (quad for health bar + outline box)
  - **Critical:** Sets `z = HUD_DEPTH` (0.9999f) on all vertices
  - Sets `rhw = 0.5` (reciprocal of homogeneous w)
  - Color packed into `argb` field (green/yellow/red based on health)
  - `frgb = 0x00000000` (no specular/fog affect)
- **Wrapped in:** `PolygonQuadElement` class (`mclib/cevfx.h:153`)
  - `init(vertices)` stores the 4-vertex quad
  - `draw()` calls `gos_DrawTriangles()` (via two triangles from quad)

#### Mission GUI Rendering

- **Entry point:** `code/mission.cpp:830` calls `missionInterface->render()`
- **Implementation:** `code/missiongui.cpp:3219` `MissionInterfaceManager::render()`
  - Draws drag-select rectangle when user drags to select units
  - Sets `gos_SetRenderState(gos_State_IsHUD, 1)` **before** drawing
  - Populates 5 `gos_VERTEX` structs (quad + close vertex for outline)
  - All vertices hardcoded `z = 0.0` (note: **NOT HUD_DEPTH**)
  - Sets alpha blend `gos_Alpha_AlphaInvAlpha`
  - Calls `gos_DrawQuads(vertices, 4)` and `gos_DrawLines(vertices, 2)`
  - Unsets `gos_State_IsHUD = 0` after drawing

#### Health Bar Render State Context

```cpp
// From PolygonQuadElement::draw()
gos_SetRenderState(gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
gos_SetRenderState(gos_State_ShadeMode, gos_ShadeGouraud);
gos_SetRenderState(gos_State_Perspective, 1);
gos_SetRenderState(gos_State_Clipping, 2);
gos_SetRenderState(gos_State_AlphaTest, 1);
gos_SetRenderState(gos_State_Specular, 0);
gos_SetRenderState(gos_State_Dither, 1);
gos_SetRenderState(gos_State_TextureMapBlend, gos_BlendModulate);
gos_SetRenderState(gos_State_Filter, gos_FilterBiLinear);
gos_SetRenderState(gos_State_Texture, 0);
gos_SetRenderState(gos_State_ZCompare, 1);
gos_SetRenderState(gos_State_ZWrite, 1);
```

---

### API LAYER

#### HUD Buffering Trigger

File: `GameOS/gameos/gameos_graphics.cpp`

**Line 4360–4365:** Frame initialization (BeginFrame)
```cpp
if (renderStates_[gos_State_IsHUD] != 0) {
    SPEW(("[HUD] gos_State_IsHUD still set at frame start\n"));
    renderStates_[gos_State_IsHUD] = 0;
}
hudBatch_.clear();
hudFlushed_ = false;
```

**Line 4517–4529:** `gos_DrawQuads()` with HUD flag
```cpp
if (renderStates_[gos_State_IsHUD]) {
    // Buffer the draw call instead of immediate GPU submission
    HudDrawCall call;
    call.kind = kHudQuadBatch;
    call.vertices = std::vector<gos_VERTEX>(v, v + numVertices);
    memcpy(call.stateSnapshot, renderStates_, sizeof(renderStates_));
    call.projection = projection_;
    hudBatch_.push_back(std::move(call));
}
```

**Line 4579–4591:** `gos_DrawLines()` with HUD flag (same buffering logic)

**Line 4649–4661:** `gos_DrawTris()` with HUD flag (same buffering logic)

#### Buffered Draw Call Structure

```cpp
struct HudDrawCall {
    enum Kind { kHudQuadBatch, kHudLineBatch, kHudTriBatch, kHudTextQuadBatch };
    Kind kind;
    std::vector<gos_VERTEX> vertices;
    uint32_t stateSnapshot[gos_MaxState];  // Full render state at draw time
    mat4 projection;  // Projection matrix at draw time
};

std::vector<HudDrawCall> hudBatch_;
bool hudFlushed_;
```

#### Why Buffer?

HUD must render **AFTER** post-process (shadow_screen.frag composite) to avoid being shadow-darkened. Buffering defers HUD draw until post-process is complete. The `gos_State_IsHUD` flag signals the driver to save the call instead of processing it immediately.

---

### ENGINE SIDE

#### Frame Execution Order

File: `GameOS/gameos/gameosmain.cpp:550–588`

```
gos_RendererBeginFrame()               [line 551]
  └─ Clear hudBatch_, set hudFlushed_=false

Environment.UpdateRenderers()           [line 552]
  ├─ Terrain render (→ FBO)
  ├─ Object/mech render (→ FBO)
  ├─ Shadows (→ FBO)
  ├─ Particles flush (→ FBO)
  └─ All calls with gos_State_IsHUD=0 or unset execute immediately

gos_RendererEndFrame()                 [line 553]
  └─ Terrain GPU state cleanup

glUseProgram(0)                         [line 557]

pp->endScene()                          [line 561]
  └─ Post-process: FBO → FB0 (shadow_screen.frag composite)

VisualDiff::onFrameTick()               [line 568]
  └─ Capture if MC2_VISUAL_DIFF_CAPTURE=1

projectz_overlay_render()               [line 579]
  └─ Debug overlay if enabled (RAlt+P)

gos_RendererFlushHUDBatch()            [line 582]
  └─ **REPLAY HUD BATCH** (to FB0, after post-process)

GuiRuntime::Render()                   [line 586]
  └─ ImGui overlay (if MC2_IMGUI enabled)
```

#### HUD Batch Replay (flushHUDBatch)

File: `GameOS/gameos/gameos_graphics.cpp:5840–5926`

**Line 5842–5845:** Early exit if batch empty
```cpp
if (hudBatch_.empty()) {
    hudFlushed_ = true;
    return;
}
```

**Line 5869–5887:** Optional HUD scaling (compress HUD for lower resolutions)
```cpp
const float scale = s_hud_scale;
if (s_hud_scale_active && scale < 0.999f) {
    // Scale all HUD vertices around bottom-center anchor
    // (used for ultrawide/resolution change accommodation)
    for (HudDrawCall& call : hudBatch_) {
        float cy = centroid_y(call.vertices);
        if (cy < bottomBand) continue;  // Skip dialogs/menus
        // Scale toward anchor (sw*0.5, sh)
    }
}
```

**Line 5891–5901:** Rebind VAO and save render state
```cpp
glBindVertexArray(gVAO);
uint32_t priorState[gos_MaxState];
memcpy(priorState, renderStates_, sizeof(priorState));
mat4 priorProjection = projection_;
```

**Line 5898–5920:** Replay each buffered call
```cpp
for (const HudDrawCall& call : hudBatch_) {
    memcpy(renderStates_, call.stateSnapshot, sizeof(renderStates_));
    renderStates_[gos_State_IsHUD] = 0;  // Prevent re-buffering
    projection_ = call.projection;

    switch (call.kind) {
        case kHudQuadBatch:
            drawQuads(call.vertices.data(), call.vertices.size());
            break;
        case kHudLineBatch:
            drawLines(call.vertices.data(), call.vertices.size());
            break;
        case kHudTriBatch:
            drawTris(call.vertices.data(), call.vertices.size());
            break;
        case kHudTextQuadBatch:
            replayTextQuads(call);
            break;
    }
}
```

**Line 5922–5926:** Restore state and mark flushed
```cpp
memcpy(renderStates_, priorState, sizeof(priorState));
projection_ = priorProjection;
hudFlushed_ = true;
```

#### HUD Shader

File: `shaders/gos_vertex.vert` and `shaders/gos_vertex.frag`

**Vertex Shader (gos_vertex.vert:**
```glsl
layout(location = 0) in vec4 pos;      // Screen x,y,z,rhw
layout(location = 1) in vec4 color;    // ARGB
layout(location = 2) in vec4 fog;      // Fog value in .w
layout(location = 3) in vec2 texcoord; // U,V (mostly unused for bars)

uniform mat4 mvp;                      // Identity for screen-space HUD

void main(void) {
    vec4 p = mvp * vec4(pos.xyz, 1);
    gl_Position = p / pos.w;  // Divide by rhw for perspective
    Color = color;
    FogValue = fog.w;
    Texcoord = texcoord;
}
```

**Fragment Shader (gos_vertex.frag):**
```glsl
in PREC vec4 Color;
in PREC vec2 Texcoord;
in PREC float FogValue;

layout(location=0) out PREC vec4 FragColor;
layout(location=1) out PREC vec4 GBuffer1;  // Post-shadow metadata

void main(void) {
    PREC vec4 c = Color.bgra;
    // Optional texture sampling (usually disabled for bars)
    if(fog_color.x > 0.0 || ...)  // Fog blend
        c.rgb = mix(fog_color.rgb, c.rgb, FogValue);
    FragColor = c;

    // CRITICAL: Signal post-shadow to skip HUD
    GBuffer1 = rc_gbuffer1_shadowHandled_flatUp();
    // Returns vec4(0.5, 0.5, 1.0, 1.0)
    //   .rgb = flat-up normal (0.5, 0.5, 1.0 encoded)
    //   .a = 1.0 → shadow_screen.frag sees a > 0.5, skips pixel
}
```

#### Post-Shadow Skip Mechanism

File: `shaders/include/render_contract.hglsl:35–37`

```glsl
PREC vec4 rc_gbuffer1_shadowHandled_flatUp() {
    return vec4(0.5, 0.5, 1.0, 1.0);
}
```

**In shadow_screen.frag:**
```glsl
bool rc_pixelHandlesOwnShadow(PREC vec4 gbuffer1) {
    return gbuffer1.a > 0.5;  // HUD GBuffer1.a = 1.0
}

// Main shadow pass:
if (!rc_pixelHandlesOwnShadow(gbuffer1)) {
    // Apply shadow darkening
    FragColor *= shadowFactor;
} else {
    // Skip shadow — HUD stays bright
    // FragColor unchanged
}
```

#### Reverse-Z Depth Semantics

- **Depth buffer initialized to 0.0** (far plane in reverse-Z)
- **Scene depth:** 1.0 (near) → 0.0 (far), written per-pixel
- **HUD depth:** `z = HUD_DEPTH = 0.9999f` (very far, almost never depth-tested away)
- **Depth test:** `gos_State_ZCompare = 1` (GL_GREATER in reverse-Z)
  - Pixel passes if `0.9999f > scene_depth`
  - True for all scene pixels (scene is 0.0–1.0, HUD is 0.9999)
  - HUD always renders on top
- **Depth write:** `gos_State_ZWrite = 1`
  - HUD_DEPTH written to depth buffer
  - Subsequent 3D geometry (if any) depth-tests against 0.9999
  - No further 3D renders after HUD, so no practical effect

---

## Key Findings

### 1. **HUD Buffering + Replay Design**

HUD geometry is **not** rendered immediately when `gos_DrawQuads()` is called. Instead:
- Driver detects `gos_State_IsHUD = 1`
- Vertices, render state, and projection are **snapshot and cached** in `hudBatch_`
- GPU submission is **deferred** until `gos_RendererFlushHUDBatch()`

This two-phase approach ensures HUD renders **AFTER** post-process (shadow_screen.frag), which reads GBuffer1 to skip shadow-darkening on HUD.

### 2. **Shadow Immunity via GBuffer1 Metadata**

HUD is immune to post-shadow darkening because:
- `gos_vertex.frag` writes `GBuffer1.a = 1.0` (shadowHandled flag)
- `shadow_screen.frag` checks `gbuffer1.a > 0.5` and **skips darkening**
- This is the render contract: HUD and terrain overlays handle their own shadow; props/objects don't

### 3. **HUD_DEPTH as Far Plane**

Setting `z = 0.9999f` (reverse-Z far) makes HUD depth-test pass over any scene geometry:
- Depth test is `GL_GREATER` (0.9999 > any scene z)
- Writes to depth buffer but no subsequent 3D renders occur
- **Single purpose:** prevent HUD occlusion by 3D scene geometry

### 4. **Screen-Space vs. World-Space Mixing**

HUD bars are screen-projected:
- `Appearance::drawBars()` reads camera projections to convert 3D position to 2D screen
- Resulting vertices are in **screen-space** (x, y ∈ 0..screenWidth/Height)
- `gos_vertex.vert` applies mvp (identity for HUD), then divides by `rhw`
- No world-space matrix in the pipeline for HUD (unlike terrain/mechs)

---

## Data Structures

### gos_VERTEX (GameOS/include/gameos.hpp:2147–2155)
```cpp
typedef struct {
    float x, y;           // Screen coordinates (0 → screenWidth/Height)
    float z;              // Depth (HUD uses 0.9999f)
    float rhw;            // Reciprocal homogeneous W (0.5 for HUD quads)
    DWORD argb;           // Color + alpha
    DWORD frgb;           // Specular + fog
    float u, v;           // Texture coordinates (0,0 for bars)
} gos_VERTEX;
```

### HudDrawCall (gameos_graphics.cpp:1617–1632, inferred from usage)
```cpp
struct HudDrawCall {
    enum Kind { kHudQuadBatch, kHudLineBatch, kHudTriBatch, kHudTextQuadBatch };
    Kind kind;
    std::vector<gos_VERTEX> vertices;
    uint32_t stateSnapshot[gos_MaxState];
    mat4 projection;
};
```

---

## Render State for HUD

| State | Value | Purpose |
|-------|-------|---------|
| `gos_State_IsHUD` | 1 (during capture), 0 (during replay) | Buffer vs. execute flag |
| `gos_State_AlphaMode` | `gos_Alpha_AlphaInvAlpha` | Transparency blending |
| `gos_State_Perspective` | 1 | Enable rhw perspective division |
| `gos_State_Clipping` | 2 | 2D clipping mode |
| `gos_State_ZCompare` | 1 | GL_GREATER (reverse-Z) |
| `gos_State_ZWrite` | 1 | Write depth (HUD_DEPTH) |
| `gos_State_Texture` | 0 | No texture sampling (mostly) |
| `gos_State_Specular` | 0 | No specular lighting |
| `gos_State_AlphaTest` | 1 | Alpha threshold test |
| `gos_State_Fog` | 0 | Fog disabled (set by caller pre-HUD) |

---

## Known Behaviors

1. **Late HUD Detection:** If `gos_DrawQuads()` is called after `gos_RendererFlushHUDBatch()` has executed, the draw is **silently discarded** with a SPEW message (lines 4519, 4581, 4651).

2. **HUD Scale Compensation:** During `flushHUDBatch()`, HUD vertices are optionally rescaled around a bottom-center anchor to compensate for ultrawide or small-screen resolutions. Dialogs (centroid > 60% screen height) are excluded from scaling.

3. **State Snapshot Per-Call:** Each buffered HUD draw captures the full render state at submission time. This allows mixed render states (e.g., some HUD quads with texture, others without) without explicit state changes during replay.

4. **ImGui on Top:** `GuiRuntime::Render()` (ImGui) runs **after** HUD batch replay, so in-game editor windows (if enabled) appear above health bars and selection overlays.

---

## References

- **Handoff:** "HUD depth reverse-Z fix — bars/brackets restored" (2026-05-24, HEAD `330e665`)
- **Related:** `docs/superpowers/specs/2026-04-26-render-contract-registry-design.md` (render contract; GBuffer1 shadow mask)
- **Reverse-Z primer:** Depth 1.0 = near plane, 0.0 = far plane; depth test is `GL_GREATER`
