add_library(imgui STATIC
    imgui.cpp
    imgui_draw.cpp
    imgui_tables.cpp
    imgui_widgets.cpp
    backends/imgui_impl_opengl3.cpp
    backends/imgui_impl_sdl2.cpp
)

target_include_directories(imgui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/backends
)

# SDL2 headers needed by imgui_impl_sdl2
# imgui_impl_sdl2 uses #include <SDL.h>; add SDL2 subdir from the 3rdparty prefix
target_include_directories(imgui PRIVATE
    ${THIRDPARTY_INCLUDE_DIRS}
    ${CMAKE_PREFIX_PATH}/include/SDL2
)

# Suppress warnings in vendored code
if(MSVC)
    target_compile_options(imgui PRIVATE /W0)
else()
    target_compile_options(imgui PRIVATE -w)
endif()
