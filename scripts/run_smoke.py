#!/usr/bin/env python3
# scripts/run_smoke.py
"""MC2 smoke matrix runner.

Examples:
  python scripts/run_smoke.py --tier tier1 --fail-fast
  python scripts/run_smoke.py --tier tier1 --with-menu-canary
  python scripts/run_smoke.py --tier tier2
  python scripts/run_smoke.py --tier tier3 --kill-existing
  python scripts/run_smoke.py --mission mc2_01 --mission mc2_03
  python scripts/run_smoke.py --menu-canary
"""
from __future__ import annotations

import argparse
import atexit
import datetime as dt
import json
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from scripts.smoke_lib import baselines, manifest, report
from scripts.smoke_lib.runner import RunConfig, run_one
# Cockpit is strictly post-verdict and must never affect the smoke verdict,
# including via an import-time failure (e.g. SyntaxError in cockpit.py).
try:
    from scripts.smoke_lib import cockpit as _cockpit
except Exception:
    _cockpit = None
# Deploy-fingerprint check (advisory by default; MC2_SMOKE_REQUIRE_FINGERPRINT=1
# = hard fail). Import-safe: a broken module degrades to "check skipped".
try:
    from scripts.smoke_lib import fingerprint as _fingerprint
except Exception:
    _fingerprint = None

DEFAULT_EXE = Path(r"A:/Games/mc2-opengl/mc2-win64-v0.4c/mc2.exe")
ARTIFACT_ROOT = ROOT / "tests" / "smoke" / "artifacts"
MANIFEST_PATH = ROOT / "tests" / "smoke" / "smoke_missions.txt"
BASELINE_PATH = ROOT / "tests" / "smoke" / "baselines.json"
DEFAULT_MENU_SCRIPT = ROOT / "tests" / "smoke" / "menu_canary_first_mission.txt"
GAME_AUTO = ROOT / "scripts" / "game_auto.py"


def _parse_veg_summary(stdout_text: str) -> dict | None:
    """Parse VEG_SUMMARY line from captured engine output.

    Returns a dict with keys instance_count (int), draw_calls (int),
    flush_reason (str) if found; None if the line is absent.

    Format emitted by GosVegetation::emitSummary():
      VEG_SUMMARY instance_count=N draw_calls=N flush_reason=<reason>
    """
    import re as _re
    _VEG_RE = _re.compile(
        r"VEG_SUMMARY\s+"
        r"instance_count=(?P<ic>\d+)\s+"
        r"draw_calls=(?P<dc>\d+)\s+"
        r"flush_reason=(?P<fr>\S+)"
    )
    for line in reversed(stdout_text.splitlines()):
        m = _VEG_RE.search(line)
        if m:
            return {
                "instance_count": int(m.group("ic")),
                "draw_calls":     int(m.group("dc")),
                "flush_reason":   m.group("fr"),
            }
    return None


def _check_veg_floor(stem: str, veg: dict | None) -> list[str]:
    """VEG-SMOKE-FLOOR-1: assert veg output contract for a single mission.

    Returns a list of failure messages (empty = pass).
    Only called when MC2_VEGETATION_CARDS=1.

    Floors:
      mc2_01: instance_count > 100000  (dense forest mission)
      others: instance_count > 1000    (drier terrain — "not zero")
    """
    if veg is None:
        return [f"VEG-SMOKE-FLOOR FAIL {stem}: VEG_SUMMARY line absent from log "
                f"(engine may not have called emitSummary, or MC2_LOG not set)"]

    floor = 100000 if stem == "mc2_01" else 1000
    fails = []
    if veg["instance_count"] <= floor:
        fails.append(
            f"VEG-SMOKE-FLOOR FAIL {stem}: "
            f"instance_count={veg['instance_count']} draw_calls={veg['draw_calls']} "
            f"flush_reason={veg['flush_reason']} "
            f"(instance_count must be > {floor})")
    if veg["draw_calls"] <= 0:
        fails.append(
            f"VEG-SMOKE-FLOOR FAIL {stem}: "
            f"instance_count={veg['instance_count']} draw_calls={veg['draw_calls']} "
            f"flush_reason={veg['flush_reason']} "
            f"(draw_calls must be > 0)")
    if veg["flush_reason"] != "submitted":
        fails.append(
            f"VEG-SMOKE-FLOOR FAIL {stem}: "
            f"instance_count={veg['instance_count']} draw_calls={veg['draw_calls']} "
            f"flush_reason={veg['flush_reason']} "
            f"(flush_reason must be 'submitted')")
    return fails


def _norm_path(p: str) -> str:
    """Normalize an exe path for comparison: resolve, forward-slash, casefold.
    Windows paths are case-insensitive and may mix slashes; an unresolvable
    string (e.g. empty ExecutablePath) yields ''."""
    if not p:
        return ""
    try:
        return str(Path(p).resolve()).replace("\\", "/").casefold()
    except Exception:
        return p.replace("\\", "/").casefold()


def _enum_mc2_processes() -> list[tuple[int, str, str]]:
    """Return (pid, exe_path, command_line) for every running mc2.exe via
    Win32_Process metadata. ExecutablePath/CommandLine may be empty on PIDs
    we lack rights to query; callers fall back to CommandLine matching.

    This is the impure half (shells PowerShell). The pure path-discrimination
    logic lives in _same_path_mc2 below, which is what the unit test exercises."""
    ps = (
        "Get-CimInstance Win32_Process -Filter \"Name='mc2.exe'\" | "
        "ForEach-Object { "
        "[pscustomobject]@{pid=$_.ProcessId;path=$_.ExecutablePath;cmd=$_.CommandLine} } | "
        "ConvertTo-Json -Compress"
    )
    try:
        out = subprocess.check_output(
            ["powershell", "-NoProfile", "-NonInteractive", "-Command", ps],
            text=True, stderr=subprocess.DEVNULL)
    except Exception:
        return []
    out = (out or "").strip()
    if not out:
        return []
    try:
        data = json.loads(out)
    except Exception:
        return []
    if isinstance(data, dict):
        data = [data]
    procs = []
    for d in data:
        try:
            pid = int(d.get("pid"))
        except Exception:
            continue
        procs.append((pid, d.get("path") or "", d.get("cmd") or ""))
    return procs


# Backwards-compat alias (older callers / tooling may import this name).
_all_mc2_procs = _enum_mc2_processes


def _same_path_mc2(procs: list[tuple[int, str, str]],
                   target_exe) -> list[tuple[int, str]]:
    """PURE filter: given (pid, exe_path, cmd) tuples, return only those PIDs
    whose exe was launched from OUR target_exe path.

    Normalizes both sides (resolve -> forward-slash -> casefold). For PIDs with
    an empty ExecutablePath (access-denied), falls back to a CommandLine
    substring match on the resolved target exe path OR its deploy directory.
    If a null-path PID has no CommandLine hint either, it is treated as FOREIGN
    (NOT blocked) -- safer for overlap: we never block on an unknown process.
    Foreign mc2.exe (a different deploy path) are never returned.

    Takes the process list as an argument so it can be unit-tested with
    synthetic inputs without launching anything."""
    tgt = _norm_path(str(target_exe))
    try:
        tgt_dir = _norm_path(str(Path(target_exe).resolve().parent))
    except Exception:
        tgt_dir = ""
    matched = []
    for pid, path, cmd in procs:
        npath = _norm_path(path)
        if npath:
            if npath == tgt:
                matched.append((pid, path))
            continue
        # ExecutablePath empty -> fall back to CommandLine substring match.
        ncmd = (cmd or "").replace("\\", "/").casefold()
        if tgt and tgt in ncmd:
            matched.append((pid, cmd))
        elif tgt_dir and tgt_dir in ncmd:
            matched.append((pid, cmd))
        # else: null path + no cmdline hint -> FOREIGN (not blocked).
    return matched


def _running_mc2(target_exe: Path) -> list[tuple[int, str]]:
    """PIDs of mc2.exe instances launched from OUR target exe path.
    Thin wrapper = filter(enum()). Path-discrimination lives in _same_path_mc2."""
    return _same_path_mc2(_enum_mc2_processes(), target_exe)


def _taskkill_mc2(pids: list[int]):
    """Kill ONLY the given PIDs (never /IM image-name kill, which would also
    nuke foreign mc2.exe from a different deploy path)."""
    for pid in pids:
        subprocess.run(["taskkill", "/F", "/PID", str(pid)],
                       stderr=subprocess.DEVNULL, stdout=subprocess.DEVNULL)


