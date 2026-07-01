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
    return mc2_vulkan_probe();
}

#endif // MC2_VULKAN
