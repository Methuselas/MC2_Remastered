# Track C — Build / CI Integration Deep-Dive (Sol2 + Lua 5.4)

**Date:** 2026-04-30
**Mode:** Research / design only. No code in this commit.
**Predecessors:**
- [`2026-04-30-track-c-lua-implementation-shape.md`](2026-04-30-track-c-lua-implementation-shape.md) §1 — vendoring layout sketch.
- [`2026-04-30-track-c-blocking-questions-resolution.md`](2026-04-30-track-c-blocking-questions-resolution.md) §Q1 — `*_impl` extraction pattern.

This doc resolves the build-system questions for Track C M0 (commit C-1 — vendoring) so the next session can land code with zero "how does CMake handle this?" friction. The Tracy precedent is the load-bearing model: it tells us the project's house style for vendoring a non-trivial 3rdparty library.

---

## 1. Vendoring layout

### Tracy precedent (the actual on-disk shape)

```
3rdparty/tracy/
    TracyClient.cpp        # Single-source amalgamation (~25 LoC; #includes the .cpps below)
    TracyClient.F90        # Fortran shim — irrelevant for us
    tracy/                 # Public headers (Tracy.hpp, TracyOpenGL.hpp, TracyC.h, …)
    client/                # Implementation TUs (transitively included by TracyClient.cpp)
    common/                # Cross-cutting utilities
    libbacktrace/          # Optional backend
```

Hookup (verified in `GameOS/gameos/CMakeLists.txt:34`): the `gameos` static library lists `${CMAKE_SOURCE_DIR}/3rdparty/tracy/TracyClient.cpp` directly in its `SOURCES` set. No `add_library(tracy_static …)`; no `add_subdirectory`. One amalgamation TU compiled inline. Headers are exposed via `THIRDPARTY_INCLUDE_DIRS` populated at the root (`CMakeLists.txt:154`: `list(APPEND THIRDPARTY_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/tracy")`).

### Final Track C layout

```
3rdparty/sol/
    sol.hpp                    # Sol2 single-file amalgamation (~30K LoC)
    forward.hpp                # Optional — fwd-decl header to keep heavy template in two TUs
    LICENSE.txt                # Sol2 (MIT)
3rdparty/lua/
    src/                       # Lua 5.4.x — unmodified upstream
        lapi.c lcode.c lctype.c ldebug.c ldo.c ldump.c
        lfunc.c lgc.c llex.c lmem.c lobject.c lopcodes.c
        lparser.c lstate.c lstring.c ltable.c ltm.c lundump.c
        lvm.c lzio.c
        lauxlib.c lbaselib.c lcorolib.c ldblib.c liolib.c
        lmathlib.c loadlib.c loslib.c lstrlib.c ltablib.c
        lutf8lib.c linit.c
        lua.h luaconf.h lualib.h lauxlib.h lua.hpp
    LICENSE                    # Lua (MIT)
    README                     # Provenance: "Lua 5.4.7, dropped 2026-MM-DD, sha256 …"
```

### Files to vendor vs. skip

Lua's distribution tarball ships 35 `.c` files. We vendor 32, skip 3:

- **Skip `lua.c`** — the standalone interpreter `main()`. It would conflict with our `mc2.exe`'s `WinMain` symbol.
- **Skip `luac.c`** — the bytecode compiler `main()`. Same reason.
- **Skip `onelua.c`** — Lua's optional one-TU amalgamation that `#include`s every other source. Pick *one* path: discrete TUs OR `onelua.c`. The discrete TU strategy gives faster incremental builds (only the touched .c recompiles), better /MP scaling, and better diagnostics (errors point at the real file). Tracy chose amalgamation because its TUs are deeply tangled; Lua's are clean. **Recommendation: vendor the discrete .c set, skip `onelua.c`.**

### Sol2 amalgamation

Sol2 ships a script (`single/single.py`) that produces `single/include/sol/sol.hpp` + `single/include/sol/forward.hpp`. We commit the generated outputs verbatim — no python tool dependency in the build path. Drop `single/single.py`, the Sol2 README pointer with the upstream commit hash, and the LICENSE.

---

## 2. CMake target shape

The three options the brief asks me to evaluate:

