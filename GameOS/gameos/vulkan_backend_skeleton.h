// VULKAN-BACKEND-SKELETON-1
// First Vulkan bootstrap slice. Compile-gated by CMake MC2_VULKAN (default OFF).
// When OFF, this header/TU compiles to nothing and no Vulkan dep is pulled in.
//
// mc2_vulkan_probe(): fail-soft. Creates a VkInstance, enumerates physical
// devices, dumps capabilities, destroys the instance. Returns true on success,
// false (with a logged reason) on ANY failure. NO swapchain, NO surface, NO
// device creation, NO window takeover, NO game render path.
//
// Runtime gate: even in an MC2_VULKAN=ON build the probe only runs when the
// MC2_VULKAN_PROBE env var is set. It is NOT wired into the frame loop.

#ifndef MC2_VULKAN_BACKEND_SKELETON_H
#define MC2_VULKAN_BACKEND_SKELETON_H

#ifdef MC2_VULKAN

// Returns true if a VkInstance was created, at least one physical device was
// enumerated, and the capability dump completed. Fail-soft: logs + returns
// false on any error.
bool mc2_vulkan_probe();

// Runs mc2_vulkan_probe() only if the MC2_VULKAN_PROBE env var is set.
// No-op (returns false) otherwise. Safe to call once at startup.
bool mc2_vulkan_probe_if_env();

#endif // MC2_VULKAN

#endif // MC2_VULKAN_BACKEND_SKELETON_H
