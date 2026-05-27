#pragma once
#include "ViewUniforms.h"
#include <cstdint>

namespace RenderCore {

using ViewId = uint32_t;
constexpr ViewId kInvalidViewId   = 0;
constexpr ViewId kMainSceneViewId = 1;

enum class ViewKind : uint32_t {
    MainScene     = 1,
    ShadowStatic  = 2,
    ShadowDynamic = 3,
};

// Per-frame view descriptor. GL-free; safe to copy.
// frustumPlanes not present in F1-4A — added when first consumer needs them.
struct alignas(16) EngineView {
    ViewId       id             = kInvalidViewId;
    ViewUniforms viewUniforms   = {};
    int32_t      viewport[4]    = {};  // x, y, w, h in pixels
    uint32_t     renderMask     = 0xFFFFFFFF;
    const char*  debugName      = nullptr;  // must point to a string literal; never heap
};

} // namespace RenderCore
