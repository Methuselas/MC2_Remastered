// code/gameplay_pick.cpp
//
// M2-pre: shared gameplay-pick spine. See gameplay_pick.h for the
// full contract. Implementation steps:
//   T1: screenToFboPixel (this file's pure transform; the lowest-risk
//       extraction with a verbatim copy of the M1.6 inline coord block).
//   T2: tryGameplayPick (the spine; gate ladder + coord scaling +
//       lookupAtPixel).
//   T4: RunGameplayPickSelfTest (gated by MC2_GAMEPLAY_PICK_SELFTEST=1).

#include "gameplay_pick.h"

#include <cstdio>   // std::fprintf for self-test result lines
#include <cstdlib>  // std::getenv for the self-test env-flag gate
#include <cstring>  // std::strcmp for env-flag value parse

// Forward-declare the GOS viewport accessor; matches the signature at
// code/missiongui.cpp:6218. Pulled forward here to avoid dragging the
// full GameOS header chain into this small TU.
extern "C" void gos_GetViewport(float* vMulX, float* vMulY,
                                float* vAddX, float* vAddY);

// Environment struct accessor: M2-pre needs Environment.drawableWidth /
// drawableHeight (same fields M1.6's inline body reads at
// code/missiongui.cpp:6219-6220). Reach into the GameOS header that
// declares Environment. The exact include matches what missiongui.cpp
// already pulls.
#include "../GameOS/include/gameos.hpp"

//----------------------------------------------------------------------
// screenToFboPixel -- pure coord transform.
//
// Derivation (verbatim from M1.6 commit 1853ad9 inline comment at
// code/missiongui.cpp:6197-6216):
//
//   missiongui mouseX/Y from userInput->getMouseX/Y() is
//   mouseXPosition * viewMulX, where mouseXPosition is normalized to
//   drawable (0.0-1.0) and viewMulX is the UI viewport width (800 in
//   the 800x600 logical canvas). So mouseX/Y is in viewport-relative
//   coords (0-vMulX, 0-vMulY), NOT FBO pixels. lookupAtPixel needs
//   FBO pixels.
//
//   Translation: mouseX/Y (viewport space) -> FBO pixel:
//     normalized = mouseX / vMulX (fraction across viewport)
//     fboPixel   = vAddX + normalized * vMulX_fbo
//   where vMulX/Y from gos_GetViewport are already in FBO-pixel units
//   (viewportRight - viewportLeft) * drawableWidth. So vMulX IS the
//   FBO-pixel width of the viewport region. But mouseXPosition is
//   normalized to drawable (not viewport), and getMouseX() multiplies
//   by vMulX -- yielding (mouseX_pixel_in_drawable / drawableWidth) *
//   viewport_pixel_width. For full-screen viewport (vMulX=drawableWidth),
//   this is identity. For sub-viewport, it's a scaled-down value.
//
//   To convert back: multiply by (drawableWidth / vMulX) to get raw
//   drawable-pixel-X. (Equivalent to "undo the viewport scaling".)
//   Then add vAddX for viewport-offset case. Y mirrors.
//
// Precondition: caller has off-screen-guarded (mouseX/Y in [0, vMulX/Y)
// AND vMulX/Y > 0). This helper does NOT re-check.
//----------------------------------------------------------------------
void screenToFboPixel(int   mouseX,         int   mouseY,
                      float vMulX,          float vMulY,
                      float vAddX,          float vAddY,
                      int   drawableWidth,  int   drawableHeight,
                      int*  outFboX,        int*  outFboY,
                      int*  outGlX,         int*  outGlY)
{
    // Undo the viewport scaling: viewport-relative -> drawable-pixel.
    const float scaleX = (vMulX > 0.0f) ? ((float)drawableWidth  / vMulX) : 1.0f;
    const float scaleY = (vMulY > 0.0f) ? ((float)drawableHeight / vMulY) : 1.0f;

    *outFboX = (int)(vAddX + (float)mouseX * scaleX);
    *outFboY = (int)(vAddY + (float)mouseY * scaleY);

    // GL convention: origin bottom-left. Mirrors mclib/mouse.cpp:225
    // and mclib/utilities.cpp:111-115 y-flip pattern.
    *outGlX  = *outFboX;
    *outGlY  = drawableHeight - 1 - *outFboY;
}

//----------------------------------------------------------------------
// tryGameplayPick -- the shared gameplay-pick spine.
// Implementation lands in T2; T1 ships a STUB so the header symbol
// resolves cleanly and the linker accepts the new TU.
//----------------------------------------------------------------------
GameplayPickResult tryGameplayPick(const GameplayPickRequest& req)
{
    // T1 stub: returns skipped. T2 replaces the body with the full
    // algorithm per spec Section 4.
    (void)req;
    GameplayPickResult r{};
    r.outcome = GameplayPickResult::Outcome::skipped;
    return r;
}

//----------------------------------------------------------------------
// RunGameplayPickSelfTest -- validation-gate self-test.
// Implementation lands in T4; T1 ships a STUB so the header symbol
// resolves cleanly. T4's body wires the 8-step assertion sequence.
//----------------------------------------------------------------------
void RunGameplayPickSelfTest()
{
    // T1 stub: no-op. T4 replaces with the env-flag gate + 8-step
    // assertion sequence per spec Section 8 Gate 2.
}
