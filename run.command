#!/bin/bash
# Double-click launcher for macOS. Creates the venv on first run.
# One-time setup: chmod +x run.command
cd "$(dirname "$0")"
if [ ! -d .venv ]; then
    python3 -m venv .venv
    ./.venv/bin/python -m pip install -r requirements.txt
fi
./.venv/bin/python rta.py
