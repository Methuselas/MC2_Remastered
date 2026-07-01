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

#include <vulkan/vulkan.h>

// VULKAN-SHADER-TOOLCHAIN-1: fail-soft SPIR-V shader-module helpers.
//
// mc2_vulkan_load_spv(): reads a .spv file, vkCreateShaderModule. Returns
// VK_NULL_HANDLE on ANY error (missing file, bad size, create fail) -- logged.
// mc2_vulkan_free_shader(): vkDestroyShaderModule (no-op on VK_NULL_HANDLE).
// No pipeline, no render -- module create/destroy only.
VkShaderModule mc2_vulkan_load_spv(VkDevice device, const char* path);
void mc2_vulkan_free_shader(VkDevice device, VkShaderModule module);

// VULKAN-SHADER-TOOLCHAIN-1: probe extension. Creates a minimal headless
// VkDevice (no surface/swapchain), loads the compiled fullscreen.vert.spv +
// fullscreen.frag.spv into VkShaderModules, reports, destroys everything.
// Fail-soft: logs + returns false on any error. spvDir = directory holding the
// compiled .spv files (build output). Runs only from mc2_vulkan_probe_if_env
// when MC2_VULKAN_PROBE is set.
bool mc2_vulkan_probe_shaders(const char* spvDir);

// Returns true if a VkInstance was created, at least one physical device was
// enumerated, and the capability dump completed. Fail-soft: logs + returns
// false on any error.
bool mc2_vulkan_probe();

// Runs mc2_vulkan_probe() only if the MC2_VULKAN_PROBE env var is set.
// No-op (returns false) otherwise. Safe to call once at startup.
bool mc2_vulkan_probe_if_env();

#endif // MC2_VULKAN

#endif // MC2_VULKAN_BACKEND_SKELETON_H
