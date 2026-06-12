#!/usr/bin/env python3
"""MC2 visual-regression comparator -- layered diff + cockpit artifact.

Compares a CANDIDATE capture dir against a BASELINE dir (default = the blessed
Baseline A goldens). For each bookmark PNG matched by filename it runs LAYERED
checks, short-circuiting on the cheap ones:

  LAYER 0  identity/provenance   -- sidecar gate_env / seed / resolution /
                                    mission / trigger_frame mismatch, or
                                    candidate deterministic:false -> SUSPECT.
                                    A pixel diff across different capture
                                    conditions is meaningless, so we bail first.
  LAYER 1  exact hash            -- equal PNG sha256 -> MATCH (0 changed px).
  LAYER 2  pixel diff            -- decode both, defilter, compare RGB.

Verdict per bookmark: MATCH / FLIP / SUSPECT. Overall exit = max severity:
  0 = all MATCH, 1 = at least one FLIP (advisory, "needs human eyes" per the
  visual-regression-lab owner doc -- NOT a hard fail), 2 = SUSPECT / error.

Visual verdicts are ADVISORY: FLIP is exit 1 so a render-branch author sees it
but is not blocked. Promotion to a hard gate is a later slice.

Python 3 stdlib ONLY (zlib, hashlib, json, base64, struct via png_io). No engine
code, no smoke / gates.py coupling.
"""

import argparse
import hashlib
import json
import os
import sys

# Allow `import png_io` / `import triptych` when run as a script from anywhere.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from png_io import decode_png  # noqa: E402
from triptych import build_heatmap, write_triptych  # noqa: E402

SCHEMA = "mc2.visual_diff/1"

# ---- Layer-2 thresholds (named + documented) -------------------------------
# Per-channel epsilon: deltas <= this many LSB are treated as noise/dither and
# do NOT count as a changed pixel. Capture is byte-deterministic (Baseline A
# proved a second capture was byte-identical), so this is a small safety margin
# for any future non-pinned dither, not an expected-drift budget.
EPSILON_LSB = 2
# A bookmark is MATCH only if EVERY pixel is within EPSILON on every channel
# (i.e. changed_pct == 0 under that epsilon). CHANGED_PCT_WARN documents the
# advisory threshold surfaced in the report; any changed pixel already -> FLIP.
CHANGED_PCT_WARN = 0.1  # percent

# Identity fields that must match between baseline and candidate sidecars.
IDENTITY_FIELDS = ("mission", "seed", "trigger_frame", "width", "height")

VERDICT_EXIT = {"MATCH": 0, "FLIP": 1, "SUSPECT": 2}

DEFAULT_BASELINE = os.path.normpath(
    os.path.join(
        os.path.dirname(os.path.abspath(__file__)),
        "..",
        "..",
        ".claude",
        "baseline-A",
        "gold",
    )
)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def load_sidecar(png_path):
    side = os.path.splitext(png_path)[0] + ".json"
    if not os.path.exists(side):
        return None
    with open(side, "r", encoding="utf-8") as f:
        return json.load(f)


def layer0_identity(base_side, cand_side):
    """Return a mismatch reason string, or None if provenance is compatible."""
    if cand_side is None:
        return "candidate sidecar missing (cannot verify capture conditions)"
    if base_side is None:
        return "baseline sidecar missing (cannot verify capture conditions)"
    if cand_side.get("deterministic") is not True:
        return "candidate deterministic=%r (non-deterministic capture)" % cand_side.get(
            "deterministic"
        )
    for fld in IDENTITY_FIELDS:
        if base_side.get(fld) != cand_side.get(fld):
            return "%s mismatch: baseline=%r candidate=%r" % (
                fld,
                base_side.get(fld),
                cand_side.get(fld),
            )
    bg = base_side.get("gate_env")
    cg = cand_side.get("gate_env")
    if bg is not None or cg is not None:
        bset = set(bg or [])
        cset = set(cg or [])
        if bset != cset:
            missing = sorted(bset - cset)
            extra = sorted(cset - bset)
            return "gate_env mismatch: missing=%r extra=%r" % (missing, extra)
    return None


