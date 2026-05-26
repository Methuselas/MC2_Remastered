# Standalone MC2R UI Editor target.
#
# This target is intentionally wired through the main MC2R CMake graph:
# - uses the project-owned 3rdparty/imgui target
# - uses root-discovered SDL2 / GLEW / OpenGL / ZLIB packages
# - avoids compiling duplicate ImGui sources
# - keeps runtime data paths rooted at the repo while debugging

file(GLOB UI_EDITOR_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
)

file(GLOB UI_EDITOR_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
)

set(UI_EDITOR_ENTRY_SOURCE "")
foreach(_candidate
        "${CMAKE_CURRENT_SOURCE_DIR}/UiEditorMain.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Main.cpp")
    if(EXISTS "${_candidate}")
        set(UI_EDITOR_ENTRY_SOURCE "${_candidate}")
        break()
    endif()
endforeach()

if("${UI_EDITOR_ENTRY_SOURCE}" STREQUAL "")
    message(STATUS "ui_editor target skipped: no UiEditorMain.cpp/main.cpp entry source found.")
    return()
endif()

add_executable(ui_editor
    ${UI_EDITOR_SOURCES}
    ${UI_EDITOR_HEADERS}
)

set(UI_EDITOR_STB_INCLUDE_DIR "")

foreach(_stb_candidate
        "${CMAKE_SOURCE_DIR}/3rdparty/stb"
        "${CMAKE_SOURCE_DIR}/3rdparty/assimp/contrib/stb")
    if(EXISTS "${_stb_candidate}/stb_image.h")
        set(UI_EDITOR_STB_INCLUDE_DIR "${_stb_candidate}")
        break()
    endif()
endforeach()

if(UI_EDITOR_STB_INCLUDE_DIR)
    target_include_directories(ui_editor PRIVATE "${UI_EDITOR_STB_INCLUDE_DIR}")
    message(STATUS "ui_editor stb_image include: ${UI_EDITOR_STB_INCLUDE_DIR}")
else()
    message(FATAL_ERROR "ui_editor requires stb_image.h, but no bundled STB include path was found.")
endif()

set(UI_EDITOR_SDL2_INCLUDE_DIRS "")

if(TARGET SDL2::SDL2)
    get_target_property(_ui_editor_sdl2_target_includes SDL2::SDL2 INTERFACE_INCLUDE_DIRECTORIES)
    if(_ui_editor_sdl2_target_includes)
        list(APPEND UI_EDITOR_SDL2_INCLUDE_DIRS ${_ui_editor_sdl2_target_includes})
    endif()
endif()

if(DEFINED SDL2_INCLUDE_DIRS)
    list(APPEND UI_EDITOR_SDL2_INCLUDE_DIRS ${SDL2_INCLUDE_DIRS})
endif()

if(DEFINED SDL2_INCLUDE_DIR)
    list(APPEND UI_EDITOR_SDL2_INCLUDE_DIRS ${SDL2_INCLUDE_DIR})
endif()

foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
    if(NOT "${_prefix}" STREQUAL "")
        file(TO_CMAKE_PATH "${_prefix}" _prefix_cmake)
        if(EXISTS "${_prefix_cmake}/include/SDL2/SDL.h")
            list(APPEND UI_EDITOR_SDL2_INCLUDE_DIRS "${_prefix_cmake}/include/SDL2")
        endif()
        if(EXISTS "${_prefix_cmake}/include/SDL.h")
            list(APPEND UI_EDITOR_SDL2_INCLUDE_DIRS "${_prefix_cmake}/include")
        endif()
    endif()
endforeach()

foreach(_repo_sdl2_candidate
        "${CMAKE_SOURCE_DIR}/3rdparty/include/SDL2"
        "${CMAKE_SOURCE_DIR}/3rdparty/include")
    if(EXISTS "${_repo_sdl2_candidate}/SDL.h")
        list(APPEND UI_EDITOR_SDL2_INCLUDE_DIRS "${_repo_sdl2_candidate}")
    endif()
endforeach()

list(REMOVE_DUPLICATES UI_EDITOR_SDL2_INCLUDE_DIRS)

if(UI_EDITOR_SDL2_INCLUDE_DIRS)
    target_include_directories(ui_editor PRIVATE ${UI_EDITOR_SDL2_INCLUDE_DIRS})
    message(STATUS "ui_editor SDL2 include dirs: ${UI_EDITOR_SDL2_INCLUDE_DIRS}")
else()
    message(FATAL_ERROR "ui_editor requires SDL.h, but no SDL2 include directory containing SDL.h was found.")
endif()

target_compile_features(ui_editor PRIVATE cxx_std_17)

target_include_directories(ui_editor PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/code"
    "${CMAKE_SOURCE_DIR}/gui"
    "${CMAKE_SOURCE_DIR}/mclib"
    "${CMAKE_SOURCE_DIR}/GameOS/include"
    "${CMAKE_SOURCE_DIR}/GameOS/gameos"
    "${CMAKE_SOURCE_DIR}/GuiRuntime"
    "${CMAKE_SOURCE_DIR}/RenderCore"
    "${CMAKE_SOURCE_DIR}/RenderWorld"
    "${CMAKE_SOURCE_DIR}/GameAdapters"
    "${CMAKE_SOURCE_DIR}/3rdparty/imgui"
    "${CMAKE_SOURCE_DIR}/3rdparty/imgui/backends"
    ${COMMON_INCLUDE_DIRS}
    ${THIRDPARTY_INCLUDE_DIRS}
)

if(MSVC)
    target_compile_definitions(ui_editor PRIVATE
        PLATFORM_WINDOWS
        _CRT_SECURE_NO_WARNINGS
    )
else()
    target_compile_definitions(ui_editor PRIVATE
        LINUX_BUILD
    )
endif()

target_link_libraries(ui_editor PRIVATE
    imgui
    GLEW::GLEW
    OpenGL::GL
    ZLIB::ZLIB
)

if(TARGET SDL2::SDL2main)
    target_link_libraries(ui_editor PRIVATE SDL2::SDL2main)
endif()

if(TARGET SDL2::SDL2)
    target_link_libraries(ui_editor PRIVATE SDL2::SDL2)
else()
    target_link_libraries(ui_editor PRIVATE ${SDL2_LIBRARIES})
endif()

if(TARGET gui_runtime)
    target_link_libraries(ui_editor PRIVATE gui_runtime)
endif()

set_target_properties(ui_editor PROPERTIES
    OUTPUT_NAME "ui_editor"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
