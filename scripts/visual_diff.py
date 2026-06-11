#!/usr/bin/env python3
"""visual_diff.py — layered image diff for MC2 visual regression (lab layers 1-3).

Layers (cheapest first, short-circuit):
  L1: byte-identical file -> PASS
  L2: per-pixel max channel delta <= 2 LSB -> PASS
  L3: changed-pixel ratio (delta > 2 LSB): <=0.05% PASS, 0.05-1% WARN, >1% FAIL

Usage:
  visual_diff.py <golden> <candidate> [--json out.json] [--html out.html]
  visual_diff.py --golden-dir X --candidate-dir Y [--json out.json] [--html out.html]
  visual_diff.py --self-test

Exit codes: 0 all-pass, 1 warns (no fails), 2 any fail.

Requires Pillow (pip install Pillow) for layers 2-3 and HTML thumbs.
TGA and PNG supported (Pillow decodes both). No FLIP / perceptual metrics
(later slice). No blessing model. Read-only on input artifacts.

Advisory pre-gate: if a sidecar state.json / *.state.json / env.json exists
next to an image, feature-gate/env fields are diffed; mismatches mark the
record SUSPECT (advisory only — never changes the verdict).
"""

import argparse
import base64
import hashlib
import io
import json
import os
import sys

EPSILON_LSB = 2          # L2: per-channel delta tolerance
PASS_PCT = 0.05          # L3: <= -> PASS
WARN_PCT = 1.0           # L3: <= -> WARN, > -> FAIL
THUMB_MAX = 384          # HTML thumbnail max dimension (px)

try:
    from PIL import Image, ImageChops
    HAVE_PIL = True
except ImportError:
    HAVE_PIL = False


def _sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _load(path):
    img = Image.open(path)
    return img.convert("RGB")


def _find_sidecar(img_path):
    """Look for a state/env sidecar next to the image."""
    d = os.path.dirname(os.path.abspath(img_path))
    stem = os.path.splitext(os.path.basename(img_path))[0]
    for cand in (os.path.join(d, stem + ".state.json"),
                 os.path.join(d, stem + ".env.json"),
                 os.path.join(d, "state.json"),
                 os.path.join(d, "env.json")):
        if os.path.isfile(cand):
            return cand
    return None


def _gate_fields(sidecar_path):
    """Extract feature-gate-ish fields from a sidecar JSON. Best-effort."""
    try:
        with open(sidecar_path, "r", encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, ValueError):
        return None
    out = {}
    if isinstance(data, dict):
        for key in ("feature_gates", "featureGates", "gates", "env"):
            v = data.get(key)
            if isinstance(v, dict):
                for k, val in v.items():
                    out["%s.%s" % (key, k)] = val
        # top-level MC2_* env-style keys
        for k, v in data.items():
            if isinstance(k, str) and k.startswith("MC2_"):
                out[k] = v
    return out or None


def _suspect_check(golden, candidate):
    """Advisory: diff sidecar gate/env fields. Returns (suspect, reasons)."""
    gs, cs = _find_sidecar(golden), _find_sidecar(candidate)
    if not gs or not cs:
        return False, []
    gf, cf = _gate_fields(gs), _gate_fields(cs)
    if gf is None or cf is None:
        return False, []
    reasons = []
    for k in sorted(set(gf) | set(cf)):
        gv, cv = gf.get(k, "<absent>"), cf.get(k, "<absent>")
        if gv != cv:
            reasons.append("%s: golden=%r candidate=%r" % (k, gv, cv))
    return bool(reasons), reasons


