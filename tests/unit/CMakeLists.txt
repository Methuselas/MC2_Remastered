# tests/unit/CMakeLists.txt
# doctest-based unit tests for pure-logic MC2 code.
# Tier 2 of docs/testing-strategy.md.
#
# STANDALONE BUILD (preferred -- leaves the main build64/ cache untouched):
#   cmake -S tests/unit -B build64-tests -G "Visual Studio 17 2022" -A x64
#   cmake --build build64-tests --config RelWithDebInfo --target mc2_tests
#   build64-tests/RelWithDebInfo/mc2_tests.exe
#
# Pure-logic only -- the test target deliberately avoids pulling in OpenGL,
# SDL, FFmpeg, or file I/O. Anything dragging those in either:
#   (a) gets factored into a header-only / leaf TU first (preferred --
#       most candidates here are already small files that could land that
#       way), or
#   (b) gets a narrowly-scoped target_sources entry that picks ONE .cpp
#       from the engine without its transitive .cpp neighbors.
#
# Vulkan-prep angle: the more units we get into a no-GL-context test
# target, the more code that survives the eventual RHI port unchanged.
# Tests of pure CPU logic outlive every renderer rewrite.

cmake_minimum_required(VERSION 3.16)
project(mc2_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Resolve the worktree root (this CMakeLists lives at <root>/tests/unit/).
get_filename_component(MC2_WORKTREE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(mc2_tests
    test_main.cpp           # owns DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
    test_hashing.cpp
    test_projection.cpp     # currently scope-stopped; placeholder skipped case
    test_argb_pack.cpp
    test_screen_pick.cpp
    test_rendercore.cpp     # RENDERCORE-UNIT-TESTS-1: GL-free substrate invariants
    test_frame_arena.cpp    # FRAME-ARENA-1: FrameArena typed L2 allocator invariants
    test_depth_hzb.cpp      # HZB-DEPTH-CONVENTION-TESTS-1: reverse-Z / HZB reduction contract
    test_host_services.cpp  # ENGINE-HOST-SERVICES-0: host-service contract + default impls

    # Leaf-TU engine sources (no transitive engine include drag).
    ${MC2_WORKTREE_ROOT}/mclib/fst_hash.cpp
    ${MC2_WORKTREE_ROOT}/RenderCore/RenderResourceRegistry.cpp
    ${MC2_WORKTREE_ROOT}/RenderCore/RenderDebugView.cpp
    ${MC2_WORKTREE_ROOT}/HostServices/DefaultHostServices.cpp  # ENGINE-HOST-SERVICES-0
)

target_include_directories(mc2_tests PRIVATE
    ${MC2_WORKTREE_ROOT}/mclib
    ${MC2_WORKTREE_ROOT}/GameOS
    ${MC2_WORKTREE_ROOT}/RenderWorld
    ${MC2_WORKTREE_ROOT}/RenderCore
    ${MC2_WORKTREE_ROOT}/HostServices
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Match main build's preprocessor environment so engine headers parse the
# same way they do in production. LINUX_BUILD is the load-bearing one
# (memory/mc2_path_separator_linux_build.md); the engine builds with it
# globally on Windows too.
target_compile_definitions(mc2_tests PRIVATE
    LINUX_BUILD
    _CRT_SECURE_NO_WARNINGS
)

if(MSVC)
    target_compile_options(mc2_tests PRIVATE /W4 /WX-)
endif()

# Make `cmake --build ... --target check` run the tests after build.
add_custom_target(check
    COMMAND mc2_tests
    DEPENDS mc2_tests
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running mc2 unit tests"
)

# Convenience target: run only the RenderCore test suite.
add_custom_target(rendercore_tests
    COMMAND mc2_tests --ts=RenderCore
    DEPENDS mc2_tests
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running RenderCore substrate unit tests"
)
