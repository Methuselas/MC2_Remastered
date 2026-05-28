// RenderCore/RenderDebugView.h
// DEBUG-VIEW-REGISTRY-1: canonical render debug view vocabulary.
// Each lane supports a subset defined by its kDebugViewMask_* constant.
#pragma once
#include <cstdint>

enum class RenderDebugView : int {
    Final        = 0,   // default: no override
    Albedo       = 1,
    Normal       = 2,
    Roughness    = 3,
    Metallic     = 4,
    LightingOnly = 5,
    IblOnly      = 6,
    SpecularOnly = 7,
    MaterialIdx  = 8,   // static_prop lane only
    TexArrayLayer= 9,   // static_prop lane only
    _Count       = 10,
};

// Per-lane support masks.  Bit N = RenderDebugView(N) is supported.
// StaticPropOpaque: modes that map to a real shader branch (no unsupported stubs).
constexpr uint32_t kDebugViewMask_StaticPropOpaque =
    (1u << int(RenderDebugView::Final))        |
    (1u << int(RenderDebugView::Albedo))       |
    (1u << int(RenderDebugView::Normal))       |
    (1u << int(RenderDebugView::Roughness))    |
    (1u << int(RenderDebugView::Metallic))     |
    (1u << int(RenderDebugView::MaterialIdx))  |
    (1u << int(RenderDebugView::TexArrayLayer));

// Placeholders -- zero until the lane implements debug views.
constexpr uint32_t kDebugViewMask_Terrain  = 0u;
constexpr uint32_t kDebugViewMask_Mech     = 0u;
constexpr uint32_t kDebugViewMask_Shadow   = 0u;
constexpr uint32_t kDebugViewMask_Vfx      = 0u;

inline bool RenderDebugViewSupported(RenderDebugView v, uint32_t mask) {
    int i = int(v);
    if (i < 0 || i >= int(RenderDebugView::_Count)) return false;
    return (mask >> i) & 1u;
}

// Declared here, defined in RenderDebugView.cpp.
const char* RenderDebugViewName(RenderDebugView v);
const char* RenderDebugViewDescription(RenderDebugView v);
