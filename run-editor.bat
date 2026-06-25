@echo off
REM Mission Editor launcher for MC2 OpenGL Remastered v0.4 beta.
REM
REM Resolves paths relative to this .bat file so the install works regardless
REM of where the user extracted the zips.
REM
REM MC2_EDITOR_TRACE=1: writes editor-startup.log to CWD for launch debugging.
REM Keep this on so logs are always captured for modder bug reports.

set MC2_EDITOR_TRACE=1
REM MC2_PATROL_TRACE=1: logs per-unit brain .abl open attempts + imported patrol
REM waypoints to editor-stderr.log (patrol/order overlay debugging).
set MC2_PATROL_TRACE=1

cd /d "%~dp0"
"%~dp0Mission Editor.exe" 2>"%~dp0editor-stderr.log"
