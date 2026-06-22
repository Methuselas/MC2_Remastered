# tools/mech_import_harness/CMakeLists.txt
# MECH-IMPORT-HARNESS-1A — game-free Assimp inspector/validator for mech GLB/FBX.
#
# Slice 1A links ONLY assimp::assimp. NO mclib, NO gameos, NO renderer, NO stubs,
# NO TG types. The linker is the proof it stays game-free. (Slices 1B/1C will add
# the mclib subset + stubs once FK / TG conversion is exercised.)
#
# STANDALONE BUILD (does not touch the main build64/ cache, no vcpkg):
#   cmake -S tools/mech_import_harness -B build64-harness -G "Visual Studio 17 2022" -A x64
#   cmake --build build64-harness --config RelWithDebInfo --target mech_import_harness
#   build64-harness/RelWithDebInfo/mech_import_harness.exe inspect  <model.glb>
#   build64-harness/RelWithDebInfo/mech_import_harness.exe validate <model.glb>

cmake_minimum_required(VERSION 3.16)
project(mech_import_harness CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(MC2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# Vendored Assimp (same flags as the root build's ENABLE_ASSIMP_IMPORTER block) —
# FBX + GLTF importers only, bundled zlib, static. Brought in EXCLUDE_FROM_ALL so
# only the harness target pulls it.
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_EXPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_FBX_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_GLTF_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ZLIB ON CACHE BOOL "" FORCE)
set(ZLIB_FOUND FALSE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
add_subdirectory(${MC2_ROOT}/3rdparty/assimp ${CMAKE_BINARY_DIR}/out/3rdparty/assimp EXCLUDE_FROM_ALL)

add_executable(mech_import_harness mech_import_harness.cpp)
target_link_libraries(mech_import_harness PRIVATE assimp::assimp)
target_compile_features(mech_import_harness PRIVATE cxx_std_17)