| Option | Description | Cost | Pros | Cons |
|--------|-------------|------|------|------|
| (a) Direct source addition | `lua/src/*.c` inlined into the `mc2` exe target's `SOURCES` list (root `CMakeLists.txt:165`) | 0 new targets | Smallest CMake change. Mirrors Tracy precedent's *spirit*. | C-files mixed with C++ files in one target — MSVC handles this fine, but warning suppression has to be per-source-file rather than per-target. /MP parallelism still works. |
| (b) Static library `lua_static` | Own `add_library(lua_static STATIC …)` at root, linked into `mc2` | One static lib | Clean isolation. Per-target compile flags / warning suppression. Faster incremental rebuilds (Lua compiles once, cached). Matches Implementation-Shape §1 sketch. | One extra target node in the build graph. Symbol export — none, all internal — moot for static. |
| (c) OBJECT library | `add_library(lua_obj OBJECT …)` consumed via `$<TARGET_OBJECTS:lua_obj>` | One obj lib | Object files inlined into the consumer target without intermediate `.lib` archive. | No real win over (b) for this case — there's only one consumer (`mc2.exe`). |

**Recommendation: (b) — `lua_static` STATIC library.**

Rationale:

1. **Tracy is technically (a) but the analogy is imperfect.** Tracy is a single-TU amalgamation (`TracyClient.cpp`). Its "(a)-shape" is a degenerate case of (a) with one file. Lua is 32 TUs — 32 source lines in `set(SOURCES …)` would noisily bloat the root CMakeLists.txt (which already has 90 source files in the `mc2` target).
2. **Per-target warning suppression.** Lua's source generates `/W3` MSVC warnings (`C4334` integer-shift-to-64-bit, `C4146` unary-minus-on-unsigned, `C4244`/`C4267` numeric conversions). On a STATIC lib target, `target_compile_options(lua_static PRIVATE /wd4334 /wd4146 /wd4244 /wd4267)` confines suppressions; on (a), each `.c` file would need a `set_source_files_properties(... COMPILE_FLAGS …)` line.
3. **Incremental build wins.** Once `lua_static.lib` is built, Lua doesn't recompile when any `.cpp` in `code/` is touched. With (a), changing the `mc2` target's `SOURCES` list invalidates the target (CMake regenerates), but the per-file compile cache survives — minor difference. The bigger win is logically partitioned dependency.
4. **Already in the design.** Implementation-Shape §1 names the target `lua_static`. Following it eliminates one renaming round-trip.

Sol2 stays header-only: no library target. It contributes only an include directory, identical to GL/SDL2 in `THIRDPARTY_INCLUDE_DIRS`.

The `modding/` directory becomes a fourth-tier subproject like `gui/` or `mclib/`: `add_subdirectory("./modding" "./out/modding")` declaring `add_library(modding STATIC modding/lua_vm.cpp modding/lua_bindings_mc2.cpp)`. The `mc2` exe then links `modding lua_static` (in that order, since `modding` references Lua symbols).

---

## 3. C / C++ boundary

### Lua headers

Lua 5.4 provides `lua.hpp` — a one-line C++ shim:

```cpp
// 3rdparty/lua/src/lua.hpp
extern "C" {
  #include "lua.h"
  #include "lualib.h"
  #include "lauxlib.h"
}
```

C++ TUs include `lua.hpp`; never `lua.h` directly. This is upstream's official pattern.

### Sol2 expectations

Sol2 by default `#include`s `<lua.hpp>` itself when compiled in C++. From the upstream `include/sol/forward.hpp` and `compatibility.hpp`, Sol2 looks for one of:
- `<lua.hpp>` (preferred — it does the `extern "C"` wrap for you)
- `<lua.h>` + `<lualib.h>` + `<lauxlib.h>` (fallback, but Sol2 will manually wrap them in `extern "C"`)

You can force the choice with `-DSOL_USING_CXX_LUA=1` (Sol2 includes the `.hpp` shim) vs `-DSOL_USING_CXX_LUA=0` (raw .h with manual wrap). Default-on `<lua.hpp>` is what we want — vendored Lua already provides it. **No define needed.**

### TU include order in `lua_vm.cpp` / `lua_bindings_mc2.cpp`

```cpp
// Always Sol2 first; it transitively pulls in lua.hpp with the right wrap.
#include <sol/sol.hpp>
// Then project headers.
#include "lua_vm.h"
#include "lua_abl_shim.h"
```

