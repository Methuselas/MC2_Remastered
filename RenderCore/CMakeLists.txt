# RenderCore/CMakeLists.txt
#
# Slice M1: header-only module. INTERFACE library exposes the public
# include directory; no compiled translation units yet.

add_library(rendercore INTERFACE)
target_include_directories(rendercore INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_features(rendercore INTERFACE cxx_std_17)