def compare_pair(golden, candidate):
    """Compare two image files. Returns a record dict (verdict etc.)."""
    rec = {
        "golden": os.path.abspath(golden),
        "candidate": os.path.abspath(candidate),
        "verdict": None,
        "layer_decided": None,
        "max_delta": None,
        "changed_pct": None,
        "suspect": False,
        "suspect_reasons": [],
        "reason": None,
    }

    for p, role in ((golden, "golden"), (candidate, "candidate")):
        if not os.path.isfile(p):
            rec["verdict"] = "FAIL"
            rec["reason"] = "missing %s file: %s" % (role, p)
            return rec

    rec["suspect"], rec["suspect_reasons"] = _suspect_check(golden, candidate)

    # L1: byte identity
    if _sha256(golden) == _sha256(candidate):
        rec["verdict"] = "PASS"
        rec["layer_decided"] = 1
        rec["max_delta"] = 0
        rec["changed_pct"] = 0.0
        return rec

    if not HAVE_PIL:
        rec["verdict"] = "FAIL"
        rec["reason"] = "Pillow not installed; cannot compare beyond byte identity (pip install Pillow)"
        return rec

    try:
        a = _load(golden)
        b = _load(candidate)
    except Exception as e:  # decode failure
        rec["verdict"] = "FAIL"
        rec["reason"] = "image decode failed: %s" % e
        return rec

    if a.size != b.size:
        rec["verdict"] = "FAIL"
        rec["reason"] = "resolution mismatch: golden %dx%d vs candidate %dx%d" % (
            a.size[0], a.size[1], b.size[0], b.size[1])
        return rec

    diff = ImageChops.difference(a, b)
    rec["max_delta"] = max(hi for (_lo, hi) in diff.getextrema())

    # L2: epsilon
    if rec["max_delta"] <= EPSILON_LSB:
        rec["verdict"] = "PASS"
        rec["layer_decided"] = 2
        rec["changed_pct"] = 0.0
        return rec

    # L3: changed-pixel ratio (per-pixel max-over-channels delta > epsilon)
    chans = diff.split()
    mask = chans[0].point(lambda v: 255 if v > EPSILON_LSB else 0)
    for c in chans[1:]:
        mask = ImageChops.lighter(mask, c.point(lambda v: 255 if v > EPSILON_LSB else 0))
    changed = mask.histogram()[255]
    total = a.size[0] * a.size[1]
    rec["changed_pct"] = round(100.0 * changed / total, 4)
    rec["layer_decided"] = 3
    if rec["changed_pct"] <= PASS_PCT:
        rec["verdict"] = "PASS"
    elif rec["changed_pct"] <= WARN_PCT:
        rec["verdict"] = "WARN"
    else:
        rec["verdict"] = "FAIL"
    return rec


def pair_dirs(golden_dir, candidate_dir):
    """Pair images by filename (exact, then stem across .tga/.png)."""
    exts = (".tga", ".png")
    def listing(d):
        return {f: os.path.join(d, f) for f in sorted(os.listdir(d))
                if f.lower().endswith(exts)}
    g, c = listing(golden_dir), listing(candidate_dir)
    g_stems = {os.path.splitext(f)[0]: p for f, p in g.items()}
    c_stems = {os.path.splitext(f)[0]: p for f, p in c.items()}
    pairs, missing = [], []
    for stem in sorted(set(g_stems) | set(c_stems)):
        gp, cp = g_stems.get(stem), c_stems.get(stem)
        if gp and cp:
            pairs.append((gp, cp))
        else:
            missing.append((stem, gp, cp))
    return pairs, missing


# ---------------- HTML triptych gallery ----------------

def _thumb_b64(img):
    t = img.copy()
    t.thumbnail((THUMB_MAX, THUMB_MAX))
    buf = io.BytesIO()
    t.save(buf, format="PNG")
    return base64.b64encode(buf.getvalue()).decode("ascii")


def _heatmap(a, b):
    """Amplified red-scale heatmap of per-pixel max channel delta."""
    diff = ImageChops.difference(a, b)
    chans = diff.split()
    m = chans[0]
    for c in chans[1:]:
        m = ImageChops.lighter(m, c)
    hi = max(m.getextrema()[1], 1)
    scale = 255.0 / hi
    m = m.point(lambda v: min(255, int(v * scale)))
    black = m.point(lambda v: 0)
    return Image.merge("RGB", (m, black, black))


_BADGE = {"PASS": "#2a9d2a", "WARN": "#d9a400", "FAIL": "#d03030"}


