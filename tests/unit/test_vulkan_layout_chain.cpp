// VULKAN-LAYOUT-CHAIN-VALIDATOR-2 offline validation.
//
// Proves the per-sampled-image Vulkan layout chain (vulkan_layout_chain.h) is COMPLETE
// (I3) and every sampled image has a PRODUCER (I4). Every CombinedImageSampler read in
// the shipped pass order carries a layout-chain row with a real sampled-as layout, and is
// either produced upstream in kFramePassOrder or has a Mission/Persistent/External
// lifetime. All offline, no GL/engine/smoke. Table-shape guards are already proven at
// compile time by the header's static_asserts; these doctest cases make the
// completeness/producer invariants visible and named at run time, plus a negative check.
#include "doctest.h"
#include "RenderCore/vulkan_layout_chain.h"

using namespace RenderCore;
using namespace RenderCore::vulkan;

TEST_SUITE("VulkanLayoutChain") {

TEST_CASE("row count is pinned and every row is well-formed (matches header static_asserts)") {
    CHECK(kVkImageLayoutChainCount == 4);
    CHECK(vkLayoutChainRowsWellFormed());
    CHECK(vkLayoutChainNoDuplicateIds());
    for (int i = 0; i < kVkImageLayoutChainCount; ++i) {
        const VkImageLayoutChain& r = kVkImageLayoutChains[i];
        INFO("row ", i, " id=", static_cast<unsigned>(r.id),
             " layout=", vkImageLayoutReadName(r.sampledAsLayout));
        CHECK(vkIsCombinedImageSampler(r.id));
        CHECK(r.sampledAsLayout != VkImageLayoutRead::Undefined);
    }
}

TEST_CASE("I3: every CombinedImageSampler pass-read has a real layout-chain row") {
    RenderResourceId offending = RenderResourceId::Unknown;
    CHECK(vkLayoutChainCompleteForAllSampledImages(&offending));
    CHECK(static_cast<unsigned>(offending) == 0u);   // Unknown == untouched
}

TEST_CASE("I4: every sampled image has an upstream producer or an external lifetime") {
    RenderResourceId offending = RenderResourceId::Unknown;
    CHECK(vkLayoutChainAllSampledImagesHaveProducers(&offending));
    CHECK(static_cast<unsigned>(offending) == 0u);
}

TEST_CASE("layout derivation: shadow is a compare sampler, color is shader-read, depth is depth-read") {
    const VkImageLayoutChain* shadow = vkFindLayoutChain(RenderResourceId::ShadowDynamicMap);
    REQUIRE(shadow != nullptr);
    CHECK(shadow->sampledAsLayout == VkImageLayoutRead::DepthStencilReadOnly);
    CHECK(shadow->producerPass == RenderPassId::Shadow);

    const VkImageLayoutChain* color = vkFindLayoutChain(RenderResourceId::MainColor);
    REQUIRE(color != nullptr);
    CHECK(color->sampledAsLayout == VkImageLayoutRead::ShaderReadOnly);

    const VkImageLayoutChain* depth = vkFindLayoutChain(RenderResourceId::MainDepth);
    REQUIRE(depth != nullptr);
    CHECK(depth->sampledAsLayout == VkImageLayoutRead::DepthReadOnly);
    CHECK(depth->firstConsumerPass == RenderPassId::Water);   // Water precedes Veg/VFX/PP

    const VkImageLayoutChain* normal = vkFindLayoutChain(RenderResourceId::MainNormal);
    REQUIRE(normal != nullptr);
    CHECK(normal->sampledAsLayout == VkImageLayoutRead::ShaderReadOnly);
}

TEST_CASE("negative: a non-sampler resource has no layout-chain row") {
    // Buffers are never CombinedImageSampler, so they are absent from the layout chain.
    CHECK_FALSE(vkIsCombinedImageSampler(RenderResourceId::LightDataSsbo));
    CHECK(vkFindLayoutChain(RenderResourceId::LightDataSsbo) == nullptr);
}

} // TEST_SUITE("VulkanLayoutChain")
