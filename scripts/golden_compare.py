#!/usr/bin/env python3
"""golden_compare.py -- GOLDEN-SCENE-NOISE-FLOOR-1 noise-floor + compare layer.

Extends GOLDEN-SCENE-MANIFEST-1 (scripts/golden_scene.py, which writes one
golden_manifest.json per capture). This tool answers the two questions a
rigorous OFF-vs-ON (or GL-vs-future-backend) parity check needs:

  1. NOISE FLOOR: across N captures of the SAME scene with the SAME gate-set,
     which manifest fields are EXACT-stable (must match on any parity run) vs
     which legitimately DRIFT run-to-run (and by how much)? The manifest
     already told us empirically that registry_hash / pixel_hash@frame1 /
     pass_counters are stable while the shutdown frame-count and epoch drift;
     this formalizes that into a per-field tolerance file (noise_floor.json).

  2. COMPARE: given two manifests A and B (typically an OFF baseline and an ON
     candidate), diff every leaf field and classify each diff as
     WITHIN-NOISE-FLOOR (ok) or BEYOND (fail). Exit 0 iff every diff is within
     the floor; nonzero + the offending fields otherwise. This is the gate a
     future OFF-vs-ON slice runs to *prove* parity instead of eyeballing.

Acceptance semantics: an OFF-vs-OFF compare MUST be all-within-floor. Fields
marked exact (pixel_hash at a fixed frame, registry_hash, exe_md5, pass
counters) tolerate ZERO change; a pixel_hash mismatch prints a clear
"PIXEL CHANGED at frame F" hint because that is the load-bearing visual proof.

Field classification is structural, not hand-listed, so new manifest fields are
handled automatically:

  * Fields whose observed value NEVER changed across the N noise-floor captures
    -> EXACT (tolerance 0). pixel_hash, registry_hash, exe_md5, gate_set,
      pass_counters normally land here.
  * Numeric fields that DID change -> DRIFT with the observed [min,max] range;
    a compare is within-floor iff both values fall in [min - pad, max + pad].
  * Non-numeric fields that changed (e.g. generated_at_epoch is numeric, but a
    path/string that varied) -> DRIFT/IGNORED: recorded as volatile, never a
    fail on its own.
  * A small IGNORE set (timestamps, absolute source paths) is always volatile
    regardless of whether it happened to be constant in the sample.

Pure stdlib. Usage:
  # build the floor from >=2 captures of the same OFF scene:
  golden_compare.py noise-floor a.json b.json c.json --out noise_floor.json
  # compare two captures against that floor:
  golden_compare.py compare A.json B.json --noise-floor noise_floor.json
"""
import argparse
import json
import sys
from pathlib import Path

# Leaf paths (dotted, with [] for list elements) that are ALWAYS volatile and
# must never cause a BEYOND classification, regardless of the sampled floor.
# These are wall-clock / absolute-path / run-identity fields, not render state.
IGNORE_SUFFIXES = (
    "generated_at_epoch",
    "dump_source",
    "tga_source",
    "exe",  # absolute path string; exe_md5 is the load-bearing identity
)

# Numeric drift fields get a small symmetric pad on the observed range so a
# value just outside the sampled envelope (e.g. one extra shutdown frame) is
# still within-floor. Pad is fraction-of-range with an absolute floor.
DRIFT_PAD_FRAC = 0.5
DRIFT_PAD_ABS = 2.0


def _is_ignored(path: str) -> bool:
    return any(path == s or path.endswith("." + s) for s in IGNORE_SUFFIXES)


def flatten(obj, prefix=""):
    """Flatten a manifest dict into {dotted_path: leaf_value}. Lists index as
    path[i]; scalars/None are leaves. Deterministic key order."""
    out = {}
    if isinstance(obj, dict):
        for k in sorted(obj.keys()):
            out.update(flatten(obj[k], "%s.%s" % (prefix, k) if prefix else str(k)))
    elif isinstance(obj, list):
        for i, v in enumerate(obj):
            out.update(flatten(v, "%s[%d]" % (prefix, i)))
    else:
        out[prefix] = obj
    return out


