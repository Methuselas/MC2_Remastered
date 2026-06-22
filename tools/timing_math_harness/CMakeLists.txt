# tools/timing_math_harness/CMakeLists.txt
# SUBSYSTEM-HARNESS-ARC / GAMEOS-TIMING-MATH-HARNESS-1
#
# STANDALONE build (no main build64/ cache, no vcpkg, no deps):
#   cmake -S tools/timing_math_harness -B build64-timing -G "Visual Studio 17 2022" -A x64
#   cmake --build build64-timing --config RelWithDebInfo --target timing_math_harness
#
# Links NO production .cpp and NO external deps — #includes the header-only pure
# math (GameOS/gameos/utils/timing_math.h, the same header production delegates to).

cmake_minimum_required(VERSION 3.16)
project(timing_math_harness CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(MC2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(timing_math_harness timing_math_harness.cpp)
target_include_directories(timing_math_harness PRIVATE
    ${MC2_ROOT}/tools/contract_harness_common
    ${MC2_ROOT}/GameOS/gameos/utils)        # for "timing_math.h"
target_compile_features(timing_math_harness PRIVATE cxx_std_17)
