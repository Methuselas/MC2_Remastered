#!/usr/bin/env python3
"""render_pass_report.py -- turn [RENDER_PASS v1] telemetry into a per-pass report.

The pixel gate (run_visual.py) tells you a frame's hash CHANGED. This tells you
WHERE: which render pass changed its FBO, viewport, draw-buffer count, or its
ORDER within the frame. It's the black-box flight recorder for render work.

Reads [RENDER_PASS v1] lines from a smoke/capture artifact dir (or a single log):
    [RENDER_PASS v1] frame=N pass=NAME fbo=F viewport=X,Y,W,H drawbuffers=D phase=begin hint=HINT

Engine emits these only when MC2_RENDER_PASS_TELEMETRY=1 (default OFF), sampled
every 300 frames (frames 1, 301, 601, ...), one line per pass per sampled frame,
in pass-BEGIN (render) order. So the line sequence within a frame IS the pass
execution order -- that's the "shadow pass changed draw order" signal.

WHAT THIS REPORTS (fields the engine actually emits today):
    frame, pass_name, order_in_frame, fbo, viewport, drawbuffers, hint
  + per-pass cross-frame stability (did fbo/viewport/drawbuffers/order change?)
  + per-source pass-order sequence

GAPS (D2-deferred in the engine -- listed, NOT invented here):
    begin/end pairing (only 'begin' emitted), draws, dispatches, clears,
    program_changes, texture_bind_count, gpu_ms, cpu_ms. These need counter
    feeds at centralized draw-submission sites (render_contract.cpp:519).

Usage:
  py -3 scripts/render_pass_report.py <artifact_dir_or_log>
  py -3 scripts/render_pass_report.py <dir> --out-dir <dir>
  py -3 scripts/render_pass_report.py <dir> --compare <prev_report.json>   # diff: WHERE changed

Exit codes: 0 report written (with or without records); 3 --compare found
differences; 6 input not found.
"""
from __future__ import annotations

import argparse
import csv
import datetime
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
try:
    import manifest_schema as ms  # noqa: E402
except Exception:
    ms = None

# [RENDER_PASS v1] frame=N pass=NAME fbo=F viewport=X,Y,W,H drawbuffers=D phase=begin hint=HINT
_LINE = re.compile(
    r"\[RENDER_PASS v1\]\s+frame=(\d+)\s+pass=(\S+)\s+fbo=(-?\d+)\s+"
    r"viewport=(-?\d+),(-?\d+),(-?\d+),(-?\d+)\s+drawbuffers=(\d+)\s+"
    r"phase=(\w+)(?:\s+hint=(.*))?$"
)

# Fields the user's wishlist wants that the engine does NOT emit yet.
GAPS = [
    ("begin_end_pairing", "only phase=begin is emitted; no end marker"),
    ("draws", "D2 deferred -- needs counter feed at draw-submission sites"),
    ("dispatches", "D2 deferred -- no compute-dispatch counter feed"),
    ("clears", "D2 deferred -- no glClear* counter feed per pass"),
    ("program_changes", "D2 deferred -- no glUseProgram counter feed per pass"),
    ("texture_bind_count", "D2 deferred -- no texture-bind counter feed per pass"),
    ("gpu_ms", "D2 deferred -- no per-pass GPU timer query feed"),
    ("cpu_ms", "D2 deferred -- no per-pass CPU timer feed"),
]
FIELDS_AVAILABLE = ["frame", "pass", "order_in_frame", "fbo", "viewport",
                    "drawbuffers", "hint"]


def _utc() -> str:
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def parse_log(path: Path) -> list[dict]:
    """Parse one log; assign order_in_frame by line sequence within each frame."""
    recs: list[dict] = []
    order_ctr: dict[int, int] = {}
    try:
        text = path.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return recs
    for line in text.splitlines():
        m = _LINE.search(line)
        if not m:
            continue
        frame = int(m.group(1))
        order = order_ctr.get(frame, 0)
        order_ctr[frame] = order + 1
        recs.append({
            "source": path.name,
            "frame": frame,
            "order_in_frame": order,
            "pass": m.group(2),
            "fbo": int(m.group(3)),
            "viewport": f"{m.group(4)},{m.group(5)},{m.group(6)},{m.group(7)}",
            "drawbuffers": int(m.group(8)),
            "phase": m.group(9),
            "hint": (m.group(10) or "").strip(),
        })
    return recs


def collect(target: Path) -> tuple[list[dict], list[str]]:
    """Return (records, source_log_names). target may be a dir or a single log."""
    recs: list[dict] = []
    sources: list[str] = []
    logs = [target] if target.is_file() else sorted(target.rglob("*.log"))
    for lg in logs:
        r = parse_log(lg)
        if r:
            recs.extend(r)
            sources.append(lg.name)
    return recs, sources


def _stable(values: list) -> dict:
    uniq = sorted(set(values), key=lambda x: (str(type(x)), x))
    return {"values": uniq, "stable": len(uniq) <= 1}


