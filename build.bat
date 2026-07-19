@echo off
rem Double-click to build ProdMesh Remote RTA (see build.ps1 for details).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1"
pause
