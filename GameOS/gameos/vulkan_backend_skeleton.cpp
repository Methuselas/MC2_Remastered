// VULKAN-BACKEND-SKELETON-1 -- first Vulkan bootstrap slice.
// Entire TU compiles to nothing unless MC2_VULKAN is defined (CMake option,
// default OFF). Fail-soft everywhere: any error logs a reason + returns false.
// NO swapchain, NO surface, NO logical device, NO window, NO render path.

#include "vulkan_backend_skeleton.h"

#ifdef MC2_VULKAN

// VULKAN-VOLK-LOADER-1: volk owns Vulkan dispatch (dynamic load; no hard link to
// vulkan-1.dll). volk.h defines VK_NO_PROTOTYPES and includes the Vulkan headers.
#include <volk.h>

// VULKAN-VMA-INTEGRATION-1: AMD Vulkan Memory Allocator (header-only). The
// single VMA_IMPLEMENTATION TU is vma_impl.cpp; here we only pull declarations.
#include "vk_mem_alloc.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

const char* device_type_str(VkPhysicalDeviceType t) {
    switch (t) {
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated-gpu";
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "discrete-gpu";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "virtual-gpu";
        case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "cpu";
        default:                                     return "other";
    }
}

void log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "[VULKAN_SKELETON] ");
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    va_end(ap);
}

// VULKAN-VOLK-LOADER-1: bring up the volk meta-loader exactly once. This is the
// single place the Vulkan runtime is opened; volkInitialize() dynamically loads
// vulkan-1.dll (via LoadLibrary) and resolves vkGetInstanceProcAddr -- there is
// no static link to the loader. If it fails (no/broken Vulkan runtime -- the
// OpenGL-user path) we log + return false and every probe fails soft, so the
// exe still runs. After a VkInstance is created, callers must volkLoadInstance()
// (and volkLoadDevice() after a VkDevice) to resolve the rest of the dispatch.
bool ensure_volk_initialized() {
    static int state = 0; // 0 = untried, 1 = ok, -1 = failed
    if (state == 1) return true;
    if (state == -1) return false;
    VkResult r = volkInitialize();
    if (r != VK_SUCCESS) {
        log("volkInitialize() failed (VkResult=%d) -- no Vulkan runtime/loader. "
            "This is the expected OpenGL-only path; Vulkan probes fail soft.", (int)r);
        state = -1;
        return false;
    }
    state = 1;
    return true;
}

} // namespace

// ============================================================================
// VULKAN-SHADER-TOOLCHAIN-1: SPIR-V shader-module load/free + shader probe.
// ============================================================================

VkShaderModule mc2_vulkan_load_spv(VkDevice device, const char* path) {
    if (device == VK_NULL_HANDLE || !path) {
        log("load_spv: null device/path. fail-soft.");
        return VK_NULL_HANDLE;
    }
    std::FILE* f = std::fopen(path, "rb");
    if (!f) {
        log("load_spv: cannot open '%s'. fail-soft.", path);
        return VK_NULL_HANDLE;
    }
    std::fseek(f, 0, SEEK_END);
    long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (len <= 0 || (len % 4) != 0) {
        log("load_spv: '%s' bad size %ld (must be >0 and 4-byte multiple). fail-soft.", path, len);
        std::fclose(f);
        return VK_NULL_HANDLE;
    }
    std::vector<uint32_t> code(static_cast<size_t>(len) / 4);
    size_t got = std::fread(code.data(), 1, static_cast<size_t>(len), f);
    std::fclose(f);
    if (got != static_cast<size_t>(len)) {
        log("load_spv: short read on '%s' (%zu/%ld). fail-soft.", path, got, len);
        return VK_NULL_HANDLE;
    }

    VkShaderModuleCreateInfo smci{};
    smci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = static_cast<size_t>(len);
    smci.pCode    = code.data();

    VkShaderModule mod = VK_NULL_HANDLE;
    VkResult r = vkCreateShaderModule(device, &smci, nullptr, &mod);
    if (r != VK_SUCCESS) {
        log("load_spv: vkCreateShaderModule('%s') failed (VkResult=%d). fail-soft.", path, (int)r);
        return VK_NULL_HANDLE;
    }
    log("load_spv: '%s' -> VkShaderModule (%ld bytes)", path, len);
    return mod;
}

void mc2_vulkan_free_shader(VkDevice device, VkShaderModule module) {
    if (device != VK_NULL_HANDLE && module != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, module, nullptr);
    }
}

bool mc2_vulkan_probe_shaders(const char* spvDir) {
    // VULKAN-VOLK-LOADER-1: open the Vulkan runtime via volk before any vk call.
    if (!ensure_volk_initialized()) return false;

    // ---- VkInstance (headless; no surface/swapchain) ------------------------
    VkApplicationInfo app{};
    app.sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "MC2 (Vulkan shader-toolchain probe)";
    app.pEngineName      = "MC2-GameOS";
    app.apiVersion       = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ici{};
    ici.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;

    VkInstance instance = VK_NULL_HANDLE;
    VkResult r = vkCreateInstance(&ici, nullptr, &instance);
    if (r != VK_SUCCESS) {
        log("shader-probe: vkCreateInstance failed (VkResult=%d). fail-soft.", (int)r);
        return false;
    }
    // VULKAN-VOLK-LOADER-1: resolve instance-level dispatch through volk.
    volkLoadInstance(instance);

    // ---- Pick first physical device -----------------------------------------
    uint32_t devCount = 0;
    r = vkEnumeratePhysicalDevices(instance, &devCount, nullptr);
    if (r != VK_SUCCESS || devCount == 0) {
        log("shader-probe: no physical devices (VkResult=%d, count=%u). fail-soft.", (int)r, devCount);
        vkDestroyInstance(instance, nullptr);
        return false;
    }
    std::vector<VkPhysicalDevice> devs(devCount);
    vkEnumeratePhysicalDevices(instance, &devCount, devs.data());
    VkPhysicalDevice phys = devs[0];

    // ---- Find any queue family (device creation needs >=1 queue) ------------
    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfCount, nullptr);
    if (qfCount == 0) {
        log("shader-probe: no queue families. fail-soft.");
        vkDestroyInstance(instance, nullptr);
        return false;
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = 0;
    qci.queueCount       = 1;
    qci.pQueuePriorities = &prio;

    VkDeviceCreateInfo dci{};
    dci.sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos    = &qci;

    VkDevice device = VK_NULL_HANDLE;
    r = vkCreateDevice(phys, &dci, nullptr, &device);
    if (r != VK_SUCCESS) {
        log("shader-probe: vkCreateDevice failed (VkResult=%d). fail-soft.", (int)r);
        vkDestroyInstance(instance, nullptr);
        return false;
    }
    // VULKAN-VOLK-LOADER-1: resolve device-level dispatch through volk.
    volkLoadDevice(device);

    // ---- Load the compiled fullscreen .spv modules --------------------------
    std::string dir = spvDir ? spvDir : ".";
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += '/';
    std::string vertPath = dir + "fullscreen.vert.spv";
    std::string fragPath = dir + "fullscreen.frag.spv";

    VkShaderModule vert = mc2_vulkan_load_spv(device, vertPath.c_str());
    VkShaderModule frag = mc2_vulkan_load_spv(device, fragPath.c_str());
    bool ok = (vert != VK_NULL_HANDLE) && (frag != VK_NULL_HANDLE);

    if (ok) log("shader-probe: both fullscreen shader modules loaded OK.");
    else    log("shader-probe: one or more shader modules failed to load.");

    mc2_vulkan_free_shader(device, vert);
    mc2_vulkan_free_shader(device, frag);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    log("shader-probe: cleaned up (device+instance destroyed).");
    return ok;
}

