#pragma once
// SCHEDULER-EDGE-CLASSIFY-1 — the FIRST tier-C artifact.
//
// A GL-free, OFFLINE edge classifier that TYPES every ordering edge implied by
// kFramePassOrder[] and PROVES the shipped order is legal (a no-op identity
// baseline). It is PROOF INFRASTRUCTURE, NOT A SCHEDULER:
//
//   - No pass is moved. No runtime change. No GL. No engine includes.
//   - Byte-identical by construction: this header has ZERO runtime callers; it is
//     included only by the offline doctest suite (tests/unit/test_frame_graph.cpp).
//   - The arbitrary-permutation oracle isReorderLegal() is SLICE 2 — deliberately
//     NOT built here. This slice = classify + identity baseline only.
//
// WHY classify edges at all? The resource validator (frame_graph_validate.h)
// answers "is every read produced before it is consumed?" but it cannot tell you
// WHICH edges a reorderer must preserve vs which are free. A legal-reorder oracle
// needs every ordering constraint TYPED:
//
//   HardResource      — a real producer->consumer data edge in the FrameLocal DAG
//                       (last in-frame writer of a read). MUST be preserved.
//   SoftState         — ambient GL-state handshake the resource DAG cannot see
//                       (e.g. terrain re-asserts glColorMask(TRUE) for following
//                       passes). MUST be preserved; not derivable from reads/writes.
//   LegacyLatch       — cross-phase CPU latch (markTerrainDrawn / sceneHasTerrain_):
//                       terrain SETS it, post sub-passes CONSUME it. MUST be preserved.
//   KnownEarly        — a config-variable draw-site that legitimately fires BEFORE its
//                       kFramePassOrder slot (LOD-chunk terrain draws pre-renderLists).
//                       A SUPPRESSION rule, NOT a violation (mirrors frame_pass_trace.h
//                       knownEarlyDrawSite).
//   ContentConditional— an ordering relationship that only exists when specific scene
//                       CONTENT is present (water-on-water-map, decals-on-cement, etc.).
//                       Content presence is NOT in the resource DAG.
//   ExternalNonEdge   — a read EXCLUDED from the HardResource set because the resource's
//                       lifetime is Mission/Persistent/External (seeded / persistent /
//                       temporal N-1). Emitted VISIBLY so the lifetime exclusion shows up
//                       in the report instead of being silently dropped.
//
// Firewall: header-only, no GL includes, no game-side includes. Pure POD + constexpr +
// inline. Mirrors the style of frame_graph_validate.h / frame_pass_trace.h.

#include <cstdint>
#include "RenderPassContract.h"        // RenderPassId, kRenderPassContracts, kFramePassOrder
#include "RenderResourceRegistry.h"    // RenderResourceId, RenderResourceLifetime
#include "frame_graph_validate.h"      // findContract, kExternalResources (GL-free external set)
#include "ambient_contract.h"          // AmbientContract: colorMask reassert + terrain latch
#include "terrain_subpass_contract.h"  // TerrainDrawSite (LOD-chunk knownEarly draw-site)

