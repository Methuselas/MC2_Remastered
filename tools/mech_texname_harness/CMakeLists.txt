# tools/mech_texname_harness/CMakeLists.txt
# SUBSYSTEM-HARNESS-ARC / GLB-TEXNAME-DERIVE-HARNESS-1
#
# STANDALONE build (no main build64/ cache, no vcpkg, no deps):
#   cmake -S tools/mech_texname_harness -B build64-texname -G "Visual Studio 17 2022" -A x64
#   cmake --build build64-texname --config RelWithDebInfo --target mech_texname_harness
#
# Links NO production .cpp and NO external deps — #includes the header-only pure
# name rules (mclib/mech_texname_derive.h, the same header production delegates to).
# No Assimp, no GL.

cmake_minimum_required(VERSION 3.16)
project(mech_texname_harness CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(MC2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(mech_texname_harness mech_texname_harness.cpp)
target_include_directories(mech_texname_harness PRIVATE
    ${MC2_ROOT}/tools/contract_harness_common
    ${MC2_ROOT}/mclib)                 # for "mech_texname_derive.h"
target_compile_features(mech_texname_harness PRIVATE cxx_std_17)