// ============================================================================
// VULKAN-FULLSCREEN-TRIANGLE-1: headless offscreen render + readback + verify.
// Proves shaders+pipeline+renderpass+draw+readback end to end. No surface/
// swapchain/window. Renders the fullscreen.vert/frag pipeline to an offscreen
// RGBA8 VkImage, copies to a host-visible buffer, reads back, verifies the
// rendered UV gradient (the oversized fullscreen triangle covers the whole
// viewport, so every pixel carries the frag's interpolated vUV; if nothing
// drew all pixels stay the clear color and the checks fail).
// Fail-soft: any VkResult error -> log + return false, no crash.
// ============================================================================

namespace {

// Pick a memory type index satisfying typeBits + required property flags.
// Returns UINT32_MAX if none.
uint32_t find_mem_type(VkPhysicalDevice phys, uint32_t typeBits,
                       VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & want) == want) {
            return i;
        }
    }
    return UINT32_MAX;
}

} // namespace

bool mc2_vulkan_probe_triangle(const char* spvDir) {
    // VULKAN-VOLK-LOADER-1: open the Vulkan runtime via volk before any vk call.
    if (!ensure_volk_initialized()) return false;

    const uint32_t W = 64, H = 64;
    const VkFormat FMT = VK_FORMAT_R8G8B8A8_UNORM;

    // ---- VkInstance (headless) ---------------------------------------------
    VkApplicationInfo app{};
    app.sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "MC2 (Vulkan triangle probe)";
    app.pEngineName      = "MC2-GameOS";
    app.apiVersion       = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ici{};
    ici.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;

    VkInstance instance = VK_NULL_HANDLE;
    VkResult r = vkCreateInstance(&ici, nullptr, &instance);
    if (r != VK_SUCCESS) {
        log("triangle-probe: vkCreateInstance failed (VkResult=%d). fail-soft.", (int)r);
        return false;
    }
    // VULKAN-VOLK-LOADER-1: resolve instance-level dispatch through volk.
    volkLoadInstance(instance);

    // ---- Physical device ----------------------------------------------------
    uint32_t devCount = 0;
    r = vkEnumeratePhysicalDevices(instance, &devCount, nullptr);
    if (r != VK_SUCCESS || devCount == 0) {
        log("triangle-probe: no physical devices (VkResult=%d, count=%u). fail-soft.", (int)r, devCount);
        vkDestroyInstance(instance, nullptr);
        return false;
    }
    std::vector<VkPhysicalDevice> devs(devCount);
    vkEnumeratePhysicalDevices(instance, &devCount, devs.data());
    VkPhysicalDevice phys = devs[0];

    // ---- Graphics queue family ---------------------------------------------
    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfCount);
    if (qfCount) vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfCount, qfs.data());
    uint32_t gfxFamily = UINT32_MAX;
    for (uint32_t q = 0; q < qfCount; ++q) {
        if (qfs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) { gfxFamily = q; break; }
    }
    if (gfxFamily == UINT32_MAX) {
        log("triangle-probe: no graphics queue family. fail-soft.");
        vkDestroyInstance(instance, nullptr);
        return false;
    }

    // ---- Logical device + queue (headless, no extensions) -------------------
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = gfxFamily;
    qci.queueCount       = 1;
    qci.pQueuePriorities = &prio;

    VkDeviceCreateInfo dci{};
    dci.sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos    = &qci;

    VkDevice device = VK_NULL_HANDLE;
    r = vkCreateDevice(phys, &dci, nullptr, &device);
    if (r != VK_SUCCESS) {
        log("triangle-probe: vkCreateDevice failed (VkResult=%d). fail-soft.", (int)r);
        vkDestroyInstance(instance, nullptr);
        return false;
    }
    // VULKAN-VOLK-LOADER-1: resolve device-level dispatch through volk. Must
    // precede vmaImportVulkanFunctionsFromVolk() below (it reads volk's device
    // table for this VkDevice).
    volkLoadDevice(device);
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, gfxFamily, 0, &queue);

    // Everything past here is cleaned up via the single `done` epilogue.
    bool ok = false;
    VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
    // VULKAN-VMA-INTEGRATION-1: VMA owns the offscreen image + readback buffer
    // allocations below (imageMem/dstMem are gone -- VMA holds the memory).
    VmaAllocator   allocator = VK_NULL_HANDLE;
    VkImage        image    = VK_NULL_HANDLE;
    VmaAllocation  imageAlloc = VK_NULL_HANDLE;
    VkImageView    view    = VK_NULL_HANDLE;
    VkRenderPass   rpass   = VK_NULL_HANDLE;
    VkFramebuffer  fb      = VK_NULL_HANDLE;
    VkPipelineLayout playout = VK_NULL_HANDLE;
    VkPipeline     pipe    = VK_NULL_HANDLE;
    VkBuffer       dst     = VK_NULL_HANDLE;
    VmaAllocation  dstAlloc = VK_NULL_HANDLE;
    VkCommandPool  cpool   = VK_NULL_HANDLE;
    VkCommandBuffer cbuf   = VK_NULL_HANDLE;
    VkFence        fence   = VK_NULL_HANDLE;

    // ---- VMA allocator ------------------------------------------------------
    // VULKAN-VMA-INTEGRATION-1 + VULKAN-VOLK-LOADER-1: one allocator per device.
    // Owns the offscreen image + readback buffer below. Vulkan is now loaded
    // dynamically by volk (VMA_STATIC_VULKAN_FUNCTIONS=0 in vma_impl.cpp), so
    // VMA has no statically-linked prototypes to call -- it MUST be handed the
    // volk-resolved function pointers. vmaImportVulkanFunctionsFromVolk() fills
    // a VmaVulkanFunctions from volk's instance globals + this device's table
    // (volkLoadDevice() above). Without this the allocator would deref null fn
    // pointers and crash.
    {
        VmaAllocatorCreateInfo aci{};
        aci.physicalDevice   = phys;
        aci.device           = device;
        aci.instance         = instance;
        aci.vulkanApiVersion = VK_API_VERSION_1_1; // matches app.apiVersion above

        VmaVulkanFunctions vkFuncs{};
        r = vmaImportVulkanFunctionsFromVolk(&aci, &vkFuncs);
        if (r != VK_SUCCESS) {
            log("triangle-probe: vmaImportVulkanFunctionsFromVolk failed (%d). fail-soft.", (int)r);
            goto done;
        }
        aci.pVulkanFunctions = &vkFuncs;

        r = vmaCreateAllocator(&aci, &allocator);
        if (r != VK_SUCCESS) {
            log("triangle-probe: vmaCreateAllocator failed (%d). fail-soft.", (int)r);
            goto done;
        }
    }

    // ---- Shader modules -----------------------------------------------------
    {
        std::string dir = spvDir ? spvDir : ".";
        if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += '/';
        vert = mc2_vulkan_load_spv(device, (dir + "fullscreen.vert.spv").c_str());
        frag = mc2_vulkan_load_spv(device, (dir + "fullscreen.frag.spv").c_str());
        if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) {
            log("triangle-probe: shader module load failed. fail-soft.");
            goto done;
        }
    }

    // ---- Offscreen color image + memory + view ------------------------------
    {
        VkImageCreateInfo ii{};
        ii.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType     = VK_IMAGE_TYPE_2D;
        ii.format        = FMT;
        ii.extent        = {W, H, 1};
        ii.mipLevels     = 1;
        ii.arrayLayers   = 1;
        ii.samples       = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ii.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        // VULKAN-VMA-INTEGRATION-1: VMA creates the image + backing device-local
        // memory + binds them in one call (replaces vkCreateImage +
        // vkGetImageMemoryRequirements + vkAllocateMemory + vkBindImageMemory).
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT; // render target
        r = vmaCreateImage(allocator, &ii, &aci, &image, &imageAlloc, nullptr);
        if (r != VK_SUCCESS) { log("triangle-probe: vmaCreateImage failed (%d). fail-soft.", (int)r); goto done; }

        VkImageViewCreateInfo vi{};
        vi.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image    = image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format   = FMT;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        r = vkCreateImageView(device, &vi, nullptr, &view);
        if (r != VK_SUCCESS) { log("triangle-probe: vkCreateImageView failed (%d). fail-soft.", (int)r); goto done; }
    }

    // ---- Render pass (1 color attachment, clear->store) ---------------------
    {
        VkAttachmentDescription att{};
        att.format         = FMT;
        att.samples        = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout    = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

        VkAttachmentReference ref{};
        ref.attachment = 0;
        ref.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription sub{};
        sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments    = &ref;

        VkRenderPassCreateInfo rpci{};
        rpci.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpci.attachmentCount = 1;
        rpci.pAttachments    = &att;
        rpci.subpassCount    = 1;
        rpci.pSubpasses      = &sub;
        r = vkCreateRenderPass(device, &rpci, nullptr, &rpass);
        if (r != VK_SUCCESS) { log("triangle-probe: vkCreateRenderPass failed (%d). fail-soft.", (int)r); goto done; }

        VkFramebufferCreateInfo fci{};
        fci.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass      = rpass;
        fci.attachmentCount = 1;
        fci.pAttachments    = &view;
        fci.width           = W;
        fci.height          = H;
        fci.layers          = 1;
        r = vkCreateFramebuffer(device, &fci, nullptr, &fb);
        if (r != VK_SUCCESS) { log("triangle-probe: vkCreateFramebuffer failed (%d). fail-soft.", (int)r); goto done; }
    }

    // ---- Graphics pipeline (fullscreen.vert/frag; no vertex input) ----------
    {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert;
        stages[0].pName  = "main";
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag;
        stages[1].pName  = "main";

        VkPipelineVertexInputStateCreateInfo vin{};
        vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkViewport vp{0.0f, 0.0f, (float)W, (float)H, 0.0f, 1.0f};
        VkRect2D   sc{{0, 0}, {W, H}};
        VkPipelineViewportStateCreateInfo vps{};
        vps.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vps.viewportCount = 1;
        vps.pViewports    = &vp;
        vps.scissorCount  = 1;
        vps.pScissors     = &sc;

        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode    = VK_CULL_MODE_NONE;
        rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth   = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        cba.blendEnable    = VK_FALSE;
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1;
        cb.pAttachments    = &cba;

        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        r = vkCreatePipelineLayout(device, &plci, nullptr, &playout);
        if (r != VK_SUCCESS) { log("triangle-probe: vkCreatePipelineLayout failed (%d). fail-soft.", (int)r); goto done; }

        VkGraphicsPipelineCreateInfo gp{};
        gp.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gp.stageCount          = 2;
        gp.pStages             = stages;
        gp.pVertexInputState   = &vin;
        gp.pInputAssemblyState = &ia;
        gp.pViewportState      = &vps;
        gp.pRasterizationState = &rs;
        gp.pMultisampleState   = &ms;
        gp.pColorBlendState    = &cb;
        gp.layout              = playout;
        gp.renderPass          = rpass;
        gp.subpass             = 0;
        r = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, nullptr, &pipe);
        if (r != VK_SUCCESS) { log("triangle-probe: vkCreateGraphicsPipelines failed (%d). fail-soft.", (int)r); goto done; }
    }

    // ---- Host-visible readback buffer (W*H*4 bytes) -------------------------
    {
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size  = (VkDeviceSize)W * H * 4;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;

        // VULKAN-VMA-INTEGRATION-1: VMA creates the buffer + host-visible memory
        // + binds them in one call. HOST_ACCESS_RANDOM_BIT makes VMA pick a
        // HOST_VISIBLE|HOST_COHERENT memory type for the CPU readback below.
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
        r = vmaCreateBuffer(allocator, &bci, &aci, &dst, &dstAlloc, nullptr);
        if (r != VK_SUCCESS) { log("triangle-probe: vmaCreateBuffer failed (%d). fail-soft.", (int)r); goto done; }
    }

    // ---- Command buffer: renderpass + draw(3) + copy image->buffer ----------
    {
        VkCommandPoolCreateInfo pci{};
        pci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.queueFamilyIndex = gfxFamily;
        r = vkCreateCommandPool(device, &pci, nullptr, &cpool);
        if (r != VK_SUCCESS) { log("triangle-probe: vkCreateCommandPool failed (%d). fail-soft.", (int)r); goto done; }

        VkCommandBufferAllocateInfo cai{};
        cai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool        = cpool;
        cai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        r = vkAllocateCommandBuffers(device, &cai, &cbuf);
        if (r != VK_SUCCESS) { log("triangle-probe: vkAllocateCommandBuffers failed (%d). fail-soft.", (int)r); goto done; }

        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        r = vkBeginCommandBuffer(cbuf, &bi);
        if (r != VK_SUCCESS) { log("triangle-probe: vkBeginCommandBuffer failed (%d). fail-soft.", (int)r); goto done; }

        VkClearValue clear{};
        clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}}; // clear = opaque black
        VkRenderPassBeginInfo rbi{};
        rbi.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rbi.renderPass        = rpass;
        rbi.framebuffer       = fb;
        rbi.renderArea        = {{0, 0}, {W, H}};
        rbi.clearValueCount   = 1;
        rbi.pClearValues      = &clear;
        vkCmdBeginRenderPass(cbuf, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdDraw(cbuf, 3, 1, 0, 0);
        vkCmdEndRenderPass(cbuf);
        // renderpass finalLayout already TRANSFER_SRC_OPTIMAL -> copy directly.

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent      = {W, H, 1};
        vkCmdCopyImageToBuffer(cbuf, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               dst, 1, &region);
        r = vkEndCommandBuffer(cbuf);
        if (r != VK_SUCCESS) { log("triangle-probe: vkEndCommandBuffer failed (%d). fail-soft.", (int)r); goto done; }

        VkFenceCreateInfo fnci{};
        fnci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        r = vkCreateFence(device, &fnci, nullptr, &fence);
        if (r != VK_SUCCESS) { log("triangle-probe: vkCreateFence failed (%d). fail-soft.", (int)r); goto done; }

        VkSubmitInfo si{};
        si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers    = &cbuf;
        r = vkQueueSubmit(queue, 1, &si, fence);
        if (r != VK_SUCCESS) { log("triangle-probe: vkQueueSubmit failed (%d). fail-soft.", (int)r); goto done; }
        r = vkWaitForFences(device, 1, &fence, VK_TRUE, 5ull * 1000 * 1000 * 1000);
        if (r != VK_SUCCESS) { log("triangle-probe: vkWaitForFences failed/timeout (%d). fail-soft.", (int)r); goto done; }
    }

    // ---- Read back + verify -------------------------------------------------
    {
        void* mapped = nullptr;
        // VULKAN-VMA-INTEGRATION-1: map through VMA (the allocation is host-
        // visible + coherent per the HOST_ACCESS_RANDOM_BIT request above).
        r = vmaMapMemory(allocator, dstAlloc, &mapped);
        if (r != VK_SUCCESS) { log("triangle-probe: vmaMapMemory failed (%d). fail-soft.", (int)r); goto done; }
        const unsigned char* px = static_cast<const unsigned char*>(mapped);
        auto at = [&](uint32_t x, uint32_t y) { return px + ((size_t)y * W + x) * 4; };

        // The fullscreen triangle is oversized (uv in [0,2]x[0,2] -> covers the
        // whole viewport), so every sample is INSIDE the triangle and carries
        // the frag's interpolated UV output vec4(vUV,0,1) -- there is no clear
        // region left visible. We verify the pipeline drew + interpolated by
        // checking the UV gradient at three probes (B~0, A~255 throughout):
        //   center (W/2,H/2)  uv~=(0.5,0.5) -> ~(128,128,0,255)
        //   top-left (0,0)    uv~=(0,0)     -> ~(0,0,0,255)
        //   top-right (W-1,0) uv~=(1,0)     -> ~(255,0,0,255)
        // If NOTHING drew, all pixels would be the clear color (0,0,0,255) and
        // center/top-right R,G would fail -> proves the draw actually ran.
        const unsigned char* c  = at(W / 2, H / 2);
        const unsigned char* tl = at(0, 0);
        const unsigned char* tr = at(W - 1, 0);

        bool centerOK = (c[0] > 90 && c[0] < 170) &&
                        (c[1] > 90 && c[1] < 170) &&
                        (c[2] < 40) && (c[3] > 200);
        bool tlOK     = (tl[0] < 40 && tl[1] < 40 && tl[2] < 40 && tl[3] > 200);
        bool trOK     = (tr[0] > 200 && tr[1] < 40 && tr[2] < 40 && tr[3] > 200);

        log("triangle-probe: center=(%u,%u,%u,%u) tl=(%u,%u,%u,%u) tr=(%u,%u,%u,%u) "
            "centerOK=%d tlOK=%d trOK=%d",
            c[0], c[1], c[2], c[3], tl[0], tl[1], tl[2], tl[3], tr[0], tr[1], tr[2], tr[3],
            centerOK ? 1 : 0, tlOK ? 1 : 0, trOK ? 1 : 0);
        vmaUnmapMemory(allocator, dstAlloc);
        ok = centerOK && tlOK && trOK;
    }

    if (ok) log("triangle-probe: PASS -- offscreen triangle rendered + readback verified.");
    else    log("triangle-probe: FAIL -- pixel verify did not match expected.");

