#!/usr/bin/env python3
"""visual_gallery.py -- registry-aware visual-regression gallery (S13+ polish).

Walks the golden registry (tests/visual/golden-sets.json), and for every set of
a chosen status (default: blessed) renders a self-contained HTML review surface:

  * golden-only  -- if no fresh capture exists, show the blessed golden frames
                    (a browseable record of what "correct" looks like).
  * triptych     -- if a capture exists under the compare dir, pair golden vs
                    candidate and show golden / candidate / amplified heatmap
                    with the visual_diff layered verdict (PASS/WARN/FAIL).

Reuses scripts/visual_diff.py for compare + heatmap + thumbnail machinery (no
duplicate diff logic). One <details> section per set; sets sorted worst-first.

Usage:
  py -3 scripts/visual_gallery.py                       # blessed sets -> default out
  py -3 scripts/visual_gallery.py --status all
  py -3 scripts/visual_gallery.py --compare-dir tests/visual/compare --out x.html

Exit codes: 0 generated OK, 3 bad env (no registry / no Pillow), 4 IO error.
(The gallery is a REVIEW surface, not a gate -- it never fails on WARN/FAIL
verdicts; use run_visual.py compare for the pass/fail gate.)

Python 3 + Pillow (via visual_diff). No emoji.
"""
from __future__ import annotations

import argparse
import html
import json
import os
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent
sys.path.insert(0, str(SCRIPT_DIR))

import visual_diff as vd  # noqa: E402  (path inserted above)

DEFAULT_REGISTRY = REPO_ROOT / "tests" / "visual" / "golden-sets.json"
DEFAULT_BASELINES = REPO_ROOT / "tests" / "visual" / "baselines"
DEFAULT_COMPARE = REPO_ROOT / "tests" / "visual" / "compare"
DEFAULT_OUT = REPO_ROOT / "tests" / "visual" / "gallery" / "index.html"

_BADGE = {"PASS": "#2a9d2a", "WARN": "#d9a400", "FAIL": "#d03030", "GOLDEN": "#3a6ea5"}
_VORDER = {"FAIL": 0, "WARN": 1, "PASS": 2, "GOLDEN": 3, None: 4}


def log(msg: str) -> None:
    print(f"[gallery] {msg}", file=sys.stderr, flush=True)


def _esc(s) -> str:
    return html.escape(str(s))


def _list_pngs(d: Path):
    if not d.is_dir():
        return {}
    return {p.stem: p for p in sorted(d.glob("*.png"))}


def _badge_html(verdict: str) -> str:
    return (f'<span style="background:{_BADGE.get(verdict, "#888")};color:#fff;'
            f'padding:2px 8px;border-radius:4px;font-weight:bold">{_esc(verdict)}</span>')


def _frame_card(name: str, golden: Path, candidate, rec) -> str:
    """One frame card: triptych if candidate present, else golden-only."""
    verdict = rec["verdict"] if rec else "GOLDEN"
    meta = ""
    if rec:
        meta = (f'layer={_esc(rec["layer_decided"])} '
                f'max_delta={_esc(rec["max_delta"])} '
                f'changed_pct={_esc(rec["changed_pct"])}%')
        if rec.get("reason"):
            meta += " &mdash; " + _esc(rec["reason"])
    suspect = ""
    if rec and rec.get("suspect"):
        suspect = ('  <span style="background:#7b3fb3;color:#fff;padding:2px 8px;'
                   'border-radius:4px">SUSPECT</span>')

    cells = []
    if vd.HAVE_PIL:
        try:
            if golden is None:
                # Orphan candidate (no blessed golden): show the candidate alone,
                # labelled honestly -- never display it in the golden slot.
                triptych = (("candidate (no golden)", vd._load(str(candidate))),)
            else:
                ga = vd._load(str(golden))
                if candidate and rec and not rec.get("reason"):
                    cb = vd._load(str(candidate))
                    if ga.size == cb.size:
                        hm = vd._heatmap(ga, cb)
                        triptych = (("golden", ga), ("candidate", cb),
                                    ("diff x amplified", hm))
                    else:
                        triptych = (("golden", ga), ("candidate", cb))  # size mismatch
                else:
                    triptych = (("golden", ga),)
            for label, im in triptych:
                cells.append(
                    '<td style="text-align:center;padding:4px;vertical-align:top">'
                    f'<img src="data:image/png;base64,{vd._thumb_b64(im)}" '
                    f'style="max-width:{vd.THUMB_MAX}px;border:1px solid #333">'
                    f'<div style="color:#999;font-size:12px">{_esc(label)}</div></td>')
        except Exception as exc:  # noqa: BLE001
            cells.append(f'<td style="color:#999">(thumbnail failed: {_esc(exc)})</td>')
    else:
        cells.append('<td style="color:#999">(Pillow not installed -- no thumbnails)</td>')

    return (
        '<div style="border:1px solid #383838;border-radius:6px;padding:10px;'
        'margin:10px 0;background:#1c1c1c">'
        f'<div style="margin-bottom:6px">{_badge_html(verdict)}{suspect} '
        f'<b>{_esc(name)}</b> '
        f'<span style="color:#aaa;font-size:13px">{meta}</span></div>'
        f'<table><tr>{"".join(cells)}</tr></table></div>')


