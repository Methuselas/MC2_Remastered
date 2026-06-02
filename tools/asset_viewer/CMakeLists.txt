# MC2 Asset Viewer standalone target.
#
# Keep SDL include resolution local to this target. The ImGui SDL backend
# includes "SDL.h", so the SDL2 include folder itself must be on the
# include path, not just the parent include directory.

set(ASSET_VIEWER_SOURCES
    main.cpp
    AssetViewerApp.cpp
    SphereMesh.cpp
    LocalPbrMaterialBackend.cpp
    MaterialTextureLoader.cpp
    # remaining sources added in later tasks
    TextureExtensions.cpp
    TextureMetadata.cpp
    TexturePreview2D.cpp
    FileBrowser.cpp
    AssetTypeSidebar.cpp
    TextureInspectorPanel.cpp
    TextureDecoderRegistry.cpp
    LegacyImageDecoder.cpp
    Ktx2Decoder.cpp
    "${CMAKE_SOURCE_DIR}/RenderCore/KtxLoader.cpp"
    "${CMAKE_SOURCE_DIR}/ui_editor/UiEditorImageCache.cpp"
    "${CMAKE_SOURCE_DIR}/GameOS/gameos/utils/Image.cpp"
)

add_executable(mc2_asset_viewer ${ASSET_VIEWER_SOURCES})

target_compile_features(mc2_asset_viewer PRIVATE cxx_std_17)
target_compile_definitions(mc2_asset_viewer PRIVATE IMGUI_IMPL_OPENGL_LOADER_GLEW)

if(NOT TARGET SDL2::SDL2)
    find_package(SDL2 CONFIG REQUIRED)
endif()

if(NOT TARGET OpenGL::GL)
    find_package(OpenGL REQUIRED)
endif()

if(NOT TARGET GLEW::GLEW)
    find_package(GLEW REQUIRED)
endif()

set(MC2R_AV_SDL_INCLUDE_DIRS)

if(DEFINED SDL2_INCLUDE_DIRS)
    list(APPEND MC2R_AV_SDL_INCLUDE_DIRS ${SDL2_INCLUDE_DIRS})
endif()

if(DEFINED SDL2_INCLUDE_DIR)
    list(APPEND MC2R_AV_SDL_INCLUDE_DIRS ${SDL2_INCLUDE_DIR})
endif()

foreach(_mc2r_prefix IN LISTS CMAKE_PREFIX_PATH)
    if(EXISTS "${_mc2r_prefix}/include/SDL2/SDL.h")
        list(APPEND MC2R_AV_SDL_INCLUDE_DIRS "${_mc2r_prefix}/include/SDL2")
    endif()

    if(EXISTS "${_mc2r_prefix}/include/SDL.h")
        list(APPEND MC2R_AV_SDL_INCLUDE_DIRS "${_mc2r_prefix}/include")
    endif()
endforeach()

if(DEFINED VCPKG_TARGET_TRIPLET)
    foreach(_mc2r_av_vcpkg_root
        "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}"
        "${CMAKE_SOURCE_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}"
    )
        if(EXISTS "${_mc2r_av_vcpkg_root}/include/SDL2/SDL.h")
            list(APPEND MC2R_AV_SDL_INCLUDE_DIRS "${_mc2r_av_vcpkg_root}/include/SDL2")
        endif()

        if(EXISTS "${_mc2r_av_vcpkg_root}/include/SDL.h")
            list(APPEND MC2R_AV_SDL_INCLUDE_DIRS "${_mc2r_av_vcpkg_root}/include")
        endif()
    endforeach()
endif()

# Common vcpkg layout for this repo's build64 tree.
foreach(_mc2r_av_vcpkg_root
    "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows"
    "${CMAKE_BINARY_DIR}/vcpkg_installed/x86-windows"
    "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-linux"
)
    if(EXISTS "${_mc2r_av_vcpkg_root}/include/SDL2/SDL.h")
        list(APPEND MC2R_AV_SDL_INCLUDE_DIRS "${_mc2r_av_vcpkg_root}/include/SDL2")
    endif()

    if(EXISTS "${_mc2r_av_vcpkg_root}/include/SDL.h")
        list(APPEND MC2R_AV_SDL_INCLUDE_DIRS "${_mc2r_av_vcpkg_root}/include")
    endif()
endforeach()

if(MC2R_AV_SDL_INCLUDE_DIRS)
    list(REMOVE_DUPLICATES MC2R_AV_SDL_INCLUDE_DIRS)
endif()

target_include_directories(mc2_asset_viewer PRIVATE
    "${CMAKE_SOURCE_DIR}/3rdparty/imgui"
    "${CMAKE_SOURCE_DIR}/3rdparty/imgui/backends"
    "${CMAKE_SOURCE_DIR}/3rdparty/stb"
    "${CMAKE_SOURCE_DIR}/GameOS/gameos"
    "${CMAKE_SOURCE_DIR}/ui_editor"
    "${CMAKE_SOURCE_DIR}/RenderCore"
    ${MC2R_AV_SDL_INCLUDE_DIRS}
)

target_link_libraries(mc2_asset_viewer PRIVATE imgui)

if(TARGET SDL2::SDL2)
    target_link_libraries(mc2_asset_viewer PRIVATE SDL2::SDL2)
elseif(DEFINED SDL2_LIBRARIES)
    target_link_libraries(mc2_asset_viewer PRIVATE ${SDL2_LIBRARIES})
else()
    message(FATAL_ERROR "SDL2 target or SDL2_LIBRARIES was not found for mc2_asset_viewer")
endif()

if(TARGET SDL2::SDL2main)
    target_link_libraries(mc2_asset_viewer PRIVATE SDL2::SDL2main)
endif()

target_link_libraries(mc2_asset_viewer PRIVATE OpenGL::GL)

if(WIN32)
    target_link_libraries(mc2_asset_viewer PRIVATE ole32 windowscodecs uuid)
endif()

target_link_libraries(mc2_asset_viewer PRIVATE GLEW::GLEW)