done:
    if (fence)   vkDestroyFence(device, fence, nullptr);
    if (cpool)   vkDestroyCommandPool(device, cpool, nullptr); // frees cbuf
    // VULKAN-VMA-INTEGRATION-1: VMA-owned buffer/image are destroyed with their
    // allocations via vmaDestroy*; the allocator is torn down last (below).
    if (dst)     vmaDestroyBuffer(allocator, dst, dstAlloc);
    if (pipe)    vkDestroyPipeline(device, pipe, nullptr);
    if (playout) vkDestroyPipelineLayout(device, playout, nullptr);
    if (fb)      vkDestroyFramebuffer(device, fb, nullptr);
    if (rpass)   vkDestroyRenderPass(device, rpass, nullptr);
    if (view)    vkDestroyImageView(device, view, nullptr);
    if (image)   vmaDestroyImage(allocator, image, imageAlloc);
    if (allocator) vmaDestroyAllocator(allocator);
    mc2_vulkan_free_shader(device, vert);
    mc2_vulkan_free_shader(device, frag);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    log("triangle-probe: cleaned up (device+instance destroyed).");
    return ok;
}

// ============================================================================
// VULKAN-DESCRIPTOR-SMOKE-1: headless descriptor-set plumbing probe.
// Builds a Set-0-shaped descriptor set layout (binding 0 = UBO, binding 1 =
// SSBO, mirroring VULKAN-DESCRIPTOR-INVENTORY-1's per-frame global set) + pool
// + set, creates a small uniform + storage buffer, binds them via
// vkUpdateDescriptorSets, destroys all. No pipeline/renderpass/draw. With
// MC2_VULKAN_VALIDATION set, a debug-utils messenger catches any validation
// error/warning and flips the probe to FAIL -- ZERO validation errors is the
// real proof the descriptor plumbing is wired correctly.
// Fail-soft: any VkResult error -> log + return false.
// ============================================================================