def _is_num(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def build_noise_floor(manifest_paths):
    """From >=2 manifests of the same scene/gate-set, emit a per-field floor:
      { field: {"kind": "exact", "value": v}
              | {"kind": "drift", "min": lo, "max": hi}
              | {"kind": "ignore"} }
    plus a fields-seen roster so compare can flag missing/new fields."""
    flats = [flatten(json.loads(Path(p).read_text(encoding="utf-8"))) for p in manifest_paths]
    all_keys = set()
    for f in flats:
        all_keys |= set(f.keys())

    floor = {}
    for key in sorted(all_keys):
        if _is_ignored(key):
            floor[key] = {"kind": "ignore"}
            continue
        vals = [f.get(key, "__MISSING__") for f in flats]
        present = [v for v in vals if v != "__MISSING__"]
        # A field absent from some captures is treated as volatile.
        if len(present) != len(flats):
            floor[key] = {"kind": "ignore", "reason": "not-in-all-captures"}
            continue
        if all(v == present[0] for v in present):
            floor[key] = {"kind": "exact", "value": present[0]}
        elif all(_is_num(v) for v in present):
            floor[key] = {"kind": "drift", "min": min(present), "max": max(present)}
        else:
            floor[key] = {"kind": "ignore", "reason": "non-numeric-drift"}
    return {
        "schema": "GOLDEN_NOISE_FLOOR_V1",
        "sample_count": len(flats),
        "sample_sources": [str(p) for p in manifest_paths],
        "fields": floor,
    }


def _within_drift(rule, a, b):
    lo, hi = rule["min"], rule["max"]
    span = hi - lo
    pad = max(span * DRIFT_PAD_FRAC, DRIFT_PAD_ABS)
    lo_p, hi_p = lo - pad, hi + pad
    ok_a = _is_num(a) and lo_p <= a <= hi_p
    ok_b = _is_num(b) and lo_p <= b <= hi_p
    return ok_a and ok_b, (lo_p, hi_p)


def compare(a_path, b_path, floor):
    """Diff two manifests; classify each differing leaf WITHIN / BEYOND floor.
    Returns (beyond[], within[], notes[])."""
    fa = flatten(json.loads(Path(a_path).read_text(encoding="utf-8")))
    fb = flatten(json.loads(Path(b_path).read_text(encoding="utf-8")))
    rules = (floor or {}).get("fields", {})

    beyond, within, notes = [], [], []
    keys = sorted(set(fa) | set(fb))
    for key in keys:
        av = fa.get(key, "__MISSING__")
        bv = fb.get(key, "__MISSING__")
        if av == bv:
            continue
        if _is_ignored(key):
            within.append((key, av, bv, "ignored"))
            continue
        rule = rules.get(key)
        # Structural mismatch (field only on one side) is always BEYOND unless
        # the floor explicitly ignores it.
        if av == "__MISSING__" or bv == "__MISSING__":
            if rule and rule.get("kind") == "ignore":
                within.append((key, av, bv, "ignored/missing"))
            else:
                beyond.append((key, av, bv, "field-present-on-one-side"))
            continue
        if rule is None:
            # No floor knowledge: differing field with no tolerance -> BEYOND
            # (conservative; a field the floor never saw changing is suspect).
            beyond.append((key, av, bv, "no-floor-rule"))
            continue
        kind = rule.get("kind")
        if kind == "ignore":
            within.append((key, av, bv, rule.get("reason", "ignored")))
        elif kind == "exact":
            beyond.append((key, av, bv, "exact-field-changed"))
        elif kind == "drift":
            ok, (lo_p, hi_p) = _within_drift(rule, av, bv)
            if ok:
                within.append((key, av, bv, "drift[%g,%g]" % (lo_p, hi_p)))
            else:
                beyond.append((key, av, bv, "drift-out-of-range[%g,%g]" % (lo_p, hi_p)))
        else:
            beyond.append((key, av, bv, "unknown-rule-kind"))
    return beyond, within, notes


def _pixel_frame(manifest_path):
    try:
        m = json.loads(Path(manifest_path).read_text(encoding="utf-8"))
        return m.get("captured_frame_request", m.get("frame", "?"))
    except Exception:
        return "?"


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    nf = sub.add_parser("noise-floor",
                        help="build a per-field noise floor from N same-scene manifests")
    nf.add_argument("manifests", nargs="+", help="manifest JSONs (>=2) of the SAME OFF scene")
    nf.add_argument("--out", default="noise_floor.json", help="output floor path")

    cp = sub.add_parser("compare", help="compare two manifests against a noise floor")
    cp.add_argument("a", help="manifest A (baseline)")
    cp.add_argument("b", help="manifest B (candidate)")
    cp.add_argument("--noise-floor", default=None,
                    help="noise_floor.json; without it every diff on a "
                         "non-ignored field is BEYOND")

    args = ap.parse_args()

    if args.cmd == "noise-floor":
        if len(args.manifests) < 2:
            print("[golden_compare] ERROR: need >=2 manifests to establish a floor",
                  file=sys.stderr)
            return 2
        floor = build_noise_floor(args.manifests)
        Path(args.out).write_text(json.dumps(floor, indent=2, sort_keys=True) + "\n",
                                  encoding="utf-8")
        fields = floor["fields"]
        n_exact = sum(1 for r in fields.values() if r["kind"] == "exact")
        n_drift = sum(1 for r in fields.values() if r["kind"] == "drift")
        n_ign = sum(1 for r in fields.values() if r["kind"] == "ignore")
        print("[golden_compare] wrote %s (%d captures)" % (args.out, floor["sample_count"]))
        print("[golden_compare] fields: %d exact-stable, %d drift, %d ignored"
              % (n_exact, n_drift, n_ign))
        for key, r in sorted(fields.items()):
            if r["kind"] == "drift":
                print("  DRIFT  %s  range=[%s,%s]" % (key, r["min"], r["max"]))
        # A couple of load-bearing exact fields, surfaced for the operator.
        for k in ("pixel_hash", "registry_hash", "exe_md5"):
            if k in fields and fields[k]["kind"] == "exact":
                print("  EXACT  %s = %s" % (k, fields[k]["value"]))
        return 0

    if args.cmd == "compare":
        floor = None
        if args.noise_floor:
            floor = json.loads(Path(args.noise_floor).read_text(encoding="utf-8"))
        beyond, within, _ = compare(args.a, args.b, floor)

        print("[golden_compare] A=%s" % args.a)
        print("[golden_compare] B=%s" % args.b)
        print("[golden_compare] within-floor diffs: %d   beyond-floor diffs: %d"
              % (len(within), len(beyond)))
        for key, av, bv, why in within:
            print("  ok    %-48s %r -> %r  (%s)" % (key, av, bv, why))
        for key, av, bv, why in beyond:
            print("  FAIL  %-48s %r -> %r  (%s)" % (key, av, bv, why))
            if key.endswith("pixel_hash") or key == "pixel_hash":
                print("        >>> PIXEL CHANGED at frame %s: the deterministic "
                      "fixed-frame screenshot differs -- visual regression."
                      % _pixel_frame(args.a))

        if beyond:
            print("[golden_compare] RESULT: BEYOND NOISE FLOOR (%d offending field(s)): %s"
                  % (len(beyond), ", ".join(k for k, *_ in beyond)))
            return 1
        print("[golden_compare] RESULT: WITHIN NOISE FLOOR (parity holds)")
        return 0


if __name__ == "__main__":
    sys.exit(main())
