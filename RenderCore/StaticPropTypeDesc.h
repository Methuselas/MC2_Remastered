// RenderCore/StaticPropTypeDesc.h
//
// Immutable per-type descriptor for static props.
// Lifetime: stable from GpuStaticPropBatcher::finalizeGeometry() to onMapUnload().
// Populated by gos_static_prop_batcher.cpp; consumed across the
// GameOS/RenderCore seam without pulling batcher internals into other TUs.
//
// v0: CPU-only. No SSBO binding, no shader access.
// See: docs/observations/2026-05-25-static-prop-type-table-design.md
#pragma once
#include <cstdint>

namespace RenderCore {

struct alignas(16) StaticPropTypeDesc {
    uint32_t typeId;       // dense index into s_types[]; matches GpuStaticPropInstance::typeID
    uint32_t firstPacket;  // index into s_packets[] (the shared IBO packet table)
    uint32_t packetCount;  // number of draw-packets for this type
    uint32_t alphaClass;   // 0=alpha-OFF, 1=alpha-ON (OR-reduce over packet materialFlags + textureAlpha)
};
static_assert(sizeof(StaticPropTypeDesc) == 16,
              "StaticPropTypeDesc must be 16 bytes");
static_assert(alignof(StaticPropTypeDesc) == 16,
              "StaticPropTypeDesc must be 16-byte aligned");

} // namespace RenderCore