def _run_menu_canary(exe: Path, script_path: Path, artifact_dir: Path,
                     keep_logs: bool, settle_s: int) -> int:
    env = os.environ.copy()
    for var in ["MC2_SMOKE_MODE", "MC2_HEARTBEAT", "MC2_SMOKE_SEED"]:
        env.pop(var, None)
    env["MC2_MENU_CANARY_SKIP_INTRO"] = "1"
    exe_dir = str(exe.resolve().parent)
    proc = subprocess.Popen(
        [str(exe)],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
        cwd=exe_dir,
        env=env,
    )

    start_wall = time.monotonic()
    time.sleep(1.0)
    exited_before_replay = proc.poll() is not None
    auto = subprocess.run(
        [sys.executable, str(GAME_AUTO), "script", str(script_path)],
        text=True,
        capture_output=True,
        cwd=str(ROOT),
    )
    replay_elapsed_s = time.monotonic() - start_wall
    time.sleep(max(0, settle_s))

    game_alive = proc.poll() is None
    if game_alive:
        try:
            proc.wait(timeout=max(1, settle_s))
        except subprocess.TimeoutExpired:
            proc.kill()
    try:
        stdout, _ = proc.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        stdout, _ = proc.communicate(timeout=2)
    exit_code = proc.returncode
    lowered = (stdout or "").lower()
    clean_exit = "[exit] gos_terminateapplication called" in lowered
    crashy = any(token in lowered for token in [
        "unhandled exception",
        "fatal error",
        "access violation",
        "stack overflow",
        "abort",
        "assert",
    ])
    early_exit = exit_code == 0 and replay_elapsed_s > 0 and not game_alive and exited_before_replay
    passed = (
        auto.returncode == 0
        and clean_exit
        and not crashy
        and exit_code == 0
        and not early_exit
    )

    md = [
        "# Menu Canary",
        "",
        f"- script: `{script_path.name}`",
        f"- replay_exit: `{auto.returncode}`",
        f"- replay_elapsed_s: `{replay_elapsed_s:.2f}`",
        f"- exited_before_replay: `{str(exited_before_replay).lower()}`",
        f"- game_alive_after_replay: `{str(game_alive).lower()}`",
        f"- game_exit_code: `{exit_code}`",
        f"- clean_exit_marker: `{str(clean_exit).lower()}`",
        f"- crash_signature: `{str(crashy).lower()}`",
        f"- early_exit: `{str(early_exit).lower()}`",
        f"- result: `{'PASS' if passed else 'FAIL'}`",
    ]
    report_text = "\n".join(md) + "\n"
    (artifact_dir / "menu_canary_report.md").write_text(report_text, encoding="utf-8")
    if keep_logs or not passed:
        (artifact_dir / "menu_canary_game.log").write_text(stdout or "", encoding="utf-8", errors="replace")
        (artifact_dir / "menu_canary_replay.log").write_text(
            (auto.stdout or "") + ("\n" if auto.stdout and auto.stderr else "") + (auto.stderr or ""),
            encoding="utf-8",
            errors="replace",
        )
    sys.stdout.buffer.write(report_text.encode("utf-8"))
    sys.stdout.buffer.flush()
    return 0 if passed else 1


