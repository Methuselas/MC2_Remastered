#pragma once
#include "ViewUniforms.h"
#include <cstdint>

namespace RenderCore {

using ViewId = uint32_t;
constexpr ViewId kInvalidViewId            = 0;
constexpr ViewId kMainSceneViewId          = 1;
// Reserved for the future shadow R→V lane; not yet registered by any caller.
constexpr ViewId kShadowDirectional0ViewId = 2;

// NOTE: ViewKind ordinals are NOT required to match ViewId constants.
// ViewId is a per-frame slot identifier; ViewKind is a classification tag.
// The numeric overlap (MainScene=1/kMainSceneViewId=1, ShadowStatic=2/kShadowDirectional0ViewId=2)
// is coincidental — do not rely on it.
enum class ViewKind : uint32_t {
    MainScene     = 1,
    ShadowStatic  = 2,
    ShadowDynamic = 3,
};

inline const char* toString(ViewKind k) {
    switch (k) {
        case ViewKind::MainScene:     return "MainScene";
        case ViewKind::ShadowStatic:  return "ShadowStatic";
        case ViewKind::ShadowDynamic: return "ShadowDynamic";
    }
    return "unknown";
}

// ViewMode: descriptive enum tagging *what kind of output* a view produces.
// Substrate-only (ENGINEVIEW-VIEWMODE-0); no consumers branch on this field yet.
// Future slices will wire ObjectIdDebug, sensor modes, etc.
enum class ViewMode : uint8_t {
    Visual          = 0,  // normal rendered output
    ObjectIdDebug   = 1,  // future: object-ID debug overlay (no implementer yet)
    TacticalOverlay = 2,  // future: tactical/UI overlay view (no implementer yet)
    Thermal         = 3,  // future: thermal sensor view (no implementer yet)
    Infrared        = 4,  // future: IR sensor view (no implementer yet)
    LowLight        = 5,  // future: low-light sensor view (no implementer yet)
};

inline const char* toString(ViewMode m) {
    switch (m) {
        case ViewMode::Visual:          return "Visual";
        case ViewMode::ObjectIdDebug:   return "ObjectIdDebug";
        case ViewMode::TacticalOverlay: return "TacticalOverlay";
        case ViewMode::Thermal:         return "Thermal";
        case ViewMode::Infrared:        return "Infrared";
        case ViewMode::LowLight:        return "LowLight";
    }
    return "unknown";
}

// Per-frame view descriptor. GL-free; safe to copy.
// frustumPlanes not present in F1-4A — added when first consumer needs them.
struct alignas(16) EngineView {
    ViewId       id             = kInvalidViewId;
    ViewUniforms viewUniforms   = {};
    int32_t      viewport[4]    = {};  // x, y, w, h in pixels
    uint32_t     renderMask     = 0xFFFFFFFF;
    const char*  debugName      = nullptr;  // must point to a string literal; never heap
    ViewMode     mode           = ViewMode::Visual;  // descriptive; no consumer branches yet
    ViewKind     kind           = ViewKind::MainScene;
};

} // namespace RenderCore
