# tinyexr

Single-header OpenEXR loader vendored from upstream:
https://github.com/syoyo/tinyexr

- Files:
  - tinyexr.h  — the loader (single-header)
  - miniz.h    — miniz public header (deps/miniz/miniz.h upstream)
  - miniz.c    — miniz implementation (deps/miniz/miniz.c upstream)
- Version: tinyexr (no formal version string; based on upstream release branch)
- Upstream commit: 4946b5d92e13bcc8102ac2c8efd129596a90bf75 (release branch)
- Vendored: 2026-05-25
- License:
  - tinyexr: BSD-3-Clause (Copyright 2014-2021 Syoyo Fujita and contributors)
  - miniz: public domain / Unlicense (miniz.c 3.0.0)

DO NOT EDIT.

`TINYEXR_IMPLEMENTATION` is defined exactly once, in
`GameOS/gameos/gos_hdri.cpp`. miniz.c is compiled separately by CMake
(registered in T7).

Used by HDRI-SKY-1 to load `.exr` equirectangular sky background
(see docs/superpowers/specs/2026-05-25-hdri-sky-1-design.md).
