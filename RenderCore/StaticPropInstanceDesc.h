// RenderCore/StaticPropInstanceDesc.h
//
// Slice M1 (C1 fix 2026-05-22): POD mirror of GpuStaticPropInstance.
// Pure C++ standard types only -- no GL, no Stuff, no mclib.
//
// Translation between this and GpuStaticPropInstance happens at the
// two engine-game seams:
//   - Game-side seam: GameAdapters/StaticPropRenderAdapter.cpp
//   - Engine-side seam: RenderWorld/legacy/static_prop_backend.cpp
//
// Both seams compile-time assert sizeof and offsetof match. If
// GpuStaticPropInstance ever drifts, the asserts fail loudly and this
// mirror must be updated in lockstep. The firewall script
// (scripts/check-include-firewall.sh) enforces that no OTHER TU
// includes gos_static_prop_batcher.h from RenderCore/ or RenderWorld/
// outside the legacy-backend TU.

#pragma once

#include <cstdint>
#include <cstddef>

namespace RenderCore {

// Field-for-field mirror of GameOS/gameos/gos_static_prop_batcher.h:15
// GpuStaticPropInstance. KEEP IN LOCKSTEP. Any change here requires a
// matching change on the engine side (and vice versa); the seam
// static_asserts will catch drift at compile time.
struct alignas(16) StaticPropInstanceDesc {
    float    modelMatrix[16];   // shape-to-world, row-major
    uint32_t typeID;
    uint32_t firstColorOffset;
    uint32_t flags;
    uint32_t lightDataIndex;
    float    aRGBHighlight[4];
    float    fogRGB[4];
};

// Local compile-time invariants. The cross-seam asserts in the adapter
// and legacy-backend TUs will additionally verify size matches the
// engine-side struct.
static_assert(sizeof(StaticPropInstanceDesc) == 112,
              "StaticPropInstanceDesc must match GpuStaticPropInstance layout");
static_assert(offsetof(StaticPropInstanceDesc, modelMatrix)      ==  0, "modelMatrix offset");
static_assert(offsetof(StaticPropInstanceDesc, typeID)           == 64, "typeID offset");
static_assert(offsetof(StaticPropInstanceDesc, firstColorOffset) == 68, "firstColorOffset offset");
static_assert(offsetof(StaticPropInstanceDesc, flags)            == 72, "flags offset");
static_assert(offsetof(StaticPropInstanceDesc, lightDataIndex)   == 76, "lightDataIndex offset");
static_assert(offsetof(StaticPropInstanceDesc, aRGBHighlight)    == 80, "aRGBHighlight offset");
static_assert(offsetof(StaticPropInstanceDesc, fogRGB)           == 96, "fogRGB offset");

} // namespace RenderCore
