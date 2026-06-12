#!/usr/bin/env python3
# scripts/test_smoke_guard.py
"""Unit test for the path-aware mc2.exe process guard in run_smoke.py.

This is the PRIMARY proof that the smoke runner's existing-process guard
discriminates by deploy path -- NOT by image name. It feeds the PURE filter
`_same_path_mc2` synthetic process lists and asserts which PIDs are returned.

No process is launched. Deterministic. Run:  py -3 scripts/test_smoke_guard.py
"""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from scripts.run_smoke import _same_path_mc2

# Canonical v0.4 deploy target (matches run_smoke.DEFAULT_EXE).
TARGET = r"A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe"
TARGET_DIR = r"A:/Games/mc2-opengl/mc2-win64-v0.4"
FOREIGN = r"A:/Games/mc2-opengl/mtp-test/mc2.exe"


def _pids(matched):
    return sorted(p for p, _ in matched)


def run():
    failures = []

    def check(name, got, expected):
        if got == expected:
            print(f"  PASS {name}: {got}")
        else:
            print(f"  FAIL {name}: got {got}, expected {expected}")
            failures.append(name)

    # T1: same-path PID (111) returned, foreign full-path PID (222) excluded.
    procs = [
        (111, TARGET, f'"{TARGET}" -mission mc2_01'),
        (222, FOREIGN, f'"{FOREIGN}" -mission mc2_01'),
    ]
    check("T1 same-path-only", _pids(_same_path_mc2(procs, TARGET)), [111])

    # T2: case + slash variants of the SAME exe still match.
    procs = [
        (111, r"A:\GAMES\MC2-OPENGL\MC2-WIN64-V0.4\MC2.EXE", ""),
    ]
    check("T2 case/slash-variant", _pids(_same_path_mc2(procs, TARGET)), [111])

    # T3: all-foreign -> empty.
    procs = [
        (222, FOREIGN, ""),
        (333, r"C:/some/other/mc2.exe", ""),
    ]
    check("T3 all-foreign", _pids(_same_path_mc2(procs, TARGET)), [])

    # T4a: null ExecutablePath but CommandLine references the v0.4 deploy dir
    #      -> matched (fallback path).
    procs = [
        (444, "", f'"{TARGET}" -mission mc2_03'),
    ]
    check("T4a null-path+v0.4-cmdline", _pids(_same_path_mc2(procs, TARGET)), [444])

    # T4b: null ExecutablePath + CommandLine pointing at a FOREIGN deploy
    #      -> not matched (foreign).
    procs = [
        (555, "", f'"{FOREIGN}" -mission mc2_03'),
    ]
    check("T4b null-path+foreign-cmdline", _pids(_same_path_mc2(procs, TARGET)), [])

    # T5: null ExecutablePath + no usable CommandLine hint -> treated FOREIGN
    #     (NOT blocked: never block on an unknown process).
    procs = [
        (666, "", ""),
        (777, "", "mc2.exe"),  # bare name, no path/dir hint
    ]
    check("T5 null-path+no-hint", _pids(_same_path_mc2(procs, TARGET)), [])

    print()
    if failures:
        print(f"RESULT: {len(failures)} FAILED: {failures}")
        return 1
    print("RESULT: ALL PASS (T1-T5)")
    return 0


if __name__ == "__main__":
    sys.exit(run())
