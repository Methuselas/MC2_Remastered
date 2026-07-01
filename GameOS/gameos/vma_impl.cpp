// VULKAN-VMA-INTEGRATION-1 -- the single translation unit that instantiates the
// AMD Vulkan Memory Allocator (VMA) implementation. VMA is header-only: exactly
// ONE TU in the whole program must define VMA_IMPLEMENTATION before including
// the header; every other user includes vk_mem_alloc.h without that define.
//
// Compiled ONLY under MC2_VULKAN (see GameOS/gameos/CMakeLists.txt and the
// vulkan_shader_probe target in the root CMakeLists). When MC2_VULKAN is OFF
// this TU is not added to any target -> the normal GL build is byte-identical
// and pulls in neither VMA nor the Vulkan headers.

#ifdef MC2_VULKAN

// We link Vulkan::Vulkan statically (the SDK loader import lib), so let VMA call
// the core Vulkan entry points directly rather than fetching them at runtime.
#define VMA_STATIC_VULKAN_FUNCTIONS  1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0

// VMA is generated code and trips a handful of MSVC /W4 warnings (unreferenced
// params, nameless struct/union, conditional-expression-is-constant, unused
// locals). Silence them locally so they don't pollute the ON build; this does
// not affect any other TU.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4100) // unreferenced formal parameter
#pragma warning(disable : 4127) // conditional expression is constant
#pragma warning(disable : 4189) // local variable initialized but not referenced
#pragma warning(disable : 4201) // nonstandard: nameless struct/union
#pragma warning(disable : 4324) // structure was padded due to alignment specifier
#endif

#define VMA_IMPLEMENTATION
#include "vk_mem_alloc.h"

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#endif // MC2_VULKAN
