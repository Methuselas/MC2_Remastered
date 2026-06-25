# UNITQUERY-SETTARGETPRIORITY-1: standalone unit-test target for the pure brain
# symbol resolver. Engine-free — compiles only the resolver TU + the test driver.
cmake_minimum_required(VERSION 3.20)
project(brain_symbol_resolve_test CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED REPO_ROOT)
    get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
endif()

add_executable(brain_symbol_resolve_test
    main.cpp
    "${REPO_ROOT}/code/brain_symbol_resolve.cpp"
)
target_include_directories(brain_symbol_resolve_test PRIVATE "${REPO_ROOT}/code")
