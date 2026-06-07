# mc2-launcher — Campaign mod selection GUI that launches mc2.exe
# with MC2_ACTIVE_MOD=<id> in the child process environment.
# WIN32 subsystem: no console window.

add_executable(mc2_launcher WIN32 mc2_launcher.cpp)

set_target_properties(mc2_launcher PROPERTIES
    OUTPUT_NAME "mc2-launcher"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out/mc2_launcher"
)

target_compile_definitions(mc2_launcher PRIVATE
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)

# Deploy alongside mc2.exe after build.
if(DEFINED MC2_DEPLOY_DIR AND EXISTS "${MC2_DEPLOY_DIR}")
    add_custom_command(TARGET mc2_launcher POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:mc2_launcher>"
            "${MC2_DEPLOY_DIR}/mc2-launcher.exe"
        COMMENT "Deploying mc2-launcher.exe to ${MC2_DEPLOY_DIR}"
    )
endif()
