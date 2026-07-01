// VULKAN-BACKEND-SKELETON-1 -- first Vulkan bootstrap slice.
// Entire TU compiles to nothing unless MC2_VULKAN is defined (CMake option,
// default OFF). Fail-soft everywhere: any error logs a reason + returns false.
// NO swapchain, NO surface, NO logical device, NO window, NO render path.

#include "vulkan_backend_skeleton.h"

#ifdef MC2_VULKAN

#include <vulkan/vulkan.h>

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

bool mc2_vulkan_probe() {
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
    log("MC2_VULKAN_PROBE: caps=%d shaders=%d", ok ? 1 : 0, shOk ? 1 : 0);
    return ok && shOk;
}

#endif // MC2_VULKAN