namespace RenderCore { namespace framegraph {

// ---------------------------------------------------------------------------
// Edge classification vocabulary
// ---------------------------------------------------------------------------
enum class EdgeClass : uint8_t {
    HardResource = 0,   // producer->consumer FrameLocal data edge; must preserve
    SoftState,          // ambient GL-state handshake (colorMask reassert)
    LegacyLatch,        // cross-phase CPU latch (sceneHasTerrain_)
    KnownEarly,         // config-variable draw-site fires early; suppression, not violation
    ContentConditional, // edge only exists when specific scene content is present
    ExternalNonEdge,    // read excluded because lifetime in {Mission,Persistent,External}
};

inline const char* edgeClassName(EdgeClass c) {
    switch (c) {
        case EdgeClass::HardResource:       return "HardResource";
        case EdgeClass::SoftState:          return "SoftState";
        case EdgeClass::LegacyLatch:        return "LegacyLatch";
        case EdgeClass::KnownEarly:         return "KnownEarly";
        case EdgeClass::ContentConditional: return "ContentConditional";
        case EdgeClass::ExternalNonEdge:    return "ExternalNonEdge";
    }
    return "?";
}

// A single classified ordering edge (or non-edge, for ExternalNonEdge / KnownEarly).
//   from/to    — the two passes the relationship is between. For ExternalNonEdge and
//                KnownEarly, `to` is the pass the note attaches to and `from` is None.
//   cls        — the classification above.
//   via        — the resource the edge flows through (Unknown for pure SoftState/Latch).
//   forbidden  — true iff a reorderer MUST keep pos(from) < pos(to). HardResource,
//                SoftState, and LegacyLatch edges are forbidden-to-reorder. KnownEarly,
//                ContentConditional, and ExternalNonEdge are NOT forbidden here (KnownEarly
//                is a suppression; ContentConditional is informational for the oracle;
//                ExternalNonEdge is a deliberately-excluded non-edge).
//   contentGated — true for ContentConditional edges (and only those).
//   note       — human-readable description.
struct PassEdge {
    RenderPassId     from        = RenderPassId::None;
    RenderPassId     to          = RenderPassId::None;
    EdgeClass        cls         = EdgeClass::HardResource;
    RenderResourceId via         = RenderResourceId::Unknown;
    bool             forbidden   = false;
    bool             contentGated= false;
    const char*      note        = "";
};

// ---------------------------------------------------------------------------
// Static (GL-free) lifetime map.
//
// RenderResourceRegistry's lifetime field is populated at RUNTIME (registerOrUpdate...),
// so it is unavailable offline. This static map encodes the same vocabulary from the
// authoritative comments in RenderResourceRegistry.h, for the resources that the
// shipped top-level contract + terrain sub-pass tables actually read. FrameLocal is the
// default for anything not listed (the pass DAG's transient color/depth/normal/shadow-
// dynamic targets). A resource whose lifetime is Mission/Persistent/External is a read
// that must be classified ExternalNonEdge (excluded from the HardResource set), exactly
// mirroring kExternalResources in frame_graph_validate.h (which is the broader
// "readable without an in-frame producer" set).
// ---------------------------------------------------------------------------
inline RenderResourceLifetime staticResourceLifetime(RenderResourceId id) {
    switch (id) {
        // Mission: seeded/rebuilt per mission load.
        case RenderResourceId::TerrainHeightTexture:
        case RenderResourceId::TerrainRecipeBuffer:
        case RenderResourceId::TerrainThinBuffer:
        case RenderResourceId::CementAtlas:
        case RenderResourceId::TransitionMaskArray:
        case RenderResourceId::TerrainHeightSsbo:
            return RenderResourceLifetime::Mission;
        // Persistent: process/long-lived; built outside the dynamic frame order.
        case RenderResourceId::ShadowStaticMap:
        case RenderResourceId::MaterialGpuBuffer:
            return RenderResourceLifetime::Persistent;
        // External: temporal / cross-frame N-1 / externally owned.
        case RenderResourceId::WaterReflectionColor:
        case RenderResourceId::WaterReflectionDepth:
            return RenderResourceLifetime::External;
        // Everything else read by a pass is a FrameLocal transient produced this frame
        // (MainColor, MainDepth, MainNormal, ShadowDynamicMap, scene copies, ...).
        default:
            return RenderResourceLifetime::FrameLocal;
    }
}

// A read is excluded from the HardResource DAG iff its resource lifetime is
// Mission/Persistent/External (seeded / persistent / temporal). FrameLocal reads ARE
// real in-frame data edges. This is the offline analog of the kExternalResources
// exclusion in validateReadsSatisfied().
inline bool isExternalLifetime(RenderResourceId id) {
    const RenderResourceLifetime lt = staticResourceLifetime(id);
    return lt == RenderResourceLifetime::Mission
        || lt == RenderResourceLifetime::Persistent
        || lt == RenderResourceLifetime::External;
}

// ===========================================================================
// HAND-DECLARED TABLES (reviewed-at-change-time, like kExternalResources).
// ===========================================================================

// (1) kContentConditionalEdges[] — ordering relationships that only exist when specific
//     scene CONTENT is present. Content presence is NOT visible in the resource DAG, so a
//     pure producer-walk cannot derive these. They are NOT forbidden-to-reorder edges in
//     the resource sense; the oracle (slice 2) treats them as "present only on maps with
//     this content". Reviewed when a content-gated pass is added/changed.
static constexpr PassEdge kContentConditionalEdges[] = {
    // Water only draws on maps that HAVE water; it overlays the opaque scene depth.
    { RenderPassId::Terrain, RenderPassId::Water, EdgeClass::ContentConditional,
      RenderResourceId::MainDepth, /*forbidden*/ false, /*contentGated*/ true,
      "water-on-water-map: Water present only when the map contains water" },
    // Terrain overlay (cement/transitions) only when the map has cement/perimeter content.
    { RenderPassId::Terrain, RenderPassId::TerrainOverlay, EdgeClass::ContentConditional,
      RenderResourceId::MainColor, /*forbidden*/ false, /*contentGated*/ true,
      "overlay-on-cement: TerrainOverlay quads present only with residual cement content" },
    // Terrain decals (craters/scorch) only when craters/footprints exist this frame.
    { RenderPassId::Terrain, RenderPassId::TerrainDecal, EdgeClass::ContentConditional,
      RenderResourceId::MainColor, /*forbidden*/ false, /*contentGated*/ true,
      "decal-on-crater: TerrainDecal present only when craters/footprints exist" },
    // SceneObjectId writes happen only when the object-id MRT buffer is enabled.
    { RenderPassId::MechOpaque, RenderPassId::PostProcess, EdgeClass::ContentConditional,
      RenderResourceId::SceneObjectId, /*forbidden*/ false, /*contentGated*/ true,
      "objectId-when-IsObjectIdBufferEnabled: SceneObjectId MRT only on the 3-entry draw-buffer set" },
    // Box decals (post-process) only under MC2_DYNAMIC_DECALS.
    { RenderPassId::VFX, RenderPassId::PostProcess, EdgeClass::ContentConditional,
      RenderResourceId::SceneDepthCopy, /*forbidden*/ false, /*contentGated*/ true,
      "BoxDecals-when-DYNAMIC_DECALS: SceneDepthCopy consumed by PostProcess BoxDecals (gated)" },
    // Scene-color copy for the compute blur substrate only under MC2_VFX_SCENECOLOR_GRAB.
    { RenderPassId::VFX, RenderPassId::PostProcess, EdgeClass::ContentConditional,
      RenderResourceId::SceneColorCopy, /*forbidden*/ false, /*contentGated*/ true,
      "SceneColorCopy-when-VFX_SCENECOLOR_GRAB: copySceneColorForVfx gated; consumed by compute blur" },
};
static constexpr int kContentConditionalEdgeCount =
    sizeof(kContentConditionalEdges) / sizeof(kContentConditionalEdges[0]);

// (2) kForbiddenReorderEdges[] — belt-and-suspenders list of must-not-move edges the
//     resource model alone can't fully derive (soft handshakes, latches, screen-space
//     ordering). The classifier ORs the derived HardResource edges WITH this set so a
//     soft edge the model misses is STILL caught as forbidden. Every entry's from/to
//     must be a real RenderPassId (asserted by a doctest). Reviewed at change time.
static constexpr PassEdge kForbiddenReorderEdges[] = {
    // Shadow must produce ShadowDynamicMap before any geometry samples it.
    { RenderPassId::Shadow, RenderPassId::StaticPropOpaque, EdgeClass::HardResource,
      RenderResourceId::ShadowDynamicMap, /*forbidden*/ true, /*contentGated*/ false,
      "Shadow->geometry: ShadowDynamicMap must be produced before opaque samples it" },
    { RenderPassId::Shadow, RenderPassId::MechOpaque, EdgeClass::HardResource,
      RenderResourceId::ShadowDynamicMap, /*forbidden*/ true, /*contentGated*/ false,
      "Shadow->geometry: ShadowDynamicMap must be produced before mech samples it" },
    { RenderPassId::Shadow, RenderPassId::Terrain, EdgeClass::HardResource,
      RenderResourceId::ShadowDynamicMap, /*forbidden*/ true, /*contentGated*/ false,
      "Shadow->geometry: ShadowDynamicMap must be produced before terrain samples it" },
    // Terrain re-asserts glColorMask(TRUE) (after shadow disabled color write) for ALL
    // following color-writing passes. Soft GL-state edge, invisible to the resource DAG.
    { RenderPassId::Terrain, RenderPassId::PostProcess, EdgeClass::SoftState,
      RenderResourceId::Unknown, /*forbidden*/ true, /*contentGated*/ false,
      "Terrain colorMask-reassert: re-enables RGBA write that following passes depend on" },
    // Terrain sets sceneHasTerrain_; PostProcess sub-passes bail if unset. Cross-phase latch.
    { RenderPassId::Terrain, RenderPassId::PostProcess, EdgeClass::LegacyLatch,
      RenderResourceId::Unknown, /*forbidden*/ true, /*contentGated*/ false,
      "sceneHasTerrain latch: terrain sets markTerrainDrawn; 5 post sub-passes consume it" },
    // Opaque scene must be drawn before VFX / vegetation / post composite read or overlay it.
    { RenderPassId::StaticPropOpaque, RenderPassId::VFX, EdgeClass::HardResource,
      RenderResourceId::MainDepth, /*forbidden*/ true, /*contentGated*/ false,
      "opaque->VFX: VFX reads scene depth produced by opaque geometry" },
    { RenderPassId::StaticPropOpaque, RenderPassId::VegetationCards, EdgeClass::HardResource,
      RenderResourceId::MainDepth, /*forbidden*/ true, /*contentGated*/ false,
      "opaque->Veg: vegetation depth-tests against the opaque scene depth" },
    { RenderPassId::StaticPropOpaque, RenderPassId::PostProcess, EdgeClass::HardResource,
      RenderResourceId::MainColor, /*forbidden*/ true, /*contentGated*/ false,
      "opaque->PP: post-process composites the opaque scene color" },
    // UI is drawn before the post composite presents the final frame.
    { RenderPassId::UI, RenderPassId::PostProcess, EdgeClass::SoftState,
      RenderResourceId::MainColor, /*forbidden*/ true, /*contentGated*/ false,
      "UI-before-composite: HUD/text must be present before the final composite" },
};
static constexpr int kForbiddenReorderEdgeCount =
    sizeof(kForbiddenReorderEdges) / sizeof(kForbiddenReorderEdges[0]);

// (3) kDeferredSoftStateEdges[] — soft-state hazards with NO stable per-pass value
//     (ambient_contract.h:54-56: "deferred axes with no stable per-pass declarable
//     value"): tex-unit latches, bound FBO, clip-control/scissor ambiguities. The oracle
//     (slice 2) uses these to say "resource-wise legal BUT blocked by a deferred
//     soft-state edge" — so it does NOT overclaim legality. These are NOT classified as
//     forbidden here (no stable from/to ordering value yet); they are an explicit
//     "unknown hazard" ledger. Reviewed at change time.
static constexpr PassEdge kDeferredSoftStateEdges[] = {
    { RenderPassId::None, RenderPassId::None, EdgeClass::SoftState,
      RenderResourceId::Unknown, /*forbidden*/ false, /*contentGated*/ false,
      "tex-unit latch hazard: NVIDIA tex-unit leak; no stable per-pass declarable value" },
    { RenderPassId::None, RenderPassId::None, EdgeClass::SoftState,
      RenderResourceId::Unknown, /*forbidden*/ false, /*contentGated*/ false,
      "bound-FBO hazard: draw-FBO is a dynamic GLuint; not a per-pass enum" },
    { RenderPassId::None, RenderPassId::None, EdgeClass::SoftState,
      RenderResourceId::Unknown, /*forbidden*/ false, /*contentGated*/ false,
      "clip-control/scissor hazard: frame-global / inherited; ambiguous per-pass" },
};
static constexpr int kDeferredSoftStateEdgeCount =
    sizeof(kDeferredSoftStateEdges) / sizeof(kDeferredSoftStateEdges[0]);

// ---------------------------------------------------------------------------
// classifyEdges — derive HardResource edges via the producer-walk, then fold in the
// SoftState/LegacyLatch (from ambient_contract), KnownEarly suppression, ExternalNonEdge
// visibility, and the three hand tables. Pure; writes into a caller-provided buffer.
//
// `out` must have room for at least classifyEdgesCapacity() entries; `count` is set to
// the number written. Generic over a contract table + order so a doctest can feed a
// synthetic table (e.g. to exercise an ExternalNonEdge read).
// ---------------------------------------------------------------------------
inline int classifyEdgesCapacity() {
    // Worst case: every (pass, read-slot) pair is an edge, plus all three hand tables.
    return kRenderPassIdCount * 4
         + kContentConditionalEdgeCount
         + kForbiddenReorderEdgeCount
         + kDeferredSoftStateEdgeCount
         + kRenderPassIdCount /* one KnownEarly suppression entry budget */;
}

inline void classifyEdgesFor(const RenderPassContract* contracts, int contractCount,
                             const RenderPassId* order, int orderCount,
                             PassEdge* out, int& count) {
    count = 0;

    // --- 1. Producer-walk: for each read, find the LAST earlier in-frame FrameLocal
    //        writer. FrameLocal reads -> HardResource edge. External-lifetime reads ->
    //        ExternalNonEdge (visible, not dropped). ---
    for (int o = 0; o < orderCount; ++o) {
        const RenderPassContract* c = findContract(contracts, contractCount, order[o]);
        if (!c) continue;
        for (int r = 0; r < 4; ++r) {
            const RenderResourceId rid = c->reads[r];
            if (rid == RenderResourceId::Unknown) break;

            if (isExternalLifetime(rid)) {
                // Lifetime-excluded read: emit a VISIBLE non-edge so the exclusion shows
                // up in the report rather than silently vanishing.
                PassEdge e;
                e.from = RenderPassId::None;
                e.to   = order[o];
                e.cls  = EdgeClass::ExternalNonEdge;
                e.via  = rid;
                e.forbidden    = false;
                e.contentGated = false;
                e.note = "read excluded: lifetime Mission/Persistent/External (no in-frame edge)";
                out[count++] = e;
                continue;
            }

            // FrameLocal read: find the last earlier writer of rid in the order.
            RenderPassId producer = RenderPassId::None;
            for (int p = o - 1; p >= 0; --p) {
                const RenderPassContract* pc = findContract(contracts, contractCount, order[p]);
                if (!pc) continue;
                bool writes = false;
                for (int w = 0; w < 4; ++w) {
                    if (pc->writes[w] == RenderResourceId::Unknown) break;
                    if (pc->writes[w] == rid) { writes = true; break; }
                }
                if (writes) { producer = order[p]; break; }
            }
            if (producer == RenderPassId::None) continue; // no in-frame producer (e.g. pre-seeded)

            PassEdge e;
            e.from = producer;
            e.to   = order[o];
            e.cls  = EdgeClass::HardResource;
            e.via  = rid;
            e.forbidden    = true;    // a real producer->consumer data edge must be preserved
            e.contentGated = false;
            e.note = "FrameLocal producer->consumer (last in-frame writer of the read)";
            out[count++] = e;
        }
    }

    // --- 2. SoftState (colorMask reassert) + LegacyLatch (sceneHasTerrain_) from the
    //        ambient contract. The producer is the pass that REASSERTS/PRODUCES; the
    //        consumer is each pass that depends on it. We model the canonical pairs:
    //        terrain-reassert -> PostProcess, terrain-latch -> PostProcess. ---
    {
        const AmbientContract* terrain = findAmbient(RenderPassId::Terrain);
        const AmbientContract* post    = findAmbient(RenderPassId::PostProcess);
        if (terrain && post) {
            if (terrain->reassertsColorMaskAllOn) {
                PassEdge e;
                e.from = RenderPassId::Terrain;
                e.to   = RenderPassId::PostProcess;
                e.cls  = EdgeClass::SoftState;
                e.via  = RenderResourceId::Unknown;
                e.forbidden    = true;
                e.contentGated = false;
                e.note = "ambient: terrain re-asserts glColorMask(TRUE) for following passes";
                out[count++] = e;
            }
            if (terrain->producesTerrainLatch && post->consumesTerrainLatch) {
                PassEdge e;
                e.from = RenderPassId::Terrain;
                e.to   = RenderPassId::PostProcess;
                e.cls  = EdgeClass::LegacyLatch;
                e.via  = RenderResourceId::Unknown;
                e.forbidden    = true;
                e.contentGated = false;
                e.note = "ambient: sceneHasTerrain_ latch (terrain sets, post consumes)";
                out[count++] = e;
            }
        }
    }

    // --- 3. KnownEarly suppression: the LOD-chunk terrain branch draws at the Gamecam
    //        site (pre-renderLists), legitimately BEFORE its kFramePassOrder slot. This is
    //        a suppression rule, NOT a violation — emit it as a KnownEarly non-forbidden
    //        marker so isCurrentOrderLegal() (and the slice-2 oracle) knows to suppress an
    //        apparent out-of-order for Terrain. ---
    {
        const TerrainSubPass* lod = findTerrainSubPass(TerrainPath::LODChunk);
        if (lod && lod->drawSite == TerrainDrawSite::Gamecam) {
            PassEdge e;
            e.from = RenderPassId::None;
            e.to   = RenderPassId::Terrain;
            e.cls  = EdgeClass::KnownEarly;
            e.via  = RenderResourceId::Unknown;
            e.forbidden    = false;   // suppression, not a constraint
            e.contentGated = false;
            e.note = "LOD-chunk terrain draws at Gamecam site (pre-renderLists); apparent-early is suppressed";
            out[count++] = e;
        }
    }

    // --- 4. Fold in the three hand tables (belt-and-suspenders + content + deferred). ---
    for (int i = 0; i < kForbiddenReorderEdgeCount; ++i)    out[count++] = kForbiddenReorderEdges[i];
    for (int i = 0; i < kContentConditionalEdgeCount; ++i)  out[count++] = kContentConditionalEdges[i];
    for (int i = 0; i < kDeferredSoftStateEdgeCount; ++i)   out[count++] = kDeferredSoftStateEdges[i];
}

// Convenience: classify the SHIPPED top-level table against the shipped order.
inline void classifyEdges(PassEdge* out, int& count) {
    classifyEdgesFor(kRenderPassContracts, kRenderPassIdCount,
                     kFramePassOrder, kFramePassOrderCount, out, count);
}

// ---------------------------------------------------------------------------
// isCurrentOrderLegal — the no-op identity baseline. Asserts kFramePassOrder satisfies
// every `forbidden` edge (pos(from) < pos(to)), honoring KnownEarly suppression. Returns
// true for the shipped order, mirroring validateShippedFrameGraph(). The arbitrary-
// permutation oracle isReorderLegal() is SLICE 2 — deliberately NOT here.
// ---------------------------------------------------------------------------
inline int orderIndexOf(RenderPassId id, const RenderPassId* order, int orderCount) {
    for (int i = 0; i < orderCount; ++i)
        if (order[i] == id) return i;
    return -1;
}

inline bool isCurrentOrderLegalFor(const RenderPassContract* contracts, int contractCount,
                                   const RenderPassId* order, int orderCount) {
    PassEdge edges[/* generous */ 256];
    int n = 0;
    classifyEdgesFor(contracts, contractCount, order, orderCount, edges, n);

    // Build the KnownEarly suppression set (passes whose apparent-early is allowed).
    bool knownEarly[static_cast<int>(RenderPassId::_SentinelLast)] = { false };
    for (int i = 0; i < n; ++i)
        if (edges[i].cls == EdgeClass::KnownEarly && edges[i].to != RenderPassId::None)
            knownEarly[static_cast<int>(edges[i].to)] = true;

    for (int i = 0; i < n; ++i) {
        const PassEdge& e = edges[i];
        if (!e.forbidden) continue;
        if (e.from == RenderPassId::None || e.to == RenderPassId::None) continue;
        const int pf = orderIndexOf(e.from, order, orderCount);
        const int pt = orderIndexOf(e.to,   order, orderCount);
        if (pf < 0 || pt < 0) continue;   // edge references a pass not in this order
        if (pf < pt) continue;            // satisfied: producer precedes consumer
        // Apparent violation. Suppress iff the CONSUMER has a known-early draw-site
        // (e.g. Terrain LOD-chunk firing before a pass it is listed after).
        if (knownEarly[static_cast<int>(e.to)]) continue;
        return false;
    }
    return true;
}

// Convenience: identity baseline over the shipped table + order.
inline bool isCurrentOrderLegal() {
    return isCurrentOrderLegalFor(kRenderPassContracts, kRenderPassIdCount,
                                  kFramePassOrder, kFramePassOrderCount);
}

}} // namespace RenderCore::framegraph
