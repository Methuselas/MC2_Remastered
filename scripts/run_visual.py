#!/usr/bin/env python3
"""run_visual.py -- the visual-regression front door (capture / compare / verify).

S9 gave deterministic capture; S12 gave the identity block; this adds the PIXEL
GATE: compare a fresh capture against a BLESSED golden set by byte-hash and emit
a PASS/FAIL verdict -- no eyeballing.

v1 scope (deliberately narrow -- see "what's NOT here"):
  - byte-hash compare ONLY (sha256 per bookmark PNG). Same machine, same GPU/
    driver, same exe lineage, same capture runner. No epsilon, no FLIP, no diff
    images -- those are v2.
  - REFUSES to compare against a non-blessed (candidate/retired) set. A golden
    you have not blessed is not a gate.
  - identity-compatibility gate first (exe sha + device), per the lab's
    identity-diff-first rule -- a mismatched build makes a pixel diff meaningless.

Subcommands:
  run_visual.py capture --set <id>            capture current build for a set's mission
  run_visual.py compare --set <id>            capture + byte-compare vs the blessed golden
  run_visual.py verify  --set <id>            alias for compare
  run_visual.py compare --set <id> --capture-dir DIR   compare an EXISTING capture dir

--set accepts an exact set_id OR a prefix matching several sets (e.g.
`baselineA-rc1` matches baselineA-rc1, baselineA-rc1-mc2_10, baselineA-rc1-mc2_17),
so one invocation can gate all of Baseline-A.

Verdicts (per set and overall, worst-case):
  PASS                       all expected frames present and byte-identical
  FAIL_CHANGED               a frame's sha differs from the golden
  FAIL_MISSING               an expected bookmark was not captured
  FAIL_IDENTITY_MISMATCH     exe sha or device differs (override flags exist)
  FAIL_NO_BLESSED_GOLDEN     the set is not status=blessed

Exit codes: 0 PASS; 2 FAIL_CHANGED; 3 FAIL_MISSING; 4 FAIL_IDENTITY_MISMATCH;
5 FAIL_NO_BLESSED_GOLDEN; 6 usage/IO error.

What's NOT here (v2+): cross-GPU epsilon tolerance, FLIP, diff/heatmap images,
HTML gallery, mc2_03/mc2_24 (animation-clock freeze), any engine change.
"""
from __future__ import annotations

import argparse
import csv
import datetime
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import manifest_schema as ms  # noqa: E402
import run_visual_capture as rvc  # noqa: E402

GOLDEN_SETS_REGISTRY = ROOT / "tests" / "visual" / "golden-sets.json"
GOLDENS_DIR = ROOT / "tests" / "visual" / "baselines"
BOOKMARK_DIR = ROOT / "tests" / "visual" / "bookmarks"
DEFAULT_OUT = ROOT / "tests" / "visual" / "compare"

# Verdicts + exit codes.
PASS = "PASS"
FAIL_CHANGED = "FAIL_CHANGED"
FAIL_MISSING = "FAIL_MISSING"
FAIL_IDENTITY = "FAIL_IDENTITY_MISMATCH"
FAIL_NO_BLESSED = "FAIL_NO_BLESSED_GOLDEN"
EXIT = {PASS: 0, FAIL_CHANGED: 2, FAIL_MISSING: 3, FAIL_IDENTITY: 4,
        FAIL_NO_BLESSED: 5}
# Severity for worst-case aggregation (higher = worse).
SEVERITY = {PASS: 0, FAIL_CHANGED: 1, FAIL_MISSING: 2, FAIL_IDENTITY: 3,
            FAIL_NO_BLESSED: 4}


def _utc() -> str:
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def load_registry() -> dict:
    if not GOLDEN_SETS_REGISTRY.exists():
        print(f"[run_visual] ERROR: registry missing: {GOLDEN_SETS_REGISTRY}",
              file=sys.stderr)
        sys.exit(6)
    return json.loads(GOLDEN_SETS_REGISTRY.read_text(encoding="utf-8"))