def write_html(records, out_path):
    order = {"FAIL": 0, "WARN": 1, "PASS": 2}
    recs = sorted(records, key=lambda r: (order.get(r["verdict"], 0),
                                          -(r["changed_pct"] or 0.0)))
    cards = []
    for i, r in enumerate(recs):
        name = os.path.basename(r["golden"])
        badge = ('<span style="background:%s;color:#fff;padding:2px 8px;'
                 'border-radius:4px;font-weight:bold">%s</span>'
                 % (_BADGE.get(r["verdict"], "#888"), r["verdict"]))
        suspect = (' <span style="background:#7b3fb3;color:#fff;padding:2px 8px;'
                   'border-radius:4px">SUSPECT</span>') if r["suspect"] else ""
        meta = "layer=%s max_delta=%s changed_pct=%s%%" % (
            r["layer_decided"], r["max_delta"], r["changed_pct"])
        if r.get("reason"):
            meta += " — " + r["reason"]
        imgs = ""
        if HAVE_PIL and r["verdict"] is not None and not r.get("reason"):
            try:
                a, b = _load(r["golden"]), _load(r["candidate"])
                if a.size == b.size:
                    hm = _heatmap(a, b)
                    cells = []
                    for label, im in (("golden", a), ("candidate", b), ("diff x amplified", hm)):
                        cells.append(
                            '<td style="text-align:center;padding:4px">'
                            '<img src="data:image/png;base64,%s" style="max-width:%dpx">'
                            '<div style="color:#999;font-size:12px">%s</div></td>'
                            % (_thumb_b64(im), THUMB_MAX, label))
                    imgs = '<table><tr>%s</tr></table>' % "".join(cells)
            except Exception:
                imgs = '<div style="color:#999">(thumbnail generation failed)</div>'
        cards.append(
            '<div class="card" data-pct="%s" style="border:1px solid #444;border-radius:6px;'
            'padding:10px;margin:10px 0;background:#1e1e1e">'
            '<div style="margin-bottom:6px">%s%s <b>%s</b> '
            '<span style="color:#aaa;font-size:13px">%s</span></div>%s</div>'
            % (r["changed_pct"] or 0, badge, suspect, name, meta, imgs))
    rollup = {"pass": sum(1 for r in recs if r["verdict"] == "PASS"),
              "warn": sum(1 for r in recs if r["verdict"] == "WARN"),
              "fail": sum(1 for r in recs if r["verdict"] == "FAIL")}
    html = (
        '<!DOCTYPE html><html><head><meta charset="utf-8">'
        '<title>visual_diff triptych</title></head>'
        '<body style="font-family:Segoe UI,sans-serif;background:#121212;color:#ddd;'
        'max-width:1280px;margin:0 auto;padding:16px">'
        '<h2>visual_diff — %d pairs (FAIL %d / WARN %d / PASS %d)</h2>'
        '<div style="color:#888;font-size:13px">sorted worst-first: FAIL &gt; WARN &gt; PASS, '
        'then changed_pct descending</div>%s</body></html>'
        % (len(recs), rollup["fail"], rollup["warn"], rollup["pass"], "".join(cards)))
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(html)


# ---------------- run / rollup ----------------

def run(pairs, missing, json_out=None, html_out=None, quiet=False):
    records = []
    for g, c in pairs:
        records.append(compare_pair(g, c))
    for stem, gp, cp in missing:
        records.append({
            "golden": gp or "<missing>", "candidate": cp or "<missing>",
            "verdict": "FAIL", "layer_decided": None, "max_delta": None,
            "changed_pct": None, "suspect": False, "suspect_reasons": [],
            "reason": "unpaired image (stem '%s' present on one side only)" % stem})
    rollup = {"pass": sum(1 for r in records if r["verdict"] == "PASS"),
              "warn": sum(1 for r in records if r["verdict"] == "WARN"),
              "fail": sum(1 for r in records if r["verdict"] == "FAIL")}
    result = {"records": records, "rollup": rollup,
              "thresholds": {"epsilon_lsb": EPSILON_LSB,
                             "pass_pct": PASS_PCT, "warn_pct": WARN_PCT}}
    if json_out:
        with open(json_out, "w", encoding="utf-8") as f:
            json.dump(result, f, indent=2)
    if html_out:
        write_html(records, html_out)
    if not quiet:
        for r in records:
            line = "%-4s L%s max=%s changed=%s%% %s" % (
                r["verdict"], r["layer_decided"], r["max_delta"],
                r["changed_pct"], os.path.basename(r["golden"]))
            if r["suspect"]:
                line += "  [SUSPECT: %s]" % "; ".join(r["suspect_reasons"])
            if r.get("reason"):
                line += "  (%s)" % r["reason"]
            print(line)
        print("rollup: pass=%d warn=%d fail=%d" %
              (rollup["pass"], rollup["warn"], rollup["fail"]))
    if rollup["fail"]:
        return 2
    if rollup["warn"]:
        return 1
    return 0


# ---------------- self-test ----------------