namespace {

// Set by the debug messenger callback if the validation layer ever reports an
// error or warning during the descriptor probe. File-static so the C callback
// (which has no user-data plumbing here) can reach it. The probe is one-shot
// and single-threaded, so a plain flag is sufficient.
bool g_desc_validation_saw_error = false;

VKAPI_ATTR VkBool32 VKAPI_CALL desc_debug_cb(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* pData,
    void* /*user*/) {
    if (severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)) {
        g_desc_validation_saw_error = true;
        log("desc-probe: VALIDATION: %s",
            pData && pData->pMessage ? pData->pMessage : "(no message)");
    }
    return VK_FALSE;
}

} // namespace

bool mc2_vulkan_probe_descriptors() {
    // VULKAN-VOLK-LOADER-1: open the Vulkan runtime via volk before any vk call
    // (the validation-layer enumeration below is already an instance-level call).
    if (!ensure_volk_initialized()) return false;

    g_desc_validation_saw_error = false;

    // ---- Optional validation layer + debug-utils messenger ------------------
    std::vector<const char*> layers;
    std::vector<const char*> instExts;
    bool wantValidation = std::getenv("MC2_VULKAN_VALIDATION") != nullptr;
    if (wantValidation) {
        uint32_t layerCount = 0;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        std::vector<VkLayerProperties> avail(layerCount);
        if (layerCount) vkEnumerateInstanceLayerProperties(&layerCount, avail.data());
        const char* want = "VK_LAYER_KHRONOS_validation";
        bool found = false;
        for (const auto& lp : avail) {
            if (std::strcmp(lp.layerName, want) == 0) { found = true; break; }
        }
        if (found) {
            layers.push_back(want);
            instExts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            log("desc-probe: validation layer %s enabled", want);
        } else {
            wantValidation = false;
            log("desc-probe: validation requested but %s not available; continuing without", want);
        }
    }

    // ---- VkInstance (headless) ----------------------------------------------
    VkApplicationInfo app{};
    app.sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "MC2 (Vulkan descriptor probe)";
    app.pEngineName      = "MC2-GameOS";
    app.apiVersion       = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ici{};
    ici.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo        = &app;
    ici.enabledLayerCount       = static_cast<uint32_t>(layers.size());
    ici.ppEnabledLayerNames     = layers.empty() ? nullptr : layers.data();
    ici.enabledExtensionCount   = static_cast<uint32_t>(instExts.size());
    ici.ppEnabledExtensionNames = instExts.empty() ? nullptr : instExts.data();

    VkInstance instance = VK_NULL_HANDLE;
    VkResult r = vkCreateInstance(&ici, nullptr, &instance);
    if (r != VK_SUCCESS) {
        log("desc-probe: vkCreateInstance failed (VkResult=%d). fail-soft.", (int)r);
        return false;
    }
    // VULKAN-VOLK-LOADER-1: resolve instance-level dispatch through volk.
    volkLoadInstance(instance);

    // ---- Debug messenger (only if validation enabled) -----------------------
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    if (wantValidation) {
        auto pfnCreate = (PFN_vkCreateDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
        if (pfnCreate) {
            VkDebugUtilsMessengerCreateInfoEXT mci{};
            mci.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            mci.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            mci.pfnUserCallback = desc_debug_cb;
            pfnCreate(instance, &mci, nullptr, &messenger);
        } else {
            log("desc-probe: vkCreateDebugUtilsMessengerEXT not found; continuing (no error capture)");
        }
    }

    // ---- Physical device + queue family -------------------------------------
    uint32_t devCount = 0;
    r = vkEnumeratePhysicalDevices(instance, &devCount, nullptr);
    if (r != VK_SUCCESS || devCount == 0) {
        log("desc-probe: no physical devices (VkResult=%d, count=%u). fail-soft.", (int)r, devCount);
        if (messenger) {
            auto d = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
            if (d) d(instance, messenger, nullptr);
        }
        vkDestroyInstance(instance, nullptr);
        return false;
    }
    std::vector<VkPhysicalDevice> devs(devCount);
    vkEnumeratePhysicalDevices(instance, &devCount, devs.data());
    VkPhysicalDevice phys = devs[0];

    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfCount, nullptr);
    if (qfCount == 0) {
        log("desc-probe: no queue families. fail-soft.");
        if (messenger) {
            auto d = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
            if (d) d(instance, messenger, nullptr);
        }
        vkDestroyInstance(instance, nullptr);
        return false;
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = 0;
    qci.queueCount       = 1;
    qci.pQueuePriorities = &prio;

    VkDeviceCreateInfo dci{};
    dci.sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos    = &qci;

    VkDevice device = VK_NULL_HANDLE;
    r = vkCreateDevice(phys, &dci, nullptr, &device);
    if (r != VK_SUCCESS) {
        // VULKAN-VOLK-LOADER-1: (device dispatch loaded below on success)
        log("desc-probe: vkCreateDevice failed (VkResult=%d). fail-soft.", (int)r);
        if (messenger) {
            auto d = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
            if (d) d(instance, messenger, nullptr);
        }
        vkDestroyInstance(instance, nullptr);
        return false;
    }
    // VULKAN-VOLK-LOADER-1: resolve device-level dispatch through volk.
    volkLoadDevice(device);

    // Everything past here is cleaned up via the single `done` epilogue.
    bool ok = false;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkDescriptorPool      pool   = VK_NULL_HANDLE;
    VkDescriptorSet       set    = VK_NULL_HANDLE; // freed with the pool
    VkBuffer       ubo     = VK_NULL_HANDLE;
    VkDeviceMemory uboMem  = VK_NULL_HANDLE;
    VkBuffer       ssbo    = VK_NULL_HANDLE;
    VkDeviceMemory ssboMem = VK_NULL_HANDLE;

    // ---- Descriptor set layout: binding0=UBO, binding1=SSBO (Set-0 shape) ---
    {
        VkDescriptorSetLayoutBinding binds[2]{};
        binds[0].binding         = 0; // ViewUniformsUbo analog
        binds[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        binds[0].descriptorCount = 1;
        binds[0].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        binds[1].binding         = 1; // LightDataSsbo analog
        binds[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binds[1].descriptorCount = 1;
        binds[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = 2;
        lci.pBindings    = binds;
        r = vkCreateDescriptorSetLayout(device, &lci, nullptr, &layout);
        if (r != VK_SUCCESS) { log("desc-probe: vkCreateDescriptorSetLayout failed (%d). fail-soft.", (int)r); goto done; }
    }

    // ---- Descriptor pool (1 UBO + 1 SSBO, 1 set) ----------------------------
    {
        VkDescriptorPoolSize sizes[2]{};
        sizes[0].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        sizes[0].descriptorCount = 1;
        sizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        sizes[1].descriptorCount = 1;

        VkDescriptorPoolCreateInfo pci{};
        pci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets       = 1;
        pci.poolSizeCount = 2;
        pci.pPoolSizes    = sizes;
        r = vkCreateDescriptorPool(device, &pci, nullptr, &pool);
        if (r != VK_SUCCESS) { log("desc-probe: vkCreateDescriptorPool failed (%d). fail-soft.", (int)r); goto done; }

        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool     = pool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts        = &layout;
        r = vkAllocateDescriptorSets(device, &dsai, &set);
        if (r != VK_SUCCESS) { log("desc-probe: vkAllocateDescriptorSets failed (%d). fail-soft.", (int)r); goto done; }
    }

    // ---- Small UBO + SSBO buffers (host-visible for simplicity) -------------
    {
        auto make_buffer = [&](VkDeviceSize size, VkBufferUsageFlags usage,
                               VkBuffer* outBuf, VkDeviceMemory* outMem) -> bool {
            VkBufferCreateInfo bci{};
            bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bci.size  = size;
            bci.usage = usage;
            VkResult rr = vkCreateBuffer(device, &bci, nullptr, outBuf);
            if (rr != VK_SUCCESS) { log("desc-probe: vkCreateBuffer failed (%d). fail-soft.", (int)rr); return false; }
            VkMemoryRequirements mr{};
            vkGetBufferMemoryRequirements(device, *outBuf, &mr);
            uint32_t mt = find_mem_type(phys, mr.memoryTypeBits,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (mt == UINT32_MAX) { log("desc-probe: no host-visible memtype. fail-soft."); return false; }
            VkMemoryAllocateInfo mai{};
            mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            mai.allocationSize  = mr.size;
            mai.memoryTypeIndex = mt;
            rr = vkAllocateMemory(device, &mai, nullptr, outMem);
            if (rr != VK_SUCCESS) { log("desc-probe: vkAllocateMemory failed (%d). fail-soft.", (int)rr); return false; }
            vkBindBufferMemory(device, *outBuf, *outMem, 0);
            return true;
        };

        // 144B mirrors ViewUniformsUbo POD (2x mat4 + vec4); 256B storage.
        if (!make_buffer(144, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &ubo, &uboMem)) goto done;
        if (!make_buffer(256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &ssbo, &ssboMem)) goto done;
    }

    // ---- Bind buffers to the set via vkUpdateDescriptorSets -----------------
    {
        VkDescriptorBufferInfo uboInfo{ ubo, 0, VK_WHOLE_SIZE };
        VkDescriptorBufferInfo ssboInfo{ ssbo, 0, VK_WHOLE_SIZE };

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = set;
        writes[0].dstBinding      = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo     = &uboInfo;
        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = set;
        writes[1].dstBinding      = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo     = &ssboInfo;

        vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
        log("desc-probe: descriptor set updated (binding0=UBO, binding1=SSBO).");
    }

    // Wiring succeeded. If validation was on, ZERO captured errors is the proof.
    ok = !(wantValidation && g_desc_validation_saw_error);
    if (wantValidation) {
        log("desc-probe: validation active; saw_error=%d", g_desc_validation_saw_error ? 1 : 0);
    }

done:
    if (ssboMem) vkFreeMemory(device, ssboMem, nullptr);
    if (ssbo)    vkDestroyBuffer(device, ssbo, nullptr);
    if (uboMem)  vkFreeMemory(device, uboMem, nullptr);
    if (ubo)     vkDestroyBuffer(device, ubo, nullptr);
    if (pool)    vkDestroyDescriptorPool(device, pool, nullptr); // frees the set
    if (layout)  vkDestroyDescriptorSetLayout(device, layout, nullptr);
    vkDestroyDevice(device, nullptr);
    if (messenger) {
        auto d = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
        if (d) d(instance, messenger, nullptr);
    }
    vkDestroyInstance(instance, nullptr);
    log("desc-probe: %s -- cleaned up (device+instance destroyed).", ok ? "PASS" : "FAIL");
    return ok;
}

// ============================================================================
// VULKAN-SAMPLED-IMAGE-SMOKE-1: headless sampled-image + sampler descriptor.
// Builds a small device-local VkImage (8x8 RGBA8), clears it + transitions to
// SHADER_READ_ONLY via a one-shot cmd-buffer barrier, VkImageView, a linear+
// repeat VkSampler + a compare VkSampler (compareEnable, shadow-sampler shape),
// a descriptor set with a COMBINED_IMAGE_SAMPLER binding, binds via
// vkUpdateDescriptorSets, destroys all. Proves the sampled-image path Vulkan
// needs (per-pass rebind does not survive Vk). No swapchain/window/game path.
// With MC2_VULKAN_VALIDATION set, ZERO validation errors is the real proof.
// Fail-soft: any VkResult error -> log + return false.
// ============================================================================

namespace {

bool g_img_validation_saw_error = false;

VKAPI_ATTR VkBool32 VKAPI_CALL img_debug_cb(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* pData,
    void* /*user*/) {
    if (severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)) {
        g_img_validation_saw_error = true;
        log("img-probe: VALIDATION: %s",
            pData && pData->pMessage ? pData->pMessage : "(no message)");
    }
    return VK_FALSE;
}

} // namespace

bool mc2_vulkan_probe_sampled_image() {
    // VULKAN-VOLK-LOADER-1: open the Vulkan runtime via volk before any vk call.
    if (!ensure_volk_initialized()) return false;

    g_img_validation_saw_error = false;
    const uint32_t W = 8, H = 8;
    const VkFormat FMT = VK_FORMAT_R8G8B8A8_UNORM;

    // ---- Optional validation layer + debug-utils messenger ------------------
    std::vector<const char*> layers;
    std::vector<const char*> instExts;
    bool wantValidation = std::getenv("MC2_VULKAN_VALIDATION") != nullptr;
    if (wantValidation) {
        uint32_t layerCount = 0;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        std::vector<VkLayerProperties> avail(layerCount);
        if (layerCount) vkEnumerateInstanceLayerProperties(&layerCount, avail.data());
        const char* want = "VK_LAYER_KHRONOS_validation";
        bool found = false;
        for (const auto& lp : avail) {
            if (std::strcmp(lp.layerName, want) == 0) { found = true; break; }
        }
        if (found) {
            layers.push_back(want);
            instExts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            log("img-probe: validation layer %s enabled", want);
        } else {
            wantValidation = false;
            log("img-probe: validation requested but %s not available; continuing without", want);
        }
    }

    // ---- VkInstance (headless) ----------------------------------------------
    VkApplicationInfo app{};
    app.sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "MC2 (Vulkan sampled-image probe)";
    app.pEngineName      = "MC2-GameOS";
    app.apiVersion       = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ici{};
    ici.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo        = &app;
    ici.enabledLayerCount       = static_cast<uint32_t>(layers.size());
    ici.ppEnabledLayerNames     = layers.empty() ? nullptr : layers.data();
    ici.enabledExtensionCount   = static_cast<uint32_t>(instExts.size());
    ici.ppEnabledExtensionNames = instExts.empty() ? nullptr : instExts.data();

    VkInstance instance = VK_NULL_HANDLE;
    VkResult r = vkCreateInstance(&ici, nullptr, &instance);
    if (r != VK_SUCCESS) {
        log("img-probe: vkCreateInstance failed (VkResult=%d). fail-soft.", (int)r);
        return false;
    }
    // VULKAN-VOLK-LOADER-1: resolve instance-level dispatch through volk.
    volkLoadInstance(instance);

    // ---- Debug messenger (only if validation enabled) -----------------------
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    if (wantValidation) {
        auto pfnCreate = (PFN_vkCreateDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
        if (pfnCreate) {
            VkDebugUtilsMessengerCreateInfoEXT mci{};
            mci.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            mci.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            mci.pfnUserCallback = img_debug_cb;
            pfnCreate(instance, &mci, nullptr, &messenger);
        } else {
            log("img-probe: vkCreateDebugUtilsMessengerEXT not found; continuing (no error capture)");
        }
    }

    auto destroy_messenger = [&]() {
        if (messenger) {
            auto d = (PFN_vkDestroyDebugUtilsMessengerEXT)
                vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
            if (d) d(instance, messenger, nullptr);
        }
    };

    // ---- Physical device + graphics queue family ----------------------------
    uint32_t devCount = 0;
    r = vkEnumeratePhysicalDevices(instance, &devCount, nullptr);
    if (r != VK_SUCCESS || devCount == 0) {
        log("img-probe: no physical devices (VkResult=%d, count=%u). fail-soft.", (int)r, devCount);
        destroy_messenger();
        vkDestroyInstance(instance, nullptr);
        return false;
    }
    std::vector<VkPhysicalDevice> devs(devCount);
    vkEnumeratePhysicalDevices(instance, &devCount, devs.data());
    VkPhysicalDevice phys = devs[0];

    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfCount);
    if (qfCount) vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfCount, qfs.data());
    uint32_t gfxFamily = UINT32_MAX;
    for (uint32_t q = 0; q < qfCount; ++q) {
        if (qfs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) { gfxFamily = q; break; }
    }
    if (gfxFamily == UINT32_MAX) {
        log("img-probe: no graphics queue family. fail-soft.");
        destroy_messenger();
        vkDestroyInstance(instance, nullptr);
        return false;
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = gfxFamily;
    qci.queueCount       = 1;
    qci.pQueuePriorities = &prio;

    VkDeviceCreateInfo dci{};
    dci.sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos    = &qci;

    VkDevice device = VK_NULL_HANDLE;
    r = vkCreateDevice(phys, &dci, nullptr, &device);
    if (r != VK_SUCCESS) {
        log("img-probe: vkCreateDevice failed (VkResult=%d). fail-soft.", (int)r);
        destroy_messenger();
        vkDestroyInstance(instance, nullptr);
        return false;
    }
    // VULKAN-VOLK-LOADER-1: resolve device-level dispatch through volk.
    volkLoadDevice(device);
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, gfxFamily, 0, &queue);

    // Everything past here is cleaned up via the single `done` epilogue.
    bool ok = false;
    VkImage         image    = VK_NULL_HANDLE;
    VkDeviceMemory  imageMem = VK_NULL_HANDLE;
    VkImageView     view     = VK_NULL_HANDLE;
    VkSampler       sampler  = VK_NULL_HANDLE;
    VkSampler       cmpSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkDescriptorPool      pool   = VK_NULL_HANDLE;
    VkDescriptorSet       set    = VK_NULL_HANDLE; // freed with the pool
    VkCommandPool   cpool = VK_NULL_HANDLE;
    VkCommandBuffer cbuf  = VK_NULL_HANDLE;
    VkFence         fence = VK_NULL_HANDLE;

    // ---- Device-local sampled VkImage (8x8 RGBA8) + memory + view -----------
    {
        VkImageCreateInfo ii{};
        ii.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType     = VK_IMAGE_TYPE_2D;
        ii.format        = FMT;
        ii.extent        = {W, H, 1};
        ii.mipLevels     = 1;
        ii.arrayLayers   = 1;
        ii.samples       = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling        = VK_IMAGE_TILING_OPTIMAL;
        // SAMPLED (shader read) + TRANSFER_DST (for the clear).
        ii.usage         = VK_IMAGE_USAGE_SAMPLED_BIT |
                           VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        r = vkCreateImage(device, &ii, nullptr, &image);
        if (r != VK_SUCCESS) { log("img-probe: vkCreateImage failed (%d). fail-soft.", (int)r); goto done; }

        VkMemoryRequirements mr{};
        vkGetImageMemoryRequirements(device, image, &mr);
        uint32_t mt = find_mem_type(phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mt == UINT32_MAX) { log("img-probe: no device-local memtype for image. fail-soft."); goto done; }
        VkMemoryAllocateInfo mai{};
        mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize  = mr.size;
        mai.memoryTypeIndex = mt;
        r = vkAllocateMemory(device, &mai, nullptr, &imageMem);
        if (r != VK_SUCCESS) { log("img-probe: vkAllocateMemory(image) failed (%d). fail-soft.", (int)r); goto done; }
        vkBindImageMemory(device, image, imageMem, 0);

        VkImageViewCreateInfo vi{};
        vi.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image    = image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format   = FMT;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        r = vkCreateImageView(device, &vi, nullptr, &view);
        if (r != VK_SUCCESS) { log("img-probe: vkCreateImageView failed (%d). fail-soft.", (int)r); goto done; }
    }

    // ---- Samplers: linear+repeat main; compare (shadow) sampler -------------
    {
        VkSamplerCreateInfo si{};
        si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter    = VK_FILTER_LINEAR;
        si.minFilter    = VK_FILTER_LINEAR;
        si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        si.maxLod       = VK_LOD_CLAMP_NONE;
        r = vkCreateSampler(device, &si, nullptr, &sampler);
        if (r != VK_SUCCESS) { log("img-probe: vkCreateSampler(linear) failed (%d). fail-soft.", (int)r); goto done; }

        // Shadow-compare sampler shape: linear PCF, clamp-to-edge, white border,
        // compareEnable + LESS_OR_EQUAL (mirrors the PCF shadow sampler config).
        VkSamplerCreateInfo cs{};
        cs.sType         = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        cs.magFilter     = VK_FILTER_LINEAR;
        cs.minFilter     = VK_FILTER_LINEAR;
        cs.mipmapMode    = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        cs.addressModeU  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        cs.addressModeV  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        cs.addressModeW  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        cs.borderColor   = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        cs.compareEnable = VK_TRUE;
        cs.compareOp     = VK_COMPARE_OP_LESS_OR_EQUAL;
        cs.maxLod        = VK_LOD_CLAMP_NONE;
        r = vkCreateSampler(device, &cs, nullptr, &cmpSampler);
        if (r != VK_SUCCESS) { log("img-probe: vkCreateSampler(compare) failed (%d). fail-soft.", (int)r); goto done; }
    }

    // ---- One-shot cmd buffer: clear image + transition to SHADER_READ_ONLY --
    {
        VkCommandPoolCreateInfo pci{};
        pci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.queueFamilyIndex = gfxFamily;
        r = vkCreateCommandPool(device, &pci, nullptr, &cpool);
        if (r != VK_SUCCESS) { log("img-probe: vkCreateCommandPool failed (%d). fail-soft.", (int)r); goto done; }

        VkCommandBufferAllocateInfo cai{};
        cai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool        = cpool;
        cai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        r = vkAllocateCommandBuffers(device, &cai, &cbuf);
        if (r != VK_SUCCESS) { log("img-probe: vkAllocateCommandBuffers failed (%d). fail-soft.", (int)r); goto done; }

        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        r = vkBeginCommandBuffer(cbuf, &bi);
        if (r != VK_SUCCESS) { log("img-probe: vkBeginCommandBuffer failed (%d). fail-soft.", (int)r); goto done; }

        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        // UNDEFINED -> TRANSFER_DST_OPTIMAL (for the clear).
        VkImageMemoryBarrier toDst{};
        toDst.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toDst.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        toDst.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.image               = image;
        toDst.subresourceRange    = range;
        toDst.srcAccessMask       = 0;
        toDst.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cbuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);

        VkClearColorValue clear{};
        clear.float32[0] = 0.25f; clear.float32[1] = 0.5f;
        clear.float32[2] = 0.75f; clear.float32[3] = 1.0f;
        vkCmdClearColorImage(cbuf, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &clear, 1, &range);

        // TRANSFER_DST_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL (the key transition).
        VkImageMemoryBarrier toRead{};
        toRead.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toRead.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toRead.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toRead.image               = image;
        toRead.subresourceRange    = range;
        toRead.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        toRead.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cbuf, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toRead);

        r = vkEndCommandBuffer(cbuf);
        if (r != VK_SUCCESS) { log("img-probe: vkEndCommandBuffer failed (%d). fail-soft.", (int)r); goto done; }

        VkFenceCreateInfo fnci{};
        fnci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        r = vkCreateFence(device, &fnci, nullptr, &fence);
        if (r != VK_SUCCESS) { log("img-probe: vkCreateFence failed (%d). fail-soft.", (int)r); goto done; }

        VkSubmitInfo si{};
        si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers    = &cbuf;
        r = vkQueueSubmit(queue, 1, &si, fence);
        if (r != VK_SUCCESS) { log("img-probe: vkQueueSubmit failed (%d). fail-soft.", (int)r); goto done; }
        r = vkWaitForFences(device, 1, &fence, VK_TRUE, 5ull * 1000 * 1000 * 1000);
        if (r != VK_SUCCESS) { log("img-probe: vkWaitForFences failed/timeout (%d). fail-soft.", (int)r); goto done; }
    }

    // ---- Descriptor set layout: binding0 = COMBINED_IMAGE_SAMPLER -----------
    {
        VkDescriptorSetLayoutBinding bind{};
        bind.binding         = 0;
        bind.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bind.descriptorCount = 1;
        bind.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = 1;
        lci.pBindings    = &bind;
        r = vkCreateDescriptorSetLayout(device, &lci, nullptr, &layout);
        if (r != VK_SUCCESS) { log("img-probe: vkCreateDescriptorSetLayout failed (%d). fail-soft.", (int)r); goto done; }
    }

    // ---- Descriptor pool + set ----------------------------------------------
    {
        VkDescriptorPoolSize size{};
        size.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        size.descriptorCount = 1;

        VkDescriptorPoolCreateInfo pci{};
        pci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets       = 1;
        pci.poolSizeCount = 1;
        pci.pPoolSizes    = &size;
        r = vkCreateDescriptorPool(device, &pci, nullptr, &pool);
        if (r != VK_SUCCESS) { log("img-probe: vkCreateDescriptorPool failed (%d). fail-soft.", (int)r); goto done; }

        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool     = pool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts        = &layout;
        r = vkAllocateDescriptorSets(device, &dsai, &set);
        if (r != VK_SUCCESS) { log("img-probe: vkAllocateDescriptorSets failed (%d). fail-soft.", (int)r); goto done; }
    }

    // ---- Bind the sampled image + sampler via vkUpdateDescriptorSets --------
    {
        VkDescriptorImageInfo imgInfo{};
        imgInfo.sampler     = sampler;
        imgInfo.imageView   = view;
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = set;
        write.dstBinding      = 0;
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo      = &imgInfo;

        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        log("img-probe: descriptor set updated (binding0=COMBINED_IMAGE_SAMPLER, "
            "image 8x8 RGBA8 in SHADER_READ_ONLY; +compare sampler created).");
    }

    // Wiring succeeded. If validation was on, ZERO captured errors is the proof.
    ok = !(wantValidation && g_img_validation_saw_error);
    if (wantValidation) {
        log("img-probe: validation active; saw_error=%d", g_img_validation_saw_error ? 1 : 0);
    }

done:
    if (fence)      vkDestroyFence(device, fence, nullptr);
    if (cpool)      vkDestroyCommandPool(device, cpool, nullptr); // frees cbuf
    if (pool)       vkDestroyDescriptorPool(device, pool, nullptr); // frees the set
    if (layout)     vkDestroyDescriptorSetLayout(device, layout, nullptr);
    if (cmpSampler) vkDestroySampler(device, cmpSampler, nullptr);
    if (sampler)    vkDestroySampler(device, sampler, nullptr);
    if (view)       vkDestroyImageView(device, view, nullptr);
    if (imageMem)   vkFreeMemory(device, imageMem, nullptr);
    if (image)      vkDestroyImage(device, image, nullptr);
    vkDestroyDevice(device, nullptr);
    destroy_messenger();
    vkDestroyInstance(instance, nullptr);
    log("img-probe: %s -- cleaned up (device+instance destroyed).", ok ? "PASS" : "FAIL");
    return ok;
}

bool mc2_vulkan_probe() {
    // VULKAN-VOLK-LOADER-1: open the Vulkan runtime via volk before any vk call.
    // This is the primary fail-soft gate: on an OpenGL-only machine with no
    // Vulkan runtime, volkInitialize() fails here and the probe returns false
    // cleanly (no crash, no missing-DLL load failure of the exe).
    if (!ensure_volk_initialized()) return false;

    // ---- Optional validation layer (MC2_VULKAN_VALIDATION env) --------------
    std::vector<const char*> layers;
    if (std::getenv("MC2_VULKAN_VALIDATION")) {
        uint32_t layerCount = 0;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        std::vector<VkLayerProperties> avail(layerCount);
        if (layerCount) vkEnumerateInstanceLayerProperties(&layerCount, avail.data());
        const char* want = "VK_LAYER_KHRONOS_validation";
        bool found = false;
        for (const auto& lp : avail) {
            if (std::strcmp(lp.layerName, want) == 0) { found = true; break; }
        }
        if (found) {
            layers.push_back(want);
            log("validation layer %s enabled", want);
        } else {
            log("validation requested but %s not available; continuing without", want);
        }
    }

    // ---- VkInstance ----------------------------------------------------------
    VkApplicationInfo app{};
    app.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName   = "MechCommander 2 (Vulkan skeleton probe)";
    app.applicationVersion = VK_MAKE_VERSION(0, 5, 0);
    app.pEngineName        = "MC2-GameOS";
    app.engineVersion      = VK_MAKE_VERSION(0, 5, 0);
    app.apiVersion         = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ici{};
    ici.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo        = &app;
    ici.enabledLayerCount       = static_cast<uint32_t>(layers.size());
    ici.ppEnabledLayerNames     = layers.empty() ? nullptr : layers.data();

    VkInstance instance = VK_NULL_HANDLE;
    VkResult r = vkCreateInstance(&ici, nullptr, &instance);
    if (r != VK_SUCCESS) {
        log("vkCreateInstance failed (VkResult=%d) -- no loader/ICD or unsupported. fail-soft.", (int)r);
        return false;
    }
    // VULKAN-VOLK-LOADER-1: resolve instance-level dispatch through volk.
    volkLoadInstance(instance);

    // ---- Enumerate physical devices -----------------------------------------
    uint32_t devCount = 0;
    r = vkEnumeratePhysicalDevices(instance, &devCount, nullptr);
    if (r != VK_SUCCESS || devCount == 0) {
        log("no physical devices (VkResult=%d, count=%u). fail-soft.", (int)r, devCount);
        vkDestroyInstance(instance, nullptr);
        return false;
    }
    std::vector<VkPhysicalDevice> devs(devCount);
    vkEnumeratePhysicalDevices(instance, &devCount, devs.data());

    log("VkInstance created; %u physical device(s):", devCount);

    for (uint32_t i = 0; i < devCount; ++i) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(devs[i], &props);

        const uint32_t av = props.apiVersion;
        log("  [%u] %s (%s)", i, props.deviceName, device_type_str(props.deviceType));
        log("      apiVersion=%u.%u.%u  driverVersion=0x%08x  vendorID=0x%04x  deviceID=0x%04x",
            VK_VERSION_MAJOR(av), VK_VERSION_MINOR(av), VK_VERSION_PATCH(av),
            props.driverVersion, props.vendorID, props.deviceID);

        const VkPhysicalDeviceLimits& L = props.limits;
        log("      limits: maxImageDim2D=%u  maxUBO=%u  maxSSBO=%u  maxComputeWG=[%u,%u,%u]  maxPushConst=%u",
            L.maxImageDimension2D, L.maxUniformBufferRange, L.maxStorageBufferRange,
            L.maxComputeWorkGroupCount[0], L.maxComputeWorkGroupCount[1], L.maxComputeWorkGroupCount[2],
            L.maxPushConstantsSize);

        // Queue families: report graphics + compute + transfer presence.
        uint32_t qfCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qfCount, nullptr);
        std::vector<VkQueueFamilyProperties> qfs(qfCount);
        if (qfCount) vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qfCount, qfs.data());

        bool hasGraphics = false, hasCompute = false;
        for (uint32_t q = 0; q < qfCount; ++q) {
            if (qfs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) hasGraphics = true;
            if (qfs[q].queueFlags & VK_QUEUE_COMPUTE_BIT)  hasCompute  = true;
        }
        // "present" needs a surface (WSI) to query properly; this skeleton
        // creates no surface, so we report graphics-queue presence as a proxy
        // (a graphics queue can present on all mainstream desktop ICDs).
        log("      queueFamilies=%u  graphics=%d  compute=%d  (present: not probed -- no surface in skeleton)",
            qfCount, hasGraphics ? 1 : 0, hasCompute ? 1 : 0);
    }

    vkDestroyInstance(instance, nullptr);
    log("probe OK; instance destroyed. (no device/surface/swapchain created)");
    return true;
}

