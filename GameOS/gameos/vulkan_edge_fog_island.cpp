// VULKAN-EDGE-FOG-ISLAND-2a -- the first REAL Vulkan render work in the MC2 frame
// loop. Entire TU compiles to nothing unless MC2_VULKAN_ISLAND is defined (CMake
// option, requires MC2_VULKAN). Fail-soft everywhere: any Vulkan error disables the
// island for the rest of the process and the caller falls back to the GL edge-fog
// path. Never crashes -- this is the OpenGL-user-without-a-Vulkan-runtime path too.
//
// What it does (per frame, only when MC2_VULKAN_EDGE_FOG_ISLAND=1 + init OK):
//   1. glGetTexImage the scene DEPTH  (GL_DEPTH_COMPONENT/GL_FLOAT)  -> VMA staging
//   2. glGetTexImage the scene COLOR  (GL_RGBA/GL_HALF_FLOAT)        -> VMA staging
//   3. vkCmdCopyBufferToImage depth staging -> D32_SFLOAT depth image (sampled)
//   4. vkCmdCopyBufferToImage color staging -> R16G16B16A16_SFLOAT color image
//   5. upload the EdgeFogParams UBO (invViewProj + fog params, std140)
//   6. render-pass LOAD the color image, draw a fullscreen triangle with AlphaBlend
//      (SRC_ALPHA / ONE_MINUS_SRC_ALPHA, depth test/write OFF, cull none) -- exactly
//      the GL edge-fog blend; the frag samples the depth image + UBO.
//   7. vkCmdCopyImageToBuffer color image -> readback buffer, fence wait
//   8. glTexSubImage2D the result back into sceneColorTex_
//
// This is NOT pixel-parity work (that is slice 2b). The bar here is: builds, runs,
// no crash, the island path actually executes. A one-shot DEPTH-CONVENTION MICRO-
// CHECK logs the unprojected world-Z of a known NDC point computed via the same
// invViewProj math the shader uses, next to what the GL path would get, so a human
// can eyeball whether the Vulkan unprojection matches GL (catches inverted-Z bugs).

#include "gos_postprocess.h"

#ifdef MC2_VULKAN_ISLAND

// volk owns Vulkan dispatch (dynamic load; no hard link to vulkan-1.dll).
#include <volk.h>
#include "vk_mem_alloc.h"

// GL for the glGetTexImage / glTexSubImage2D bridge + the sampleable GL textures.
#include "utils/gl_utils.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

void vlog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "[VK_EDGE_FOG] ");
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    va_end(ap);
}

// Bring up volk exactly once (shared idea with the skeleton). On failure the
// island fails soft and the caller uses the GL path.
bool ensure_volk_initialized() {
    static int state = 0; // 0 untried, 1 ok, -1 failed
    if (state == 1) return true;
    if (state == -1) return false;
    VkResult r = volkInitialize();
    if (r != VK_SUCCESS) {
        vlog("volkInitialize() failed (VkResult=%d) -- no Vulkan runtime. "
             "Falling back to GL edge fog.", (int)r);
        state = -1;
        return false;
    }
    state = 1;
    return true;
}

// Row-major mat4 (the 16 floats the GL path uploads, GL_FALSE) times a vec4.
// out[r] = sum_c m[r*4 + c] * v[c].  Used only by the micro-check so it exactly
// mirrors the GL-side reference unprojection (see gos_postprocess.cpp comments:
// inverseViewProj_ is uploaded row-major direct).
struct Vec4 { double x, y, z, w; };
Vec4 mul_rowmajor(const float* m, const Vec4& v) {
    Vec4 o;
    o.x = (double)m[0]  * v.x + (double)m[1]  * v.y + (double)m[2]  * v.z + (double)m[3]  * v.w;
    o.y = (double)m[4]  * v.x + (double)m[5]  * v.y + (double)m[6]  * v.z + (double)m[7]  * v.w;
    o.z = (double)m[8]  * v.x + (double)m[9]  * v.y + (double)m[10] * v.z + (double)m[11] * v.w;
    o.w = (double)m[12] * v.x + (double)m[13] * v.y + (double)m[14] * v.z + (double)m[15] * v.w;
    return o;
}

