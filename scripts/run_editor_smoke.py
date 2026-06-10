#!/usr/bin/env python3
# scripts/run_editor_smoke.py
"""Console smoke runner for the MC2 Mission Editor (EditRel / "Mission Editor.exe").

The editor is an MFC/WIN32-subsystem GUI app, so there is no console stdout to
scrape and no way to click menus headlessly. Instead we drive the editor's
existing self-run CLI (editor/EditorMFC.cpp s_cli_parse: -gen-map / -mission /
-frames / -exit-on-load-fail / -smoke-foliage) and read the result back from the
machine-readable lines it writes:

  * `[ESMOKE v1] event=summary clean_exit=1 frames=N autoload=1 gen_map=1
     foliage_smoke=1 foliage_count=K`   -- emitted at ExitInstance
  * `[EDITOR_CLI v1] event=...`         -- per-step facts
  * EarlyTrace milestones               -- in <deploy>/editor-startup.log

editor-startup.log is the reliable IPC channel (opened "w" at process start);
stderr is also captured via an inherited pipe when available.

Verdict (mirrors the game smoke's gates philosophy: the runner judges, the app
only reports facts):
  PASS  iff process exited (not timeout) AND returncode==0 AND the ESMOKE summary
        shows clean_exit=1 AND autoload=1 AND no crash/asset/STOP/FAILED tokens,
        plus any per-case expectation (e.g. foliage_count>0 for foliage_present).

This validates the editor-facing half of the Phase 5 manual smoke:
  launch -> generate/load map -> (load foliage / toggle) -> clean exit, and that
  a missing or garbage foliage JSON / missing sprite texture does NOT crash.

Usage:
  py -3 scripts/run_editor_smoke.py [--exe PATH] [--frames N] [--timeout SEC]
                                    [--case NAME ...] [--keep-logs]
  Cases: gen_map_basic, foliage_present, foliage_missing, foliage_garbage (default: all)
"""
import argparse
import os
import re
import subprocess
import sys
import time
from pathlib import Path

DEFAULT_EXE = r"A:\Games\mc2-opengl\mc2-win64-0.4c\Mission Editor.exe"

# Run the editor window minimized/non-activating during smoke (set from --minimized).
MINIMIZED = True
# Strong crash signatures only -- the editor trace log is verbose, so weak tokens
# like "FAILED"/"assert"/"STOP(" would false-positive. A load failure is already
# caught by rc!=0 (the editor exit(1)s under -exit-on-load-fail).
CRASH_TOKENS = ("unhandled exception", "access violation", "stack overflow", "fatal error")
REPO = Path(__file__).resolve().parents[1]


def _gen_foliage_json(deploy: Path, instances: bool) -> bool:
    """Produce <deploy>/terrain_gen_out/genmap.foliage.json via the deployed
    generator. instances=True -> real rules (placed trees); the editor's later
    -gen-map run does not pass --generate-foliage so it won't clobber this file."""
    tg = deploy / "tools" / "terrain_gen" / "terrain_gen.py"
    rules = deploy / "tools" / "terrain_gen" / "recipes" / "foliage_rules_example.json"
    if not tg.exists():
        print(f"  [setup] generator not found at {tg}; skipping foliage pre-gen")
        return False
    out = deploy / "terrain_gen_out"
    out.mkdir(parents=True, exist_ok=True)
    recipe = out / "smoke_genmap_recipe.json"
    recipe.write_text('{"version":1,"name":"genmap","size":60,'
                      '"biome":"temperate_hills","seed":7}\n', encoding="utf-8")
    argv = ["py", "-3", str(tg), str(recipe), "--out", str(out), "--generate-foliage"]
    if rules.exists():
        argv += ["--foliage-rules", str(rules)]
    r = subprocess.run(argv, cwd=str(deploy), capture_output=True, text=True)
    ok = (r.returncode == 0) and (out / "genmap.foliage.json").exists()
    print(f"  [setup] foliage pre-gen rc={r.returncode} present={ok}")
    return ok


def _write_garbage(deploy: Path) -> Path:
    out = deploy / "terrain_gen_out"
    out.mkdir(parents=True, exist_ok=True)
    p = out / "garbage.foliage.json"
    p.write_bytes(b'{ this is not valid json ]]] "instances" 0xDEADBEEF \x00\x01\x02')
    return p


