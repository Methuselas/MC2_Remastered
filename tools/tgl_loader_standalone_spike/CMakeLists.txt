# tools/tgl_loader_standalone_spike/CMakeLists.txt
# NS3-GEOMETRY-LOADER-STANDALONE — game-free TGL/ASE loader LINK-PROOF spike.
#
# The linker is the proof: this target links ONLY mclib TUs + Stuff TUs +
# our own stub .cpp. NO code/ (game) TU, NO GameOS gameos.cpp, NO GL/SDL.
# If any code/ game symbol were required on the load path, the link would
# fail — a clean link proves TG_TypeMultiShape::LoadBinaryCopy is game-free.
#
# STANDALONE BUILD (does not touch the main build64/ cache, no vcpkg):
#   cmake -S tools/tgl_loader_standalone_spike -B build64-tglspike -G "Visual Studio 17 2022" -A x64
#   cmake --build build64-tglspike --config RelWithDebInfo --target tgl_loader_spike
#   build64-tglspike/RelWithDebInfo/tgl_loader_spike.exe <path-to-a-real.tgl>

cmake_minimum_required(VERSION 3.16)
project(tgl_loader_spike CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(MC2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(tgl_loader_spike
    main.cpp
    stubs.cpp
    # --- mclib TUs (geometry loader closure) ---
    ${MC2_ROOT}/mclib/msl.cpp
    ${MC2_ROOT}/mclib/tgl.cpp
    ${MC2_ROOT}/mclib/cident.cpp        # FullPathFileName helpers
    ${MC2_ROOT}/mclib/mathfunc.cpp      # Rotate / RotateLight
    # GameOS cross-platform string shim (game-free; NOT gameos.cpp).
    ${MC2_ROOT}/GameOS/src/platform_str.cpp   # S_stricmp / S_strnicmp / S_strlwr
    ${MC2_ROOT}/mclib/file.cpp
    ${MC2_ROOT}/mclib/fastfile.cpp
    ${MC2_ROOT}/mclib/ffile.cpp         # FastFile::open/readFast/...
    ${MC2_ROOT}/mclib/packet.cpp        # PacketFile::readPacket
    ${MC2_ROOT}/mclib/lzdecomp.cpp      # FastFile LZ decompression
    ${MC2_ROOT}/mclib/fst_hash.cpp      # elfHash (FastFile dictionary hash)
    ${MC2_ROOT}/mclib/heap.cpp
    ${MC2_ROOT}/mclib/paths.cpp
    ${MC2_ROOT}/mclib/timing.cpp
    # mclib helper TUs the loader's (uncalled) transform/render methods
    # reference at link time. All mclib — none are code/ (game) TUs.
    ${MC2_ROOT}/mclib/cpu_proj_cost_split.cpp     # mc2_cpu_proj_cost::*
    ${MC2_ROOT}/mclib/projectz_trace.cpp          # projectz_trace_dispatch, g_pzTrace
    ${MC2_ROOT}/mclib/object_admission_predicate.cpp  # clipSpaceFrustumAdmit/eligibleForGpuObjects
    # --- Stuff library TUs (game-free math + containers) ---
    ${MC2_ROOT}/mclib/stuff/affinematrix.cpp
    ${MC2_ROOT}/mclib/stuff/angle.cpp
    ${MC2_ROOT}/mclib/stuff/color.cpp
    ${MC2_ROOT}/mclib/stuff/extentbox.cpp
    ${MC2_ROOT}/mclib/stuff/line.cpp
    ${MC2_ROOT}/mclib/stuff/linearmatrix.cpp
    ${MC2_ROOT}/mclib/stuff/matrix.cpp
    ${MC2_ROOT}/mclib/stuff/motion.cpp
    ${MC2_ROOT}/mclib/stuff/normal.cpp
    ${MC2_ROOT}/mclib/stuff/obb.cpp
    ${MC2_ROOT}/mclib/stuff/origin.cpp
    ${MC2_ROOT}/mclib/stuff/plane.cpp
    ${MC2_ROOT}/mclib/stuff/point3d.cpp
    ${MC2_ROOT}/mclib/stuff/polar.cpp
    ${MC2_ROOT}/mclib/stuff/random.cpp
    ${MC2_ROOT}/mclib/stuff/ray.cpp
    ${MC2_ROOT}/mclib/stuff/rotation.cpp
    ${MC2_ROOT}/mclib/stuff/scalar.cpp
    ${MC2_ROOT}/mclib/stuff/sphere.cpp
    ${MC2_ROOT}/mclib/stuff/unitvector.cpp
    ${MC2_ROOT}/mclib/stuff/vector3d.cpp
    ${MC2_ROOT}/mclib/stuff/vector4d.cpp
    ${MC2_ROOT}/mclib/stuff/matrixstack.cpp
    ${MC2_ROOT}/mclib/stuff/memoryblock.cpp
    ${MC2_ROOT}/mclib/stuff/mstring.cpp
    ${MC2_ROOT}/mclib/stuff/chain.cpp
    ${MC2_ROOT}/mclib/stuff/hash.cpp
    ${MC2_ROOT}/mclib/stuff/iterator.cpp
    ${MC2_ROOT}/mclib/stuff/link.cpp
    ${MC2_ROOT}/mclib/stuff/node.cpp
    ${MC2_ROOT}/mclib/stuff/plug.cpp
    ${MC2_ROOT}/mclib/stuff/registeredclass.cpp
    ${MC2_ROOT}/mclib/stuff/safechain.cpp
    ${MC2_ROOT}/mclib/stuff/safesocket.cpp
    ${MC2_ROOT}/mclib/stuff/slot.cpp
    ${MC2_ROOT}/mclib/stuff/socket.cpp
    ${MC2_ROOT}/mclib/stuff/sortedchain.cpp
    ${MC2_ROOT}/mclib/stuff/sortedsocket.cpp
    ${MC2_ROOT}/mclib/stuff/table.cpp
    ${MC2_ROOT}/mclib/stuff/tree.cpp
    ${MC2_ROOT}/mclib/stuff/database.cpp
    ${MC2_ROOT}/mclib/stuff/filestream.cpp
    ${MC2_ROOT}/mclib/stuff/filestreammanager.cpp
    ${MC2_ROOT}/mclib/stuff/memorystream.cpp
    ${MC2_ROOT}/mclib/stuff/namelist.cpp
    ${MC2_ROOT}/mclib/stuff/notationfile.cpp
    ${MC2_ROOT}/mclib/stuff/note.cpp
    ${MC2_ROOT}/mclib/stuff/page.cpp
    ${MC2_ROOT}/mclib/stuff/style.cpp
    ${MC2_ROOT}/mclib/stuff/trace.cpp
    ${MC2_ROOT}/mclib/stuff/stuff.cpp
)

target_compile_definitions(tgl_loader_spike PRIVATE
    PLATFORM_WINDOWS            # use real <windows.h>, not the cross-compile shims
    LINUX_BUILD                 # match the engine's global build env
    _CRT_SECURE_NO_WARNINGS
    # NOTE: deliberately NOT defining ENABLE_ASSIMP_IMPORTER (keeps Assimp out)
)

target_include_directories(tgl_loader_spike PRIVATE
    ${MC2_ROOT}/mclib
    ${MC2_ROOT}/mclib/stuff
    ${MC2_ROOT}/GameOS/include
    ${MC2_ROOT}/GameOS/gameos
    # GL/glew.h is needed only as a HEADER (struct/typedef decls pulled in
    # transitively by tgl.cpp via gos_static_prop_batcher.h). No GL .lib is
    # linked and no GL entry point is called on the load path, so the link
    # stays GL-free — header availability != linkage.
    ${MC2_ROOT}/3rdparty/include        # zlib.h (1.2.3) for ffile/packet/lzdecomp
    # Tracy headers (msl.cpp -> gos_profiler.h -> tracy/Tracy.hpp). Without
    # TRACY_ENABLE the profiling macros are no-ops, so no Tracy linkage.
    ${MC2_ROOT}/3rdparty/tracy
    ${MC2_ROOT}
)

# zlib (1.2.3) — the game-free compression lib FastFile/PacketFile/lzdecomp
# use. NOT a game/render dependency. Prebuilt zlib.lib is a gitignored build
# artifact; search the worktree first, then the parent repo's vendored copy.
find_library(MC2_ZLIB
    NAMES zlib zlibstatic
    PATHS
        ${MC2_ROOT}/3rdparty/3rdparty/lib/x64
        ${MC2_ROOT}/3rdparty/lib/x64
        A:/Games/mc2-opengl-src/3rdparty/3rdparty/lib/x64
    NO_DEFAULT_PATH
)
if(NOT MC2_ZLIB)
    message(FATAL_ERROR "zlib.lib not found — copy 3rdparty/3rdparty/lib/x64/zlib.lib from a built tree")
endif()
target_link_libraries(tgl_loader_spike PRIVATE ${MC2_ZLIB})

