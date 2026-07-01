// VULKAN-SHADER-TOOLCHAIN-1 -- standalone probe harness.
// Compiled ONLY under MC2_VULKAN (see CMake). Links the Vulkan loader + the
// skeleton TU. No GL, no SDL, no window, no game. Runs the headless
// shader-module load probe against the compiled fullscreen .spv files.
//
// Usage: vulkan_shader_probe [spv-dir]   (default arg: shaders/vulkan)
// Exit 0 = both fullscreen shader modules created+destroyed OK; 1 otherwise.
#include "vulkan_backend_skeleton.h"

#include <cstdio>

int main(int argc, char** argv) {
    const char* spvDir = (argc > 1) ? argv[1] : "shaders/vulkan";
    std::printf("[vulkan_shader_probe] spvDir=%s\n", spvDir);
    bool caps    = mc2_vulkan_probe();
    bool shaders = mc2_vulkan_probe_shaders(spvDir);
    std::printf("[vulkan_shader_probe] caps=%d shaders=%d\n", caps ? 1 : 0, shaders ? 1 : 0);
    return (caps && shaders) ? 0 : 1;
}
