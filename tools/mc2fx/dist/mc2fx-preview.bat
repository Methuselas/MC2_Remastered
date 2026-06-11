@echo off
rem Double-click: launches the gosFX curve previewer on the stock effect blob.
rem cwd is set to the install root so SDL2.dll / glew32.dll and data\ resolve.
cd /d "%~dp0.."
start "" "%~dp0mc2fx_preview.exe" data\effects\mc2.fx
