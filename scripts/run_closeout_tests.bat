@echo off
setlocal EnableDelayedExpansion
REM ============================================================================
REM  MC2 terrain-closeout validation launcher
REM  Runs the interactive/visible tests that the headless smoke harness cannot:
REM    1. Game FASTPATH steady-state proof   (MC2_FASTPATH_DROP_LOG=1)
REM    2. Editor arming (VISIBLE, not headless -- headless frustum is degenerate)
REM    3. A5 mine-handle warmup test          (MC2_SETUPSKIP_WARMUP=1)
REM
REM  DEPLOY-TARGET TRAP (load-bearing): the GAME runs from mc2-win64-v0.4 and the
REM  EDITOR runs from mc2-win64-0.4c. BOTH folders contain mc2.exe AND
REM  "Mission Editor.exe" -- do NOT cross them. Pinned explicitly below.
REM
REM  Each test launches VISIBLE and blocks until you CLOSE the app, then prints a
REM  summary of [FASTPATH_DROP] transitions found in the captured stdout log.
REM  stdout is redirected to scripts\closeout-logs\<test>.log (overwritten each run).
REM ============================================================================

set "GAME_DIR=A:\Games\mc2-opengl\mc2-win64-v0.4"
set "GAME_EXE=mc2.exe"
set "EDITOR_DIR=A:\Games\mc2-opengl\mc2-win64-0.4c"
set "EDITOR_EXE=Mission Editor.exe"
set "LOGDIR=%~dp0closeout-logs"
if not exist "%LOGDIR%" mkdir "%LOGDIR%"

:menu
echo.
echo ============= MC2 Terrain Closeout Tests =============
echo   1. Game FASTPATH (interactive)
echo        v0.4\mc2.exe  +  MC2_FASTPATH_DROP_LOG=1
echo        Goal: 0 steady-state drops during pan/destruction/mine play.
echo.
echo   2. Editor arming (VISIBLE, interactive)
echo        0.4c\"Mission Editor.exe"  +  MC2_FASTPATH_DROP_LOG=1
echo        Goal: confirm GPU-indirect arms; toggle View ^> Show Passability
echo              Map to see the drawTerrainGrid (T13) fallback transition.
echo.
echo   3. Mine warmup test  (A5 setupTextures-deletion fix validation)
echo        v0.4\mc2.exe  +  MC2_SETUPSKIP_WARMUP=1  +  MC2_FASTPATH_DROP_LOG=1
echo        NOTE: MC2_SETUPSKIP_WARMUP is a PLANNED gate -- it does nothing until
echo              the A5 fix lands. Load a mission with MINE tiles; mines must
echo              still render once the fix + this env gate are implemented.
echo.
echo   Q. Quit
echo =====================================================
set "choice="
set /p choice="Select (1/2/3/Q): "
if /I "%choice%"=="1" goto game_fastpath
if /I "%choice%"=="2" goto editor_arming
if /I "%choice%"=="3" goto mine_warmup
if /I "%choice%"=="Q" goto end
goto menu

:game_fastpath
call :checkrunning "%GAME_EXE%"
set "LOG=%LOGDIR%\game_fastpath.log"
echo.
echo Launching GAME (v0.4) with MC2_FASTPATH_DROP_LOG=1
echo   Play a mission: pan hard, destroy structures, drive over mines.
echo   Then EXIT the game normally to capture the log.
echo   Log: %LOG%
pushd "%GAME_DIR%"
set "MC2_FASTPATH_DROP_LOG=1"
"%GAME_EXE%" > "%LOG%" 2>&1
set "MC2_FASTPATH_DROP_LOG="
popd
call :summarize "%LOG%"
goto menu

:editor_arming
call :checkrunning "%EDITOR_EXE%"
set "LOG=%LOGDIR%\editor_arming.log"
echo.
echo Launching EDITOR (0.4c) VISIBLE with MC2_FASTPATH_DROP_LOG=1
echo   Generate/open a map. Confirm terrain renders (NOT black).
echo   Toggle View ^> Show Passability Map ON then OFF to drive the
echo   drawTerrainGrid (T13) fallback -- you should see ARMED_TO_FALLBACK
echo   then RECOVERY transitions in the summary.
echo   Then CLOSE the editor to capture the log.
echo   Log: %LOG%
pushd "%EDITOR_DIR%"
set "MC2_FASTPATH_DROP_LOG=1"
"%EDITOR_EXE%" > "%LOG%" 2>&1
set "MC2_FASTPATH_DROP_LOG="
popd
call :summarize "%LOG%"
goto menu

:mine_warmup
call :checkrunning "%GAME_EXE%"
set "LOG=%LOGDIR%\mine_warmup.log"
echo.
echo Launching GAME (v0.4) with MC2_SETUPSKIP_WARMUP=1 + MC2_FASTPATH_DROP_LOG=1
echo   This forces skipSetup from frame 0 (no setupTextures warmup pass).
echo   Load a mission with MINE tiles and confirm mines RENDER (not black).
echo   Pass  = mines visible  -> A5 mine-handle migration is correct.
echo   Fail  = mine tiles black -> InitMineTextureHandles() not wired yet.
echo   (Until the A5 fix lands this env is inert; mines render via the warmup pass.)
echo   Then EXIT the game to capture the log.
echo   Log: %LOG%
pushd "%GAME_DIR%"
set "MC2_SETUPSKIP_WARMUP=1"
set "MC2_FASTPATH_DROP_LOG=1"
"%GAME_EXE%" > "%LOG%" 2>&1
set "MC2_SETUPSKIP_WARMUP="
set "MC2_FASTPATH_DROP_LOG="
popd
call :summarize "%LOG%"
goto menu

REM --- helpers ----------------------------------------------------------------
:checkrunning
tasklist /FI "IMAGENAME eq %~1" 2>NUL | find /I "%~1" >NUL
if not errorlevel 1 (
  echo   WARNING: %~1 is already running. Close that instance first to avoid
  echo            smoke-lock / orphan-process issues, then continue.
  pause
)
goto :eof

:summarize
echo.
echo ----- [FASTPATH_DROP] transitions in %~nx1 -----
findstr /C:"[FASTPATH_DROP]" "%~1"
if errorlevel 1 (
  echo   ^(none found -- stayed armed, or nothing transitioned^)
) else (
  for /f %%C in ('findstr /C:"[FASTPATH_DROP]" "%~1" ^| find /c /v ""') do echo   total transitions: %%C
  echo   ^>^> classify: warmup/one-shot are OK; CONTINUOUS steady-state = 8z blocker.
)
echo   full log: %~1
echo ------------------------------------------------
goto :eof

:end
endlocal
