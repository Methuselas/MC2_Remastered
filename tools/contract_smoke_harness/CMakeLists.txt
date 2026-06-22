# tools/contract_smoke_harness/CMakeLists.txt
# SUBSYSTEM-HARNESS-ARC / HARNESS-TEMPLATE-1 — reference contract harness.
#
# STANDALONE build (does NOT touch the main build64/ cache, no vcpkg, no deps):
#   cmake -S tools/contract_smoke_harness -B build64-contract -G "Visual Studio 17 2022" -A x64
#   cmake --build build64-contract --config RelWithDebInfo --target contract_smoke_harness
#   build64-contract/RelWithDebInfo/contract_smoke_harness.exe --list
#
# This harness links NO production code and pulls NO external dependencies — the
# linker is the proof it stays game-free. It is the copy-me template for real
# subsystem harnesses; those add the smallest production TU subset they need.

cmake_minimum_required(VERSION 3.16)
project(contract_smoke_harness CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(MC2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(contract_smoke_harness contract_smoke_harness.cpp)
target_include_directories(contract_smoke_harness PRIVATE
    ${MC2_ROOT}/tools/contract_harness_common)
target_compile_features(contract_smoke_harness PRIVATE cxx_std_17)
