@echo off
REM Mission Editor launcher for claude/nifty-mendeleev (canonical worktree).
REM
REM CWD: A:\Games\mc2-opengl\mc2-win64-v0.4
REM   -- editor + game now share one install (migrated 2026-05-25).
REM   -- deploy editor into v0.4 with: scripts/deploy-editor.sh
REM
REM Editor-specific content this install needs (vs a game-only v0.4):
REM   data/art/Buildings.csv      <- v0.1.1 stock (fitID-coupled with Object2.pak)
REM                                  Game (mc2.exe) does NOT read this file --
REM                                  only EditorInterface.cpp:596 does.
REM   data/art/editorGui.fit      <- already in v0.4 (PR#36)
REM   data/effects/mc2.fx         <- already in v0.4 (PR#36)
REM   data/defs/catalogs|menus|text/en_us/editor/*.fit <- already in v0.4
REM   esplash.bmp, tacsplash.bmp  <- already in v0.4
REM   system.cfg                  <- required or Editor.cpp:351 early-returns
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

cd /d "A:\Games\mc2-opengl\mc2-win64-v0.4"
"A:\Games\mc2-opengl\mc2-win64-v0.4\Mission Editor.exe" 2>"A:\Games\mc2-opengl\mc2-win64-v0.4\editor-stderr.log"
