// EditorBridge/EditorRenderBridge.cpp
//
// CARVE-OUT: this is the ONLY file in EditorBridge/ permitted to include
// both game-side and engine-side headers.
// See: scripts/check-include-firewall.allowlist
//
// EditorBridge v0.
// Spec: docs/superpowers/specs/mission-editor-render-bridge-v0-spec.md

#include "EditorRenderBridge.h"

#include <cstdlib>   // std::getenv
#include <cstring>   // std::strcmp

// Engine side
#include "../RenderWorld/RenderWorld.h"
#include "../RenderWorld/ScreenPick.h"   // ScreenPickContext, screenPickCompute

// Game side
#include "../mclib/terrain.h"   // Terrain, TerrainPtr, land
#include "../mclib/camera.h"    // Camera, CameraPtr (via dcamera.h), eye

// GameOS (gos_VERTEX, gos_DrawLines, gos_SetRenderState, gos_GetViewport, Environment)
#include "../GameOS/include/gameos.hpp"

// ---- game-side globals wired by the editor at startup ----
extern TerrainPtr land;   // defined in mclib/terrain.cpp
extern CameraPtr  eye;    // defined in mclib/camera.cpp

namespace EditorBridge {

// ---- internal state ----

static bool s_enabled = false;

// ---- helpers ----

// Convert spec RGBA (R=bits31-24, G=23-16, B=15-8, A=7-0) to gos_VERTEX::argb
// (A=bits31-24, R=23-16, G=15-8, B=7-0).
static uint32_t rgbaToArgb(uint32_t rgba) {
    const uint8_t r = (rgba >> 24) & 0xFFu;
    const uint8_t g = (rgba >> 16) & 0xFFu;
    const uint8_t b = (rgba >>  8) & 0xFFu;
    const uint8_t a = (rgba       ) & 0xFFu;
    return (uint32_t(a) << 24) | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
}

static gos_VERTEX makeVertex(float sx, float sy, uint32_t argb) {
    gos_VERTEX v{};
    v.x    = sx;
    v.y    = sy;
    v.z    = 0.0f;
    v.rhw  = 1.0f;
    v.argb = argb;
    v.frgb = 0;
    v.u    = 0.f;
    v.v    = 0.f;
    return v;
}

// Draw one line segment between two screen-space points.
// gos_DrawLines(pts, NumVertices) -- second arg is vertex count, not line count.
// 2 vertices = 1 line. Confirmed from gameos.hpp signature.
static void drawLine(float x0, float y0, float x1, float y1, uint32_t argb) {
    gos_VERTEX pts[2];
    pts[0] = makeVertex(x0, y0, argb);
    pts[1] = makeVertex(x1, y1, argb);
    gos_DrawLines(pts, 2);   // 2 vertices = 1 line segment
}

// Local runtime wrapper for screen->FBO-pixel coord transform.
// Owns gos_GetViewport -- must NOT be in RenderWorld (GameOS is not a RenderWorld dep).
// Math delegated to RenderWorld::screenPickCompute (pure, no GameOS).
static bool editorScreenToFboPixel(int screenX, int screenY,
                                    RenderWorld::ScreenPickContext* out) {
    if (!out) return false;
    out->mouseX = screenX;
    out->mouseY = screenY;
    gos_GetViewport(&out->vMulX, &out->vMulY, &out->vAddX, &out->vAddY);
    out->drawableWidth  = Environment.drawableWidth;
    out->drawableHeight = Environment.drawableHeight;
    if (out->vMulX <= 0.0f || out->vMulY <= 0.0f) return false;
    RenderWorld::screenPickCompute(out);
    return true;
}

static bool projectToScreen(const Stuff::Vector3D& worldPt,
                            float* outSx, float* outSy) {
    Stuff::Vector3D pt = worldPt;
    Stuff::Vector4D screen{};
    if (!eye->projectZ(pt, screen)) return false;
    *outSx = screen.x;
    *outSy = screen.y;
    return true;
}

// Push overlay render state (no Z test, alpha blend, HUD layer).
// v0 discipline: does NOT preserve prior state.
static void pushOverlayState() {
    gos_SetRenderState(gos_State_ZCompare,  0);
    gos_SetRenderState(gos_State_ZWrite,    0);
    gos_SetRenderState(gos_State_Texture,   0);
    gos_SetRenderState(gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
    gos_SetRenderState(gos_State_IsHUD,     1);
}

static void popOverlayState() {
    gos_SetRenderState(gos_State_IsHUD,     0);
    gos_SetRenderState(gos_State_ZCompare,  1);
    gos_SetRenderState(gos_State_ZWrite,    1);
    gos_SetRenderState(gos_State_AlphaMode, gos_Alpha_OneZero);
}

// ---- lifecycle ----

void init() {
    const char* val = std::getenv("MC2_EDITOR_MODE");
    s_enabled = (val && std::strcmp(val, "1") == 0);
}

void shutdown() {
    s_enabled = false;
}

bool isEnabled() { return s_enabled; }

// ---- stub implementations (replaced in Tasks 5-7) ----

EditorPickResult pickAt(int /*screenX*/, int /*screenY*/) {
    return {};  // Task 5
}

RenderWorld::VisibilityResult queryVisibility(RenderWorld::VisibilityRequest /*req*/) {
    return {};  // Task 6
}

void drawSelectionBounds(const EditorAabb& /*bounds*/, SelectionBoundsStyle /*style*/) {
    // Task 6
}

void drawTerrainTileOutline(const TerrainTileOverlayDesc& /*desc*/) {
    // Task 7
}

} // namespace EditorBridge
