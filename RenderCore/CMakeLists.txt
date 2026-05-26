# RenderCore/CMakeLists.txt
#
# KtxLoader.cpp promoted this from INTERFACE to STATIC.
# All INTERFACE consumers (renderworld, gameadapters, editorbridge) still link
# correctly; the PUBLIC include dir + compile feature propagate unchanged.

add_library(rendercore STATIC
    KtxLoader.cpp
    PipelineRegistry.cpp
)
target_include_directories(rendercore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_features(rendercore PUBLIC cxx_std_17)