def pixel_diff(base_png_bytes, cand_png_bytes):
    """Layer 2. Returns dict with changed_px, changed_pct, max_delta, mean_delta
    and the decoded RGB buffers (for the heatmap)."""
    bw, bh, bc, bp = decode_png(base_png_bytes)
    cw, ch, cc, cp = decode_png(cand_png_bytes)
    if (bw, bh) != (cw, ch):
        raise ValueError(
            "dimension mismatch baseline=%dx%d candidate=%dx%d" % (bw, bh, cw, ch)
        )
    # Normalize both to RGB for comparison/heatmap.
    base_rgb = _to_rgb(bp, bc, bw, bh)
    cand_rgb = _to_rgb(cp, cc, cw, ch)
    n = bw * bh
    changed = 0
    max_delta = 0
    total = 0
    for p in range(n):
        i = p * 3
        d0 = abs(base_rgb[i] - cand_rgb[i])
        d1 = abs(base_rgb[i + 1] - cand_rgb[i + 1])
        d2 = abs(base_rgb[i + 2] - cand_rgb[i + 2])
        m = d0 if d0 >= d1 else d1
        if d2 > m:
            m = d2
        if m > max_delta:
            max_delta = m
        total += d0 + d1 + d2
        if m > EPSILON_LSB:
            changed += 1
    return {
        "width": bw,
        "height": bh,
        "changed_px": changed,
        "changed_pct": (100.0 * changed / n) if n else 0.0,
        "max_delta": max_delta,
        "mean_delta": (total / (n * 3.0)) if n else 0.0,
        "base_rgb": base_rgb,
        "cand_rgb": cand_rgb,
    }


def _to_rgb(pix, channels, w, h):
    if channels == 3:
        return pix
    out = bytearray(w * h * 3)
    for p in range(w * h):
        out[p * 3] = pix[p * 4]
        out[p * 3 + 1] = pix[p * 4 + 1]
        out[p * 3 + 2] = pix[p * 4 + 2]
    return out


def compare_bookmark(name, base_png, cand_png):
    """Run the layered checks for one bookmark. Returns (report_entry, heat_ctx)
    where heat_ctx holds decoded RGB buffers for the triptych (or None)."""
    entry = {
        "name": name,
        "verdict": None,
        "changed_px": None,
        "changed_pct": None,
        "max_delta": None,
        "mean_delta": None,
        "layer_decided": None,
        "mismatch_reason": None,
    }

    # LAYER 0 -- provenance
    base_side = load_sidecar(base_png)
    cand_side = load_sidecar(cand_png)
    reason = layer0_identity(base_side, cand_side)
    if reason is not None:
        entry["verdict"] = "SUSPECT"
        entry["layer_decided"] = 0
        entry["mismatch_reason"] = reason
        return entry, None

    base_bytes = open(base_png, "rb").read()
    cand_bytes = open(cand_png, "rb").read()

    # LAYER 1 -- exact hash
    if hashlib.sha256(base_bytes).digest() == hashlib.sha256(cand_bytes).digest():
        entry["verdict"] = "MATCH"
        entry["changed_px"] = 0
        entry["changed_pct"] = 0.0
        entry["max_delta"] = 0
        entry["mean_delta"] = 0.0
        entry["layer_decided"] = 1
        return entry, None

    # LAYER 2 -- pixel diff
    try:
        d = pixel_diff(base_bytes, cand_bytes)
    except Exception as exc:  # decode / dimension errors -> SUSPECT
        entry["verdict"] = "SUSPECT"
        entry["layer_decided"] = 2
        entry["mismatch_reason"] = "pixel decode failed: %s" % exc
        return entry, None

    entry["changed_px"] = d["changed_px"]
    entry["changed_pct"] = round(d["changed_pct"], 6)
    entry["max_delta"] = d["max_delta"]
    entry["mean_delta"] = round(d["mean_delta"], 6)
    entry["layer_decided"] = 2
    if d["changed_px"] == 0 and d["max_delta"] <= EPSILON_LSB:
        entry["verdict"] = "MATCH"
    else:
        entry["verdict"] = "FLIP"
        entry["mismatch_reason"] = "%d px changed (%.4f%%), max_delta=%d" % (
            d["changed_px"],
            d["changed_pct"],
            d["max_delta"],
        )
    heat_ctx = {
        "width": d["width"],
        "height": d["height"],
        "base_rgb": d["base_rgb"],
        "cand_rgb": d["cand_rgb"],
    }
    return entry, heat_ctx


def find_bookmarks(baseline_dir, candidate_dir):
    """Match by filename: PNGs present in BOTH dirs."""
    def pngs(d):
        return {
            f
            for f in os.listdir(d)
            if f.lower().endswith(".png") and os.path.isfile(os.path.join(d, f))
        }

    base = pngs(baseline_dir)
    cand = pngs(candidate_dir)
    return sorted(base & cand), sorted(base - cand), sorted(cand - base)


