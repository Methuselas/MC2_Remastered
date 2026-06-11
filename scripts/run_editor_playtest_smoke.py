#!/usr/bin/env python3
# scripts/run_editor_playtest_smoke.py
r"""End-to-end PLAYTEST smoke for the MC2 Mission Editor.

Drives the editor's `-playtest` CLI mode (editor/EditorMFC.cpp): the editor loads
a mission headless, then triggers the REAL one-click Playtest path
(EditorPlaytest::Start()) -- exactly what a user clicking the "Playtest in Game"
button does -- with the launched game in smoke mode so it auto-quits cleanly.
The editor then exits with a code that encodes the child's fate:

  editor rc 0  -> child exited 0           (PASS)
  editor rc 2  -> launch/bridge/save abort (Start never went Running)
  editor rc 3  -> child exited nonzero     (game crash / mission-content fail)
  editor rc 4  -> timeout (child never completed in the window)

The verdict is read from the machine-readable line the editor emits on
completion (mirrors run_smoke.py / run_editor_smoke.py: the app reports facts,
the runner judges):

  [ESMOKE v1] event=playtest exit=<childExit> log=<archivedLogPath> mod=<id-or-none>

editor-startup.log (opened "w" at process start, under the deploy dir) is the
reliable IPC channel for a WIN32-subsystem app; stdout is also captured via an
inherited pipe.

SINGLE-INSTANCE GUARD (run_smoke.py lock philosophy): the playtest spawns a real
mc2.exe child. If an mc2.exe is ALREADY running we refuse to start, so we never
fight a concurrent game/smoke for the deployed exe + data\missions bridge.

Usage:
  py -3 scripts/run_editor_playtest_smoke.py --mission <pak path> [--timeout SEC]
                                             [--exe PATH] [--keep-logs]
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
DEFAULT_EXE = r"A:\Games\mc2-opengl\mc2-win64-0.4c\Mission Editor.exe"
ARTIFACT_ROOT = REPO / "tests" / "smoke" / "editor"

# Strong crash signatures (the editor trace is verbose; weak tokens false-positive).
CRASH_TOKENS = ("unhandled exception", "access violation", "stack overflow", "fatal error")


def _running_mc2() -> list[int]:
    """PIDs of any live mc2.exe (the game the playtest will spawn)."""
    try:
        out = subprocess.check_output(
            ["tasklist", "/FI", "IMAGENAME eq mc2.exe", "/NH", "/FO", "CSV"],
            text=True, stderr=subprocess.DEVNULL)
    except Exception:
        return []
    pids = []
    for line in out.splitlines():
        if "mc2.exe" in line:
            parts = [p.strip('"') for p in line.split(",")]
            if len(parts) > 1 and parts[1].isdigit():
                pids.append(int(parts[1]))
    return pids


def _parse_playtest(text: str) -> dict:
    """Pull the `[ESMOKE v1] event=playtest ...` k=v fields (last match wins)."""
    fields = {}
    for m in re.finditer(r"\[ESMOKE v1\] event=playtest (.+)", text):
        fields = {}
        for kv in m.group(1).split():
            if "=" in kv:
                k, v = kv.split("=", 1)
                fields[k] = v
    return fields


def main() -> int:
    ap = argparse.ArgumentParser(description="MC2 Mission Editor end-to-end playtest smoke")
    ap.add_argument("--mission", required=True,
                    help="path to the mission .pak the editor loads then playtests")
    ap.add_argument("--exe", default=DEFAULT_EXE, help="path to Mission Editor.exe")
    ap.add_argument("--timeout", type=int, default=120,
                    help="editor -playtest-timeout-sec (and the process backstop margin)")
    ap.add_argument("--active-mod", default=None,
                    help="MC2_ACTIVE_MOD for the editor (default: auto-detected from the "
                         "mission path's mods/<id>/ component, so the editor mounts the "
                         "mod's appearance assets when loading)")
    ap.add_argument("--keep-logs", action="store_true",
                    help="copy editor-startup.log into the report dir")
    ap.add_argument("--minimized", dest="minimized", action="store_true", default=True)
    ap.add_argument("--no-minimized", dest="minimized", action="store_false")
    args = ap.parse_args()

    exe = Path(args.exe)
    deploy = exe.parent
    mission = Path(args.mission)
    if not exe.exists():
        print(f"ERROR: editor exe not found: {exe}\n  Build EditRel + deploy-editor (DEPLOY=...0.4c) first.")
        return 2
    if not mission.exists():
        print(f"ERROR: mission pak not found: {mission}")
        return 2

    # SINGLE-INSTANCE GUARD: refuse if a game is already running (the playtest
    # spawns its own mc2.exe and bridges into the deployed data\missions).
    pids = _running_mc2()
    if pids:
        print(f"ERROR: mc2.exe already running (pids {pids}); refusing to run the "
              f"playtest smoke (it would fight for the deployed exe / mission bridge).\n"
              f"  Close the game first.")
        return 3

    log = deploy / "editor-startup.log"
    try:
        if log.exists():
            log.unlink()
    except OSError:
        pass

    # The editor backstop is the playtest timeout; give the OUTER process wait a
    # generous margin (editor warm frames + child launch + child duration + archive).
    proc_timeout = args.timeout + 120

    argv = [str(exe), "-playtest", f"-playtest-timeout-sec={args.timeout}",
            f"-mission={mission}", "-exit-on-load-fail"]
    env = dict(os.environ)
    env["MC2_EDITOR_TRACE"] = "1"

    # Mount the mod so the editor can load the mission's mech/object appearances.
    # Auto-detect mods/<id>/ in the mission path unless explicitly overridden.
    active_mod = args.active_mod
    if active_mod is None:
        parts = [p.lower() for p in mission.parts]
        if "mods" in parts:
            idx = parts.index("mods")
            if idx + 1 < len(mission.parts):
                active_mod = mission.parts[idx + 1]
    if active_mod:
        env["MC2_ACTIVE_MOD"] = active_mod
        print(f"[playtest] MC2_ACTIVE_MOD={active_mod}")

    startupinfo = None
    if args.minimized and os.name == "nt":
        startupinfo = subprocess.STARTUPINFO()
        startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startupinfo.wShowWindow = 7  # SW_SHOWMINNOACTIVE

    print(f"[playtest] launch editor: -mission {mission.name}  timeout={args.timeout}s"
          f"{'  [minimized]' if startupinfo else ''}")
    t0 = time.time()
    timed_out = False
    try:
        proc = subprocess.Popen(argv, cwd=str(deploy), env=env, startupinfo=startupinfo,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            out, _ = proc.communicate(timeout=proc_timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            proc.kill()
            out, _ = proc.communicate()
        rc = proc.returncode
    except FileNotFoundError:
        print(f"ERROR: could not launch editor exe: {exe}")
        return 2
    dt = round(time.time() - t0, 1)

    logtext = log.read_text(errors="replace") if log.exists() else ""
    combined = (out or "") + "\n" + logtext
    pt = _parse_playtest(combined)

    # Verdict --------------------------------------------------------------------
    bucket = ""
    if timed_out:
        bucket = "process_timeout"   # editor never exited (its own backstop failed)
    elif not pt:
        bucket = "no_playtest_summary"
    else:
        status = pt.get("status", "")
        child_exit = pt.get("exit", "?")
        if status == "launch_abort":
            bucket = "launch_abort"
        elif status == "timeout":
            bucket = "playtest_timeout"
        elif child_exit != "0":
            bucket = f"child_nonzero:{child_exit}"
        else:
            low = combined.lower()
            for tok in CRASH_TOKENS:
                if tok in low:
                    bucket = f"crash_token:{tok}"
                    break

    passed = (bucket == "") and rc == 0
    verdict = "PASS" if passed else "FAIL"

    # Report ---------------------------------------------------------------------
    ts = time.strftime("%Y-%m-%dT%H-%M-%S")
    rptdir = ARTIFACT_ROOT / f"playtest-{ts}"
    rptdir.mkdir(parents=True, exist_ok=True)

    esmoke_line = ""
    m = re.search(r"\[ESMOKE v1\] event=playtest .+", combined)
    if m:
        esmoke_line = m.group(0)

    lines = [
        f"# Editor playtest smoke {ts}  result={verdict}",
        "",
        f"- mission: `{mission}`",
        f"- editor exit: {rc}",
        f"- child exit: {pt.get('exit', '-')}",
        f"- mod: {pt.get('mod', '-')}",
        f"- archived log: {pt.get('log', '-')}",
        f"- bucket: {bucket or '-'}",
        f"- wall seconds: {dt}",
        "",
        "## ESMOKE line",
        "",
        "```",
        esmoke_line or "(none)",
        "```",
    ]
    report = "\n".join(lines) + "\n"
    (rptdir / "report.md").write_text(report, encoding="utf-8")
    if args.keep_logs and logtext:
        (rptdir / "editor-startup.log").write_text(logtext, encoding="utf-8", errors="replace")
    (rptdir / "editor-stdout.txt").write_text(out or "", encoding="utf-8", errors="replace")

    print("\n" + report)
    print(f"report: {rptdir / 'report.md'}")

    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
