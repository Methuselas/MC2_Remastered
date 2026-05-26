add_definitions(-DUSE_ASSEMBLER_CODE=0)
add_definitions(-DLINUX_BUILD)


set(TEXT_TOOL_SOURCES "main.cpp")

include_directories(${THIRDPARTY_INCLUDE_DIRS})
add_executable(text_tool ${TEXT_TOOL_SOURCES})
target_link_libraries(text_tool gameos windows ${SDL2_LIBRARIES} SDL2_ttf::SDL2_ttf GLEW::GLEW ${ADDITIONAL_LIBS} OpenGL::GL)
