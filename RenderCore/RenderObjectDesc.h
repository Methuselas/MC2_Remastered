// RenderCore/RenderObjectDesc.h
//
// Slice M1: engine-side descriptor for the static-prop upsert path.
// Spec: docs/superpowers/specs/2026-05-22-renderworld-boundary-spec.md
//       section 4 (render object lifecycle).
//
// M1 scope: this is the descriptor the StaticPropRenderAdapter builds
// from an Appearance and passes into RenderWorld::upsertStaticProp.
// Phase 1 documentary: only the fields the static-prop path actually
// consumes are populated. ArchetypeFlags and LayerMask are declared
// for shape correctness but ignored by the M1 forwarder.

#pragma once

#include <cstdint>
#include <vector>

#include "Handle.h"
#include "StaticPropInstanceDesc.h"  // C1 fix: pure POD mirror, NOT GpuStaticPropInstance.

// Forward-decl engine-side TG_MultiShape (C1 fix: prefer typed
// forward-decl over `void*`). Defined in mclib/tgl.h, but
// tgl.h is engine-side (not game-side: AI/mission/Appearance). Per
// spec Section 12 allowance, engine-side geometry forward-decls in
// RenderCore are permitted; the storage backend takes the pointer
// through and never dereferences it inside RenderCore.
class TG_MultiShape;

namespace RenderCore {

// Documentary-only in M1; M2+ consumers populate.
struct ArchetypeFlags {
    uint32_t castsShadow         : 1;
    uint32_t receivesShadow      : 1;
    uint32_t selectable          : 1;
    uint32_t usesImpostor        : 1;
    uint32_t hasClusterLod       : 1;
    uint32_t isSensorVisibleOnly : 1;
    uint32_t isOverlayOnly       : 1;
    uint32_t isStaticForMission  : 1;
    uint32_t reserved            : 24;
};

// Documentary-only in M1.
using LayerMask = uint32_t;
constexpr LayerMask kLayerMain   = 1u << 0;
constexpr LayerMask kLayerShadow = 1u << 1;
constexpr LayerMask kLayerAll    = 0xFFFFFFFFu;

// M1 static-prop descriptor.
//
// Per Decision D2.A (by-value payload): the adapter MOVES the batch
// into this desc; the legacy backend translates each element onward
// into the engine-side GpuStaticPropInstance. One owner at all times.
// No lifetime contract across TUs.
//
// C1 fix: `batch` is the POD-mirror type, not GpuStaticPropInstance.
struct StaticPropDesc {
    TG_MultiShape*                          shape    = nullptr;
    std::vector<StaticPropInstanceDesc>     batch;
    ArchetypeFlags                          archetype{};
    LayerMask                               layers   = kLayerAll;
    uint32_t                                gameObjectId = 0;  // opaque echo
};

} // namespace RenderCore
