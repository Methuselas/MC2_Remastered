# BRAIN-MISSIONFIT-OPORD-CONSUMER-1: standalone unit test for the pure mission.fit
# OPORD parser. Engine-free — compiles only the parser TU + the test driver.
cmake_minimum_required(VERSION 3.20)
project(brain_missionfit_oporbd_test CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED REPO_ROOT)
    get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
endif()

add_executable(brain_missionfit_oporbd_test
    main.cpp
    "${REPO_ROOT}/code/brain_missionfit_oporbd.cpp"
)
target_include_directories(brain_missionfit_oporbd_test PRIVATE "${REPO_ROOT}/code")