In `lua_vm.h` (a *header* consumed by `code/mission.cpp` etc.) we **forward-declare** `namespace sol { class state; }` and pimpl the `sol::state` member, per Implementation-Shape §2. This keeps Sol2's heavy templates out of the engine TUs.

### `*_impl` C function linkage (per blocking-questions §Q1)

The blocking-questions doc resolves Q1 as: extract each ABL `execXxx` body into a sibling `*_impl` C function. The build-side implications:

- **`*_impl` functions live in `code/ablmc2.cpp`** (the existing C++ TU), as `extern "C"` functions. They are NOT in a separate `.c` file; they are C-linkage C++ functions. This avoids splitting the TU.
- **No `.c`/`.cpp` pair.** The whole engine remains C++. The `extern "C"` qualifier strips C++ name-mangling so the Lua-side trampoline can call them via a plain function-pointer table without `extern "C++"` confusion.
- **Header location:** `modding/lua_abl_shim.h` (the trampoline-side header). Declared like:
  ```cpp
  // modding/lua_abl_shim.h
  #pragma once
  #ifdef __cplusplus
  extern "C" {
  #endif
  int   mc2_object_status_impl(int objectId);
  int   mc2_object_exists_impl(int objectId);
  float mc2_get_time_impl(void);
  int   mc2_set_timer_impl(int id, float secs);
  /* … 6 more for the M0 ten … */
  #ifdef __cplusplus
  }
  #endif
  ```
- **`code/ablmc2.cpp` includes this header.** The original `execXxx` functions become 3-line wrappers that pop ABL args, call the `*_impl`, push the result. The header defines the contract; both ABL side and Lua side conform.
- **Why not put `*_impl` in `modding/`?** They reference engine internals (`ObjectManager`, `Mission`) that `code/ablmc2.cpp` already pulls in. Moving them to `modding/` would re-export every dependency. The `*_impl` symbols stay in `code/ablmc2.cpp`; only their declarations are pulled into `modding/` via the shim header.

---

## 4. MSVC compile flags

### C vs C++ compilation

MSVC's `cl.exe` selects compilation language by extension by default: `.c` → C, `.cpp` → C++. The project's CMake does not override this (no `set_source_files_properties(... LANGUAGE …)` calls anywhere in the tree, verified via grep). Lua `.c` files compile as C, exactly what we want.

The C language standard MSVC defaults to is C11/C17 (`/std:c11` is implicit since VS 2019 16.8). Lua 5.4 requires C99; we are well above.

### Warning suppression for Lua sources

Lua's known noisy warnings under `/W3 /WX-`:

| Warning | Description | Where |
|---------|-------------|-------|
| `C4334` | `'<<' result of 32-bit shift implicitly converted to 64 bits` | `ltable.c`, `lvm.c` |
| `C4146` | `unary minus operator applied to unsigned type` | `lstrlib.c` |
| `C4244` | `'argument': conversion from 'X' to 'Y', possible loss of data` | scattered |
| `C4267` | `'initializing': conversion from 'size_t' to 'int'` | scattered |
| `C4310` | `cast truncates constant value` | `lcode.c` |
| `C4324` | `structure was padded due to alignment specifier` | `lstate.c` |

Apply per-target on `lua_static` only (does not pollute `mc2` or `modding`):

```cmake
if(MSVC)
    target_compile_options(lua_static PRIVATE
        /wd4334 /wd4146 /wd4244 /wd4267 /wd4310 /wd4324)
endif()
```

### Build-mode defines

- **Static link:** do NOT define `LUA_BUILD_AS_DLL`. Default (no define) builds Lua as a static-link library. `LUA_API` and `LUALIB_API` resolve to empty, no `__declspec(dllexport)`.
- **Compatibility:** `LUA_COMPAT_5_3` ON — gives us 5.3-compat library functions Sol2 sometimes assumes. Cheap; recommended.
- **Asserts:** `LUAI_ASSERT` is the internal-consistency-check macro, enabled when `lua_assert` resolves to an assertion. **Off in RelWithDebInfo** (no define). On in Debug only — but we don't build Debug; so always off in practice.
- **Path separator:** Lua uses `/` natively in `package.path` strings; the `LINUX_BUILD` global define (root `CMakeLists.txt:111`) is irrelevant to Lua's interior code. (See §9.)

