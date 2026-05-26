#pragma once
#include <cstdint>

// Per-draw-slot dispatch metadata for the static-prop coalesce path.
// Produced in lockstep with RenderCore::DrawPacket[] by flush() before
// the v6 dispatch loop. Sized for L1 locality on mc2_24's worst-case 753 slots.
struct StaticPropDispatchMeta {
    uint32_t sortedSlot;        // absolute coalesce slot (== drawIDBase)
    uint32_t globalPacketIdx;   // index into s_packets[] (the GpuStaticPropPacket)
    uint32_t typeId;            // pkt.owningTypeID
    uint32_t group;             // 0=alpha-OFF (s_texArrayOff), 1=alpha-ON (s_texArrayOn)
    uint32_t instanceCount;     // CPU snapshot count; 0 = no draw for this slot
    uint32_t baseInstance;      // prefix-sum base from s_baseInstanceByCmdSsbo[sortedSlot]
    uint32_t drawIDBase;        // == sortedSlot; uploaded as u_drawIDBase uniform per draw
    int32_t  baseVertex;        // pkt.baseVertex into shared VBO
};
static_assert(sizeof(StaticPropDispatchMeta) == 32,
    "StaticPropDispatchMeta must be exactly 32 bytes");
