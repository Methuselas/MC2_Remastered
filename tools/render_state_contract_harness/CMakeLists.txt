# tools/render_state_contract_harness/CMakeLists.txt
# SUBSYSTEM-HARNESS-ARC / RENDER-STATE-CONTRACT-HARNESS-1
#
# STANDALONE build (does NOT touch the main build64/ cache, no vcpkg):
#   cmake -S tools/render_state_contract_harness -B build64-renderstate -G "Visual Studio 17 2022" -A x64
#   cmake --build build64-renderstate --config RelWithDebInfo --target render_state_contract_harness
#   build64-renderstate/RelWithDebInfo/render_state_contract_harness.exe --list
#
# Links the REAL production contract TU (mclib/render_contract.cpp +
# RenderCore/RenderResourceRegistry.cpp) — same set as tests/unit/
# test_render_contract_3.cpp — so the harness exercises production logic, not a
# copy. render_contract.cpp includes <GL/glew.h> for types, so glew32+opengl32
# are link-time deps; the scope/order paths under test make NO GL calls and need
# NO GL context. No game window, no mission data, no ObjectManager.

cmake_minimum_required(VERSION 3.16)
project(render_state_contract_harness CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(MC2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(render_state_contract_harness
    render_state_contract_harness.cpp
    ${MC2_ROOT}/mclib/render_contract.cpp
    ${MC2_ROOT}/RenderCore/RenderResourceRegistry.cpp
)

target_include_directories(render_state_contract_harness PRIVATE
    ${MC2_ROOT}/tools/contract_harness_common
    ${MC2_ROOT}                       # "RenderCore/RenderPassContract.h" style
    ${MC2_ROOT}/mclib
    ${MC2_ROOT}/RenderCore
    ${MC2_ROOT}/GameOS
    ${MC2_ROOT}/3rdparty/include
)

# Mirror the existing unit-test target's flags (LINUX_BUILD trims Windows GL/d3d
# pulls in render_contract.cpp; _CRT_SECURE_NO_WARNINGS for getenv/_putenv_s).
target_compile_definitions(render_state_contract_harness PRIVATE
    LINUX_BUILD
    _CRT_SECURE_NO_WARNINGS
)

target_link_libraries(render_state_contract_harness PRIVATE
    ${MC2_ROOT}/3rdparty/lib/x64/glew32.lib
    opengl32
)

target_compile_features(render_state_contract_harness PRIVATE cxx_std_17)
