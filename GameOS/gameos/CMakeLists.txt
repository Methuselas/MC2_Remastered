cmake_minimum_required (VERSION 3.10)
project(gameos)

set(SOURCES ${SOURCES}
    gameos.cpp
    gameos_graphics.cpp
    gameos_res.cpp
    MC2Strings.cpp
    gameos_fileio.cpp
    gameos_input.cpp
    gameos_debugging.cpp
    gameos_sound.cpp
    gos_render.cpp
    gos_postprocess.cpp
    gos_static_prop_batcher.cpp
    gos_static_prop_registry.cpp
    gos_particle_bridge.cpp
    gos_mech_batcher.cpp
    gos_object_recon_tracy.cpp
    gos_terrain_patch_stream.cpp
    gos_terrain_water_stream.cpp
    gos_terrain_indirect.cpp
    gos_terrain_surface.cpp
    gos_terrain_mask_dispatch.cpp
    gos_terrain_lighting.cpp
    gos_object_parity.cpp
    gos_validate.cpp
    gos_screenshot.cpp
    gos_visual_diff.cpp
    gos_rdoc_capture.cpp
    gos_crashbundle.cpp
    gos_smoke.cpp
    debug_renderer.cpp
    gos_font.cpp
    gos_input.cpp
    asset_scale.cpp
    gpu_driven_common.cpp
    gpu_cull_record.cpp
    gpu_cull_substrate.cpp
    gpu_cull_parity.cpp
    gpu_cull_compute.cpp
    gpu_cull_readback.cpp

    utils/stream.cpp
    utils/camera.cpp
    utils/gl_utils.cpp
    utils/gl_render_constants.cpp
    utils/Image.cpp
    utils/logging.cpp
    utils/shader_builder.cpp
    utils/matrix.cpp
    utils/vec.cpp
    utils/timing.cpp
    ${CMAKE_SOURCE_DIR}/3rdparty/tracy/TracyClient.cpp
    utils/string_utils.cpp
    utils/file_utils.cpp
    )

set(MAIN_SRC gameosmain.cpp)

#find_package(SDL2 REQUIRED PATHS "/opt" NO_DEFAULT_PATH)
#include_directories("${SDL2_PREFIX}/include")

include_directories("../include"  ".")
include_directories(${THIRDPARTY_INCLUDE_DIRS})
# gos_static_prop_batcher.cpp pulls tgl.h / msl.h / Stuff from mclib.
include_directories("${CMAKE_SOURCE_DIR}/mclib" "${CMAKE_SOURCE_DIR}/mclib/stuff")

add_library(gameos ${SOURCES})

# gameos_editor: same sources as gameos minus gos_render.cpp (the GL
# renderer main). The MC2 Mission Editor (EditRel) links this instead of
# gameos to avoid pulling in a second renderer init. From engine-standalone PR #36.
set(GAMEOS_EDITOR_SOURCES ${SOURCES})
list(REMOVE_ITEM GAMEOS_EDITOR_SOURCES gos_render.cpp)
add_library(gameos_editor ${GAMEOS_EDITOR_SOURCES})

add_library(gameos_main ${MAIN_SRC})

