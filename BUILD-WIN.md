Building from source on Windows
===============================

**Just want to play?** You don't need this file. Grab the five zips from the [latest release](https://github.com/ThranduilsRing/mc2-opengl-remastered/releases/latest), create a new empty folder, extract all five into it, and run `mc2.exe`. This document is for developers building the engine from source.

## Quick build (using the vendored `3rdparty.zip`)

This is the fast path — verified end-to-end from a fresh clone.

**Prerequisites:**
- Visual Studio 2022 Build Tools with the **MSVC v143** workload
- Git with Git LFS — run `git lfs install` once after installing Git. Without this, `3rdparty.zip` will clone as a ~134-byte pointer stub and the build will fail to find headers. If that happens, run `git lfs pull`.

1. Clone the repo. Git LFS will pull `3rdparty.zip` automatically.
2. Extract `3rdparty.zip` at the repo root (creates `3rdparty/{cmake,include,lib,tracy}`).
3. Configure and build with an **absolute** path to the `3rdparty/` folder:

```bat
cmake -G "Visual Studio 17 2022" -DCMAKE_PREFIX_PATH=C:/absolute/path/to/repo/3rdparty -DCMAKE_LIBRARY_ARCHITECTURE=x64 -B build64
cmake --build build64 --config RelWithDebInfo --target mc2
```

Output: `build64/RelWithDebInfo/mc2.exe`

**Always use `RelWithDebInfo`.** `Release` builds crash on GL debug callback registration.

**Why the absolute path?** `CMakeLists.txt` expands `${CMAKE_PREFIX_PATH}/include` literally when setting include directories. Relative paths do not resolve correctly from the build directory, causing headers to silently fail to be found at compile time.

### Deploy

Copy `build64/RelWithDebInfo/mc2.exe` to your game deploy directory.

If you changed any shaders, copy the updated files from `shaders/` to `<game-deploy-dir>/shaders/`.

---

## Manual 3rdparty build (optional)

Use this section only if you need to rebuild the 3rdparty libraries from source instead of using `3rdparty.zip`.

### zlib

1. Download zlib sources: https://gnuwin32.sourceforge.net/downlinks/zlib-src-zip.php
2. Download `unistd.h` for Windows: https://gist.githubusercontent.com/mbikovitsky/39224cf521bfea7eabe9/raw/69e4852c06452a368a174ca1f0f33ce87bb52985/unistd.h
3. Open `unistd.h` and comment out `#include <getopt.h>` and the integer typedef block at the end.
4. Place `unistd.h` next to the zlib source files, then open `zconf.h` and change `#include <unistd.h>` to `#include "unistd.h"`. (Alternative: put `unistd.h` in your compiler's system include path.)
5. Open an **x86 Native Tools Command Prompt for VS 2022** and `cd` to the zlib directory.
6. `nmake -f win32\Makefile.msc`
7. Copy the resulting `.dll` and `.lib` to `3rdparty\lib\x86\`.
8. Repeat steps 5–7 in an **x64 Native Tools Command Prompt**; copy results to `3rdparty\lib\x64\`.
9. Copy `unistd.h` to `3rdparty\include\`.

### SDL2, SDL2\_mixer, SDL2\_ttf

| Library | Release page | Direct download |
|---|---|---|
| SDL2 | https://github.com/libsdl-org/SDL/releases/tag/release-2.30.11 | [SDL2-devel-2.30.11-VC.zip](https://github.com/libsdl-org/SDL/releases/download/release-2.30.11/SDL2-devel-2.30.11-VC.zip) |
| SDL2\_mixer | https://github.com/libsdl-org/SDL_mixer/releases | [SDL2\_mixer-devel-2.8.0-VC.zip](https://github.com/libsdl-org/SDL_mixer/releases/download/release-2.8.0/SDL2_mixer-devel-2.8.0-VC.zip) |
| SDL2\_ttf | https://github.com/libsdl-org/SDL_ttf/releases/tag/release-2.24.0 | [SDL2\_ttf-devel-2.24.0-VC.zip](https://github.com/libsdl-org/SDL_ttf/releases/download/release-2.24.0/SDL2_ttf-devel-2.24.0-VC.zip) |

For each package:
- Copy x86 and x64 libs/DLLs to the corresponding `3rdparty\lib\x86\` and `3rdparty\lib\x64\` folders.
- Copy headers to `3rdparty\include\SDL2\`.
- Copy the `cmake\` folder contents to `3rdparty\cmake\`.

### GLEW

From binaries (recommended): https://glew.sourceforge.net/

From source:
1. Download: https://sourceforge.net/projects/glew/files/glew/snapshots/glew-20190928.tgz/download
2. Open `build\vs12\glew.sln` and build for both x64 and Win32.

Copy libs/DLLs to `3rdparty\lib\x86\` and `3rdparty\lib\x64\`; copy headers to `3rdparty\include\GL\`.

### Compiling mc2 against manually-built 3rdparty

```bat
cmake -G "Visual Studio 17 2022" -DCMAKE_PREFIX_PATH=C:/absolute/path/to/3rdparty -DCMAKE_LIBRARY_ARCHITECTURE=x64 -B build64
cmake --build build64 --config RelWithDebInfo --target mc2
```

Output: `build64/RelWithDebInfo/mc2.exe`

### Deploy

Copy `build64/RelWithDebInfo/mc2.exe` to your game deploy directory.

All runtime DLLs (SDL2, GLEW, FFmpeg) are included in `mc2-remastered-engine.zip` from the release. If building entirely from scratch, copy the DLLs from `3rdparty\lib\x64\` to the same folder as `mc2.exe`.