def resolve_sets(registry: dict, name: str) -> list[str]:
    """Exact set_id, else all set_ids with `name` as a prefix (sorted)."""
    sets = registry.get("sets", {})
    if name in sets:
        return [name]
    pref = sorted(k for k in sets if k.startswith(name))
    return pref


def load_golden(set_id: str) -> dict:
    """Read the on-disk set.json (authoritative golden frame hashes + identity)."""
    p = GOLDENS_DIR / set_id / "set.json"
    if not p.exists():
        raise FileNotFoundError(f"set.json missing for {set_id}: {p}")
    return json.loads(p.read_text(encoding="utf-8"))


def capture_current(mission: str, exe: Path, out_dir: Path,
                    trigger: int, settle: int, duration: int) -> dict:
    """Capture the current build for `mission` with a warmup-discard (run 1
    cold, run 2 kept) so the comparison frame is not first-use noise. Returns
    the kept run's result dict (rvc.run_one shape)."""
    bookmarks = BOOKMARK_DIR / f"{mission}.json"
    if not bookmarks.exists():
        raise FileNotFoundError(f"bookmarks missing for {mission}: {bookmarks}")
    warm_dir = out_dir.parent / f"{out_dir.name}_warmup"
    print(f"[run_visual] warmup capture (discarded) for {mission} ...")
    rvc.run_one(exe, mission, bookmarks, warm_dir, trigger, settle, duration, True)
    print(f"[run_visual] compare capture for {mission} ...")
    return rvc.run_one(exe, mission, bookmarks, out_dir, trigger, settle,
                       duration, True)


def existing_capture(mission: str, capture_dir: Path) -> dict:
    """Build an rvc-style result dict from an existing capture dir."""
    bookmarks = BOOKMARK_DIR / f"{mission}.json"
    file_mission, marks = rvc._load_bookmarks(bookmarks)
    token = file_mission or mission
    bms = []
    for m in marks:
        png = capture_dir / f"{token}_{rvc._safe_name(m['name'])}.png"
        side = png.with_suffix(".json")
        det = None
        if side.exists():
            try:
                det = json.loads(side.read_text(encoding="utf-8")).get("deterministic")
            except Exception:
                det = None
        bms.append({"name": m["name"], "png": str(png), "present": png.exists(),
                    "sha256": ms.file_sha256(png) if png.exists() else None,
                    "covers": m.get("covers", []), "engine_deterministic": det})
    return {"mission": token, "out_dir": str(capture_dir),
            "engine_capture_fired": any(b["present"] for b in bms),
            "bookmarks": bms}


def current_identity(result: dict, exe: Path) -> dict:
    return ms.identity_block(generator="visual_compare", exe_path=exe,
                             repo_root=str(ROOT),
                             deploy_target=str(exe.parent))