def build_report(target: Path, records: list[dict], sources: list[str]) -> dict:
    # per-pass rollup
    passes: dict[str, dict] = {}
    by_pass: dict[str, list[dict]] = {}
    for r in records:
        by_pass.setdefault(r["pass"], []).append(r)
    for name, rs in sorted(by_pass.items()):
        passes[name] = {
            "records": len(rs),
            "frames": sorted({r["frame"] for r in rs}),
            "sources": sorted({r["source"] for r in rs}),
            "fbo": _stable([r["fbo"] for r in rs]),
            "viewport": _stable([r["viewport"] for r in rs]),
            "drawbuffers": _stable([r["drawbuffers"] for r in rs]),
            "order_in_frame": _stable([r["order_in_frame"] for r in rs]),
            "hints": sorted({r["hint"] for r in rs if r["hint"]}),
        }
    # per-source pass-order (render order) per sampled frame
    per_source: dict[str, dict] = {}
    for r in records:
        s = per_source.setdefault(r["source"], {})
        fr = s.setdefault(str(r["frame"]), [])
        fr.append((r["order_in_frame"], r["pass"]))
    for s in per_source.values():
        for fr in list(s):
            s[fr] = [p for _o, p in sorted(s[fr])]

    identity = None
    if ms is not None:
        # Reuse the run manifest's identity if present in the artifact dir.
        man = (target if target.is_dir() else target.parent) / "manifest.json"
        if man.exists():
            try:
                doc = json.loads(man.read_text(encoding="utf-8"))
                identity = doc.get("identity")
            except Exception:
                identity = None
        if identity is None:
            identity = ms.identity_block(generator="render_pass_report",
                                         repo_root=str(ROOT))

    return {
        "kind": "render-pass-report",
        "schema": "mc2-render-pass/1",
        "generated_utc": _utc(),
        "source": str(target),
        "sources": sources,
        "telemetry_present": bool(records),
        "telemetry_note": (None if records else
                           "no [RENDER_PASS v1] records found -- run the engine "
                           "with MC2_RENDER_PASS_TELEMETRY=1 (default OFF)."),
        "record_count": len(records),
        "fields_available": FIELDS_AVAILABLE,
        "gaps": [{"field": f, "status": "not_emitted", "reason": why}
                 for f, why in GAPS],
        "identity": identity,
        "passes": passes,
        "per_source_pass_order": per_source,
    }


def write_outputs(report: dict, records: list[dict], out_dir: Path) -> tuple[Path, Path]:
    out_dir.mkdir(parents=True, exist_ok=True)
    csv_path = out_dir / "render_pass_report.csv"
    with open(csv_path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["source", "frame", "order_in_frame", "pass", "fbo",
                    "viewport", "drawbuffers", "phase", "hint"])
        for r in records:
            w.writerow([r["source"], r["frame"], r["order_in_frame"], r["pass"],
                        r["fbo"], r["viewport"], r["drawbuffers"], r["phase"],
                        r["hint"]])
    json_path = out_dir / "render_pass_report.json"
    json_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return csv_path, json_path


def compare(cur: dict, prev_path: Path) -> list[str]:
    """Diff two reports -> human 'where changed' lines. Empty == no change."""
    prev = json.loads(prev_path.read_text(encoding="utf-8"))
    diffs: list[str] = []
    cp, pp = cur.get("passes", {}), prev.get("passes", {})
    for name in sorted(set(cp) | set(pp)):
        if name not in pp:
            diffs.append(f"pass APPEARED: {name}")
            continue
        if name not in cp:
            diffs.append(f"pass VANISHED: {name}")
            continue
        for field in ("fbo", "viewport", "drawbuffers", "order_in_frame"):
            cv = cp[name].get(field, {}).get("values")
            pv = pp[name].get(field, {}).get("values")
            if cv != pv:
                diffs.append(f"{name}: {field} {pv} -> {cv}")
    return diffs


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("target", help="artifact dir or a single .log file")
    ap.add_argument("--out-dir", default=None,
                    help="report output dir (default: the target dir)")
    ap.add_argument("--compare", default=None,
                    help="prev render_pass_report.json to diff against (WHERE changed)")
    args = ap.parse_args()

    target = Path(args.target)
    if not target.exists():
        print(f"[render-pass-report] ERROR: not found: {target}", file=sys.stderr)
        return 6

    records, sources = collect(target)
    report = build_report(target, records, sources)
    out_dir = Path(args.out_dir) if args.out_dir else (
        target if target.is_dir() else target.parent)
    csv_path, json_path = write_outputs(report, records, out_dir)

    print(f"[render-pass-report] records={len(records)} passes={len(report['passes'])} "
          f"sources={len(sources)}")
    if not records:
        print(f"[render-pass-report] NOTE: {report['telemetry_note']}")
    else:
        for name, info in report["passes"].items():
            flags = [f for f in ("fbo", "viewport", "drawbuffers", "order_in_frame")
                     if not info[f]["stable"]]
            flag = (" CHANGED:" + ",".join(flags)) if flags else ""
            print(f"  {name:16} recs={info['records']:3} frames={len(info['frames'])}"
                  f" fbo={info['fbo']['values']} db={info['drawbuffers']['values']}"
                  f" order={info['order_in_frame']['values']}{flag}")
    print(f"[render-pass-report] json: {json_path}")
    print(f"[render-pass-report] csv:  {csv_path}")

    if args.compare:
        prev = Path(args.compare)
        if not prev.exists():
            print(f"[render-pass-report] --compare not found: {prev}", file=sys.stderr)
            return 6
        diffs = compare(report, prev)
        print(f"\n[render-pass-report] === COMPARE vs {prev} ===")
        if not diffs:
            print("  no per-pass differences (fbo/viewport/drawbuffers/order)")
        else:
            for d in diffs:
                print(f"  {d}")
            return 3
    return 0


if __name__ == "__main__":
    sys.exit(main())