def select_missions(entries, missions):
    """Resolve --mission names against manifest entries.

    Returns (selected, unknown): selected preserves manifest order and
    drops skip-tier entries and duplicates; unknown is the sorted list of
    requested names that matched no non-skip manifest entry.
    """
    wanted = set(missions)
    selected = []
    seen = set()
    for e in entries:
        if e.tier == "skip" or e.stem not in wanted or e.stem in seen:
            continue
        selected.append(e)
        seen.add(e.stem)
    unknown = sorted(wanted - seen)
    return selected, unknown


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tier", choices=["tier1", "tier2", "tier3"])
    ap.add_argument("--mission", action="append", default=[])
    ap.add_argument("--menu-canary", action="store_true")
    ap.add_argument("--with-menu-canary", action="store_true")
    ap.add_argument("--menu-script", default=str(DEFAULT_MENU_SCRIPT))
    ap.add_argument("--menu-settle", type=int, default=5)
    ap.add_argument("--fail-fast", action="store_true")
    ap.add_argument("--continue", dest="cont", action="store_true", default=True)
    ap.add_argument("--keep-logs", action="store_true")
    ap.add_argument("--baseline-update", action="store_true")
    ap.add_argument("--kill-existing", action="store_true")
    # Safety hatch: restore old global-block behavior (ANY mc2.exe, regardless
    # of deploy path, blocks the run). Default is path-aware (foreign exes are
    # ignored with an advisory).
    ap.add_argument("--block-any-mc2", action="store_true",
                    help="block on any running mc2.exe regardless of its deploy "
                         "path (legacy global-block behavior; default is "
                         "path-aware)")
    ap.add_argument("--duration", type=int)
    ap.add_argument("--profile", default="stock")
    ap.add_argument("--exe", default=str(DEFAULT_EXE))
    # Tier 1.2 (docs/testing-strategy.md): opt-in safety net that promotes
    # GL_DEBUG_SEVERITY_HIGH from a silent log to abort(). Off by default.
    ap.add_argument("--gl-debug-fatal", action="store_true",
                    help="set MC2_GL_DEBUG_FATAL=1 (abort on GL_DEBUG_SEVERITY_HIGH)")
    # Deploy-coherence preflight (reuses deploy_payload.py --verify-only; does
    # NOT reimplement hashing). --verify-only = pure preflight (no mission, no
    # lock, no artifacts), advisory unless --strict. --verify-preflight = run
    # the verify THEN proceed to the normal smoke only if it passes.
    ap.add_argument("--verify-only", action="store_true",
                    help="reconcile the deploy target against its manifest and "
                         "exit; no mission launch (advisory unless --strict)")
    ap.add_argument("--verify-preflight", action="store_true",
                    help="run --verify-only as a gate, then proceed to the "
                         "normal smoke only if verify passes (implies --strict)")
    ap.add_argument("--strict", action="store_true",
                    help="with --verify-only/--verify-preflight: a hard "
                         "mismatch (stale/missing) exits 1 instead of advisory")
    args = ap.parse_args()
    if args.gl_debug_fatal:
        os.environ["MC2_GL_DEBUG_FATAL"] = "1"

    # ----- Deploy-coherence verify (preflight) ---------------------------
    # The target dir run_smoke would launch from = parent of --exe. We call
    # deploy_payload.py --verify-only against THAT dir, reusing its sha256
    # reconciliation (no hashing reimplemented here). This block runs BEFORE
    # the concurrency lock / mission setup, so --verify-only takes no lock,
    # launches nothing, and writes nothing into the smoke artifact tree.
    def _run_verify(strict: bool) -> int:
        verify_target = str(Path(args.exe).resolve().parent)
        cmd = [sys.executable, str(ROOT / "scripts" / "deploy_payload.py"),
               verify_target, "--verify-only"]
        if strict:
            cmd.append("--strict")
        print(f"[runner] [VERIFY] reconciling deploy target {verify_target} "
              f"(strict={str(strict).lower()})", file=sys.stderr)
        try:
            _v = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
        except Exception as _ve:
            print(f"[runner] [VERIFY] check error: {_ve}", file=sys.stderr)
            return 2
        for _line in ((_v.stdout or "") + (_v.stderr or "")).splitlines():
            if _line.strip():
                print(f"[runner] [VERIFY] {_line}", file=sys.stderr)
        # deploy_payload.py exit codes: 0 = clean OR advisory-clean (no
        # manifest / drift-without-strict); nonzero = hard mismatch under
        # --strict or bad config (we surface as 1).
        return 0 if _v.returncode == 0 else 1

    if args.verify_only:
        if args.verify_preflight:
            ap.error("--verify-only and --verify-preflight are mutually exclusive")
        rc = _run_verify(args.strict)
        if rc == 0:
            print("[runner] [VERIFY] preflight OK (advisory clean or match)"
                  if not args.strict else "[runner] [VERIFY] preflight OK (strict)",
                  file=sys.stderr)
        else:
            print("[runner] [VERIFY] preflight reported a hard mismatch "
                  "(--strict)", file=sys.stderr)
        sys.exit(rc)

    if args.verify_preflight:
        # Strict gate: verify must pass, else abort BEFORE any lock/mission.
        # On pass, fall through to the byte-identical normal smoke path.
        rc = _run_verify(strict=True)
        if rc != 0:
            print("[runner] [VERIFY] --verify-preflight gate FAILED; not "
                  "launching smoke.", file=sys.stderr)
            sys.exit(1)
        print("[runner] [VERIFY] --verify-preflight gate PASSED; proceeding "
              "to normal smoke.", file=sys.stderr)

    # Deploy-coherence advisory (scripts/check-deploy-coherence.py): detects
    # a stale deployed exe (fix built but never copied to the run dir).
    # STRICTLY advisory: never changes verdict, exit code, or gate behavior.
    try:
        _coh = subprocess.run(
            [sys.executable, str(ROOT / "scripts" / "check-deploy-coherence.py"),
             str(Path(args.exe).resolve().parent), "--worktree", str(ROOT)],
            capture_output=True, text=True, timeout=30)
        for _line in ((_coh.stdout or "") + (_coh.stderr or "")).splitlines():
            if _line.strip():
                print(f"[runner] {_line}", file=sys.stderr)
    except Exception as _e:
        print(f"[runner] [DEPLOY_COHERENCE] check skipped: {_e}", file=sys.stderr)

    # Shader BOM preflight: UTF-8 BOM in any shader = silent compile death
    # (driver reports "unexpected token '€'" with no file name). Hard-fail
    # so a BOM-infected deploy never silently passes the smoke gate.
    try:
        _bom = subprocess.run(
            [sys.executable, str(ROOT / "scripts" / "check_shader_bom.py")],
            capture_output=True, text=True, timeout=15)
        for _line in ((_bom.stdout or "") + (_bom.stderr or "")).splitlines():
            if _line.strip():
                print(f"[runner] [BOM-CHECK] {_line}", file=sys.stderr)
        if _bom.returncode != 0:
            print("[runner] [BOM-CHECK] FATAL: BOM detected in shader source(s). "
                  "Run `py -3 scripts/check_shader_bom.py --fix` to strip.",
                  file=sys.stderr)
            sys.exit(2)
    except Exception as _e:
        print(f"[runner] [BOM-CHECK] check skipped: {_e}", file=sys.stderr)

    # F1 unified-projection: forbid MC2_DISABLE_GOSFX=0 in smoke runs.
    # Visual regression accepted only in dev-override sessions; smoke must
    # represent shipped default state.
    if os.environ.get("MC2_DISABLE_GOSFX") == "0":
        print("[run_smoke] FATAL: MC2_DISABLE_GOSFX=0 conflicts with unified "
              "projection; smoke would record regressed visuals. Unset or set =1.",
              file=sys.stderr)
        sys.exit(2)

    # Concurrency lock: prevent two smoke runners from stepping on each other.
    # A second invocation with --kill-existing would taskkill the first run's
    # owned mc2.exe (image-name kill, not PID-specific), producing crash_silent
    # for whichever mission was in flight.  The lock is a flat file next to the
    # artifact root; open(..., 'x') is atomic on Windows (CreateFile CREATE_NEW).
    _LOCK_PATH = ARTIFACT_ROOT / "smoke.lock"
    _LOCK_PATH.parent.mkdir(parents=True, exist_ok=True)
    try:
        _lf = open(_LOCK_PATH, 'x')
        _lf.write(str(os.getpid()))
        _lf.flush()
        atexit.register(lambda: _LOCK_PATH.unlink(missing_ok=True))
    except FileExistsError:
        # May be stale (previous run crashed without cleanup). Check the PID.
        try:
            _stale_pid = int(_LOCK_PATH.read_text().strip())
            _still_alive = subprocess.run(
                ["tasklist", "/FI", f"PID eq {_stale_pid}", "/NH", "/FO", "CSV"],
                capture_output=True, text=True).stdout.count(str(_stale_pid)) > 0
        except Exception:
            _still_alive = False
        if _still_alive:
            print(f"[runner] ERROR: another smoke run is already in progress "
                  f"(PID {_stale_pid}); wait for it to finish or remove "
                  f"{_LOCK_PATH} if it is stale.", file=sys.stderr)
            sys.exit(5)
        # Stale lock — re-acquire.
        _LOCK_PATH.unlink(missing_ok=True)
        _lf = open(_LOCK_PATH, 'x')
        _lf.write(str(os.getpid()))
        _lf.flush()
        atexit.register(lambda: _LOCK_PATH.unlink(missing_ok=True))

    # Existing-process safety (path-aware). Only mc2.exe launched from OUR
    # target exe path blocks the run; foreign mc2.exe from a different deploy
    # path are advised and ignored. --block-any-mc2 restores global blocking.
    _target_exe = Path(args.exe)
    _all_procs = _enum_mc2_processes()
    same_path = _same_path_mc2(_all_procs, _target_exe)
    foreign = []
    _same_pids = {p for p, _ in same_path}
    for pid, path, cmd in _all_procs:
        if pid in _same_pids:
            continue
        foreign.append((pid, path or cmd))
    for pid, path in foreign:
        print(f"[runner] ignoring foreign mc2.exe PID {pid} at {path}",
              file=sys.stderr)
    if args.block_any_mc2:
        blockers = same_path + foreign
    else:
        blockers = same_path
    if blockers:
        block_pids = [p for p, _ in blockers]
        if args.kill_existing:
            # Kill ONLY same-path PIDs even under --block-any-mc2 we still only
            # taskkill same-path here unless legacy block selected the foreign.
            kill_pids = block_pids if args.block_any_mc2 else [p for p, _ in same_path]
            print(f"[runner] killing existing mc2.exe PIDs {kill_pids}",
                  file=sys.stderr)
            _taskkill_mc2(kill_pids)
        else:
            print(f"[runner] ERROR: mc2.exe already running (PIDs {block_pids}); "
                  f"pass --kill-existing to override.", file=sys.stderr)
            sys.exit(4)

    if args.menu_canary:
        if args.tier or args.mission:
            ap.error("--menu-canary cannot be combined with --tier/--mission")
        timestamp = dt.datetime.now().strftime("%Y-%m-%dT%H-%M-%S")
        artifact_dir = ARTIFACT_ROOT / timestamp
        artifact_dir.mkdir(parents=True, exist_ok=True)
        sys.exit(_run_menu_canary(Path(args.exe), Path(args.menu_script),
                                  artifact_dir, args.keep_logs, args.menu_settle))

    entries = manifest.parse_manifest(MANIFEST_PATH)
    if args.mission:
        selected, unknown = select_missions(entries, args.mission)
        if unknown:
            # Config-error class (exit 2, same as argparse / GOSFX fatal):
            # a typo'd mission list must never read as PASS to a CI gate.
            print(f"[runner] ERROR: unknown mission name(s) "
                  f"{', '.join(unknown)} not found in {MANIFEST_PATH} "
                  f"(or skip-tier).", file=sys.stderr)
            sys.exit(2)
    elif args.tier:
        selected = [e for e in entries if e.tier == args.tier]
    else:
        ap.error("--tier or --mission required")

    if not selected:
        print("[runner] ERROR: no missions selected; empty selection is a "
              "config error, not a pass.", file=sys.stderr)
        sys.exit(2)

    baseline_data = baselines.load(BASELINE_PATH)
    timestamp = dt.datetime.now().strftime("%Y-%m-%dT%H-%M-%S")
    artifact_dir = ARTIFACT_ROOT / timestamp
    artifact_dir.mkdir(parents=True, exist_ok=True)

    menu_canary_rc = None
    if args.with_menu_canary:
        print("[runner] running menu canary", file=sys.stderr)
        menu_canary_rc = _run_menu_canary(Path(args.exe), Path(args.menu_script),
                                          artifact_dir, args.keep_logs, args.menu_settle)
        if menu_canary_rc != 0 and args.fail_fast:
            print("[runner] --fail-fast: stopping after menu canary failure", file=sys.stderr)
            sys.exit(menu_canary_rc)

    # Deploy-fingerprint: expected sha = HEAD of the worktree this runner
    # lives in. Computed once; compared against the exe's startup banner.
    _fp_expected_sha = None
    _fp_hard_fail = False
    _fp_require = os.environ.get("MC2_SMOKE_REQUIRE_FINGERPRINT") == "1"
    if _fingerprint is not None:
        try:
            _fp_expected_sha = subprocess.check_output(
                ["git", "-C", str(ROOT), "rev-parse", "HEAD"],
                text=True, stderr=subprocess.DEVNULL, timeout=15).strip()
        except Exception:
            _fp_expected_sha = None

    # VEG-SMOKE-FLOOR-1: only active when MC2_VEGETATION_CARDS=1.
    _veg_floor_active = os.environ.get("MC2_VEGETATION_CARDS") == "1"
    _veg_floor_failures: list[str] = []  # accumulated across all missions

    rows: list[report.Row] = []
    # MC2_DIAGNOSTIC_TRACE_FILE: normalise to absolute once, outside the
    # per-mission loop.  The game launches with cwd=exe_dir (not the runner
    # cwd), so a relative path would land in the deploy dir rather than the
    # runner's working directory — the MCP tool (mc2_render_state_server.py)
    # would then be unable to find it.  Absolute paths pass through unchanged.
    _exe_dir = Path(args.exe).resolve().parent
    _trace_file_raw = os.environ.get("MC2_DIAGNOSTIC_TRACE_FILE", "")
    _trace_file_abs = (
        str(_exe_dir / _trace_file_raw)
        if _trace_file_raw and not Path(_trace_file_raw).is_absolute()
        else _trace_file_raw
    )

    for e in selected:
        duration = args.duration or e.duration or 120
        tier = args.tier or "adhoc"
        cfg = RunConfig(
            exe=[args.exe],
            profile=e.profile or args.profile,
            stem=e.stem,
            duration=duration,
            heartbeat_timeout_load_s=e.heartbeat_timeout_load or 60,
            heartbeat_timeout_play_s=e.heartbeat_timeout_play or 3,
            grace_s=60,
            allow_asset_oob=e.allow_asset_oob,
            env_extra={
                "MC2_SMOKE_SEED": "0xC0FFEE",
                # MC2_DIAGNOSTIC_TRACE_FILE is always absolute (computed above)
                # so the engine writes to _exe_dir/debug_state/... regardless
                # of where the smoke runner is invoked from.
                **({"MC2_DIAGNOSTIC_TRACE_FILE": _trace_file_abs} if _trace_file_abs else {}),
                # Propagate PatchStream env vars from parent if set —
                # subprocess.Popen's env arg replaces the inherited env
                # entirely, so vars not explicitly listed get dropped.
                **{k: v for k, v in os.environ.items()
                   if k in ("MC2_FX_COUNT_LOG",
                            "MC2_RES_DIAG",  # [RES_DIAG v1] HUD/scene resolution-split one-shot dump
                            "MC2_TEXMGR_KTX_PRIMARY",  # route-2 CPU BC7 decode test gate
                            "MC2_TEXMGR_LOAD_TRACE",  # residency ground-truth load trace
                            # S9D deterministic fixed-timestep smoke clock
                            # (opt-in; Popen replaces env so it must be listed).
                            "MC2_SMOKE_FIXED_TIMESTEP",
                            "MC2_SCREENSHOT_AT_FRAME",
                            "MC2_SCREENSHOT_PATH",
                            "MC2_OBJECT_RECON_TRACY",
                            "MC2_HITCH_TRACE",
                            "MC2_HITCH_MS",
                            # [RENDER_PASS_TIME v1] coarse per-pass GPU timers
                            "MC2_RENDER_PASS_TIME",
                            "MC2_RENDER_PASS_TIME_EVERY",
                            # [FRAME_PASS_STATS v1] per-pass advisory stats
                            "MC2_FRAME_PASS_STATS",
                            "MC2_FRAME_PASS_STATS_EVERY",
                            "MC2_DEBUG_STATE_DUMP",
                            "MC2_PRESWAP_FINISH",
                            "MC2_MODERN_TERRAIN_SURFACE",
                            "MC2_MODERN_TERRAIN_PATCHES",
                            "MC2_SHAPE_C_PARITY_CHECK",
                            "MC2_PATCH_STREAM_TRACE",
                            "MC2_PATCH_STREAM_FORCE_INIT_FAIL",
                            "MC2_PATCHSTREAM_QUAD_RECORDS",
                            "MC2_PATCHSTREAM_QUAD_RECORDS_DRAW",
                            "MC2_PATCHSTREAM_THIN_RECORDS",
                            "MC2_PATCHSTREAM_THIN_RECORDS_DRAW",
                            "MC2_PATCHSTREAM_THIN_RECORD_FASTPATH",
                            "MC2_THIN_DEBUG",
                            "MC2_WATER_DEBUG",
                            "MC2_WATER_STREAM_DEBUG",
                            "MC2_RENDER_WATER_FASTPATH",
                            "MC2_RENDER_WATER_PARITY_CHECK",
                            "MC2_VERTEX_PROJECT_FAST",
                            "MC2_VERTEX_PROJECT_PARITY",
                            "MC2_TERRAIN_INDIRECT",
                            "MC2_TERRAIN_INDIRECT_PARITY_CHECK",
                            "MC2_TERRAIN_INDIRECT_TRACE",
                            # Decal static-bake (drawPass-retirement Slice A) kill-switch
                            "MC2_TERRAIN_INDIRECT_OVERLAY",
                            # Ring-buffer hazard probe (raster-triangle bug)
                            "MC2_RING_TRACE",
                            # Probe 7: force glFinish() between compute and draw
                            "MC2_RING_FORCE_FINISH",
                            # WATER-THINRING-FENCE-1: per-slot GLsync fence trace
                            # on the water thin-record ring (diagnostics-only).
                            "MC2_WATER_THINRING_TRACE",
                            # R2B-STATIC-NATURAL-TOUCH-PRESERVE-1: default-ON
                            # killswitch + diagnostics for the static-natural
                            # (tree/pine-building) registry stale-drop fix. Without
                            # these allowlisted, Popen drops them and the gate-OFF
                            # A/B (preserve=0) plus both trace streams are inert.
                            "MC2_R2B_TOUCH_PRESERVE",
                            "MC2_R2B_STATIC_NATURAL_TRACE",
                            "MC2_REGFLUSH_DIAG_TRACE",
                            "MC2_STATIC_STALE_DROP_FATAL",
                            "MC2_REGFLUSH_GUARD2",
                            "MC2_TARGETING_GUARD",
                            "MC2_ANIM_CADENCE_GUARD",
                            "MC2_ANIM_CADENCE_FIX",
                            # GPU object batcher gate (bisect partner for terrain bug)
                            "MC2_GPU_OBJECTS",
                            # Shadow-lane gated features (smoke coverage for the
                            # dynamic sun-shadow caster path + static building map).
                            "MC2_SHADOW_ENABLE",
                            # SHADOW-STABILITY-1: per-pass GL-state trace gate.
                            "MC2_SHADOW_STATE_TRACE",
                            # GLSTATE-SHADOW-CLIP-RESTORE-1: post-shadow GL state restore trace.
                            "MC2_GLSTATE_TRACE",
                            # GLSTATE slice 1: SSBO slot save/restore trace.
                            "MC2_GLSTATEGUARD_LOG",
                            "MC2_STATIC_PROP_BUILDING_SHADOW",
                            "MC2_SHADOW_DYNAMIC_PROP_CASTERS",
                            "MC2_SHADOW_BOUNDED_NEAR_FIT",
                            "MC2_SHADOW_BOUNDED_NEAR_RADIUS",
                            "MC2_SHADOW_FRUSTUM_DIAG",
                            # SHADOW-CASTER-LIGHTBOX-CULL-1 (2026-06-04): per-frame
                            # cull of dynamic prop shadow casters to the shadow
                            # frustum (default OFF). Without these in the allowlist
                            # subprocess.Popen drops them and the gate-ON smoke
                            # state is meaningless (cull never engages).
                            "MC2_SHADOW_CASTER_LIGHTBOX_CULL",
                            "MC2_SHADOW_CASTER_CULL_MARGIN",
                            "MC2_SHADOW_CULL_DEBUG",
                            # SHADOW-FOCUS-CENTER-1 (2026-06-04): center the
                            # dynamic shadow box on the camera near-ground focus
                            # point instead of the frustum-corner AABB centroid
                            # (default OFF). Popen replaces env -- without these
                            # in the allowlist the gate-ON smoke does nothing.
                            "MC2_SHADOW_FOCUS_CENTER",
                            "MC2_SHADOW_FOCUS_DIST",
                            # Mask-dispatch (pre-bake-terrain merge)
                            "MC2_TERRAIN_MASK_DISPATCH",
                            "MC2_TERRAIN_MASK_DISPATCH_PARITY",
                            "MC2_TERRAIN_INDIRECT_MINE",
                            "MC2_TERRAIN_INDIRECT_OVERLAY",
                            "MC2_TERRAIN_INDIRECT_OVERLAY_PARITY_CHECK",
                            "MC2_TERRAIN_COST_SPLIT",
                            "MC2_FX_COST_SPLIT",
                            # FX-FORCE-SPAWN fixture (code/warrior.cpp): forces up
                            # to 8 mechs to fire all weapons (PPC -> tube ribbons)
                            # so the otherwise-idle tier1 fly-throughs exercise the
                            # tube/particle FX path. MC2_LOG=1 surfaces its stdout
                            # confirmation. Both must be allowlisted or Popen drops
                            # them and the fixture never fires.
                            "MC2_FX_FORCE_SPAWN",
                            "MC2_LOG",
                            # MCO magic* AI behavior trace (code/ablmc2.cpp magicAiLog):
                            # logs whether ClanEagle brains call magicPatrol/Guard/Escort
                            # and what target the engine acquired. Allowlist or Popen drops it.
                            "MC2_MAGIC_AI_LOG",
                            # Logistics/purchase-screen load trace (code/logisticsdata.cpp
                            # updateAvailability): logs resolved purchase file path, open
                            # OK/FAIL, each Mech read, available counts. For POAR bay debug.
                            "MC2_LOG_LOGISTICS",
                            # Direct-fire projectile speed multiplier (PPC/AC/gauss);
                            # default 1.0 (stock). Allowlisted so the fixture can A/B it.
                            "MC2_PROJECTILE_SPEED_MULT",
                            # Direct-fire de-curve (straight flight + spawn lead),
                            # default-off. Allowlisted for the fixture A/B.
                            "MC2_DIRECT_FIRE_STRAIGHT",
                            # Active mod id — drives model_override + anim_override
                            # registry mod-merge (mods/<id>/...). Allowlisted so mod
                            # smokes actually load the mod; without it Popen drops it.
                            "MC2_ACTIVE_MOD",
                            # Launcher-supplied compatibility layer(s) (file.cpp MC2_MOD_DEPS):
                            # e.g. mco-compat / mc2x-compat. Required to replicate a launcher
                            # campaign launch in smoke (loads corebrain.abx + object2.pak).
                            "MC2_MOD_DEPS",
                            "MC2_LIGHT_COST_SPLIT",
                            "MC2_SLIM_COST_SPLIT",
                            "MC2_TOBJ_COST_SPLIT",
                            "MC2_STATIC_PROP_FLUSH_COST_SPLIT",
                            "MC2_STATIC_PROP_FLUSH_CACHED_BLOB",
                            "MC2_STATIC_PROP_FLUSH_CACHED_BLOB_COMPARE",
                            "MC2_STATIC_PROP_COLORS_FILL",
                            "MC2_STATIC_PROP_PERSISTENT_BUCKETS",
                            "MC2_STATIC_PROP_PERSISTENT_BUCKETS_COMPARE",
                            "MC2_BUCKET_ORDER_TRACE",
                            # GPU-cull ownership-port Slice A: cut-off upper-bound oracle
                            "MC2_GPU_CULL_OWNERSHIP_PARITY",
                            # Task 7 — superset-parity counter probe (proof-gate #2)
                            "MC2_TOBJ_PARITY",
                            # alpha-Stage 1 §5 Stage 0 — candidate-predicate
                            # disagreement probe for inView unconflation.
                            "MC2_INVIEW_CONFLATION_TRACE",
                            "MC2_GPUPROPS_TRACE",
                            "MC2_MECH_RESTORE_TRACE",
                            # Phase 1 — terrain lighting GPU compute
                            "MC2_TERRAIN_LIGHTING_GPU",
                            "MC2_TERRAIN_LIGHTING_PARITY",
                            "MC2_TERRAIN_LIGHTING_GPU_TRACE",
                            # Track A1 — object admission predicate + trace
                            "MC2_OBJECT_ADMISSION_PREDICATE",
                            "MC2_PROJECTZ_TRACE",
                            "MC2_PROJECTZ_HEATMAP",
                            "MC2_PROJECTZ_SUMMARY",
                            "MC2_PROJECTZ_GUARD_PX",
                            # Task 7 — cascade-safety DESTROY trace
                            "MC2_DESTROY_TRACE",
                            # Track C0/C1a — GPU cull substrate + AABB parity + compute dispatch
                            "MC2_GPU_CULL_SUBSTRATE",
                            "MC2_GPU_CULL_AABB_PARITY",
                            "MC2_GPU_CULL_SUBSTRATE_TRACE",
                            "MC2_GPU_CULL",
                            "MC2_GPU_CULL_COMPUTE_TRACE",
                            "MC2_CAMERA_MOVE_DIAG",
                            "MC2_STATIC_FORCE_ADMIT",
                            "MC2_WATER_GATE_DIAG",
                            "MC2_MISSION_INTRO_LEGACY_RENDER",
                            # Track C2 — async readback ring buffer
                            "MC2_GPU_CULL_READBACK",
                            "MC2_GPU_CULL_FORCE_FENCE_NOT_READY",
                            "MC2_GPU_CULL_READBACK_TRACE",
                            # Track C3 — lifecycle gates (mech3d/gvactor/objmgr routing)
                            # m-4: missing permutation: LIFECYCLE=1+READBACK=1 and
                            # LIFECYCLE=1+READBACK=0 (fail-open regression) smoke runs.
                            # Both require --with-env overrides; not automated yet.
                            "MC2_GPU_CULL_LIFECYCLE",
                            "MC2_GPU_CULL_LIFECYCLE_TRACE",
                            # Phase C — GPU-driven unified path
                            "MC2_GPU_DRIVEN",
                            "MC2_GPU_DRIVEN_WATER",
                            "MC2_GPU_DRIVEN_TERRAIN_SOLID",
                            "MC2_GPU_DRIVEN_OVERLAY",
                            "MC2_GPU_DRIVEN_PARITY",
                            "MC2_GPU_DRIVEN_TRACE",
                            # Reverse-Z depth verification probes (2026-05-18):
                            # depth-transition zoom-pop, water render/depth
                            # parity, reverse-Z proj-matrix lifecycle trace.
                            "MC2_DEPTH_TRANSITION_PROBE",
                            "MC2_WATER_RENDERPROBE",
                            "MC2_WATER_DEPTHPROBE",
                            "MC2_REVERSE_Z_TRACE",
                            # Terrain continuous-surface producer (2026-05-18):
                            # MC2_TERRAIN_SURFACE = path-select kill-switch
                            # (PR-1+, default-OFF); MC2_TERRAIN_SURFACE_TRACE =
                            # the PR-0 [TERRAIN_SURFACE v1] lifecycle trace gate
                            # (trace-only, default-OFF). Without these in the
                            # allowlist subprocess.Popen drops them and the
                            # forced-ON / trace smoke states are meaningless.
                            "MC2_TERRAIN_SURFACE",
                            "MC2_TERRAIN_SURFACE_TRACE",
                            # quadSetupTextures-retirement recon (2026-05-19):
                            # MC2_WATER_INVPROJ_PARITY = the [WATER_INVPROJ v1]
                            # snapshot-A-vs-B 6-tuple parity probe (terrain.cpp;
                            # trace-only, default-OFF). Decides whether the
                            # setupTextures water block contributes unique
                            # extrema beyond slimReduce. Without this in the
                            # allowlist subprocess.Popen drops it and the probe
                            # never fires.
                            "MC2_WATER_INVPROJ_PARITY",
                            # F3 CPU-projection cost-split (2026-05-20):
                            # MC2_CPU_PROJ_COST_SPLIT = master env gate for the
                            # measurement-only bucket/sidecar instrumentation
                            # (mclib/cpu_proj_cost_split.{h,cpp}). Without this
                            # in the allowlist subprocess.Popen drops it and
                            # tier1 captures emit "[CPU_PROJ v1 disabled]".
                            "MC2_CPU_PROJ_COST_SPLIT",
                            # Tier 1.2 — KHR_debug fatal-on-HIGH opt-in
                            # (docs/testing-strategy.md). Forwarded so
                            # --gl-debug-fatal reaches the engine subprocess.
                            "MC2_GL_DEBUG_FATAL",
                            # Integrated gosFX-retirement / GPU-particles plan
                            # (2026-05-20-integrated-gosfx-retirement-*):
                            # MC2_FX_TRACE = neutral fx invocation counter
                            # (mclib/fx_trace; default-OFF). Without this in
                            # the allowlist subprocess.Popen drops it and the
                            # Stage 0' content-recon trace never fires.
                            # MC2_DISABLE_GOSFX = A1 MLR work-leaf gate
                            # (default-ON since A2). Forwarded so Stage 0'
                            # captures can opt back to legacy-gosFX-ON for
                            # the histogram-baseline run only.
                            "MC2_FX_TRACE",
                            "MC2_DISABLE_GOSFX",
                            # B1 Stage 1'+: GPU particle batcher opt-in gate.
                            # (default-OFF; flipped ON for Stage 1' canaries.)
                            "MC2_GPU_PARTICLES",
                            # VFX oracle render gate (CPU-side harvest path).
                            # MC2_VFX_ORACLE_RENDER=1 enables the oracle path in
                            # CardCloud/ShardCloud/Card Draw() (default-OFF).
                            # MC2_GPU_PARTICLES_LOG=1 enables [VFX_ORACLE v1] stderr.
                            "MC2_VFX_ORACLE_RENDER",
                            "MC2_GPU_PARTICLES_LOG",
                            # VFX-DEBUG-VIEWS-1: particle billboard debug mode.
                            # 0=Final (default/byte-identical), 1=Albedo, 2=Alpha,
                            # 3=ParticleKind, 4=Overdraw, 5=Age (VFX-SHADER-AGE-FADE-PARITY-1).
                            "MC2_VFX_DEBUG_MODE",
                            # VFX-TUNING-UI-1: look-only VFX intensity scales.
                            # All default 1.0 or 0.0 (byte-identical no-ops).
                            "MC2_TUNE_VFX_BRIGHTNESS",
                            "MC2_TUNE_VFX_ADDITIVE_BRIGHTNESS",
                            "MC2_TUNE_VFX_ALPHA_SCALE",
                            # VFX-SHADER-AGE-FADE-PARITY-1: age-driven soft death
                            # fade for oracle particles. Default 0.0 = byte-identical.
                            "MC2_TUNE_VFX_AGE_FADE",
                            # VFX-SOFT-PARTICLES-MVP-1 / VFX-LIT-PARTICLES-MVP-1
                            "MC2_VFX_SOFT_PARTICLES",
                            "MC2_VFX_LIT_PARTICLES",
                            "MC2_TUNE_VFX_LIT_STRENGTH",
                            # StaticPropTypeTable v0 Task 5: candidate draw log
                            # proof gate. MC2_TYPE_TABLE_CAND_LOG=1 enables the
                            # [DRAW_CAND v0] summary line every frame.
                            # MC2_TYPE_TABLE_CAND_VERBOSE=1 also enables per-packet
                            # detail lines every frame (not just first-frame).
                            "MC2_TYPE_TABLE_CAND_LOG",
                            "MC2_TYPE_TABLE_CAND_VERBOSE",
                            # DrawPacket v2 per-frame field compare gate.
                            # MC2_DRAW_PACKET_COMPARE=1 enables [DRAW_PACKET_COMPARE v1]
                            # summary lines. MC2_DRAW_PACKET_COMPARE_VERBOSE=1 adds per-packet detail.
                            "MC2_DRAW_PACKET_COMPARE",
                            "MC2_DRAW_PACKET_COMPARE_VERBOSE",
                            # DrawPacket v3 ABI-promotion probe gate.
                            # MC2_DRAW_PACKET_V3=1 enables [DRAW_PACKET v3] build-stats line per frame.
                            "MC2_DRAW_PACKET_V3",
                            # DrawPacket v4B count-level coalesce coverage compare.
                            "MC2_DRAW_PACKET_COALESCE_COMPARE",
                            "MC2_DRAW_PACKET_COALESCE_VERBOSE",
                            # DrawPacket v4C slot-level coalesce coverage soak.
                            "MC2_DRAW_PACKET_COALESCE_V4C",
                            # DrawPacket v5 substitutive per-draw-call dispatch.
                            "MC2_DRAW_PACKET_COALESCE_V5",
                            "MC2_DRAW_PACKET_COALESCE_V5_TRACE",
                            # DrawPacket v6 canonical packet+meta array dispatch.
                            "MC2_DRAW_PACKET_STATIC_PROP_V6",
                            "MC2_DRAW_PACKET_STATIC_PROP_V6_TRACE",
                            # DrawPacket v7 kill-switch (reverts to legacy multidraw).
                            "MC2_STATIC_PROP_LEGACY_DISPATCH",
                            # DrawPacket v8 kill-switch: =1 restores live build + snapshot compare.
                            "MC2_STATIC_PROP_LIVE_BUILDER",
                            # MaterialGpu v7 kill-switch (set =0 to disable shader sampling).
                            "MC2_MATERIAL_GPU_SAMPLE",
                            # MaterialGpu master kill-switch (set =0 to disable table upload).
                            "MC2_MATERIAL_GPU",
                            # Extraction v2.3: snapshot-assisted static-prop
                            # snap-cull gate. MC2_SNAP_CULL=1 enables the v6
                            # dispatch loop to skip draw slots whose instanceCount
                            # was zero in the previous frame's RenderSnapshot. Opt-in
                            # for testing; requires snap->ok==1. Forwarded in the
                            # allowlist so tier1 smoke runs can use this kill-switch.
                            "MC2_SNAP_CULL",
                            # Extraction v3 snapshot build gate + log gate.
                            # MC2_SNAPSHOT_STATIC_PROP_BUILD=1 enables the v3
                            # draw-packet builder from snapshot rows. Forwarded so
                            # tier1 forced-ON canaries work correctly.
                            # MC2_RENDER_SNAPSHOT_LOG=1 enables the per-frame
                            # [RENDER_SNAPSHOT v3] + [v3 build] log lines (used
                            # to verify spBuild* counters in smoke step 2).
                            "MC2_SNAPSHOT_STATIC_PROP_BUILD",
                            "MC2_RENDER_SNAPSHOT_LOG",
                            # MC2_SNAPSHOT_MECH_EXTRACT=1 enables MECH-EXTRACTION-0
                            # persist buffer + compare counters (gate OFF by default).
                            "MC2_SNAPSHOT_MECH_EXTRACT",
                            # F1-3A ViewUniforms UBO upload gate (default-OFF).
                            # MC2_VIEW_UNIFORMS=1 uploads per-frame view matrices
                            # to UBO at binding=3. No shader consumption yet (F1-3B).
                            "MC2_VIEW_UNIFORMS",
                            # StaticPropAmbientV1 hemisphere ambient term (default-OFF).
                            # MC2_STATIC_PROP_AMBIENT_V1=1 enables the ambient V1 path.
                            "MC2_STATIC_PROP_AMBIENT_V1",
                            # V-MATERIAL-DEBUG-1 gated debug material views
                            # (default-OFF). 1=albedo, 2=materialIdx, 3=normal,
                            # 4=texArrayLayer, 5=roughness, 6=metallic.
                            # Clamped 0..6 by CPU.
                            "MC2_STATIC_PROP_DEBUG_MATERIAL",
                            # V-IBL-STATIC-1 SH-L2 image-based ambient (default-ON).
                            # MC2_STATIC_PROP_IBL_SH=0 disables; ImGui slider
                            # tunes u_iblShStrength when the gate is on.
                            "MC2_STATIC_PROP_IBL_SH",
                            # V-IBL-STATIC-1-SOAK: optional default strength
                            # override (clamped 0..3). Only meaningful when
                            # MC2_STATIC_PROP_IBL_SH=1. ImGui slider may
                            # still override at runtime.
                            "MC2_STATIC_PROP_IBL_SH_STRENGTH",
                            # V-IBL-STATIC-2: optional SH-set override by name
                            # (e.g. "default"). Unset/empty/unknown falls back
                            # to mission registry or default set. Dev knob.
                            "MC2_STATIC_PROP_IBL_SH_SET",
                            # V-MATERIAL-PBR-3: per-fragment Schlick-Fresnel
                            # + power-lobe specular gate (default-OFF).
                            # MC2_STATIC_PROP_PBR_V1=1 enables; ImGui slider
                            # tunes u_pbrV1Strength when on.
                            "MC2_STATIC_PROP_PBR_V1",
                            # V-MATERIAL-PBR-3: optional default-strength
                            # override (clamped 0..3). Only meaningful when
                            # MC2_STATIC_PROP_PBR_V1=1.
                            "MC2_STATIC_PROP_PBR_V1_STRENGTH",
                            # V-MATERIAL-PBR-3-DIAG: diagnostic sunFound
                            # visualizer (cyan/magenta). Default-OFF; only
                            # meaningful with MC2_STATIC_PROP_PBR_V1=1.
                            "MC2_STATIC_PROP_PBR_V1_DIAG_SUNFOUND",
                            # STATICPROP-MATERIAL-ORM-1: per-bucket linear ORM
                            # (occlusion-roughness-metallic) texture slots +
                            # sidecar feed (default-OFF). =1 enables.
                            "MC2_STATICPROP_MATERIAL_PBR_SLOTS",
                            # STATICPROP-MATERIAL-ORM-1: ORM strength tuning
                            # (only meaningful with MC2_STATICPROP_MATERIAL_PBR_SLOTS=1).
                            "MC2_STATICPROP_ORM_STRENGTH",
                            # Render-contract Phase 2 assert mode (default-OFF).
                            # MC2_RENDER_CONTRACT_ASSERT=1 enables runtime GL state
                            # validation against declared render_contract expectations.
                            "MC2_RENDER_CONTRACT_ASSERT",
                            # [RENDER_PASS v1] advisory per-pass telemetry
                            # (slice D1). Default-OFF; =1 emits one
                            # [RENDER_PASS v1] line per pass every 300 frames.
                            # Log-only; never affects verdicts. (Allowlist
                            # entry is convention/documentation — runner.py
                            # inherits the parent env via os.environ.copy().)
                            "MC2_RENDER_PASS_TELEMETRY",
                            # DEBUG-STATE-DUMP-1: JSON render-state snapshots.
                            # Default-OFF; =1 writes debug_state/latest_render_state.json.
                            "MC2_DEBUG_STATE_DUMP",
                            # DEBUG-STATE-DUMP-1: override output directory.
                            "MC2_DEBUG_STATE_DUMP_DIR",
                            # DEBUG-STATE-DUMP-2: rolling 8-slot history ring.
                            "MC2_DEBUG_STATE_DUMP_HISTORY",
                            # STATICPROP-SNAPSHOT-BRIDGE-COMPARE-1: independent
                            # registry cache + field-by-field compare probe.
                            # Default-OFF; =1 emits [SNAPSHOT_BRIDGE_COMPARE v1]
                            # per-frame to stderr.
                            "MC2_STATIC_PROP_SNAPSHOT_BRIDGE_COMPARE",
                            # STATICPROP-SNAPSHOT-FILL-DIRTYONLY-1: clean-generation
                            # fast path that skips fillStaticPropSlots + WriteLoop
                            # and memcpy's cached rows into the snapshot arena.
                            # Default-OFF; =1 enables the dirty-only path.
                            "MC2_STATIC_PROP_SNAPSHOT_FILL_DIRTYONLY",
                            # Terrain fast-path drop transition log (default-OFF).
                            # MC2_FASTPATH_DROP_LOG=1 emits [FASTPATH_DROP] on
                            # armed<->fallback transitions in terrain.cpp geometry().
                            "MC2_FASTPATH_DROP_LOG",
                            # MC2_MOVE_RECON=1: per-frame + atexit pathfinding
                            # cost instrumentation (move_recon.h/cpp). Default-OFF.
                            # Popen replaces env — must be in allowlist.
                            "MC2_MOVE_RECON",
                            # MC2_MOVE_CHUNK_SHADOW=1: alt-enable for the local-A*
                            # chunk/rect-corridor shadow (move_recon.cpp). Default-OFF.
                            "MC2_MOVE_CHUNK_SHADOW",
                            # MC2_MOVE_PATH_CACHE_SHADOW=1: same-query path-cache
                            # would-hit / nodes-saved / mover-divergence shadow.
                            "MC2_MOVE_PATH_CACHE_SHADOW",
                            # MC2_MISSION_SPLIT=1: wall-ms split of Mission::update
                            # sub-calls (land_update/pathmgr/clearVerts/geometry/
                            # objmgr...) to locate the 1K-map logic hotspot.
                            "MC2_MISSION_SPLIT",
                            "MC2_WATER_S6_COST",
                            "MC2_QUADSETUP_ARMED_SKIP",
                            "MC2_GEOM_PHASE_SPLIT",
                            "MC2_MIF_SPLIT",
                            "MC2_PICK_RECON",
                            # Diagnostic JSONL trace (diagnostic_trace.cpp).
                            # MC2_DIAGNOSTIC_TRACE_FILE is handled above (absolute
                            # path override) so only the tag filter needs passthrough.
                            "MC2_DIAG_TAGS",
                            # VEG-SMOKE-FLOOR-1: vegetation card output contract gate.
                            # MC2_VEGETATION_CARDS=1 enables GPU instanced vegetation
                            # and triggers VEG-SMOKE-FLOOR-1 assertions in this runner.
                            # Without this entry Popen drops it and the floor check
                            # compares against an engine that never drew vegetation.
                            "MC2_VEGETATION_CARDS",
                            # VEG-FLUSH-REASON-1: atlas path override for vegetation tests.
                            "MC2_VEGETATION_ATLAS",
                            # FRAME-JOBS-2x: parallel touch/bounds pre-pass gates.
                            # Without these in the allowlist Popen drops them and
                            # the gates never reach the engine.
                            "MC2_FRAME_JOBS",
                            "MC2_FRAME_JOBS_WORKERS",
                            "MC2_FRAME_JOBS_BATCH",
                            "MC2_FRAME_JOBS_TRACE",
                            "MC2_FRAME_JOBS_TOUCH",
                            "MC2_FRAME_JOBS_TOUCH_DIAG",
                            # FRAME-JOBS-2F: touch-entry unification stamp diagnostics.
                            "MC2_FRAME_JOBS_TOUCH_DIAG",
                            # FRAME-JOBS-2G: Path B terrain-loop touch cost diag.
                            "MC2_FRAME_JOBS_PATHB_DIAG",
                            # FRAME-JOBS stable-light skip gate.
                            "MC2_LIGHTBRIDGE_STABLE_SKIP",
                            "MC2_LIGHTBRIDGE_COMMIT_TRACE",
                            # STATIC-SCENE-PROXY-RECON-1: proxy candidate classifier.
                            "MC2_STATIC_PROXY_RECON",
                            # STATIC-REGISTRY-COVERAGE-RECON-1: sub-classify rej_no_static_reg.
                            "MC2_STATIC_REG_COVERAGE",
                            # STATIC-REG-PREWARM-QUEUE-1: mission-load off-screen light bake.
                            # MC2_STATIC_REG_PREWARM=1 enables prewarmStaticPropLightBakes().
                            # MC2_STATIC_REG_PREWARM_TRACE=1 adds per-object diagnostic.
                            "MC2_STATIC_REG_PREWARM",
                            "MC2_STATIC_REG_PREWARM_TRACE")},
            },
        )
        # Clear the file-sink probe log next to mc2.exe before each mission
        # so each run captures its own probe events.  See gos_terrain_indirect
        # PROBE_LOG / RING_SINK in the engine: ring-buffer / cmd-patch /
        # overshoot / near-clip-w / recipe-spread tripwires.
        ring_sink_path = Path(cfg.exe[0]).resolve().parent / "ring_trace.log"
        try:
            ring_sink_path.unlink(missing_ok=True)
        except Exception:
            pass

        print(f"[runner] running {e.stem} (tier={tier} duration={duration})",
              file=sys.stderr)
        result = run_one(cfg)

        # Deploy-fingerprint scan of the captured log (advisory by default;
        # never touches the mission verdict — see _fp_require at the end).
        if _fingerprint is not None:
            try:
                _fp = _fingerprint.parse_fingerprint(result.stdout_text)
                _fp_lines, _fp_bad = _fingerprint.check_fingerprint(_fp, _fp_expected_sha)
                _fp_hard_fail = _fp_hard_fail or _fp_bad
                for _l in _fp_lines:
                    print(f"[runner] {_l}", file=sys.stderr)
            except Exception as _fpe:
                print(f"[runner] [DEPLOY_FINGERPRINT] check skipped: {_fpe}",
                      file=sys.stderr)

        key = baselines.key(cfg.profile, e.stem, tier, duration)
        delta = baselines.destroys_delta(baseline_data, key, result.summary.destroys)

        rows.append(report.Row(stem=e.stem, verdict=result.verdict,
                               summary=result.summary, destroys_delta=delta or 0))

        if not result.verdict.passed or args.keep_logs:
            (artifact_dir / f"{e.stem}.log").write_text(result.stdout_text, encoding="utf-8", errors="replace")
            # Snapshot the probe file-sink alongside the regular log.  Survives
            # across runs (the per-mission unlink above resets it for next).
            try:
                if ring_sink_path.exists():
                    (artifact_dir / f"{e.stem}.ring_trace.log").write_text(
                        ring_sink_path.read_text(encoding="utf-8", errors="replace"),
                        encoding="utf-8", errors="replace")
            except Exception as exc:
                print(f"[runner] could not snapshot ring_trace.log: {exc}", file=sys.stderr)
        if args.baseline_update and result.verdict.passed:
            baseline_data.setdefault(key, {})["destroys"] = {
                "mean": result.summary.destroys, "stddev": 0, "samples": 1,
                "updated": timestamp,
            }
            baseline_data[key]["perf"] = {
                "avg_fps": result.summary.perf.avg_fps,
                "p1low_fps": result.summary.perf.p1low_fps,
                "peak_ms": result.summary.perf.peak_ms,
            }

        # VEG-SMOKE-FLOOR-1: check vegetation output contract when gate is active.
        # Only fires when MC2_VEGETATION_CARDS=1; never affects non-veg smoke runs.
        if _veg_floor_active:
            _veg = _parse_veg_summary(result.stdout_text)
            _veg_fails = _check_veg_floor(e.stem, _veg)
            for _vf in _veg_fails:
                print(f"[runner] {_vf}", file=sys.stderr)
                _veg_floor_failures.append(_vf)
            if _veg:
                print(
                    f"[runner] [VEG-SMOKE-FLOOR] {e.stem}: "
                    f"instance_count={_veg['instance_count']} "
                    f"draw_calls={_veg['draw_calls']} "
                    f"flush_reason={_veg['flush_reason']} "
                    f"{'PASS' if not _veg_fails else 'FAIL'}",
                    file=sys.stderr)
            else:
                print(f"[runner] [VEG-SMOKE-FLOOR] {e.stem}: VEG_SUMMARY absent",
                      file=sys.stderr)

        if args.fail_fast and not result.verdict.passed:
            print(f"[runner] --fail-fast: stopping at {e.stem}", file=sys.stderr)
            break

        # 2s grace for PDB lock release before next spawn.
        import time as _t; _t.sleep(2)

    md = report.render_markdown(rows, tier=args.tier or "adhoc",
                                 profile=args.profile, timestamp=timestamp)
    (artifact_dir / "report.md").write_text(md, encoding="utf-8")
    (artifact_dir / "report.json").write_text(
        json.dumps(report.render_json(rows, tier=args.tier or "adhoc",
                                      profile=args.profile, timestamp=timestamp),
                   indent=2), encoding="utf-8")

    if args.baseline_update:
        baselines.save(BASELINE_PATH, baseline_data)

    sys.stdout.buffer.write(md.encode("utf-8"))
    sys.stdout.buffer.write(b"\n")
    sys.stdout.buffer.flush()
    passed = all(r.verdict.passed for r in rows) and (menu_canary_rc in (None, 0))

    # VEG-SMOKE-FLOOR-1: promote veg floor failures to hard fail when active.
    # Only active when MC2_VEGETATION_CARDS=1; non-veg runs are never affected.
    if _veg_floor_active and _veg_floor_failures:
        print(f"[runner] [VEG-SMOKE-FLOOR] HARD FAIL: {len(_veg_floor_failures)} "
              f"vegetation floor check(s) failed", file=sys.stderr)
        for _vf in _veg_floor_failures:
            print(f"[runner]   {_vf}", file=sys.stderr)
        passed = False

    # MC2_SMOKE_REQUIRE_FINGERPRINT=1: promote fingerprint mismatch/absence to
    # a hard failure. Default (unset) keeps the verdict untouched (advisory).
    if _fp_require and _fp_hard_fail:
        print("[runner] [DEPLOY_FINGERPRINT] HARD FAIL: fingerprint mismatch or "
              "absent and MC2_SMOKE_REQUIRE_FINGERPRINT=1", file=sys.stderr)
        passed = False

    # VISUAL advisory (S13+).  Summarises the most recent run_visual.py compare
    # report (zero engine cost -- it does NOT capture). Advisory by default:
    # never changes the exit code. MC2_SMOKE_REQUIRE_VISUAL=1 promotes a non-PASS
    # visual verdict to a hard fail (mirrors the DEPLOY_FINGERPRINT opt-in). Runs
    # BEFORE the cockpit hook so any hard-fail is reflected in the cockpit result.
    try:
        _vis_report = os.environ.get(
            "MC2_SMOKE_VISUAL_REPORT",
            str(Path(__file__).resolve().parent.parent /
                "tests" / "visual" / "compare" / "visual_compare_report.json"))
        if os.path.isfile(_vis_report):
            with open(_vis_report, "r", encoding="utf-8") as _vf:
                _vrep = json.load(_vf)
            _vverdict = str(_vrep.get("overall_verdict", "UNKNOWN"))
            _vsets = len(_vrep.get("sets", []) or [])
            _vwhen = _vrep.get("generated_utc", "?")
            _vline = (f"[visual-advisory] last compare: {_vverdict} "
                      f"({_vsets} sets) generated_utc={_vwhen}")
            if os.environ.get("MC2_SMOKE_REQUIRE_VISUAL") == "1" and _vverdict != "PASS":
                print(f"[runner] [VISUAL] HARD FAIL: visual verdict {_vverdict} and "
                      "MC2_SMOKE_REQUIRE_VISUAL=1", file=sys.stderr)
                passed = False
        else:
            _vline = ("[visual-advisory] no visual compare on record "
                      "(run scripts/run_visual.py compare to populate)")
        print(_vline, file=sys.stderr)
        with open(artifact_dir / "report.md", "a", encoding="utf-8") as _rf:
            _rf.write("\n" + _vline + "\n")
    except Exception as _vexc:  # noqa: BLE001  advisory must never break the gate
        print(f"[runner] [visual-advisory] skipped ({_vexc})", file=sys.stderr)

    # Diag-state check: verify V2 schema + shutdown dump + CONFIG trace event.
    # Advisory by default; MC2_SMOKE_REQUIRE_DIAG_STATE=1 = hard fail on any
    # failed check. Only runs when MC2_DEBUG_STATE_DUMP=1 was active.
    try:
        _diag_require = os.environ.get("MC2_SMOKE_REQUIRE_DIAG_STATE") == "1"
        _diag_dump_active = os.environ.get("MC2_DEBUG_STATE_DUMP") == "1"
        if _diag_dump_active:
            _diag_state_dir_env = os.environ.get("MC2_DEBUG_STATE_DUMP_DIR", "")
            _diag_state_dir = (Path(_diag_state_dir_env)
                               if _diag_state_dir_env
                               else Path(args.exe).resolve().parent / "debug_state")
            _diag_latest = _diag_state_dir / "latest_render_state.json"

            _diag_checks: list[str] = []  # one line per check: PASS/FAIL + detail
            _diag_ok = True

            # --- schema check ---
            if not _diag_latest.exists():
                _diag_checks.append("FAIL latest_render_state.json not found")
                _diag_ok = False
            else:
                try:
                    _ds = json.loads(_diag_latest.read_text(encoding="utf-8"))
                    _sv = _ds.get("schema_version")
                    _dk = _ds.get("dump_kind")
                    _sid = _ds.get("session_id", "")
                    _pid = _ds.get("pid")
                    _wat = _ds.get("written_at_epoch")
                    if _sv != 2:
                        _diag_checks.append(
                            f"FAIL schema_version={_sv!r} (expected 2)")
                        _diag_ok = False
                    else:
                        _diag_checks.append("PASS schema_version==2")
                    if _dk != "shutdown":
                        _diag_checks.append(
                            f"FAIL dump_kind={_dk!r} (expected 'shutdown' after exit)")
                        _diag_ok = False
                    else:
                        _diag_checks.append("PASS dump_kind=shutdown")
                    for _field, _val in (("session_id", _sid), ("pid", _pid),
                                         ("written_at_epoch", _wat)):
                        if _val is None or _val == "":
                            _diag_checks.append(f"FAIL {_field} missing/empty")
                            _diag_ok = False
                        else:
                            _diag_checks.append(f"PASS {_field} present")
                except Exception as _je:
                    _diag_checks.append(f"FAIL cannot parse latest_render_state.json: {_je}")
                    _diag_ok = False
                    _sid = ""

            # --- CONFIG event check (diagnostic_trace.jsonl) ---
            # Relative MC2_DIAGNOSTIC_TRACE_FILE resolves from the exe dir
            # (engine CWD), not the runner CWD.  Absolute paths pass through.
            _trace_env = os.environ.get("MC2_DIAGNOSTIC_TRACE_FILE", "")
            if _trace_env:
                _tp = Path(_trace_env)
                _trace_path = _tp if _tp.is_absolute() else Path(args.exe).resolve().parent / _tp
            else:
                _trace_path = _diag_state_dir / "diagnostic_trace.jsonl"
            if _trace_path.exists():
                try:
                    _trace_lines = _trace_path.read_text(
                        encoding="utf-8-sig", errors="replace").splitlines()
                    # utf-8-sig strips a leading BOM if present (guards against
                    # stale trace files written by the now-fixed _wfopen bug).
                    # Scan last 200 lines for a CONFIG startup event.
                    _cfg_found = False
                    for _tline in _trace_lines[-200:]:
                        try:
                            _te = json.loads(_tline)
                            if _te.get("tag") == "CONFIG" and "diagnostic_trace_initialized" in str(_te.get("data", {})):
                                _cfg_found = True
                                break
                        except Exception:
                            continue
                    if _cfg_found:
                        _diag_checks.append("PASS diagnostic_trace.jsonl has CONFIG startup event")
                    else:
                        _diag_checks.append("FAIL no CONFIG startup event in diagnostic_trace.jsonl")
                        _diag_ok = False
                except Exception as _te:
                    _diag_checks.append(f"FAIL cannot read diagnostic_trace.jsonl: {_te}")
                    _diag_ok = False
            else:
                _diag_checks.append(
                    f"WARN diagnostic_trace.jsonl not found at {_trace_path} "
                    f"(set MC2_DIAGNOSTIC_TRACE_FILE or MC2_DIAG_TAGS to enable)")

            _diag_verdict = "PASS" if _diag_ok else "FAIL"
            _diag_lines = [f"# Diag-state check: {_diag_verdict}",
                           ""] + [f"- {c}" for c in _diag_checks]
            _diag_report_text = "\n".join(_diag_lines) + "\n"
            (artifact_dir / "diag_state_report.md").write_text(
                _diag_report_text, encoding="utf-8")

            _diag_summary = (f"[diag-state] {_diag_verdict}: "
                             + "; ".join(_diag_checks))
            if _diag_require and not _diag_ok:
                print(f"[runner] [DIAG_STATE] HARD FAIL: schema/trace check "
                      f"failed and MC2_SMOKE_REQUIRE_DIAG_STATE=1",
                      file=sys.stderr)
                passed = False
            print(_diag_summary, file=sys.stderr)
            with open(artifact_dir / "report.md", "a", encoding="utf-8") as _rf:
                _rf.write("\n" + _diag_summary + "\n")
    except Exception as _diag_exc:  # noqa: BLE001  advisory must never break the gate
        print(f"[runner] [diag-state] skipped ({_diag_exc})", file=sys.stderr)

    # Post-verdict cockpit hook (S2).  Verdict is already frozen above.
    # Any exception here is swallowed by cockpit.write_cockpit_artifacts;
    # it never changes the exit code.
    if _cockpit is not None:
        _cockpit.write_cockpit_artifacts(
            artifact_dir,
            exe_path=args.exe,
            tier=args.tier or "adhoc",
            profile=args.profile,
            missions=[r.stem for r in rows],
            durations={r.stem: (args.duration or 120) for r in rows},
            result="PASS" if passed else "FAIL",
            source="smoke",
        )

    sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()