bool mc2_vulkan_probe_if_env() {
    if (!std::getenv("MC2_VULKAN_PROBE")) {
        return false; // default: never runs
    }
    log("MC2_VULKAN_PROBE set -- running one-shot probe.");
    bool ok = mc2_vulkan_probe();
    // VULKAN-SHADER-TOOLCHAIN-1: also exercise the SPIR-V shader-module load
    // path on a headless device. spvDir defaults to the exe's working dir;
    // MC2_VULKAN_SPV_DIR overrides (build output holds the compiled .spv).
    const char* spvDir = std::getenv("MC2_VULKAN_SPV_DIR");
    bool shOk = mc2_vulkan_probe_shaders(spvDir ? spvDir : "shaders/vulkan");
    // VULKAN-FULLSCREEN-TRIANGLE-1: capstone -- full headless offscreen render.
    bool triOk = mc2_vulkan_probe_triangle(spvDir ? spvDir : "shaders/vulkan");
    // VULKAN-DESCRIPTOR-SMOKE-1: headless descriptor-set plumbing probe.
    bool descOk = mc2_vulkan_probe_descriptors();
    // VULKAN-SAMPLED-IMAGE-SMOKE-1: headless sampled-image + sampler descriptor.
    bool imgOk = mc2_vulkan_probe_sampled_image();
    log("MC2_VULKAN_PROBE: caps=%d shaders=%d triangle=%d descriptors=%d sampled_image=%d",
        ok ? 1 : 0, shOk ? 1 : 0, triOk ? 1 : 0, descOk ? 1 : 0, imgOk ? 1 : 0);
    return ok && shOk && triOk && descOk && imgOk;
}

#endif // MC2_VULKAN
