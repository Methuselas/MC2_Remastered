#pragma once
// VULKAN-LAYOUT-CHAIN-VALIDATOR-2 — GL-free, OFFLINE image-layout-chain contract + validators.
//
// This is the FIRST image-layout-chain validator (slice 2 of the Vulkan-descriptor
// substrate: slice 1 = descriptor-CLASS coverage in vulkan_contract.h; slice 2 = this,
// the per-sampled-image LAYOUT chain). For every FBO-backed image that vulkan_contract.h
// classifies as a CombinedImageSampler, it records the Vulkan image LAYOUT that image
// must be in when it is sampled downstream (VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL /
// DEPTH_STENCIL_READ_ONLY / DEPTH_READ_ONLY / GENERAL), plus the producing pass and the
// first consuming pass (both verified against kRenderPassContracts reads[]/writes[] and
// kFramePassOrder). Two constexpr validators then prove:
//
//   I3 (layout completeness) — every pass READ that is a CombinedImageSampler (per
//        kVkResourceContracts) has a layout-chain row whose sampledAsLayout != Undefined.
//   I4 (producer presence)   — every such sampled read is either WRITTEN by an upstream
//        pass in kFramePassOrder, OR its lifetime (staticResourceLifetime) is
//        Mission/Persistent/External (seeded/persistent/temporal — no in-frame producer
//        needed). A sampled image that is neither is a genuine modeling gap.
//
// ★ PROOF-ONLY. SCHEMA + VALIDATION, NOT authoritative, NOT wired into the renderer.
//   ZERO runtime callers; included only by the offline doctest suite
//   (tests/unit/test_vulkan_layout_chain.cpp). Byte-identical by construction — exactly
//   the discipline of vulkan_contract.h / scheduler_legal_reorder.h.
//
// Firewall: header-only, no GL includes, no game-side includes. Includes ONLY the GL-free
// substrate headers (registry enum, pass contract, descriptor-class contract, static
// lifetime map).

#include <cstdint>
#include "RenderResourceRegistry.h"       // RenderResourceId, RenderResourceKind, RenderResourceLifetime
#include "RenderPassContract.h"           // RenderPassId, kRenderPassContracts, kFramePassOrder
#include "vulkan_contract.h"              // kVkResourceContracts, VkDescriptorClass
#include "scheduler_legal_reorder.h"      // framegraph::staticResourceLifetime, isExternalLifetime

