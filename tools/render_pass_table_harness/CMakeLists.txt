# tools/render_pass_table_harness/CMakeLists.txt
# SUBSYSTEM-HARNESS-ARC / RENDER-PASS-TABLE-HARNESS-1
#
# STANDALONE build (does NOT touch the main build64/ cache, no vcpkg, no deps):
#   cmake -S tools/render_pass_table_harness -B build64-rptable -G "Visual Studio 17 2022" -A x64
#   cmake --build build64-rptable --config RelWithDebInfo --target render_pass_table_harness
#   build64-rptable/RelWithDebInfo/render_pass_table_harness.exe --list
#
# Links NO production .cpp and NO external deps — it #includes the header-only
# constexpr tables (RenderCore/RenderPassContract.h + RenderResourceRegistry.h,
# both "no GL, no game-side includes"). The linker is the proof it stays
# game-free. Cheapest arc tier (no glew/opengl link, unlike render_state).

cmake_minimum_required(VERSION 3.16)
project(render_pass_table_harness CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(MC2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(render_pass_table_harness render_pass_table_harness.cpp)
target_include_directories(render_pass_table_harness PRIVATE
    ${MC2_ROOT}/tools/contract_harness_common
    ${MC2_ROOT})                       # for "RenderCore/RenderPassContract.h" style
target_compile_features(render_pass_table_harness PRIVATE cxx_std_17)
