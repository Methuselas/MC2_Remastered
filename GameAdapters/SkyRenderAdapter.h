// GameAdapters/SkyRenderAdapter.h
//
// Slice HDRI-SKY-1 (v1): sky rendering firewall bridge.
// Spec: docs/superpowers/specs/2026-05-22-renderworld-boundary-spec.md (section 10)
//
// Include discipline (load-bearing):
//   - Header MUST NOT include any engine headers (gos_postprocess.h, GL, RenderWorld).
//   - Header MUST NOT include any game-side headers.
//   - Header is pure interface only — zero #include directives.
//   - The .cpp includes gos_postprocess.h (the only TU that may).
//   - v1 is route-only — no RenderWorld object handle per render contract.

#pragma once

namespace GameAdapters {
namespace Sky {

// True when the HDRI texture loaded successfully and the shader
// compiled. False when MC2_HDRI_SKY=0, asset missing, shader broken,
// or init has not yet been called.
bool isHdriReady();

// Renders the HDRI background as a fullscreen pass into the currently
// bound scene FBO. No-op when isHdriReady() is false.
//
// viewMat, projMat: column-major float[16] from the current camera.
// Only the rotational part of viewMat is used — translation is
// intentionally excluded so the sky does not parallax with the camera.
void renderHdri(const float* viewMat, const float* projMat);

}  // namespace Sky
}  // namespace GameAdapters
