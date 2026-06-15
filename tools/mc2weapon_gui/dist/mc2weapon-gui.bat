@echo off
rem Launches the ImGui weapon editor. Run from the game install root so SDL2.dll /
rem glew32.dll resolve and effects.csv is found under data\objects\.
rem compbas.csv is packed in v0.4 -- pass a loose copy with --compbas, e.g.:
rem   mc2weapon-gui.bat --compbas mods\my-weapons\data\objects\compbas.csv
cd /d "%~dp0.."
start "" "%~dp0mc2weapon_gui.exe" %*
