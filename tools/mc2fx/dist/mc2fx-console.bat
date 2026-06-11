@echo off
rem Double-click: opens a console at the install root with the mc2fx tools on PATH.
rem (mc2fx.exe is a command-line tool; this is the convenient way to run it.)
cd /d "%~dp0.."
set PATH=%~dp0;%PATH%
echo MC2 gosFX tools.  data\effects\mc2.fx is here.  See tools\README.md.
echo   mc2fx dump --full data\effects\mc2.fx
echo   mc2fx clone data\effects\mc2.fx PPC_Trail Plasma_Trail my.fx
echo   mc2fx build data\effects\mc2.fx patch.json out.fx
echo   mc2fx_preview data\effects\mc2.fx
echo.
cmd /k
