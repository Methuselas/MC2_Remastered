# tools/rendercore_standalone_spike/CMakeLists.txt
# RENDERCORE-STANDALONE-INIT-0 spike.
#
# STANDALONE BUILD (does not touch the main build64/ cache, no vcpkg):
#   cmake -S tools/rendercore_standalone_spike -B build64-spike -G "Visual Studio 17 2022" -A x64
#   cmake --build build64-spike --config RelWithDebInfo --target mc2_rendercore_spike
#   build64-spike/RelWithDebInfo/mc2_rendercore_spike.exe   # exit 0 = pass
#
# SDL2 + GLEW come from the repo's vendored 3rdparty/{include,lib/x64}. The
# lib/x64 directory is gitignored (build artifact); populate it by copying from
# any built worktree if absent. The spike links ONLY SDL2 + GLEW + opengl32 and
# three engine TUs that the firewall recon proved are game-free.

cmake_minimum_required(VERSION 3.16)
project(mc2_rendercore_spike CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(MC2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(mc2_rendercore_spike
    main.cpp
    # Slice-1 host-services default impls (the seam this spike consumes).
    ${MC2_ROOT}/HostServices/DefaultHostServices.cpp
    # Engine TUs: game-free per scripts/check-include-firewall.sh recon.
    ${MC2_ROOT}/GameOS/gameos/view_uniforms_gl.cpp
    ${MC2_ROOT}/GameOS/gameos/pipeline_binder.cpp
    ${MC2_ROOT}/RenderCore/PipelineRegistry.cpp
)

target_compile_definitions(mc2_rendercore_spike PRIVATE
    LINUX_BUILD              # match the engine's global build env
    _CRT_SECURE_NO_WARNINGS
)

target_include_directories(mc2_rendercore_spike PRIVATE
    ${MC2_ROOT}/3rdparty/include
    ${MC2_ROOT}/3rdparty/include/SDL2
    ${MC2_ROOT}/HostServices
    ${MC2_ROOT}/RenderCore
    ${MC2_ROOT}/GameOS/gameos
)

target_link_directories(mc2_rendercore_spike PRIVATE
    ${MC2_ROOT}/3rdparty/lib/x64
)

# glew32 / SDL2 are dynamic import libs (glew32.dll / SDL2.dll). opengl32 is
# the system GL loader lib.
target_link_libraries(mc2_rendercore_spike PRIVATE
    SDL2
    glew32
    opengl32
)

# Stage the runtime DLLs next to the exe so it runs in place.
add_custom_command(TARGET mc2_rendercore_spike POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${MC2_ROOT}/3rdparty/lib/x64/SDL2.dll"  "$<TARGET_FILE_DIR:mc2_rendercore_spike>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${MC2_ROOT}/3rdparty/lib/x64/glew32.dll" "$<TARGET_FILE_DIR:mc2_rendercore_spike>"
    VERBATIM
)