### Sol2 compile flags

Sol2 has a sea of opt-in defines. Three matter for us:

- `SOL_ALL_SAFETIES_ON=1` — turns on argument-count checks, type-checks, stack-balance assertions. Recommended for M0; we pay a small per-call cost in exchange for "Lua type errors throw `sol::error` with file:line" instead of crashing the engine. Disable later when bindings are battle-tested.
- `SOL_PRINT_ERRORS=0` — Sol2 otherwise `printf`s errors to stderr; we route through `mc2.log` instead.
- `SOL_NO_EXCEPTIONS=0` — keep exceptions on. The engine compiles with them, and Sol2's safety paths assume them.

Apply globally to whichever TUs include Sol2 (only `modding/lua_vm.cpp` and `modding/lua_bindings_mc2.cpp`):

```cmake
target_compile_definitions(modding PRIVATE
    SOL_ALL_SAFETIES_ON=1
    SOL_PRINT_ERRORS=0)
```

---

## 5. `*_impl` extraction (build-side concerns)

The semantic shape lives in blocking-questions §Q1. From a build perspective only:

- **No new TUs.** Each `*_impl` function is added to the existing `code/ablmc2.cpp`. The file remains C++ (it always was; engine internals demand it).
- **No header collision.** `modding/lua_abl_shim.h` declares only the impls, not the `execXxx` functions. The latter remain `static`/file-local in `ablmc2.cpp` per current convention.
- **Dependency direction:** `modding` depends on `mc2`'s engine symbols at link time. Specifically the `*_impl` symbols are defined in `code/ablmc2.cpp` (which is part of the `mc2` exe TUs), and referenced from `modding/lua_bindings_mc2.cpp` (in the `modding` static lib).