// Column-major mat4 (how the shader's mat4 receives the SAME bytes when GLSL reads
// a std140 mat4: column-major). out[r] = sum_c m[c*4 + r] * v[c]. This is what the
// Vulkan fragment shader actually computes, so logging it lets us confirm the CPU
// upload convention matches what the shader will do.
Vec4 mul_colmajor(const float* m, const Vec4& v) {
    Vec4 o;
    o.x = (double)m[0]  * v.x + (double)m[4]  * v.y + (double)m[8]  * v.z + (double)m[12] * v.w;
    o.y = (double)m[1]  * v.x + (double)m[5]  * v.y + (double)m[9]  * v.z + (double)m[13] * v.w;
    o.z = (double)m[2]  * v.x + (double)m[6]  * v.y + (double)m[10] * v.z + (double)m[14] * v.w;
    o.w = (double)m[3]  * v.x + (double)m[7]  * v.y + (double)m[11] * v.z + (double)m[15] * v.w;
    return o;
}

// Resolve the directory holding the compiled .spv files. Default "shaders/vulkan"
// (relative to the exe cwd, same default the standalone probe uses); overridable
// via MC2_VULKAN_SPV_DIR.
std::string spv_dir() {
    const char* env = std::getenv("MC2_VULKAN_SPV_DIR");
    std::string d = (env && *env) ? env : "shaders/vulkan";
    if (!d.empty() && d.back() != '/' && d.back() != '\\') d += '/';
    return d;
}

VkShaderModule load_spv(VkDevice device, const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { vlog("cannot open spv '%s'.", path.c_str()); return VK_NULL_HANDLE; }
    std::fseek(f, 0, SEEK_END);
    long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (len <= 0 || (len % 4) != 0) {
        vlog("spv '%s' bad size %ld.", path.c_str(), len);
        std::fclose(f);
        return VK_NULL_HANDLE;
    }
    std::vector<uint32_t> code(static_cast<size_t>(len) / 4);
    size_t got = std::fread(code.data(), 1, static_cast<size_t>(len), f);
    std::fclose(f);
    if (got != static_cast<size_t>(len)) { vlog("spv short read '%s'.", path.c_str()); return VK_NULL_HANDLE; }
    VkShaderModuleCreateInfo smci{};
    smci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = static_cast<size_t>(len);
    smci.pCode    = code.data();
    VkShaderModule mod = VK_NULL_HANDLE;
    VkResult r = vkCreateShaderModule(device, &smci, nullptr, &mod);
    if (r != VK_SUCCESS) { vlog("vkCreateShaderModule('%s') failed (%d).", path.c_str(), (int)r); return VK_NULL_HANDLE; }
    return mod;
}

// std140 layout matching shaders/vulkan/edge_fog.frag EdgeFogParams. Total 100B;
// padded to 112 for the UBO alloc. Field offsets: invViewProj @0, u_fogColor @64,
// _pad0 @76, u_halfExtent @80, u_fogStart @84, u_fogHeight @88, u_fogMax @92,
// u_waterElevation @96.
struct EdgeFogParams {
    float invViewProj[16]; // @0  (same 16 floats the GL path uploads, row-major)
    float fogColor[3];     // @64
    float _pad0;           // @76
    float halfExtent;      // @80
    float fogStart;        // @84
    float fogHeight;       // @88
    float fogMax;          // @92
    float waterElevation;  // @96
};
static_assert(sizeof(EdgeFogParams) == 100, "EdgeFogParams std140 offsets drifted");

const VkFormat kDepthFmt = VK_FORMAT_D32_SFLOAT;
const VkFormat kColorFmt = VK_FORMAT_R16G16B16A16_SFLOAT;

} // namespace

// Persistent Vulkan island state. Sized lazily to (width_, height_) on first init;
// if the framebuffer resizes we tear down + rebuild.
struct gosPostProcess::VulkanEdgeFogIsland {
    bool disabled = false;   // init failed once -> never retry, always GL path
    bool inited   = false;
    bool microCheckDone = false;

    int width  = 0;
    int height = 0;

    VkInstance       instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys     = VK_NULL_HANDLE;
    VkDevice         device   = VK_NULL_HANDLE;
    uint32_t         gfxFamily = UINT32_MAX;
    VkQueue          queue    = VK_NULL_HANDLE;
    VmaAllocator     allocator = VK_NULL_HANDLE;

