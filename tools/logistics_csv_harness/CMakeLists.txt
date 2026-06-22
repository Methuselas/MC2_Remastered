# tools/logistics_csv_harness/CMakeLists.txt
# SUBSYSTEM-HARNESS-ARC / LOGISTICS-CSV-TOKENIZER-HARNESS-1
#
# STANDALONE build (does NOT touch the main build64/ cache, no vcpkg, no deps):
#   cmake -S tools/logistics_csv_harness -B build64-logcsv -G "Visual Studio 17 2022" -A x64
#   cmake --build build64-logcsv --config RelWithDebInfo --target logistics_csv_harness
#
# Links NO production .cpp and NO external deps — it #includes the header-only
# tokenizer (code/logistics_csv.h, the same header production delegates to). The
# linker is the proof it stays game-free.

cmake_minimum_required(VERSION 3.16)
project(logistics_csv_harness CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(MC2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(logistics_csv_harness logistics_csv_harness.cpp)
target_include_directories(logistics_csv_harness PRIVATE
    ${MC2_ROOT}/tools/contract_harness_common
    ${MC2_ROOT}/code)                  # for "logistics_csv.h"
target_compile_features(logistics_csv_harness PRIVATE cxx_std_17)
