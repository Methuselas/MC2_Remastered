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
REM MC2_EDITOR_BYPASS_BLDG_CULL=1: recalcBounds in bdactor.cpp has ~87%
REM false-negative rate at zoomed-out camera after the 2026-05-18 projected-
REM body deletion.  The game survives via GPU compute cull; the editor has
REM neither GPU cull nor BldgAppearance::render bypass.  This forces
REM recalcBounds to return true for Bldg/Tree appearances so they render.
REM
REM MC2_EDITOR_TRACE=1: writes editor-startup.log to CWD for launch debugging.
REM Keep this on so logs are always captured.

set MC2_EDITOR_TRACE=1
set MC2_EDITOR_BYPASS_BLDG_CULL=1
REM MC2_GPU_DRIVEN=0 retired 2026-05-25 -- editor terrain MVP publish
REM converged to gos_SetWorldToClipGL (commit 2160fc9c); GPU cull-compute
REM now fires correctly. Editor is a GPU-only test bed per
REM memory/editor_is_gpu_only_testbed.md.
REM MC2_GPU_OBJECTS=0 retired 2026-05-24 -- editor now wires GPU static-prop batcher
REM MC2_GPU_MECHS=0   retired 2026-05-24 -- editor now wires GPU mech batcher
REM Retired 2026-05-25 (E4c):
REM   MC2_STATIC_PROP_REGISTRY=0  -- editor now wires GpuStaticPropRegistry
REM                                  init/destroy/frameBegin (this commit).
REM                                  Editor on same GPU path as game per
REM                                  memory/editor_is_gpu_only_testbed.md.
REM See docs/superpowers/plans/2026-05-24-editor-object-loop-gpu-port.md

cd /d "A:\Games\mc2-opengl\mc2-editor"
"A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\build64\out\editor\RelWithDebInfo\Mission Editor.exe"
