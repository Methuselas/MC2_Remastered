# tools/icon_atlas_harness/CMakeLists.txt
# SUBSYSTEM-HARNESS-ARC / ICON-ATLAS-HARNESS-1
#
# STANDALONE build (does NOT touch the main build64/ cache, no vcpkg, no deps):
#   cmake -S tools/icon_atlas_harness -B build64-iconatlas -G "Visual Studio 17 2022" -A x64
#   cmake --build build64-iconatlas --config RelWithDebInfo --target icon_atlas_harness
#   build64-iconatlas/RelWithDebInfo/icon_atlas_harness.exe --list
#
# Links NO production .cpp and NO external deps — it #includes the header-only
# pure math (code/icon_atlas_cell.h, the same header the production call sites
# include). The linker is the proof it stays game-free.

cmake_minimum_required(VERSION 3.16)
project(icon_atlas_harness CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(MC2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(icon_atlas_harness icon_atlas_harness.cpp)
target_include_directories(icon_atlas_harness PRIVATE
    ${MC2_ROOT}/tools/contract_harness_common
    ${MC2_ROOT}/code)                  # for "icon_atlas_cell.h"
target_compile_features(icon_atlas_harness PRIVATE cxx_std_17)
