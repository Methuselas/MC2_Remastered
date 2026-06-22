# tools/objmgr_contract_harness/CMakeLists.txt
# OBJMGR-CONTRACT-HARNESS-1 — GameObjectManager watch-list policy contract harness.
#
# STANDALONE build (does NOT touch the main build64/ cache, no vcpkg, no deps):
#   cmake -S tools/objmgr_contract_harness -B build64-objmgr -G "Visual Studio 17 2022" -A x64
#   cmake --build build64-objmgr --config RelWithDebInfo --target objmgr_contract_harness
#   build64-objmgr/RelWithDebInfo/objmgr_contract_harness.exe --list
#
# Links ONLY the header-only watch policy (code/objmgr_watch_policy.h) plus the
# contract_harness framework — NO objmgr.cpp, no gameos, no GL. The linker is the
# proof it stays game-free.

cmake_minimum_required(VERSION 3.16)
project(objmgr_contract_harness CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(MC2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(objmgr_contract_harness objmgr_contract_harness.cpp)
target_include_directories(objmgr_contract_harness PRIVATE
    ${MC2_ROOT}/tools/contract_harness_common
    ${MC2_ROOT}/code)
target_compile_features(objmgr_contract_harness PRIVATE cxx_std_17)