def build_set_section(set_id: str, entry: dict, baselines_dir: Path, compare_dir: Path):
    """Return (html, rollup, worst_verdict) for one registry set."""
    golden_dir = baselines_dir / set_id
    cand_dir = compare_dir / set_id
    goldens = _list_pngs(golden_dir)
    cands = _list_pngs(cand_dir)
    rollup = {"pass": 0, "warn": 0, "fail": 0, "golden": 0}
    worst = "GOLDEN"
    cards = []

    for stem in sorted(set(goldens) | set(cands)):
        g = goldens.get(stem)
        c = cands.get(stem)
        if g is None:
            # candidate with no golden -> unpaired, flag as FAIL card
            rec = {"verdict": "FAIL", "layer_decided": None, "max_delta": None,
                   "changed_pct": None, "suspect": False, "suspect_reasons": [],
                   "reason": "candidate has no blessed golden"}
            cards.append((rec["verdict"], _frame_card(stem, None, c, rec)))
            rollup["fail"] += 1
            worst = min(worst, "FAIL", key=lambda v: _VORDER.get(v, 4))
            continue
        if c is None:
            cards.append(("GOLDEN", _frame_card(stem, g, None, None)))
            rollup["golden"] += 1
            continue
        rec = vd.compare_pair(str(g), str(c))
        v = rec["verdict"]
        rollup[v.lower()] = rollup.get(v.lower(), 0) + 1
        worst = min(worst, v, key=lambda x: _VORDER.get(x, 4))
        cards.append((v, _frame_card(stem, g, c, rec)))

    cards.sort(key=lambda t: _VORDER.get(t[0], 4))
    has_cands = bool(cands)
    summary = (f'goldens={len(goldens)} captures={len(cands)} | '
               f'PASS {rollup["pass"]} / WARN {rollup["warn"]} / FAIL {rollup["fail"]}'
               + (f' / golden-only {rollup["golden"]}' if rollup["golden"] else ''))
    status = _esc(entry.get("status", "?"))
    blessed_commit = _esc(entry.get("blessed_commit", ""))[:12]
    header = (f'{_badge_html(worst)} <b>{_esc(set_id)}</b> '
              f'<span style="color:#888;font-size:13px">[{status}] '
              f'{blessed_commit} &mdash; {summary}</span>')
    body = "".join(c for _v, c in cards) or '<div style="color:#999">(no frames found)</div>'
    section = (f'<details {"open" if (has_cands and worst in ("FAIL", "WARN")) else ""}>'
               f'<summary style="cursor:pointer;font-size:16px;padding:8px 0">{header}</summary>'
               f'{body}</details>')
    return section, rollup, worst


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Registry-aware visual-regression gallery.")
    ap.add_argument("--registry", default=str(DEFAULT_REGISTRY))
    ap.add_argument("--baselines-dir", default=str(DEFAULT_BASELINES))
    ap.add_argument("--compare-dir", default=str(DEFAULT_COMPARE))
    ap.add_argument("--out", default=str(DEFAULT_OUT))
    ap.add_argument("--status", default="blessed",
                    help="registry status filter, or 'all' (default: blessed)")
    args = ap.parse_args(argv)

    registry = Path(args.registry)
    if not registry.is_file():
        log(f"FATAL: registry not found: {registry}")
        return 3
    if not vd.HAVE_PIL:
        log("WARN: Pillow not installed -- gallery will render without thumbnails")

    try:
        reg = json.loads(registry.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        log(f"FATAL: cannot read registry: {exc}")
        return 4

    sets = reg.get("sets", {})
    want = None if args.status == "all" else args.status
    chosen = {sid: e for sid, e in sets.items()
              if want is None or e.get("status") == want}
    if not chosen:
        log(f"no sets matched status={args.status} (registry has {len(sets)} sets)")

    baselines_dir = Path(args.baselines_dir)
    compare_dir = Path(args.compare_dir)
    sections = []
    total = {"pass": 0, "warn": 0, "fail": 0, "golden": 0}
    set_verdicts = []
    for sid in sorted(chosen):
        sec, rollup, worst = build_set_section(sid, chosen[sid], baselines_dir, compare_dir)
        for k in total:
            total[k] += rollup.get(k, 0)
        set_verdicts.append((worst, sid, sec))
        log(f"{sid}: worst={worst} "
            f"pass={rollup['pass']} warn={rollup['warn']} fail={rollup['fail']} "
            f"golden-only={rollup['golden']}")

    set_verdicts.sort(key=lambda t: _VORDER.get(t[0], 4))
    overall = set_verdicts[0][0] if set_verdicts else "GOLDEN"

    head = (
        '<!DOCTYPE html><html><head><meta charset="utf-8">'
        '<title>MC2 visual-regression gallery</title></head>'
        '<body style="font-family:Segoe UI,sans-serif;background:#121212;color:#ddd;'
        'max-width:1320px;margin:0 auto;padding:16px">'
        f'<h2>MC2 visual-regression gallery {_badge_html(overall)}</h2>'
        f'<div style="color:#888;font-size:13px">status filter: {_esc(args.status)} '
        f'&mdash; {len(chosen)} sets &mdash; frames: PASS {total["pass"]} / '
        f'WARN {total["warn"]} / FAIL {total["fail"]} / golden-only {total["golden"]}<br>'
        'golden-only = no fresh capture yet (run run_visual.py compare to populate). '
        'This is a review surface; the pass/fail gate is run_visual.py compare.</div><hr>')
    body = "".join(sec for _v, _s, sec in set_verdicts)
    doc = head + body + '</body></html>'

    out = Path(args.out)
    try:
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(doc, encoding="utf-8")
    except OSError as exc:
        log(f"FATAL: cannot write gallery: {exc}")
        return 4

    log(f"gallery written: {out}  (overall={overall}, {len(chosen)} sets, "
        f"{out.stat().st_size} bytes)")
    print(str(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