def self_test():
    if not HAVE_PIL:
        print("SELF-TEST SKIP: Pillow not installed (pip install Pillow)")
        return 1
    import tempfile
    import shutil
    tmp = tempfile.mkdtemp(prefix="visual_diff_selftest_")
    failures = []
    try:
        W, H = 320, 240
        base = Image.new("RGB", (W, H))
        px = base.load()
        for y in range(H):
            for x in range(W):
                px[x, y] = ((x * 7) % 256, (y * 5) % 256, (x + y) % 256)

        def p(name, img):
            path = os.path.join(tmp, name)
            img.save(path)
            return path

        def check(label, rec, want_verdicts, want_layer=None):
            ok = rec["verdict"] in want_verdicts
            if want_layer is not None and rec["layer_decided"] != want_layer:
                ok = False
            status = "ok" if ok else "FAIL"
            print("  [%s] %-22s verdict=%s layer=%s max=%s changed=%s%% %s" % (
                status, label, rec["verdict"], rec["layer_decided"],
                rec["max_delta"], rec["changed_pct"], rec.get("reason") or ""))
            if not ok:
                failures.append(label)

        # 1. identical bytes -> L1 PASS
        g = p("a.png", base)
        c = os.path.join(tmp, "a_copy.png")
        shutil.copyfile(g, c)
        check("identical", compare_pair(g, c), {"PASS"}, want_layer=1)

        # 2. 1-LSB noise everywhere -> L2 PASS
        noisy = base.point(lambda v: min(255, v + 1))
        check("1-lsb noise", compare_pair(g, p("noise.png", noisy)), {"PASS"}, want_layer=2)

        # 3. small patch changed (~0.33% of pixels) -> WARN per thresholds
        patch = base.copy()
        ppx = patch.load()
        for y in range(8):           # 8x32 = 256 px of 76800 = 0.333%
            for x in range(32):
                ppx[x, y] = (255, 0, 255)
        check("small patch", compare_pair(g, p("patch.png", patch)), {"WARN", "FAIL"}, want_layer=3)

        # 4. big change -> FAIL
        big = Image.new("RGB", (W, H), (255, 255, 255))
        check("big change", compare_pair(g, p("big.png", big)), {"FAIL"}, want_layer=3)

        # 5. resolution mismatch -> FAIL with reason, no crash
        small = base.resize((W // 2, H // 2))
        rec = compare_pair(g, p("small.png", small))
        check("res mismatch", rec, {"FAIL"})
        if not (rec.get("reason") and "resolution mismatch" in rec["reason"]):
            failures.append("res-mismatch-reason")

        # 6. TGA round-trip: identical content saved as TGA -> L1 or L2 PASS
        tg = p("a.tga", base)
        tc = os.path.join(tmp, "a_copy.tga")
        shutil.copyfile(tg, tc)
        check("tga identical", compare_pair(tg, tc), {"PASS"}, want_layer=1)

        # 7. sidecar SUSPECT advisory (verdict unchanged)
        with open(os.path.join(tmp, "a.state.json"), "w") as f:
            json.dump({"feature_gates": {"MC2_TERRAIN_LOD_CHUNK": 1}}, f)
        with open(os.path.join(tmp, "a_copy.state.json"), "w") as f:
            json.dump({"feature_gates": {"MC2_TERRAIN_LOD_CHUNK": 0}}, f)
        rec = compare_pair(g, c)
        ok = rec["verdict"] == "PASS" and rec["suspect"]
        print("  [%s] %-22s suspect=%s verdict=%s" % (
            "ok" if ok else "FAIL", "sidecar suspect", rec["suspect"], rec["verdict"]))
        if not ok:
            failures.append("sidecar-suspect")

        # 8. HTML + JSON generation smoke
        jout = os.path.join(tmp, "out.json")
        hout = os.path.join(tmp, "out.html")
        code = run([(g, os.path.join(tmp, "patch.png")), (g, os.path.join(tmp, "big.png"))],
                   [], json_out=jout, html_out=hout, quiet=True)
        ok = (code == 2 and os.path.getsize(jout) > 0 and os.path.getsize(hout) > 1000)
        print("  [%s] %-22s exit=%d json=%dB html=%dB" % (
            "ok" if ok else "FAIL", "json+html outputs", code,
            os.path.getsize(jout), os.path.getsize(hout)))
        if not ok:
            failures.append("outputs")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    if failures:
        print("SELF-TEST FAIL: %s" % ", ".join(failures))
        return 2
    print("SELF-TEST PASS (8 checks)")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("golden", nargs="?", help="golden image")
    ap.add_argument("candidate", nargs="?", help="candidate image")
    ap.add_argument("--golden-dir", help="batch: golden directory")
    ap.add_argument("--candidate-dir", help="batch: candidate directory")
    ap.add_argument("--json", dest="json_out", help="write run JSON here")
    ap.add_argument("--html", dest="html_out", help="write triptych HTML gallery here")
    ap.add_argument("--self-test", action="store_true", help="run synthetic self-test")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()

    if args.golden_dir or args.candidate_dir:
        if not (args.golden_dir and args.candidate_dir):
            ap.error("--golden-dir and --candidate-dir must be given together")
        pairs, missing = pair_dirs(args.golden_dir, args.candidate_dir)
        return run(pairs, missing, args.json_out, args.html_out)

    if not (args.golden and args.candidate):
        ap.error("need <golden> <candidate>, or --golden-dir/--candidate-dir, or --self-test")
    return run([(args.golden, args.candidate)], [], args.json_out, args.html_out)


if __name__ == "__main__":
    sys.exit(main())