def compare_set(set_id: str, registry: dict, exe: Path, capture_dir: Path | None,
                trigger: int, settle: int, duration: int,
                allow_exe: bool, allow_device: bool) -> dict:
    """Compare one set; return a per-set result record."""
    reg_entry = registry["sets"][set_id]
    status = reg_entry.get("status")
    golden = load_golden(set_id)
    mission = golden.get("mission") or reg_entry.get("mission")
    rows: list[dict] = []
    rec = {"golden_set": set_id, "golden_status": status, "mission": mission,
           "expected_count": len(golden.get("bookmarks", [])),
           "matched_count": 0, "missing_count": 0, "changed_count": 0,
           "verdict": PASS, "rows": rows}

    # HARD RULE 1: refuse non-blessed golden.
    if status != "blessed":
        rec["verdict"] = FAIL_NO_BLESSED
        rec["reason"] = f"golden status is {status!r}, not 'blessed'"
        return rec

    # Capture (or load existing).
    if capture_dir is not None:
        cur = existing_capture(mission, capture_dir)
    else:
        cap_out = DEFAULT_OUT / set_id
        cur = capture_current(mission, exe, cap_out, trigger, settle, duration)
    rec["capture_dir"] = cur["out_dir"]
    cur_ident = current_identity(cur, exe)

    # HARD RULE 2: identity compatibility (exe sha + device).
    g_ident = golden.get("identity", {})
    g_exe = (g_ident.get("exe") or {}).get("sha256")
    c_exe = (cur_ident.get("exe") or {}).get("sha256")
    if g_exe != c_exe and not allow_exe:
        rec["verdict"] = FAIL_IDENTITY
        rec["reason"] = (f"exe sha mismatch: golden {(g_exe or '?')[:12]} != "
                         f"current {(c_exe or '?')[:12]} (use --allow-exe-mismatch)")
        return rec
    g_host = g_ident.get("host")
    c_host = cur_ident.get("host")
    if g_host != c_host and not allow_device:
        rec["verdict"] = FAIL_IDENTITY
        rec["reason"] = (f"device (host) mismatch: golden {g_host} != current "
                         f"{c_host} (use --allow-device-mismatch). NOTE v1 uses "
                         "host as the GPU/driver proxy; GPU string capture is v2.")
        return rec

    # Per-bookmark byte compare.
    cur_by_name = {b["name"]: b for b in cur["bookmarks"]}
    worst = PASS
    for gbm in golden.get("bookmarks", []):
        name = gbm["name"]
        exp = gbm.get("sha256")
        cbm = cur_by_name.get(name)
        if cbm is None or not cbm.get("present"):
            result, reason = FAIL_MISSING, "current capture missing this bookmark"
            rec["missing_count"] += 1
        elif cbm.get("engine_deterministic") is not True:
            result, reason = FAIL_MISSING, "current capture not deterministic (sidecar deterministic!=true)"
            rec["missing_count"] += 1
        elif cbm.get("sha256") == exp:
            result, reason = PASS, "byte-identical"
            rec["matched_count"] += 1
        else:
            result, reason = FAIL_CHANGED, "sha256 differs from golden"
            rec["changed_count"] += 1
        if SEVERITY[result] > SEVERITY[worst]:
            worst = result
        rows.append({"mission": mission, "bookmark": name,
                     "expected_sha256": exp,
                     "actual_sha256": (cbm or {}).get("sha256"),
                     "result": result, "reason": reason})
    rec["verdict"] = worst
    return rec


def write_reports(out_dir: Path, set_results: list[dict], overall: str,
                  exe: Path) -> tuple[Path, Path]:
    out_dir.mkdir(parents=True, exist_ok=True)
    # CSV (flat per-bookmark rows).
    csv_path = out_dir / "visual_compare_report.csv"
    with open(csv_path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["mission", "bookmark", "expected_sha256", "actual_sha256",
                    "result", "reason"])
        for sr in set_results:
            for r in sr.get("rows", []):
                w.writerow([r["mission"], r["bookmark"],
                            r["expected_sha256"] or "", r["actual_sha256"] or "",
                            r["result"], r["reason"]])
            if not sr.get("rows"):  # identity/no-blessed failures have no rows
                w.writerow([sr.get("mission", ""), "", "", "", sr["verdict"],
                            sr.get("reason", "")])
    # JSON (S12 identity + per-set rollup).
    ident = ms.identity_block(generator="visual_compare", exe_path=exe,
                              repo_root=str(ROOT), deploy_target=str(exe.parent))
    doc = ms.attach({"kind": "visual-compare", "generated_utc": _utc()}, ident)
    doc["overall_verdict"] = overall
    doc["sets"] = [{k: sr[k] for k in
                    ("golden_set", "golden_status", "mission", "expected_count",
                     "matched_count", "missing_count", "changed_count",
                     "verdict")} | ({"reason": sr["reason"]} if "reason" in sr else {})
                   for sr in set_results]
    json_path = out_dir / "visual_compare_report.json"
    json_path.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
    return csv_path, json_path


