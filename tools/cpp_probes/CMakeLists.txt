# Offline asset-pipeline probe host tools (ASSIMP-IMPORTER-PHASE-0 +).
#
# These are OFFLINE tooling: they link ONLY assimp::assimp (and, in a later
# slice, meshoptimizer). They MUST NOT link mclib, gameos, SDL2, GLEW, or any
# runtime/renderer target. Gated by ENABLE_ASSET_CPP_PROBES in the root
# CMakeLists.txt; this subdir is only added when that option is ON.

# assimp_probe — import a model via Assimp and emit a geometry-summary JSON
# matching the manifest `geometry` section (tools/validate_asset_manifest.py).
add_executable(assimp_probe assimp_probe.cpp)
target_link_libraries(assimp_probe PRIVATE assimp::assimp)
target_compile_features(assimp_probe PRIVATE cxx_std_17)

# A later slice will add:
#   add_executable(meshopt_lod_probe meshopt_lod_probe.cpp)
#   target_link_libraries(meshopt_lod_probe PRIVATE meshoptimizer)
