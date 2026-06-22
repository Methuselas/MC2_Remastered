# tools/camera_frustum_harness/CMakeLists.txt
# SUBSYSTEM-HARNESS-ARC / CAMERA-FRUSTUM-HARNESS-1
#
# STANDALONE build (no main build64/ cache, no vcpkg, no deps):
#   cmake -S tools/camera_frustum_harness -B build64-camfr -G "Visual Studio 17 2022" -A x64
#   cmake --build build64-camfr --config RelWithDebInfo --target camera_frustum_harness
#
# Links NO production .cpp and NO external deps — #includes the header-only pure
# math (mclib/camera_frustum_math.h, the same header production delegates to).

cmake_minimum_required(VERSION 3.16)
project(camera_frustum_harness CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(MC2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(camera_frustum_harness camera_frustum_harness.cpp)
target_include_directories(camera_frustum_harness PRIVATE
    ${MC2_ROOT}/tools/contract_harness_common
    ${MC2_ROOT}/mclib)                 # for "camera_frustum_math.h"
target_compile_features(camera_frustum_harness PRIVATE cxx_std_17)