def cmd_compare(args: argparse.Namespace) -> int:
    registry = load_registry()
    set_ids = resolve_sets(registry, args.set)
    if not set_ids:
        print(f"[run_visual] ERROR: no set matches {args.set!r}. Known: "
              f"{sorted(registry.get('sets', {}))}", file=sys.stderr)
        return 6
    exe = Path(args.exe)
    if args.capture_dir is None and not exe.exists():
        print(f"[run_visual] ERROR: exe missing: {exe}", file=sys.stderr)
        return 6
    capture_dir = Path(args.capture_dir) if args.capture_dir else None

    set_results = []
    for sid in set_ids:
        print(f"[run_visual] === set {sid} (status="
              f"{registry['sets'][sid].get('status')}) ===")
        sr = compare_set(sid, registry, exe, capture_dir, args.trigger_frame,
                         args.settle, args.duration, args.allow_exe_mismatch,
                         args.allow_device_mismatch)
        set_results.append(sr)
        print(f"[run_visual]   verdict={sr['verdict']} "
              f"matched={sr['matched_count']}/{sr['expected_count']} "
              f"missing={sr['missing_count']} changed={sr['changed_count']}"
              + (f"  ({sr['reason']})" if sr.get("reason") else ""))

    overall = PASS
    for sr in set_results:
        if SEVERITY[sr["verdict"]] > SEVERITY[overall]:
            overall = sr["verdict"]

    out_dir = Path(args.out_dir) if args.out_dir else DEFAULT_OUT
    csv_path, json_path = write_reports(out_dir, set_results, overall, exe)
    print(f"[run_visual] OVERALL {overall}")
    print(f"[run_visual] report: {json_path}")
    print(f"[run_visual] report: {csv_path}")
    return EXIT[overall]


def cmd_capture(args: argparse.Namespace) -> int:
    registry = load_registry()
    set_ids = resolve_sets(registry, args.set)
    if not set_ids:
        print(f"[run_visual] ERROR: no set matches {args.set!r}", file=sys.stderr)
        return 6
    exe = Path(args.exe)
    if not exe.exists():
        print(f"[run_visual] ERROR: exe missing: {exe}", file=sys.stderr)
        return 6
    rc = 0
    for sid in set_ids:
        mission = registry["sets"][sid].get("mission")
        out = DEFAULT_OUT / sid
        try:
            r = capture_current(mission, exe, out, args.trigger_frame,
                                args.settle, args.duration)
            present = all(b["present"] for b in r["bookmarks"])
            print(f"[run_visual] {sid}: captured {mission} present={present} -> {out}")
            rc = rc or (0 if present else 1)
        except Exception as e:
            print(f"[run_visual] {sid}: capture FAILED: {e}", file=sys.stderr)
            rc = 1
    return rc


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description="Visual-regression front door "
                                 "(capture / compare / verify).")
    sub = ap.add_subparsers(dest="cmd", required=True)

    def add_common(p):
        p.add_argument("--set", required=True,
                       help="golden set_id or prefix (e.g. baselineA-rc1)")
        p.add_argument("--exe", default=str(rvc.DEFAULT_EXE))
        p.add_argument("--trigger-frame", type=int, default=120)
        p.add_argument("--settle", type=int, default=30)
        p.add_argument("--duration", type=int, default=45)

    pc = sub.add_parser("capture", help="capture current build for a set's mission(s)")
    add_common(pc)

    for verb in ("compare", "verify"):
        pp = sub.add_parser(verb, help="byte-compare current capture vs blessed golden")
        add_common(pp)
        pp.add_argument("--capture-dir", default=None,
                        help="compare an EXISTING capture dir instead of capturing now")
        pp.add_argument("--out-dir", default=None,
                        help=f"report output dir (default {DEFAULT_OUT})")
        pp.add_argument("--allow-exe-mismatch", action="store_true",
                        help="do not fail on exe-sha mismatch (use with care)")
        pp.add_argument("--allow-device-mismatch", action="store_true",
                        help="do not fail on device/host mismatch (cross-machine; "
                             "byte-compare is NOT valid cross-GPU -- v1 caveat)")
    return ap


def main() -> int:
    args = build_parser().parse_args()
    if args.cmd == "capture":
        return cmd_capture(args)
    return cmd_compare(args)  # compare + verify


if __name__ == "__main__":
    sys.exit(main())
