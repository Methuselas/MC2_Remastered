<<<<<<< Updated upstream
set(VIEWER_SOURCES 
    "../gui/aanimobject.cpp"
    "../code/gamesound.cpp"
    "../code/logisticscomponent.cpp"
    "../code/logisticsdata.cpp"
    "../code/logisticsmech.cpp"
    "../code/logisticsmissioninfo.cpp"
    "../code/logisticspilot.cpp"
    "../code/logisticsvariant.cpp"
    "../code/mechlopedia.cpp"
    "../code/mission2.cpp"
    "../code/prefs.cpp"
    "../code/radio.cpp"
    "../code/simplecamera.cpp"
    "../code/simplecomponentlistbox.cpp"
    #    "../strings.res.cpp"
    "View.cpp"
    "ViewerHostGlobals.cpp"
    )

add_definitions(-DVIEWER=1)
#add_definitions(-D_ARMOR)
#add_definitions(-D_DEBUG)

include_directories("./window" ${COMMON_INCLUDE_DIRS} "../mclib" "../gui" "../code" "../netlib")
include_directories(${CMAKE_PREFIX_PATH}/include)

add_executable(viewer ${VIEWER_SOURCES})
target_link_libraries(viewer
    mclib
    fx_trace
    particles
    gosfx
    mlr
    stuff
    gui
    gameos
    gameos_main
    windows
    rendercore
    renderworld
    gameadapters
    ZLIB::ZLIB
    ${SDL2_LIBRARIES}
    GLEW::GLEW
    SDL2_mixer::SDL2_mixer
    ${ADDITIONAL_LIBS}
    OpenGL::GL
)

=======
set(VIEWER_SOURCES 
    "../gui/aanimobject.cpp"
    "../code/gamesound.cpp"
    "../code/logisticscomponent.cpp"
    "../code/logisticsdata.cpp"
    "../code/logisticsmech.cpp"
    "../code/logisticsmissioninfo.cpp"
    "../code/logisticspilot.cpp"
    "../code/logisticsvariant.cpp"
    "../code/mechlopedia.cpp"
    "../code/mission2.cpp"
    "../code/prefs.cpp"
    "../code/radio.cpp"
    "../code/simplecamera.cpp"
    "../code/simplecomponentlistbox.cpp"
    #    "../strings.res.cpp"
    "View.cpp"
    "ViewerImgui.cpp"
    "ViewerGuiFit.cpp"
    "ViewerFontCatalog.cpp"
    "ViewerVersion.cpp"
    "ViewerVersion.h"
    "ViewerTextureLoader.cpp"
    )

add_definitions(-DVIEWER=1)
#add_definitions(-D_ARMOR)
#add_definitions(-D_DEBUG)

include_directories("./window" ${COMMON_INCLUDE_DIRS} "../mclib" "../gui" "../code" "../netlib")
include_directories(${CMAKE_PREFIX_PATH}/include)

add_executable(viewer ${VIEWER_SOURCES})

if(WIN32)
    enable_language(RC)
    target_sources(viewer PRIVATE "Viewer.rc")
endif()
target_link_libraries(viewer mclib gosfx mlr stuff gui gameos gameos_main windows ZLIB::ZLIB ${SDL2_LIBRARIES} GLEW::GLEW SDL2_mixer::SDL2_mixer ${ADDITIONAL_LIBS} OpenGL::GL)


set(IMGUI_ROOT "${CMAKE_SOURCE_DIR}/3rdparty/imgui")
if(EXISTS "${IMGUI_ROOT}/imgui.cpp")
    target_sources(viewer PRIVATE
        "${IMGUI_ROOT}/imgui.cpp"
        "${IMGUI_ROOT}/imgui_draw.cpp"
        "${IMGUI_ROOT}/imgui_tables.cpp"
        "${IMGUI_ROOT}/imgui_widgets.cpp"
        "${IMGUI_ROOT}/backends/imgui_impl_opengl3.cpp"
    )

    target_include_directories(viewer PRIVATE
        "${IMGUI_ROOT}"
        "${IMGUI_ROOT}/backends"
    )
else()
    message(FATAL_ERROR "ImGui source not found at ${IMGUI_ROOT}. Expected 3rdparty/imgui.")
endif()


set(STB_ROOT "${CMAKE_SOURCE_DIR}/3rdparty/stb")
if(EXISTS "${STB_ROOT}/stb_image.h")
    target_include_directories(viewer PRIVATE "${STB_ROOT}")
else()
    message(FATAL_ERROR "stb_image.h not found at ${STB_ROOT}. Expected 3rdparty/stb.")
endif()

# Runtime font assets. Fonts are shipped as art assets and referenced by fonts.fit.
set(MC2R_RUNTIME_FONT_DIR "${CMAKE_SOURCE_DIR}/assets/graphics/fonts")
if(EXISTS "${MC2R_RUNTIME_FONT_DIR}")
    add_custom_command(TARGET viewer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:viewer>/assets/graphics/fonts"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${MC2R_RUNTIME_FONT_DIR}"
            "$<TARGET_FILE_DIR:viewer>/assets/graphics/fonts"
        COMMENT "Copying MC2R runtime fonts for Viewer")
endif()


# Prototype GUI FIT data for Viewer ImGui/FIT bridge.
set(MC2R_RUNTIME_UI_DEFS_DIR "${CMAKE_SOURCE_DIR}/data/defs/ui")
if(EXISTS "${MC2R_RUNTIME_UI_DEFS_DIR}")
    add_custom_command(TARGET viewer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${MC2R_RUNTIME_UI_DEFS_DIR}"
            "$<TARGET_FILE_DIR:viewer>/data/defs/ui"
        COMMENT "Copying MC2R UI FIT defs for Viewer")
endif()


# Runtime text defs for Viewer ImGui/FIT TextID resolution.
set(MC2R_RUNTIME_TEXT_DEFS_DIR "${CMAKE_SOURCE_DIR}/data/defs/text")
if(EXISTS "${MC2R_RUNTIME_TEXT_DEFS_DIR}")
    add_custom_command(TARGET viewer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${MC2R_RUNTIME_TEXT_DEFS_DIR}"
            "$<TARGET_FILE_DIR:viewer>/data/defs/text"
        COMMENT "Copying MC2R text defs for Viewer")
endif()



# Runtime art copy for Viewer ImGui/FIT UI assets.
# Keep this broad enough for legacy GUI pages while the shared UI layout is migrated.
set(MC2R_RUNTIME_ART_DIR "${CMAKE_SOURCE_DIR}/data/art")
if(EXISTS "${MC2R_RUNTIME_ART_DIR}")
    add_custom_command(TARGET viewer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${MC2R_RUNTIME_ART_DIR}"
            "$<TARGET_FILE_DIR:viewer>/data/art"
        COMMENT "Copying MC2R runtime art for Viewer")
endif()

>>>>>>> Stashed changes
