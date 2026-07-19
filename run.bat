@echo off
rem Double-click launcher for Windows. Creates the venv on first run.
cd /d %~dp0
if not exist .venv (
    where py >nul 2>nul && (py -3 -m venv .venv) || (python -m venv .venv)
    .venv\Scripts\python -m pip install -r requirements.txt
)
.venv\Scripts\python rta.py