namespace RenderCore { namespace vulkan {

// ---------------------------------------------------------------------------
// Vulkan image layout a sampled image must be in when read downstream.
// (Subset of VkImageLayout relevant to the sampled-read side of the chain; the
// attachment-write layouts already live in RenderPassContract.h's ImageLayout.)
// ---------------------------------------------------------------------------
enum class VkImageLayoutRead : uint8_t {
    Undefined = 0,           // VK_IMAGE_LAYOUT_UNDEFINED — sentinel; a row at this is a bug (I3)
    ShaderReadOnly,          // VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL (color sampled)
    DepthStencilReadOnly,    // VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL (compare sampler)
    DepthReadOnly,           // VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL (plain depth sample)
    General,                 // VK_IMAGE_LAYOUT_GENERAL (compute/transfer copy target sampled)
};

inline const char* vkImageLayoutReadName(VkImageLayoutRead l) {
    switch (l) {
        case VkImageLayoutRead::Undefined:            return "Undefined";
        case VkImageLayoutRead::ShaderReadOnly:       return "ShaderReadOnly";
        case VkImageLayoutRead::DepthStencilReadOnly: return "DepthStencilReadOnly";
        case VkImageLayoutRead::DepthReadOnly:        return "DepthReadOnly";
        case VkImageLayoutRead::General:              return "General";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// A single sampled-image layout-chain row. HAND-DECLARED, reviewed at change time.
//   id                — the CombinedImageSampler resource (must be a sampler row in
//                       kVkResourceContracts).
//   sampledAsLayout   — layout the image must be in when a downstream pass samples it.
//   producerPass      — first pass in kFramePassOrder that WRITES id (RenderPassId::None
//                       iff the resource is seeded/persistent and has no in-frame writer).
//   firstConsumerPass — first pass in kFramePassOrder that READS id.
//   note              — human-readable derivation.
// ---------------------------------------------------------------------------
struct VkImageLayoutChain {
    RenderResourceId id;
    VkImageLayoutRead sampledAsLayout;
    RenderPassId     producerPass;
    RenderPassId     firstConsumerPass;
    const char*      note;
};

// ---------------------------------------------------------------------------
// kVkImageLayoutChains[] — one row per CombinedImageSampler image in
// kVkResourceContracts whose producer/consumer passes are verifiable from
// kRenderPassContracts reads[]/writes[]. There are exactly 4 sampler rows in
// vulkan_contract.h: MainColor, MainDepth, MainNormal, ShadowDynamicMap.
//
// Producer/firstConsumer derived from kFramePassOrder against the shipped
// reads[]/writes[]:
//   ShadowDynamicMap: written by Shadow (order[0]); first read by StaticPropOpaque.
//   MainColor:        written by StaticPropOpaque; first read by PostProcess.
//   MainDepth:        written by StaticPropOpaque; first read by Water (Water precedes
//                     VegetationCards/VFX/PostProcess in kFramePassOrder).
//   MainNormal:       written by StaticPropOpaque; first read by PostProcess.
//
// Layout derivation from resource semantics:
//   ShadowDynamicMap — depth compare-sampler (shadow PCF) -> DepthStencilReadOnly.
//   MainDepth        — plain depth sampled for soft-depth reject -> DepthReadOnly.
//   MainColor / MainNormal — color / GBuffer sampled -> ShaderReadOnly.
// ---------------------------------------------------------------------------
static constexpr VkImageLayoutChain kVkImageLayoutChains[] = {
    { RenderResourceId::ShadowDynamicMap, VkImageLayoutRead::DepthStencilReadOnly,
      RenderPassId::Shadow, RenderPassId::StaticPropOpaque,
      "dynamic shadow depth; compare-sampled by StaticProp/Terrain/Mech/PostProcess "
      "-> DEPTH_STENCIL_READ_ONLY" },
    { RenderResourceId::MainColor, VkImageLayoutRead::ShaderReadOnly,
      RenderPassId::StaticPropOpaque, RenderPassId::PostProcess,
      "scene HDR color (sceneFBO attach0); sampled by PostProcess -> SHADER_READ_ONLY" },
    { RenderResourceId::MainDepth, VkImageLayoutRead::DepthReadOnly,
      RenderPassId::StaticPropOpaque, RenderPassId::Water,
      "scene depth; sampled by Water/Veg/VFX/PostProcess (soft depth) -> DEPTH_READ_ONLY" },
    { RenderResourceId::MainNormal, VkImageLayoutRead::ShaderReadOnly,
      RenderPassId::StaticPropOpaque, RenderPassId::PostProcess,
      "GBuffer1 normal (attach1); sampled by PostProcess SSAO/Shoreline/ScreenShadow "
      "-> SHADER_READ_ONLY" },
};

static constexpr int kVkImageLayoutChainCount =
    sizeof(kVkImageLayoutChains) / sizeof(kVkImageLayoutChains[0]);

// ---------------------------------------------------------------------------
// Helpers.
// ---------------------------------------------------------------------------

// Is `id` a CombinedImageSampler per the descriptor-class contract?
constexpr bool vkIsCombinedImageSampler(RenderResourceId id) {
    for (int i = 0; i < kVkResourceContractCount; ++i)
        if (kVkResourceContracts[i].id == id
            && kVkResourceContracts[i].cls == VkDescriptorClass::CombinedImageSampler)
            return true;
    return false;
}

// Find the layout-chain row for `id` (nullptr if absent).
constexpr const VkImageLayoutChain* vkFindLayoutChain(RenderResourceId id) {
    for (int i = 0; i < kVkImageLayoutChainCount; ++i)
        if (kVkImageLayoutChains[i].id == id) return &kVkImageLayoutChains[i];
    return nullptr;
}

// Does any pass BEFORE `order[consumerIdx]` in `order` write `id`?
// NOTE: inline (not constexpr) — framegraph::findContract is an inline helper, so this
// walk is a RUNTIME check proven by the doctest CHECKs, not a static_assert. The table
// well-formedness guards below remain constexpr/static_assert.
inline bool vkHasUpstreamProducer(RenderResourceId id, int consumerIdx) {
    for (int p = 0; p < consumerIdx; ++p) {
        const RenderPassContract* pc =
            framegraph::findContract(kRenderPassContracts, kRenderPassIdCount, kFramePassOrder[p]);
        if (!pc) continue;
        for (int w = 0; w < 4; ++w) {
            if (pc->writes[w] == RenderResourceId::Unknown) break;
            if (pc->writes[w] == id) return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// I3 — layout completeness. For every pass READ (in kFramePassOrder) that is a
// CombinedImageSampler, assert a layout-chain row exists with sampledAsLayout != Undefined.
// On failure, *offending (if non-null) receives the first offending resource id.
// inline (runtime) — walks the pass table via framegraph::findContract; proven by doctest.
// ---------------------------------------------------------------------------
inline bool vkLayoutChainCompleteForAllSampledImages(RenderResourceId* offending = nullptr) {
    for (int o = 0; o < kFramePassOrderCount; ++o) {
        const RenderPassContract* c =
            framegraph::findContract(kRenderPassContracts, kRenderPassIdCount, kFramePassOrder[o]);
        if (!c) continue;
        for (int r = 0; r < 4; ++r) {
            const RenderResourceId rid = c->reads[r];
            if (rid == RenderResourceId::Unknown) break;
            if (!vkIsCombinedImageSampler(rid)) continue;
            const VkImageLayoutChain* row = vkFindLayoutChain(rid);
            if (!row || row->sampledAsLayout == VkImageLayoutRead::Undefined) {
                if (offending) *offending = rid;
                return false;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// I4 — producer presence. For every sampled read, assert an upstream pass writes it OR
// its lifetime is Mission/Persistent/External (framegraph::isExternalLifetime). A sampled
// image with neither is a real modeling gap. On failure *offending receives the id.
// inline (runtime) — walks the pass table via framegraph::findContract; proven by doctest.
// ---------------------------------------------------------------------------
inline bool vkLayoutChainAllSampledImagesHaveProducers(RenderResourceId* offending = nullptr) {
    for (int o = 0; o < kFramePassOrderCount; ++o) {
        const RenderPassContract* c =
            framegraph::findContract(kRenderPassContracts, kRenderPassIdCount, kFramePassOrder[o]);
        if (!c) continue;
        for (int r = 0; r < 4; ++r) {
            const RenderResourceId rid = c->reads[r];
            if (rid == RenderResourceId::Unknown) break;
            if (!vkIsCombinedImageSampler(rid)) continue;
            if (vkHasUpstreamProducer(rid, o)) continue;
            if (framegraph::isExternalLifetime(rid)) continue;
            if (offending) *offending = rid;
            return false;
        }
    }
    return true;
}

// No duplicate ids in the layout-chain table.
constexpr bool vkLayoutChainNoDuplicateIds() {
    for (int i = 0; i < kVkImageLayoutChainCount; ++i)
        for (int j = i + 1; j < kVkImageLayoutChainCount; ++j)
            if (kVkImageLayoutChains[i].id == kVkImageLayoutChains[j].id)
                return false;
    return true;
}

// Every layout-chain row keys on a real CombinedImageSampler resource with a real layout.
constexpr bool vkLayoutChainRowsWellFormed() {
    for (int i = 0; i < kVkImageLayoutChainCount; ++i) {
        const VkImageLayoutChain& r = kVkImageLayoutChains[i];
        if (!vkIsCombinedImageSampler(r.id)) return false;
        if (r.sampledAsLayout == VkImageLayoutRead::Undefined) return false;
        if (r.firstConsumerPass == RenderPassId::None) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Compile-time guards. Table-shape invariants that touch ONLY constexpr data
// (my own table + the constexpr kVkResourceContracts scan) are static_asserts and
// fire at build time, mirroring vulkan_contract.h. I3/I4 walk the pass table via the
// inline framegraph::findContract helper, so they are RUNTIME validators proven by the
// doctest CHECKs (test_vulkan_layout_chain.cpp) rather than static_asserts.
// ---------------------------------------------------------------------------
static_assert(kVkImageLayoutChainCount == 4,
              "kVkImageLayoutChains row count changed; there are 4 CombinedImageSampler "
              "rows in vulkan_contract.h (MainColor/MainDepth/MainNormal/ShadowDynamicMap)");
static_assert(vkLayoutChainRowsWellFormed(),
              "a layout-chain row keys on a non-sampler id, has Undefined layout, or None consumer");
static_assert(vkLayoutChainNoDuplicateIds(),
              "vulkan_layout_chain: duplicate RenderResourceId in the table");

}} // namespace RenderCore::vulkan