def run(baseline_dir, candidate_dir, json_out, html_out):
    matched, only_base, only_cand = find_bookmarks(baseline_dir, candidate_dir)

    bookmarks = []
    rows = []
    cand_commit = None
    base_commit = None
    for fn in matched:
        name = os.path.splitext(fn)[0]
        base_png = os.path.join(baseline_dir, fn)
        cand_png = os.path.join(candidate_dir, fn)
        entry, heat_ctx = compare_bookmark(name, base_png, cand_png)
        bookmarks.append(entry)

        cs = load_sidecar(cand_png)
        if cs and cand_commit is None:
            cand_commit = (cs.get("build") or {}).get("sha")
        bs = load_sidecar(base_png)
        if bs and base_commit is None:
            base_commit = (bs.get("build") or {}).get("sha")

        # Triptych row (heatmap only when we decoded pixels).
        row = {
            "name": name,
            "verdict": entry["verdict"],
            "detail": _row_detail(entry),
            "base_png": open(base_png, "rb").read(),
            "cand_png": open(cand_png, "rb").read(),
            "heat_png": None,
        }
        if heat_ctx is not None:
            row["heat_png"] = build_heatmap(
                heat_ctx["width"],
                heat_ctx["height"],
                heat_ctx["base_rgb"],
                heat_ctx["cand_rgb"],
            )
        rows.append(row)

    summary = {
        "match": sum(1 for b in bookmarks if b["verdict"] == "MATCH"),
        "flip": sum(1 for b in bookmarks if b["verdict"] == "FLIP"),
        "suspect": sum(1 for b in bookmarks if b["verdict"] == "SUSPECT"),
    }
    exit_code = 0
    for b in bookmarks:
        exit_code = max(exit_code, VERDICT_EXIT.get(b["verdict"], 2))
    if not matched:
        # Nothing to compare is itself suspect (wrong dir / empty candidate).
        exit_code = max(exit_code, 2)

    report = {
        "schema": SCHEMA,
        "baseline_dir": os.path.abspath(baseline_dir),
        "candidate_dir": os.path.abspath(candidate_dir),
        "baseline_commit": base_commit,
        "generated_for_commit": cand_commit,
        "bookmarks": bookmarks,
        "unmatched_baseline_only": only_base,
        "unmatched_candidate_only": only_cand,
        "summary": summary,
        "exit": exit_code,
    }

    if json_out:
        with open(json_out, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=2)
    if html_out:
        write_triptych(html_out, report, rows)

    return report


def _row_detail(entry):
    bits = ["layer %s" % entry["layer_decided"]]
    if entry["changed_px"] is not None:
        bits.append(
            "changed_px=%d (%.4f%%)" % (entry["changed_px"], entry["changed_pct"] or 0.0)
        )
        bits.append("max_delta=%s" % entry["max_delta"])
        bits.append("mean_delta=%.4f" % (entry["mean_delta"] or 0.0))
    if entry["mismatch_reason"]:
        bits.append(entry["mismatch_reason"])
    return "\n".join(bits)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument(
        "--candidate",
        required=True,
        help="candidate capture dir (PNGs + .json sidecars)",
    )
    ap.add_argument(
        "--baseline",
        default=DEFAULT_BASELINE,
        help="baseline dir (default: blessed Baseline A goldens)",
    )
    ap.add_argument("--json", default=None, help="write cockpit visual_diff.json here")
    ap.add_argument("--html", default=None, help="write self-contained triptych .html here")
    ap.add_argument(
        "--quiet", action="store_true", help="suppress the per-bookmark summary line"
    )
    args = ap.parse_args(argv)

    if not os.path.isdir(args.baseline):
        print("ERROR: baseline dir not found: %s" % args.baseline, file=sys.stderr)
        return 2
    if not os.path.isdir(args.candidate):
        print("ERROR: candidate dir not found: %s" % args.candidate, file=sys.stderr)
        return 2

    report = run(args.baseline, args.candidate, args.json, args.html)

    if not args.quiet:
        for b in report["bookmarks"]:
            print(
                "%-28s %-8s (layer %s) %s"
                % (
                    b["name"],
                    b["verdict"],
                    b["layer_decided"],
                    b["mismatch_reason"] or "",
                )
            )
        s = report["summary"]
        print(
            "summary: MATCH=%d FLIP=%d SUSPECT=%d  exit=%d"
            % (s["match"], s["flip"], s["suspect"], report["exit"])
        )
        if args.json:
            print("json:  %s" % os.path.abspath(args.json))
        if args.html:
            print("html:  %s" % os.path.abspath(args.html))

    return report["exit"]


if __name__ == "__main__":
    sys.exit(main())