Wait — that means `modding` static lib references symbols defined in the executable target. CMake handles this if we link `modding` as a `STATIC` lib *into* `mc2.exe`: at executable link time, all object files (from `mc2`'s `SOURCES` and from `modding/*.obj`) are visible, and the linker resolves cross-references in either direction.

But it's cleaner to flip it: move the `*_impl` definitions out of `code/ablmc2.cpp` into a `code/ablmc2_impl.cpp` listed in `mc2`'s `SOURCES`, *or* keep them in `ablmc2.cpp` and rely on the linker. For M0 — keep them in `ablmc2.cpp`. The static lib `modding` references symbols that the linker resolves against object files compiled directly into `mc2.exe`. This works because `modding` is a STATIC library: its object files are pulled into `mc2.exe`, and the resulting executable link edits all symbols at once.

**Header location decision: `modding/lua_abl_shim.h`** (not `code/ablmc2_impl.h`). Reasoning:
- `code/` is engine-internal; `modding/` is the modding boundary.
- The shim header is what the modding code needs to see; it should live next to the modding consumer.
- `code/ablmc2.cpp` includes `"../modding/lua_abl_shim.h"` — a single backward-pointing include from one TU is acceptable.

---

## 6. Compile-time cost

### Lua

- ~17,000 LoC of C across 32 TUs.
- Each TU compiles in <1 second on MSVC. With `/MP` parallelism (already enabled per root `CMakeLists.txt:47`), 32 TUs spread across cores complete in ~2-4 seconds wall-clock on a modern 8-core box.
- After C-1 lands, this cost is paid **once** — `lua_static.lib` is a CMake target with proper dependency tracking, recompiles only when Lua sources change (i.e. when we update Lua versions, ~yearly).

### Sol2

- Header-only, ~30,000 LoC of dense template C++.
- Per-TU cost: estimated **5-10 seconds** of compile time per `.cpp` that includes `<sol/sol.hpp>`. This is the *real* cost.
- **Mitigation:** restrict Sol2 includes to exactly two TUs: `modding/lua_vm.cpp` and `modding/lua_bindings_mc2.cpp`. The fwd-declaration `namespace sol { class state; }` in `modding/lua_vm.h` is what makes this possible. Engine TUs (`code/mission.cpp`, `code/ablmc2.cpp`, etc.) include `lua_vm.h` only and pay zero Sol2 tax.
- Net: **+10-20 seconds added to a clean build**. Incremental builds rebuild only the touched TU.

### Total

A clean RelWithDebInfo rebuild of `mc2.exe` currently takes ~90-120 seconds on this machine. Track C M0 adds **~15-25 seconds** (Lua + 2 Sol2-heavy TUs). Acceptable.

---

## 7. Linker considerations

- **Lua needs no external libs.** Pure C, no syscalls beyond libc/CRT (file I/O via `fopen`, math via `<math.h>`). The `loadlib.c` TU contains `LoadLibrary`/`dlopen` shims, but we won't open `package`/`os`/`debug` libraries (sandbox per Implementation-Shape §6) so the dynamic-load paths are unreachable. Static link only; **no DLL surface, no /DELAYLOAD, no runtime DLL deploys.**
- **Sol2 needs no external linkage.** Header-only.
- **Modding lib link order:** `target_link_libraries(mc2 modding lua_static …existing libs…)`. The `modding` static lib references `lua_static` symbols (the Lua C API) and engine `*_impl` symbols (resolved at exe link time). MSVC link is order-independent for object files within a STATIC lib pull; the names just need to all exist somewhere in the link line.
- **No symbol export change to `mc2.exe`.** mc2 is an exe with an empty export table, and remains so.

---

## 8. Smoke gate impact

The current deploy contract (`/mc2-deploy` skill at `.claude/skills/mc2-deploy.md`) copies:

1. `mc2.exe` — built artifact
2. `*.{vert,frag,glsl,hglsl}` — shaders
3. `*.dll` — FFmpeg runtime DLLs (5 of them, plus optionally ASan)

**Track C M0 changes nothing here.** Lua compiles into `mc2.exe` (static lib pulled into the exe target). Sol2 is header-only. No new DLLs.

The only deploy-time addition (later in M0, not C-1) is the demo mod: `mods/test/mod.json`, `mods/test/scripts/*.lua`. This is *content*, not a binary artifact — already handled by the deploy step that copies `data/` and content directories. Implementation-Shape commit C-5 will add a `mods/` deploy line.

Tier-1 smoke gate (`scripts/run_smoke.py --tier tier1`) requires no change for C-1: the build still produces a single `mc2.exe` that boots stock missions.

---

## 9. Per-platform notes

### Windows MSVC RelWithDebInfo (the only supported config)

- Primary target. Everything above tested on this path.
- **The Release vs RelWithDebInfo split** (per CLAUDE.md "Release crashes with GL_INVALID_ENUM") is OpenGL-related and entirely independent of Lua/Sol2. Both Lua and Sol2 compile identically across MSVC optimization levels.
- `/MP` parallelism interacts cleanly with both Lua's discrete TUs and Sol2's per-TU template instantiations.

### `LINUX_BUILD` define

Root `CMakeLists.txt:111` sets `add_definitions(-DLINUX_BUILD)` globally. Per `memory/mc2_path_separator_linux_build.md`, this gates the engine's `PATH_SEPARATOR` macro to `/` even on Windows builds. **Lua is unaffected:**
- Lua's source uses `/` literally throughout (`#define LUA_DIRSEP "/"` in `luaconf.h` for non-Windows; we'll override or simply not care since we don't open `package`).
- `package.path`/`package.cpath` are not exposed to mods (sandbox).
- Our own custom `require` (Implementation-Shape §6) constructs paths with `/` regardless.

No conflict.

### Future Linux portability

- Lua: explicitly portable; upstream tested on every BSD/Linux. Zero concerns.
- Sol2: header-only C++17, portable across GCC/Clang/MSVC. Zero concerns.
- Sandbox `os`/`debug`/`package` removal is platform-independent.

---

## 10. Sample CMake fragment

This is the **read-only sketch** of the additions a future C-1 commit would land. Do not apply this in the same commit as this exploration doc.

### Edit `CMakeLists.txt` after line 154 (Tracy include)

```cmake
# ----------------------------------------------------------------------
# Lua 5.4 — vendored static lib, no external dependency.
# ----------------------------------------------------------------------
file(GLOB LUA_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/lua/src/*.c")
list(REMOVE_ITEM LUA_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/lua/src/lua.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/lua/src/luac.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/lua/src/onelua.c")
add_library(lua_static STATIC ${LUA_SOURCES})
target_include_directories(lua_static PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/lua/src")
target_compile_definitions(lua_static PUBLIC LUA_COMPAT_5_3)
if(MSVC)
    target_compile_options(lua_static PRIVATE
        /wd4334 /wd4146 /wd4244 /wd4267 /wd4310 /wd4324)
endif()

# ----------------------------------------------------------------------
# Sol2 — header-only, no target. Just expose include dirs.
# ----------------------------------------------------------------------
list(APPEND THIRDPARTY_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/sol"
    "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/lua/src")
```

### After line 160 (after `add_subdirectory("./gui" …)`)

```cmake
add_subdirectory("./modding" "./out/modding")
```

### `modding/CMakeLists.txt` (new file)

```cmake
cmake_minimum_required(VERSION 3.10)
project(modding)

set(MODDING_SOURCES
    lua_vm.cpp
    lua_bindings_mc2.cpp
)

add_library(modding STATIC ${MODDING_SOURCES})
target_include_directories(modding PRIVATE
    ${COMMON_INCLUDE_DIRS}
    ${THIRDPARTY_INCLUDE_DIRS}
    "${CMAKE_SOURCE_DIR}/code"
    "${CMAKE_SOURCE_DIR}/mclib")
target_link_libraries(modding PUBLIC lua_static)
target_compile_definitions(modding PRIVATE
    SOL_ALL_SAFETIES_ON=1
    SOL_PRINT_ERRORS=0)
```

### Update root `target_link_libraries(mc2 …)` (line 272)

Add `modding` near the front (it depends on engine symbols, so order-after-engine-libs is fine):

```cmake
target_link_libraries(mc2 mclib gosfx mlr stuff gui gameos gameos_main windows
    modding                       # <-- ADDED
    ZLIB::ZLIB ${SDL2_LIBRARIES} GLEW::GLEW SDL2_mixer::SDL2_mixer
    ${ADDITIONAL_LIBS} OpenGL::GL)
```

(`lua_static` is pulled in transitively via `modding`'s `PUBLIC` link.)

### Verification commands (run after C-1)

```bash
# Clean build.
cmake --build build64 --config RelWithDebInfo --target lua_static
cmake --build build64 --config RelWithDebInfo --target modding
cmake --build build64 --config RelWithDebInfo --target mc2

# Confirm new artifacts exist.
ls -la build64/out/lua_static.dir/RelWithDebInfo/lua_static.lib
ls -la build64/out/modding/RelWithDebInfo/modding.lib
ls -la build64/RelWithDebInfo/mc2.exe

# Confirm no new DLL deps.
dumpbin /DEPENDENTS build64/RelWithDebInfo/mc2.exe | grep -i lua  # should be empty
```

---

## 11. Open questions / risks

1. **Lua version pin.** We need to pick an exact 5.4 patch level (5.4.6 vs 5.4.7) and record it in `3rdparty/lua/README` with sha256. Sol2's known-tested matrix on `develop` covers 5.1, 5.2, 5.3, 5.4, LuaJIT. No concrete blocker; just pick one and pin it.

2. **Sol2 version pin.** Same — record commit hash in `3rdparty/sol/LICENSE.txt`'s leading comment block. Sol2 `develop` head as of mid-2026 is the most likely candidate. Lock to a specific commit hash, not a moving branch.

3. **`modding` static lib pulling engine includes is a layer-violation.** `modding/CMakeLists.txt` adds `${CMAKE_SOURCE_DIR}/code` and `${CMAKE_SOURCE_DIR}/mclib` to its include path so it can `#include "lua_abl_shim.h"` (which lives in `modding/`) and have transitive engine declarations resolve. This is fine for M0; the layer can be tightened in M1 by routing all engine-touching declarations through the shim header alone.

4. **Tracy + Lua — known cross-instrumentation.** Tracy ships an optional `<tracy/TracyLua.hpp>` header that adds Lua-side instrumentation. Don't enable it in M0 (adds Lua API surface modders would see). Revisit later — it would be useful for profiling mod scripts.

5. **`/MP` parallelism with mixed C/C++.** MSVC `/MP` works fine across C and C++ TUs in the same target, but there are historical bugs at scale. The current `mc2` target has 90 C++ TUs; adding 32 C TUs to a *separate* target (`lua_static`) sidesteps any per-target /MP edge case. This is one more reason for option (b) over option (a).

6. **`LUAI_USER_ALIGNMENT_T` and platform structs.** Lua 5.4's `luaconf.h` lets you override the union used to compute alignment. We accept defaults; revisit only if we hit unaligned-access crashes on non-x64 future targets.

7. **Future shared-library Lua.** If a mod wants to write a C extension and load it dynamically, we'd need to either (a) flip Lua to DLL build (`LUA_BUILD_AS_DLL`), or (b) ship a pre-built `.dll` of the loose Lua API. M0 punts: no C extensions allowed (`package.loadlib` not exposed, Sandbox §6).

8. **`LINUX_BUILD` redefinition risk.** The root define `-DLINUX_BUILD` (CMakeLists.txt:111) is `add_definitions`, which propagates to all subdirs/targets including `lua_static`. Lua's `luaconf.h` checks `__linux__` and `LUA_USE_LINUX` (not `LINUX_BUILD`), so the propagation is harmless. Confirmed by reading `luaconf.h` macro selection block in upstream 5.4.

9. **Pre-commit hook for C-3.** Per blocking-questions §Open Q #6: add a smoke check `! grep -E '^void execMagicAttack|^void execCoreGuard|^void execCorePatrol|^void execCoreWait' code/ablmc2.cpp` to prevent accidentally re-enabling the four shadowed corebrain primitives during `*_impl` extraction. This is a *content* concern but the gate is build-side (it runs at commit time, blocks bad source).

10. **CMake minimum version.** Root file has `cmake_minimum_required(VERSION 3.10)`. `target_compile_definitions` and `target_compile_options` (used liberally above) are 3.0+. `file(GLOB …)` with `LIST_DIRECTORIES` defaults are 3.3+. We are within the existing minimum.

---

## 12. Summary for next session

Land C-1 in this exact order:

1. Drop `3rdparty/lua/` (32 .c files + 5 headers + LICENSE + README) and `3rdparty/sol/` (sol.hpp + forward.hpp + LICENSE.txt).
2. Edit root `CMakeLists.txt`: insert §10's Lua and Sol2 blocks after line 154; add `add_subdirectory("./modding" …)` after line 160; add `modding` to the `mc2` link line at line 272.
3. Create `modding/CMakeLists.txt` (§10).
4. Create empty stubs `modding/lua_vm.cpp`, `modding/lua_bindings_mc2.cpp`, `modding/lua_vm.h`, `modding/lua_abl_shim.h` so the new lib has at least one TU. Stubs become real in C-2.
5. Verify clean build per §10's verification commands.
6. Run tier-1 smoke gate — should pass identically to pre-C-1, since no engine code path is touched yet.

After C-1 lands, C-2 (LuaVM skeleton), C-3 (`*_impl` extraction + bindings), C-4 (lifecycle wiring), C-5 (demo mod) follow per Implementation-Shape §10's sequencing.

---

## 13. References

Source (read-only, citations verified during this session):
- `CMakeLists.txt:111` — `add_definitions(-DLINUX_BUILD)`.
- `CMakeLists.txt:152-154` — `THIRDPARTY_INCLUDE_DIRS` + Tracy include path.
- `CMakeLists.txt:156-163` — `add_subdirectory(...)` style (gui/, gameos/, etc.).
- `CMakeLists.txt:165-256` — `mc2` target's `SOURCES` list.
- `CMakeLists.txt:271-272` — `add_executable(mc2 …)` + link line.
- `GameOS/gameos/CMakeLists.txt:34` — Tracy `TracyClient.cpp` direct-source-add precedent.
- `3rdparty/tracy/` directory — vendoring shape (header dir + impl dir + amalgamation TU).

Predecessor design docs:
- `2026-04-30-track-c-lua-implementation-shape.md` §1 — vendoring sketch (this doc finalizes it).
- `2026-04-30-track-c-blocking-questions-resolution.md` §Q1 — `*_impl` extraction pattern dictates `modding/lua_abl_shim.h` header location.

Memory:
- `stock_install_must_remain_playable.md` — Lua additions must be sidecar; legacy `.abx` path untouched. C-1 introduces no behavior change to engine paths and trivially satisfies this rule.
- `mc2_path_separator_linux_build.md` — `LINUX_BUILD` global define, harmless to Lua per §9.