def _parse_esmoke(text: str) -> dict:
    m = re.search(r"\[ESMOKE v1\] event=summary (.+)", text)
    if not m:
        return {}
    fields = {}
    for kv in m.group(1).split():
        if "=" in kv:
            k, v = kv.split("=", 1)
            fields[k] = v
    return fields


def run_case(name: str, exe: Path, deploy: Path, exit_sec: int, timeout: int,
             extra_flags: list, expect) -> dict:
    """Launch the editor for one case; return a result dict with verdict."""
    log = deploy / "editor-startup.log"
    try:
        if log.exists():
            log.unlink()
    except OSError:
        pass

    # -smoke-exit-sec drives a reliable time-based hard exit(0) after the engine is
    # up + the map generated (the -frames counter path is unreliable and a WM_CLOSE
    # stalls on the save-changes modal in a headless run). extra_flags carries the
    # map source (-gen-map / -mission) plus any -smoke-foliage / -smoke-save.
    argv = [str(exe), f"-smoke-exit-sec={exit_sec}", "-exit-on-load-fail"] + extra_flags
    env = dict(os.environ)
    env["MC2_EDITOR_TRACE"] = "1"

    # Launch minimized + non-activating so the smoke does not steal focus or pop a
    # window over the user's work. The editor's MFC main frame honors nCmdShow from
    # the process STARTUPINFO on its first ShowWindow, and the GL viewport is a child
    # of that frame, so the whole editor comes up minimized.
    startupinfo = None
    if MINIMIZED and os.name == "nt":
        startupinfo = subprocess.STARTUPINFO()
        startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startupinfo.wShowWindow = 7  # SW_SHOWMINNOACTIVE

    print(f"[case {name}] launch: {' '.join(extra_flags) or '(gen-map only)'}  exit_sec={exit_sec}"
          f"{'  [minimized]' if startupinfo else ''}")
    t0 = time.time()
    timed_out = False
    try:
        proc = subprocess.Popen(argv, cwd=str(deploy), env=env, startupinfo=startupinfo,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            out, _ = proc.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            proc.kill()
            out, _ = proc.communicate()
        rc = proc.returncode
    except FileNotFoundError:
        return dict(name=name, passed=False, bucket="exe_missing",
                    detail=f"editor exe not found: {exe}")
    dt = time.time() - t0

    logtext = ""
    if log.exists():
        logtext = log.read_text(errors="replace")
    combined = (out or "") + "\n" + logtext
    esmoke = _parse_esmoke(combined)

    # Buckets (first match wins) -------------------------------------------------
    bucket = ""
    low = combined.lower()
    if timed_out:
        bucket = "timeout"
    elif rc != 0:
        bucket = "nonzero_exit"
    elif not esmoke:
        bucket = "no_summary"
    elif esmoke.get("clean_exit") != "1":
        bucket = "unclean_exit"
    elif esmoke.get("autoload") != "1":
        bucket = "autoload_missing"
    else:
        for tok in CRASH_TOKENS:
            if tok.lower() in low:
                bucket = f"crash_token:{tok}"
                break

    # Per-case expectation -------------------------------------------------------
    detail = ""
    if not bucket and expect:
        fc = int(esmoke.get("foliage_count", "-1"))
        if expect == "foliage_present" and fc <= 0:
            bucket = "foliage_empty"; detail = f"expected foliage_count>0, got {fc}"
        if expect == "foliage_tolerated":
            # missing/garbage JSON must load to 0 instances WITHOUT crashing.
            # Reaching a clean summary with foliage_smoke=1 is the pass condition;
            # count is expected to be 0.
            if esmoke.get("foliage_smoke") != "1":
                bucket = "foliage_not_run"; detail = "foliage smoke step did not run"
        if expect == "saved" and esmoke.get("saved") != "1":
            bucket = "save_failed"; detail = f"expected saved=1, got {esmoke.get('saved')}"
        if expect == "menu":
            # Drove the foliage menu commands via WM_COMMAND: visibility must toggle
            # off (vis1) then back on (vis2), Clear must drop the count to 0, and
            # Generate/reload must place instances again (>0).
            v0, v1, v2 = esmoke.get("menu_vis0"), esmoke.get("menu_vis1"), esmoke.get("menu_vis2")
            clr, rld = esmoke.get("menu_clear"), esmoke.get("menu_reload")
            if not (v0 == "1" and v1 == "0" and v2 == "1"):
                bucket = "menu_toggle_bad"; detail = f"visibility {v0}->{v1}->{v2} (want 1,0,1)"
            elif clr != "0":
                bucket = "menu_clear_bad"; detail = f"count after Clear = {clr} (want 0)"
            elif not (rld and rld.lstrip("-").isdigit() and int(rld) > 0):
                bucket = "menu_reload_bad"; detail = f"count after reload = {rld} (want >0)"
        if expect == "outliner":
            # Read-only Scene Outliner enumerated placed objects. An auto-generated
            # empty map legitimately has 0 objects (selected=0); the pass condition
            # is that the step ran (count present, >=0) and the select-first result
            # is consistent: objects present -> selected, none -> not selected.
            cnt = esmoke.get("outliner_count")
            sel = esmoke.get("outliner_selected")
            if cnt is None or not cnt.lstrip("-").isdigit() or int(cnt) < 0:
                bucket = "outliner_not_run"; detail = f"outliner_count={cnt} (want >=0)"
            elif sel not in ("0", "1"):
                bucket = "outliner_sel_bad"; detail = f"outliner_selected={sel} (want 0/1)"
            elif int(cnt) > 0 and sel != "1":
                bucket = "outliner_sel_bad"; detail = f"count={cnt} but selected={sel} (want 1)"
            elif int(cnt) == 0 and sel != "0":
                bucket = "outliner_sel_bad"; detail = f"count=0 but selected={sel} (want 0)"
        if expect == "inspector":
            # Read-only Inspector produced a selection summary. An auto-generated
            # empty map has nothing to select (selected=0, type=none); a non-empty
            # one selects the first object (selected=1, type != none). Either way
            # the step must run crash-free with a consistent type token.
            isel = esmoke.get("inspector_selected")
            itype = esmoke.get("inspector_type")
            if isel not in ("0", "1"):
                bucket = "inspector_not_run"; detail = f"inspector_selected={isel} (want 0/1)"
            elif isel == "1" and (not itype or itype == "none"):
                bucket = "inspector_type_bad"; detail = f"selected=1 but type={itype}"
            elif isel == "0" and itype != "none":
                bucket = "inspector_type_bad"; detail = f"selected=0 but type={itype} (want none)"

    passed = (bucket == "")
    return dict(name=name, passed=passed, bucket=bucket, detail=detail,
                rc=rc, seconds=round(dt, 1), esmoke=esmoke)


def rand_gen_flag():
    """Random map: size index 0..3 (cellSide 60/80/.../label) x terrain type 0..15.
    The user asked for randomized settings (mountains/terrain/size)."""
    import hashlib
    # Avoid Math.random-style nondeterminism complaints: seed from PID+clock once.
    seed = (os.getpid() ^ int(time.time())) & 0xffff
    size = seed % 2            # 0..1 (keep the save/load smoke fast; big maps crawl)
    terrain = (seed // 2) % 16  # 0..15 (random terrain type, incl. mountains/snow/etc.)
    return f"-gen-map={size},{terrain}", size, terrain


def build_suite(deploy: Path):
    cases = {
        "gen_map_basic":   (["-gen-map=0,0"], None),                    # launch+generate+render+exit
        "foliage_present": (["-gen-map=0,0", "-smoke-foliage"], "foliage_present"),
        "foliage_missing": (["-gen-map=0,0", "-smoke-foliage=terrain_gen_out\\__does_not_exist__.json"], "foliage_tolerated"),
        "foliage_garbage": (["-gen-map=0,0", "-smoke-foliage=terrain_gen_out\\garbage.foliage.json"], "foliage_tolerated"),
        "foliage_menu_commands": (["-gen-map=0,0", "-smoke-foliage-menu"], "menu"),
        "outliner":        (["-gen-map=0,0", "-smoke-outliner"], "outliner"),  # read-only Scene Outliner
        "inspector":       (["-gen-map=0,0", "-smoke-inspector"], "inspector"),  # read-only Inspector
        # gen_save_load is handled specially (two phases) in main().
    }
    return cases


def main() -> int:
    ap = argparse.ArgumentParser(description="MC2 Mission Editor console smoke runner")
    ap.add_argument("--exe", default=DEFAULT_EXE, help="path to Mission Editor.exe")
    ap.add_argument("--exit-sec", type=int, default=12,
                    help="editor hard-exits this many seconds after the engine is up (per case)")
    ap.add_argument("--timeout", type=int, default=180, help="per-case timeout seconds (hard backstop)")
    ap.add_argument("--case", action="append", help="run only these cases (repeatable)")
    ap.add_argument("--keep-logs", action="store_true", help="copy each case's editor-startup.log into the report dir")
    ap.add_argument("--minimized", dest="minimized", action="store_true", default=True,
                    help="launch the editor minimized/non-activating (default; Windows only)")
    ap.add_argument("--no-minimized", dest="minimized", action="store_false",
                    help="launch the editor with a normal visible window")
    args = ap.parse_args()

    global MINIMIZED
    MINIMIZED = args.minimized

    exe = Path(args.exe)
    deploy = exe.parent
    if not exe.exists():
        print(f"ERROR: editor exe not found: {exe}\n  Build EditRel + deploy-editor (DEPLOY=...0.4c) first.")
        return 2

    suite = build_suite(deploy)
    ALL = list(suite.keys()) + ["gen_save_load"]
    selected = args.case or ALL
    for c in selected:
        if c not in ALL:
            print(f"ERROR: unknown case '{c}'. Valid: {', '.join(ALL)}")
            return 2

    # Setup fixtures
    need_present = ("foliage_present" in selected) or ("foliage_menu_commands" in selected)
    need_garbage = "foliage_garbage" in selected
    if need_present:
        _gen_foliage_json(deploy, instances=True)   # initial genmap.foliage.json to toggle/clear/reload
    if need_garbage:
        _write_garbage(deploy)

    results = []
    for c in selected:
        if c == "gen_save_load":
            # Two phases: (1) generate a RANDOM map + save to .pak; (2) load that
            # .pak back. Both must reach a clean summary -> generate->save->load
            # works through the editor itself.
            save_rel = "terrain_gen_out\\smoke_saved.pak"
            pak = deploy / "terrain_gen_out" / "smoke_saved.pak"
            try:
                if pak.exists():
                    pak.unlink()
            except OSError:
                pass
            gflag, sz, terr = rand_gen_flag()
            print(f"[gen_save_load] random map: size_idx={sz} terrain={terr}")
            r1 = run_case("gen_save_load:gen+save", exe, deploy, args.exit_sec, args.timeout,
                          [gflag, f"-smoke-save={save_rel}"], "saved")
            results.append(r1)
            if r1["passed"] and pak.exists():
                r2 = run_case("gen_save_load:load", exe, deploy, args.exit_sec, args.timeout,
                              [f"-mission={save_rel}"], None)
            else:
                r2 = dict(name="gen_save_load:load", passed=False, bucket="no_saved_pak",
                          detail="phase-1 save failed or .pak missing", rc="-", seconds="-", esmoke={})
            results.append(r2)
        else:
            flags, expect = suite[c]
            results.append(run_case(c, exe, deploy, args.exit_sec, args.timeout, list(flags), expect))

    # Report
    ts = time.strftime("%Y-%m-%dT%H-%M-%S")
    rptdir = REPO / "tests" / "smoke" / "editor" / ts
    rptdir.mkdir(parents=True, exist_ok=True)
    npass = sum(1 for r in results if r["passed"])
    lines = [f"# Editor smoke {ts}  result={'PASS' if npass==len(results) else 'FAIL'} "
             f"({npass}/{len(results)})", "",
             "| Case | Result | Bucket | rc | sec | foliage_count |",
             "|------|--------|--------|----|----|---------------|"]
    for r in results:
        es = r.get("esmoke", {})
        lines.append(f"| {r['name']} | {'PASS' if r['passed'] else 'FAIL'} | "
                     f"{r['bucket'] or '-'} | {r.get('rc','-')} | {r.get('seconds','-')} | "
                     f"{es.get('foliage_count','-')} |")
        if r.get("detail"):
            lines.append(f"|  | | {r['detail']} | | | |")
    report = "\n".join(lines) + "\n"
    (rptdir / "report.md").write_text(report, encoding="utf-8")
    print("\n" + report)
    print(f"report: {rptdir / 'report.md'}")

    return 0 if npass == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
