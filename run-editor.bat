@echo off
REM Mission Editor launcher for claude/nifty-mendeleev.
REM
REM CWD: A:\Games\mc2-opengl\mc2-editor  (mc2-win64-v0.4 clone with overlays)
REM   data/art/editorGui.fit      <- PR#36 (Omnitech-dir overlay)
REM   data/effects/mc2.fx         <- PR#36 (Omnitech-dir overlay)
REM   data/art/Buildings.csv      <- v0.1.1 stock (fitID-coupled with Object2.pak)
REM   data/objects/Object2.pak    <- v0.1.1 stock
REM   data/defs/catalogs|menus|text/en_us/editor/*.fit <- committed in this worktree
REM   system.cfg                  <- required or InitializeGameEngine() early-returns
REM
REM Required files without which Editor.cpp:351 early-returns and eye/editor stay
REM NULL, making every interaction crash:
REM   data/effects/mc2.fx
REM   data/art/editorGui.fit
REM   system.cfg
REM
REM MC2_EDITOR_TRACE=1: writes editor-startup.log to CWD for launch debugging.
REM Keep this on so logs are always captured.
REM
REM S3 (2026-05-25): MC2_GPU_DRIVEN / MC2_EDITOR_BYPASS_BLDG_CULL /
REM MC2_STATIC_PROP_REGISTRY env sidesteps retired. After S2..S2.10 wired the
REM editor onto the canonical render-world chain, the editor now runs the
REM default-ON game paths (GPU-driven terrain/water, static-prop registry).
REM See docs/superpowers/plans/2026-05-25-editor-rebuild.md.

set MC2_EDITOR_TRACE=1

cd /d "A:\Games\mc2-opengl\mc2-editor"
"A:\Games\mc2-opengl\mc2-editor\Mission Editor.exe" 2>"A:\Games\mc2-opengl\mc2-editor\editor-stderr.log"