    // Depth-in image (sampled by the frag) + color-out image (attachment).
    VkImage       depthImage = VK_NULL_HANDLE;  VmaAllocation depthAlloc = VK_NULL_HANDLE;
    VkImageView   depthView   = VK_NULL_HANDLE;
    VkImage       colorImage = VK_NULL_HANDLE;  VmaAllocation colorAlloc = VK_NULL_HANDLE;
    VkImageView   colorView   = VK_NULL_HANDLE;
    VkSampler     sampler     = VK_NULL_HANDLE;

    // Host-visible staging/readback + param UBO.
    VkBuffer      depthStaging = VK_NULL_HANDLE; VmaAllocation depthStagingAlloc = VK_NULL_HANDLE;
    VkBuffer      colorStaging = VK_NULL_HANDLE; VmaAllocation colorStagingAlloc = VK_NULL_HANDLE; // upload scene color
    VkBuffer      colorReadback = VK_NULL_HANDLE; VmaAllocation colorReadbackAlloc = VK_NULL_HANDLE;
    VkBuffer      ubo          = VK_NULL_HANDLE;  VmaAllocation uboAlloc = VK_NULL_HANDLE;

    VkRenderPass      rpass  = VK_NULL_HANDLE;
    VkFramebuffer     fb     = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkDescriptorPool  pool   = VK_NULL_HANDLE;
    VkDescriptorSet   set    = VK_NULL_HANDLE;
    VkPipelineLayout  playout = VK_NULL_HANDLE;
    VkPipeline        pipe   = VK_NULL_HANDLE;
    VkShaderModule    vert   = VK_NULL_HANDLE;
    VkShaderModule    frag   = VK_NULL_HANDLE;

    VkCommandPool     cpool  = VK_NULL_HANDLE;
    VkCommandBuffer   cbuf   = VK_NULL_HANDLE;
    VkFence           fence  = VK_NULL_HANDLE;

    // CPU scratch for the GL<->staging bridge.
    std::vector<float>    depthScratch;  // width*height floats
    std::vector<uint16_t> colorScratch;  // width*height*4 halfs
};

