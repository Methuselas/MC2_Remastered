#!/usr/bin/env python3
"""
Worktree-local smoke wrapper: forces --exe to v0.4 so automated runs test
the actual deployed binary rather than the shared DEFAULT_EXE (v0.3).
Usage: py -3 scripts/run_smoke_local.py [same args as run_smoke.py]
"""
import sys
import subprocess
from pathlib import Path

V4_EXE = Path(r"A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe")

smoke = Path(__file__).parent / "run_smoke.py"

args = sys.argv[1:]
if "--exe" not in args:
    args = ["--exe", str(V4_EXE)] + args

result = subprocess.run([sys.executable, str(smoke)] + args)
sys.exit(result.returncode)
