#!/usr/bin/env python3
# scripts/oracle_report.py
"""Turn smoke logs into a comparable render-oracle + perf-budget report.

Reads per-mission smoke stdout/stderr logs, parses the render oracles
(oracleparse) and the [PERF v1] line (logparse), judges them against the
perf budget (docs/perf-budget.md), and emits a markdown + JSON report.

This is the parser/budget harness that makes future runs comparable to
Baseline A (82add3ca). It does NOT modify the live tier1 fault gate.

Usage:
  py -3 scripts/oracle_report.py --logs DIR [--glob "*.out.log"] \
        [--budget docs/perf-budget.json] [--json OUT.json] [--md OUT.md] [--strict]

  --logs DIR    directory holding <mission>.out.log / <mission>.err.log pairs
                (mission stem = filename up to the first '.')
  --strict      exit 1 if any mission fails oracle asserts or breaks budget
  --budget      JSON of per-mission {avg_fps_floor, p99_ms_ceil, p1low_fps_floor};
                omit to skip perf-budget checks (oracle asserts still run)

Exit 0 = all clean (and within budget if --strict). Nonzero = regression.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))

from smoke_lib.logparse import parse_log               # noqa: E402
from smoke_lib.oracleparse import parse_oracles, judge_oracles  # noqa: E402


def discover(logs: Path, glob: str) -> dict[str, list[Path]]:
    """Map mission stem -> [log paths]. Pairs .out.log + .err.log by stem."""
    missions: dict[str, list[Path]] = {}
    for p in sorted(logs.glob(glob)) + sorted(logs.glob(glob.replace("out", "err"))):
        stem = p.name.split(".")[0]
        missions.setdefault(stem, []).append(p)
    return missions


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--logs", required=True, type=Path)
    ap.add_argument("--glob", default="*.out.log")
    ap.add_argument("--budget", type=Path)
    ap.add_argument("--json", type=Path)
    ap.add_argument("--md", type=Path)
    ap.add_argument("--strict", action="store_true")
    args = ap.parse_args()

    budget = json.loads(args.budget.read_text()) if args.budget else {}
    missions = discover(args.logs, args.glob)
    if not missions:
        print(f"no logs matched {args.glob} in {args.logs}", file=sys.stderr)
        return 2

    rows = []
    any_fail = False
    for stem, paths in missions.items():
        text = "\n".join(p.read_text(encoding="utf-8", errors="replace") for p in paths)
        s = parse_log(text)
        orc = parse_oracles(text)
        verdict = judge_oracles(orc)

        # perf-budget check
        budget_fails = []
        b = budget.get(stem) or budget.get("_default") or {}
        p = s.perf
        if b.get("avg_fps_floor") and p.avg_fps and p.avg_fps < b["avg_fps_floor"]:
            budget_fails.append(f"avg_fps {p.avg_fps:.0f} < floor {b['avg_fps_floor']}")
        if b.get("p99_ms_ceil") and p.p99_ms and p.p99_ms > b["p99_ms_ceil"]:
            budget_fails.append(f"p99_ms {p.p99_ms:.1f} > ceil {b['p99_ms_ceil']}")
        if b.get("p1low_fps_floor") and p.p1low_fps and p.p1low_fps < b["p1low_fps_floor"]:
            budget_fails.append(f"p1low_fps {p.p1low_fps:.0f} < floor {b['p1low_fps_floor']}")

        failed = bool(verdict.fails or budget_fails)
        any_fail = any_fail or failed
        rows.append(dict(stem=stem, perf=p, orc=orc, verdict=verdict,
                         budget_fails=budget_fails, failed=failed))

    md = render_md(rows)
    print(md)
    if args.md:
        args.md.write_text(md, encoding="utf-8")
    if args.json:
        args.json.write_text(json.dumps(
            [{"stem": r["stem"], "failed": r["failed"],
              "perf": vars(r["perf"]), "oracle": vars(r["orc"]),
              "oracle_fails": r["verdict"].fails, "oracle_warns": r["verdict"].warns,
              "budget_fails": r["budget_fails"]} for r in rows],
            indent=2), encoding="utf-8")

    return 1 if (args.strict and any_fail) else 0


def render_md(rows) -> str:
    out = ["# Oracle + perf-budget report\n"]
    out.append("| Mission | avg_fps | p50_ms | p99_ms | p1low | peak_ms | "
               "terrain | parity | slim | fastpath | rsnap_fb | tex_mis | mech_mis | objb_cpu_fb | verdict |")
    out.append("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for r in rows:
        p, o, v = r["perf"], r["orc"], r["verdict"]
        status = "FAIL" if r["failed"] else ("WARN" if v.warns else "clean")
        out.append(
            f"| {r['stem']} | {p.avg_fps:.0f} | {p.p50_ms:.1f} | {p.p99_ms:.1f} | "
            f"{p.p1low_fps:.0f} | {p.peak_ms:.0f} | {o.terrain_arm_path} | "
            f"{o.parity_match}/{o.parity_mismatch} | {o.slim_verts_last} | "
            f"{o.fastpath_drops} | {o.rsnap_fallback} | {o.tex_mismatches} | "
            f"{o.mech_mismatches} | {o.objb_cpu_fallback}({o.objb_late_register_skips}lr) | {status} |")
    out.append("")
    for r in rows:
        if r["verdict"].fails or r["budget_fails"] or r["verdict"].warns:
            out.append(f"**{r['stem']}**")
            for f in r["verdict"].fails:   out.append(f"  - FAIL (oracle): {f}")
            for f in r["budget_fails"]:    out.append(f"  - FAIL (budget): {f}")
            for w in r["verdict"].warns:   out.append(f"  - warn: {w}")
    return "\n".join(out) + "\n"


if __name__ == "__main__":
    raise SystemExit(main())
