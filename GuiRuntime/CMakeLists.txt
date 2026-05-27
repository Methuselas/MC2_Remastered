add_library(gui_runtime STATIC
    GuiRuntime.cpp
    EditorInspector.cpp
    GraphicsOptionsWindow.cpp
)

target_include_directories(gui_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_include_directories(gui_runtime PRIVATE
    ${THIRDPARTY_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/GameOS/include
    ${CMAKE_SOURCE_DIR}/GameOS/gameos
    ${CMAKE_SOURCE_DIR}/RenderCore
)

target_link_libraries(gui_runtime PUBLIC imgui renderworld)