namespace {

// Tear down everything the island owns (safe on partially-built state).
void destroy_island(gosPostProcess::VulkanEdgeFogIsland* s) {
    if (!s) return;
    VkDevice d = s->device;
    if (d != VK_NULL_HANDLE) vkDeviceWaitIdle(d);
    if (s->fence)  vkDestroyFence(d, s->fence, nullptr);
    if (s->cpool)  vkDestroyCommandPool(d, s->cpool, nullptr);
    if (s->pipe)   vkDestroyPipeline(d, s->pipe, nullptr);
    if (s->playout) vkDestroyPipelineLayout(d, s->playout, nullptr);
    if (s->pool)   vkDestroyDescriptorPool(d, s->pool, nullptr);
    if (s->dsl)    vkDestroyDescriptorSetLayout(d, s->dsl, nullptr);
    if (s->frag)   vkDestroyShaderModule(d, s->frag, nullptr);
    if (s->vert)   vkDestroyShaderModule(d, s->vert, nullptr);
    if (s->fb)     vkDestroyFramebuffer(d, s->fb, nullptr);
    if (s->rpass)  vkDestroyRenderPass(d, s->rpass, nullptr);
    if (s->sampler) vkDestroySampler(d, s->sampler, nullptr);
    if (s->colorView) vkDestroyImageView(d, s->colorView, nullptr);
    if (s->depthView) vkDestroyImageView(d, s->depthView, nullptr);
    if (s->allocator) {
        if (s->ubo)           vmaDestroyBuffer(s->allocator, s->ubo, s->uboAlloc);
        if (s->colorReadback) vmaDestroyBuffer(s->allocator, s->colorReadback, s->colorReadbackAlloc);
        if (s->colorStaging)  vmaDestroyBuffer(s->allocator, s->colorStaging, s->colorStagingAlloc);
        if (s->depthStaging)  vmaDestroyBuffer(s->allocator, s->depthStaging, s->depthStagingAlloc);
        if (s->colorImage)    vmaDestroyImage(s->allocator, s->colorImage, s->colorAlloc);
        if (s->depthImage)    vmaDestroyImage(s->allocator, s->depthImage, s->depthAlloc);
        vmaDestroyAllocator(s->allocator);
    }
    if (d != VK_NULL_HANDLE) vkDestroyDevice(d, nullptr);
    if (s->instance) vkDestroyInstance(s->instance, nullptr);
    // Reset all handles so a rebuild (resize) starts clean.
    gosPostProcess::VulkanEdgeFogIsland fresh;
    fresh.disabled       = s->disabled;
    fresh.microCheckDone = s->microCheckDone;
    *s = fresh;
}

// Build the whole persistent Vulkan pipeline sized to WxH. Returns false (fail-soft)
// on any error, leaving *s in a torn-down-but-not-disabled state.
bool build_island(gosPostProcess::VulkanEdgeFogIsland* s, int W, int H) {
    if (!ensure_volk_initialized()) return false;

    s->width = W; s->height = H;
    VkResult r;

    // ---- Instance (headless) ----
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "MC2 (Vulkan edge-fog island)";
    app.pEngineName = "MC2-GameOS";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    r = vkCreateInstance(&ici, nullptr, &s->instance);
    if (r != VK_SUCCESS) { vlog("vkCreateInstance failed (%d).", (int)r); return false; }
    volkLoadInstance(s->instance);

    // ---- Physical device + graphics queue family ----
    uint32_t devCount = 0;
    r = vkEnumeratePhysicalDevices(s->instance, &devCount, nullptr);
    if (r != VK_SUCCESS || devCount == 0) { vlog("no physical devices (%d,%u).", (int)r, devCount); return false; }
    std::vector<VkPhysicalDevice> devs(devCount);
    vkEnumeratePhysicalDevices(s->instance, &devCount, devs.data());
    s->phys = devs[0];

    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(s->phys, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfCount);
    if (qfCount) vkGetPhysicalDeviceQueueFamilyProperties(s->phys, &qfCount, qfs.data());
    for (uint32_t q = 0; q < qfCount; ++q) {
        if (qfs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) { s->gfxFamily = q; break; }
    }
    if (s->gfxFamily == UINT32_MAX) { vlog("no graphics queue family."); return false; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = s->gfxFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    r = vkCreateDevice(s->phys, &dci, nullptr, &s->device);
    if (r != VK_SUCCESS) { vlog("vkCreateDevice failed (%d).", (int)r); return false; }
    volkLoadDevice(s->device);
    vkGetDeviceQueue(s->device, s->gfxFamily, 0, &s->queue);

    // ---- VMA allocator (volk-imported functions) ----
    {
        VmaAllocatorCreateInfo aci{};
        aci.physicalDevice = s->phys;
        aci.device = s->device;
        aci.instance = s->instance;
        aci.vulkanApiVersion = VK_API_VERSION_1_1;
        VmaVulkanFunctions fns{};
        r = vmaImportVulkanFunctionsFromVolk(&aci, &fns);
        if (r != VK_SUCCESS) { vlog("vmaImportVulkanFunctionsFromVolk failed (%d).", (int)r); return false; }
        aci.pVulkanFunctions = &fns;
        r = vmaCreateAllocator(&aci, &s->allocator);
        if (r != VK_SUCCESS) { vlog("vmaCreateAllocator failed (%d).", (int)r); s->allocator = VK_NULL_HANDLE; return false; }
    }

    auto make_image = [&](VkFormat fmt, VkImageUsageFlags usage, VkImage* img, VmaAllocation* alloc) -> bool {
        VkImageCreateInfo ii{};
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = fmt;
        ii.extent = {(uint32_t)W, (uint32_t)H, 1};
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = usage;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        VkResult rr = vmaCreateImage(s->allocator, &ii, &aci, img, alloc, nullptr);
        if (rr != VK_SUCCESS) { vlog("vmaCreateImage failed (%d).", (int)rr); return false; }
        return true;
    };
    // Depth: sampled by the frag + transfer-dst (upload from staging).
    if (!make_image(kDepthFmt, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    &s->depthImage, &s->depthAlloc)) return false;
    // Color: attachment + transfer-dst (preload scene color) + transfer-src (readback).
    if (!make_image(kColorFmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &s->colorImage, &s->colorAlloc)) return false;

    auto make_view = [&](VkImage img, VkFormat fmt, VkImageAspectFlags aspect, VkImageView* view) -> bool {
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = fmt;
        vi.subresourceRange = {aspect, 0, 1, 0, 1};
        VkResult rr = vkCreateImageView(s->device, &vi, nullptr, view);
        if (rr != VK_SUCCESS) { vlog("vkCreateImageView failed (%d).", (int)rr); return false; }
        return true;
    };
    if (!make_view(s->depthImage, kDepthFmt, VK_IMAGE_ASPECT_DEPTH_BIT, &s->depthView)) return false;
    if (!make_view(s->colorImage, kColorFmt, VK_IMAGE_ASPECT_COLOR_BIT, &s->colorView)) return false;

    // ---- Sampler for the depth texture (nearest/clamp -- depth read, no filtering) ----
    {
        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = VK_FILTER_NEAREST;
        si.minFilter = VK_FILTER_NEAREST;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        r = vkCreateSampler(s->device, &si, nullptr, &s->sampler);
        if (r != VK_SUCCESS) { vlog("vkCreateSampler failed (%d).", (int)r); return false; }
    }

    // ---- Host-visible staging/readback + UBO ----
    auto make_buffer = [&](VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer* buf, VmaAllocation* alloc) -> bool {
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = size;
        bci.usage = usage;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
        VkResult rr = vmaCreateBuffer(s->allocator, &bci, &aci, buf, alloc, nullptr);
        if (rr != VK_SUCCESS) { vlog("vmaCreateBuffer failed (%d).", (int)rr); return false; }
        return true;
    };
    const VkDeviceSize depthBytes = (VkDeviceSize)W * H * 4;      // R32 float
    const VkDeviceSize colorBytes = (VkDeviceSize)W * H * 4 * 2;  // RGBA16F
    if (!make_buffer(depthBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &s->depthStaging, &s->depthStagingAlloc)) return false;
    if (!make_buffer(colorBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &s->colorStaging, &s->colorStagingAlloc)) return false;
    if (!make_buffer(colorBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &s->colorReadback, &s->colorReadbackAlloc)) return false;
    if (!make_buffer(sizeof(EdgeFogParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &s->ubo, &s->uboAlloc)) return false;

    // ---- Render pass: 1 color attachment, LOAD (keep preloaded scene color) -> STORE ----
    {
        VkAttachmentDescription att{};
        att.format = kColorFmt;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;   // fog blends OVER the preloaded scene color
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; // barrier'd there before the pass
        att.finalLayout   = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;     // ready to copy back
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ref;
        VkRenderPassCreateInfo rpci{};
        rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpci.attachmentCount = 1;
        rpci.pAttachments = &att;
        rpci.subpassCount = 1;
        rpci.pSubpasses = &sub;
        r = vkCreateRenderPass(s->device, &rpci, nullptr, &s->rpass);
        if (r != VK_SUCCESS) { vlog("vkCreateRenderPass failed (%d).", (int)r); return false; }

        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = s->rpass;
        fci.attachmentCount = 1;
        fci.pAttachments = &s->colorView;
        fci.width = W; fci.height = H; fci.layers = 1;
        r = vkCreateFramebuffer(s->device, &fci, nullptr, &s->fb);
        if (r != VK_SUCCESS) { vlog("vkCreateFramebuffer failed (%d).", (int)r); return false; }
    }

    // ---- Descriptor set layout: binding0 = combined image sampler, binding1 = UBO ----
    {
        VkDescriptorSetLayoutBinding binds[2]{};
        binds[0].binding = 0;
        binds[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binds[0].descriptorCount = 1;
        binds[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        binds[1].binding = 1;
        binds[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        binds[1].descriptorCount = 1;
        binds[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = 2;
        lci.pBindings = binds;
        r = vkCreateDescriptorSetLayout(s->device, &lci, nullptr, &s->dsl);
        if (r != VK_SUCCESS) { vlog("vkCreateDescriptorSetLayout failed (%d).", (int)r); return false; }

        VkDescriptorPoolSize sizes[2]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sizes[0].descriptorCount = 1;
        sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;         sizes[1].descriptorCount = 1;
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = 1;
        pci.poolSizeCount = 2;
        pci.pPoolSizes = sizes;
        r = vkCreateDescriptorPool(s->device, &pci, nullptr, &s->pool);
        if (r != VK_SUCCESS) { vlog("vkCreateDescriptorPool failed (%d).", (int)r); return false; }

        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = s->pool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &s->dsl;
        r = vkAllocateDescriptorSets(s->device, &dsai, &s->set);
        if (r != VK_SUCCESS) { vlog("vkAllocateDescriptorSets failed (%d).", (int)r); return false; }

        // Bind depth image + UBO once (images/buffers are persistent).
        VkDescriptorImageInfo imgInfo{};
        imgInfo.sampler = s->sampler;
        imgInfo.imageView = s->depthView;
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorBufferInfo uboInfo{ s->ubo, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = s->set; writes[0].dstBinding = 0; writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &imgInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = s->set; writes[1].dstBinding = 1; writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[1].pBufferInfo = &uboInfo;
        vkUpdateDescriptorSets(s->device, 2, writes, 0, nullptr);
    }

    // ---- Pipeline (fullscreen tri; AlphaBlend; depth off; cull none) ----
    {
        s->vert = load_spv(s->device, spv_dir() + "edge_fog.vert.spv");
        s->frag = load_spv(s->device, spv_dir() + "edge_fog.frag.spv");
        if (s->vert == VK_NULL_HANDLE || s->frag == VK_NULL_HANDLE) return false;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = s->vert; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = s->frag; stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vin{};
        vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport vp{0.0f, 0.0f, (float)W, (float)H, 0.0f, 1.0f};
        VkRect2D sc{{0, 0}, {(uint32_t)W, (uint32_t)H}};
        VkPipelineViewportStateCreateInfo vps{};
        vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &sc;
        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        // AlphaBlend: SRC_ALPHA / ONE_MINUS_SRC_ALPHA (matches the GL edge-fog blend).
        VkPipelineColorBlendAttachmentState cba{};
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1; cb.pAttachments = &cba;

        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1; plci.pSetLayouts = &s->dsl;
        r = vkCreatePipelineLayout(s->device, &plci, nullptr, &s->playout);
        if (r != VK_SUCCESS) { vlog("vkCreatePipelineLayout failed (%d).", (int)r); return false; }

        VkGraphicsPipelineCreateInfo gp{};
        gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gp.stageCount = 2; gp.pStages = stages;
        gp.pVertexInputState = &vin;
        gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vps;
        gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms;
        gp.pColorBlendState = &cb;
        gp.layout = s->playout;
        gp.renderPass = s->rpass;
        gp.subpass = 0;
        r = vkCreateGraphicsPipelines(s->device, VK_NULL_HANDLE, 1, &gp, nullptr, &s->pipe);
        if (r != VK_SUCCESS) { vlog("vkCreateGraphicsPipelines failed (%d).", (int)r); return false; }
    }

    // ---- Command pool + buffer + fence (reused each frame) ----
    {
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = s->gfxFamily;
        r = vkCreateCommandPool(s->device, &pci, nullptr, &s->cpool);
        if (r != VK_SUCCESS) { vlog("vkCreateCommandPool failed (%d).", (int)r); return false; }
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = s->cpool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        r = vkAllocateCommandBuffers(s->device, &cai, &s->cbuf);
        if (r != VK_SUCCESS) { vlog("vkAllocateCommandBuffers failed (%d).", (int)r); return false; }
        VkFenceCreateInfo fnci{};
        fnci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        r = vkCreateFence(s->device, &fnci, nullptr, &s->fence);
        if (r != VK_SUCCESS) { vlog("vkCreateFence failed (%d).", (int)r); return false; }
    }

    s->depthScratch.resize((size_t)W * H);
    s->colorScratch.resize((size_t)W * H * 4);
    s->inited = true;
    vlog("island initialized: %dx%d device ready (depth D32_SFLOAT, color RGBA16F).", W, H);
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// vulkanEdgeFogIslandEnabled(): env gate + lazy init. Returns true only when the
// island should run this frame (env=1, not disabled, init OK, size matches).
// ---------------------------------------------------------------------------
bool gosPostProcess::vulkanEdgeFogIslandEnabled()
{
    // Env resolved once (matches the other MC2_* one-shot gates).
    static int envState = -2; // -2 untested
    if (envState == -2) {
        const char* e = std::getenv("MC2_VULKAN_EDGE_FOG_ISLAND");
        envState = (e && (e[0] == '1')) ? 1 : 0;
        if (envState == 1) vlog("MC2_VULKAN_EDGE_FOG_ISLAND=1 -- island gate ON (lazy init on first edge-fog).");
    }
    if (envState != 1) return false;

    if (width_ <= 0 || height_ <= 0 || sceneColorTex_ == 0 || sceneDepthTex_ == 0) return false;

    if (!vkFogIsland_) vkFogIsland_ = new VulkanEdgeFogIsland();
    VulkanEdgeFogIsland* s = vkFogIsland_;
    if (s->disabled) return false;

    // (Re)build on first use or on a framebuffer resize.
    if (!s->inited || s->width != width_ || s->height != height_) {
        if (s->inited) destroy_island(s);   // resize -> rebuild
        if (!build_island(s, width_, height_)) {
            vlog("island init FAILED -- disabling, falling back to GL edge fog for the rest of the run.");
            destroy_island(s);
            s->disabled = true;
            return false;
        }
    }
    return s->inited && !s->disabled;
}

// ---------------------------------------------------------------------------
// runEdgeFogVulkan(): per-frame GL->Vulkan->GL edge-fog composite. Assumes
// vulkanEdgeFogIslandEnabled() returned true (island built + sized).
// ---------------------------------------------------------------------------
void gosPostProcess::runEdgeFogVulkan()
{
    VulkanEdgeFogIsland* s = vkFogIsland_;
    if (!s || !s->inited || s->disabled) return;

    // Same gameplay gates the GL runEdgeFog() applies.
    if (!edgeFogEnabled_) return;
    if (mapHalfExtent_ <= 0.0f) return;
    if (!sceneHasTerrain_) return;

    const int W = s->width, H = s->height;

    // ---- 1) Read GL scene depth + color into staging (via CPU scratch) ----
    // Depth: normalized [0,1] float, same value the GL depth sampler .r returns.
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sceneDepthTex_);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, s->depthScratch.data());
    glBindTexture(GL_TEXTURE_2D, sceneColorTex_);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_HALF_FLOAT, s->colorScratch.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    // ---- DEPTH-CONVENTION MICRO-CHECK (one-shot) ----
    // Unproject a known NDC point at a known depth via the SAME invViewProj math the
    // shader does, and log it next to the row-major (GL upload convention) reference.
    // If the Vulkan (column-major, as GLSL reads std140) result matches the GL
    // (row-major direct upload) result, the depth/matrix convention is consistent.
    if (!s->microCheckDone) {
        s->microCheckDone = true;
        const float* m = inverseViewProj_;
        // Screen center, mid depth in reverse-Z [0,1].
        Vec4 ndc{0.0, 0.0, 0.5, 1.0};
        Vec4 glW  = mul_rowmajor(m, ndc);  // what the GL path (GL_FALSE row-major upload) yields
        Vec4 vkW  = mul_colmajor(m, ndc);  // what the Vulkan frag (std140 column-major) computes
        double glZ = glW.w != 0.0 ? glW.z / glW.w : 0.0;
        double vkZ = vkW.w != 0.0 ? vkW.z / vkW.w : 0.0;
        vlog("MICRO-CHECK depth-convention @ ndc(0,0,0.5): GL-world-Z=%.4f  VK-world-Z=%.4f  "
             "(match=%s) -- if these diverge wildly the invViewProj/Z convention is inverted.",
             glZ, vkZ, (glZ * vkZ > 0.0 && (glZ==0.0 || (vkZ/glZ > 0.5 && vkZ/glZ < 2.0))) ? "yes" : "NO");
    }

    // Copy scratch into the mapped staging buffers.
    {
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, s->depthStagingAlloc, &p) == VK_SUCCESS) {
            std::memcpy(p, s->depthScratch.data(), (size_t)W * H * 4);
            vmaUnmapMemory(s->allocator, s->depthStagingAlloc);
        }
        if (vmaMapMemory(s->allocator, s->colorStagingAlloc, &p) == VK_SUCCESS) {
            std::memcpy(p, s->colorScratch.data(), (size_t)W * H * 4 * 2);
            vmaUnmapMemory(s->allocator, s->colorStagingAlloc);
        }
    }

    // ---- 2) Upload the param UBO ----
    {
        EdgeFogParams params{};
        std::memcpy(params.invViewProj, inverseViewProj_, 16 * sizeof(float));
        params.fogColor[0] = edgeFogColor_[0];
        params.fogColor[1] = edgeFogColor_[1];
        params.fogColor[2] = edgeFogColor_[2];
        params._pad0 = 0.0f;
        params.halfExtent = mapHalfExtent_;
        params.fogStart = edgeFogStart_;
        params.fogHeight = edgeFogHeight_;
        params.fogMax = edgeFogMax_;
        params.waterElevation = waterElevation_;
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, s->uboAlloc, &p) == VK_SUCCESS) {
            std::memcpy(p, &params, sizeof(params));
            vmaUnmapMemory(s->allocator, s->uboAlloc);
        }
    }

    // ---- 3) Record + submit: upload images, render pass, readback ----
    vkResetCommandBuffer(s->cbuf, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(s->cbuf, &bi) != VK_SUCCESS) return;

    auto barrier = [&](VkImage img, VkImageAspectFlags aspect, VkImageLayout oldL, VkImageLayout newL,
                       VkAccessFlags srcA, VkAccessFlags dstA, VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = oldL; b.newLayout = newL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img;
        b.subresourceRange = {aspect, 0, 1, 0, 1};
        b.srcAccessMask = srcA; b.dstAccessMask = dstA;
        vkCmdPipelineBarrier(s->cbuf, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
    };

    VkBufferImageCopy depthRegion{};
    depthRegion.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    depthRegion.imageExtent = {(uint32_t)W, (uint32_t)H, 1};
    VkBufferImageCopy colorRegion{};
    colorRegion.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    colorRegion.imageExtent = {(uint32_t)W, (uint32_t)H, 1};

    // depth: UNDEFINED -> TRANSFER_DST, copy, -> SHADER_READ_ONLY
    barrier(s->depthImage, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdCopyBufferToImage(s->cbuf, s->depthStaging, s->depthImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &depthRegion);
    barrier(s->depthImage, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    // color: UNDEFINED -> TRANSFER_DST, preload scene color, -> COLOR_ATTACHMENT
    barrier(s->colorImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdCopyBufferToImage(s->cbuf, s->colorStaging, s->colorImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &colorRegion);
    barrier(s->colorImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    // render pass: LOAD preloaded color, blend fog over it, STORE. finalLayout=TRANSFER_SRC.
    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = s->rpass;
    rbi.framebuffer = s->fb;
    rbi.renderArea = {{0, 0}, {(uint32_t)W, (uint32_t)H}};
    rbi.clearValueCount = 0; // LOAD, no clear
    vkCmdBeginRenderPass(s->cbuf, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(s->cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe);
    vkCmdBindDescriptorSets(s->cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout, 0, 1, &s->set, 0, nullptr);
    vkCmdDraw(s->cbuf, 3, 1, 0, 0);
    vkCmdEndRenderPass(s->cbuf);

    // color image is now TRANSFER_SRC -> copy to readback buffer.
    vkCmdCopyImageToBuffer(s->cbuf, s->colorImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->colorReadback, 1, &colorRegion);

    if (vkEndCommandBuffer(s->cbuf) != VK_SUCCESS) return;

    vkResetFences(s->device, 1, &s->fence);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s->cbuf;
    if (vkQueueSubmit(s->queue, 1, &si, s->fence) != VK_SUCCESS) return;
    if (vkWaitForFences(s->device, 1, &s->fence, VK_TRUE, 5ull * 1000 * 1000 * 1000) != VK_SUCCESS) {
        vlog("fence wait failed/timeout -- disabling island, GL fallback next frame.");
        s->disabled = true;
        return;
    }

    // ---- 4) Read back the blended color and write it into sceneColorTex_ ----
    {
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, s->colorReadbackAlloc, &p) == VK_SUCCESS) {
            std::memcpy(s->colorScratch.data(), p, (size_t)W * H * 4 * 2);
            vmaUnmapMemory(s->allocator, s->colorReadbackAlloc);
            glBindTexture(GL_TEXTURE_2D, sceneColorTex_);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_HALF_FLOAT, s->colorScratch.data());
            glBindTexture(GL_TEXTURE_2D, 0);
        }
    }
    glActiveTexture(GL_TEXTURE0);
}

// ---------------------------------------------------------------------------
// destroyVulkanEdgeFogIsland(): called from gosPostProcess::destroy().
// ---------------------------------------------------------------------------
void gosPostProcess::destroyVulkanEdgeFogIsland()
{
    if (!vkFogIsland_) return;
    destroy_island(vkFogIsland_);
    delete vkFogIsland_;
    vkFogIsland_ = nullptr;
}

#endif // MC2_VULKAN_ISLAND
