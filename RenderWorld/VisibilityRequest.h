// RenderWorld/VisibilityRequest.h
//
// Seq C, step 1: VisibilityRequest v0 -- reporting-only cull query.
//
// v0 contract (spec in user conversation 2026-05-24):
//   - wraps existing visibility/cull facts; no new culling decisions
//   - output: counts by kind only (handles deferred to v1+)
//   - terrain=deferred, vfx=prohibited, overlay=undefined
//   - no draw-submission change, no behavior change
//
// Forbidden (v0 scope):
//   - replace terrain / mech / static-prop culling
//   - feed draw submission
//   - become authoritative visibility

#pragma once
#include <cstdint>

namespace RenderWorld {

// Bit field for the kinds to include in a VisibilityResult.
// Matches RenderObjectKind ordinals (0=StaticProp, 1=Mech).
struct VisibilityKindMask {
    static constexpr uint32_t StaticProp = 1u << 0;
    static constexpr uint32_t Mech       = 1u << 1;
    static constexpr uint32_t All        = StaticProp | Mech;
};

// Reserved layer bits. v0: all bits ignored.
struct VisibilityLayerMask {
    static constexpr uint32_t Default = 0u;
};

// Input to queryVisibility().
// viewId / kindMask / layerMask are stored but unused in v0.
struct VisibilityRequest {
    uint32_t viewId    = 0;                        // opaque camera token
    uint32_t kindMask  = VisibilityKindMask::All;  // which kinds to count
    uint32_t layerMask = VisibilityLayerMask::Default;
};

// Output of queryVisibility().
// Counts reflect current-frame registered-and-alive objects.
// terrain/vfx/overlay are not counted in v0 (represented as log tags only).
struct VisibilityResult {
    uint64_t static_props = 0;  // from GpuStaticPropRegistry active count
    uint64_t mechs        = 0;  // from RenderWorld mech-alive counter
    // terrain  -> deferred (CPU terrain picking remains canonical)
    // vfx      -> prohibited (no stable object identity)
    // overlay  -> undefined
};

} // namespace RenderWorld
