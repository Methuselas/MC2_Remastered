#!/usr/bin/env python3
"""golden_diff_report.py -- GOLDEN-SCENE-DIFF-REPORT-1 diagnostic layer.

Completes the golden-scene proof harness. Prior slices give us:
  * golden_scene.py   -- capture -> golden_manifest.json (pixel_hash, pass
                         counters, render-health, tga_source).
  * golden_compare.py -- noise-floor classification; on a pixel_hash mismatch
                         it prints "PIXEL CHANGED at frame F" -- but only that
                         the picture changed, not WHAT or WHERE.

This tool turns a pixel_hash / counter mismatch between two manifests A and B
into an actionable diagnosis:

  1. PIXEL DIFF IMAGE -- load both captures' TGAs (manifest.tga_source), compute
     per-pixel abs-diff, and SAVE a diff image (changed pixels highlighted on a
     dim background; an abs-diff heatmap alongside) to the artifact dir.
  2. FIRST-CHANGED-REGION -- bounding box (x,y,w,h) enclosing every changed
     pixel + the changed-pixel count.
  3. PASS HINT -- diff the pass_counters and render-health counters between the
     two manifests; name which pass(es)/counter(s) changed. A pixel regression
     with a lone pass-counter delta points straight at the culprit pass.
  4. golden_report.json + a one-line human summary tying it together.

Uses PIL if importable (nicer PNG output); otherwise hand-rolls an uncompressed
24bpp TGA writer so the tool has ZERO hard deps. TGA reading is hand-rolled to
match gos::screenshot::writeTGA (header[2]==2, 24bpp, bottom-up) exactly, same
as golden_scene.read_tga_pixels.

Usage:
  golden_diff_report.py A.json B.json --out-dir artifacts/
  # writes artifacts/golden_diff.(png|tga), artifacts/golden_diff_heat.(png|tga),
  # artifacts/golden_report.json; prints the human summary; exit 1 iff any diff.
"""
import argparse
import json
import struct
import sys
from pathlib import Path

# Counter groups in a manifest whose leaf ints we treat as "pass/health
# counters" for the pass-hint diff. render_health nests a few sub-dicts.
_COUNTER_ROOTS = ("pass_counters", "render_health")


# ---------------------------------------------------------------------------
# TGA read (matches golden_scene.read_tga_pixels: uncompressed true-color, the
# format gos::screenshot::writeTGA emits -- header[2]==2, 24bpp, bottom-up).
# ---------------------------------------------------------------------------
def read_tga(path):
    raw = Path(path).read_bytes()
    if len(raw) < 18:
        raise ValueError("TGA too short: %s" % path)
    idlen = raw[0]
    imgtype = raw[2]
    w = raw[12] | (raw[13] << 8)
    h = raw[14] | (raw[15] << 8)
    bpp = raw[16]
    if imgtype != 2 or bpp not in (24, 32):
        raise ValueError("unsupported TGA (type=%d bpp=%d): %s" % (imgtype, bpp, path))
    off = 18 + idlen
    stride = bpp // 8
    px = raw[off:off + w * h * stride]
    return px, w, h, stride


# ---------------------------------------------------------------------------
# Image writers. PIL if present; else a minimal uncompressed 24bpp TGA writer.
# Pixel data passed here is top-down RGB rows (list-of-bytes, len == w*h*3).
# ---------------------------------------------------------------------------
def _try_pil():
    try:
        from PIL import Image  # noqa
        return Image
    except Exception:
        return None


def write_image(rgb_top_down, w, h, out_stem):
    """Write rgb_top_down (bytes, w*h*3, row 0 = top) to <out_stem>.png via PIL,
    else <out_stem>.tga hand-rolled. Returns the path actually written."""
    Image = _try_pil()
    if Image is not None:
        img = Image.frombytes("RGB", (w, h), bytes(rgb_top_down))
        p = out_stem + ".png"
        img.save(p)
        return p
    # Hand-rolled uncompressed 24bpp TGA. TGA is BGR + bottom-up.
    p = out_stem + ".tga"
    hdr = struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, w, h, 24, 0)
    body = bytearray(w * h * 3)
    for y in range(h):
        src_row = (h - 1 - y) * w * 3  # bottom-up
        dst_row = y * w * 3
        for x in range(w):
            s = src_row + x * 3
            d = dst_row + x * 3
            body[d] = rgb_top_down[s + 2]      # B
            body[d + 1] = rgb_top_down[s + 1]  # G
            body[d + 2] = rgb_top_down[s]      # R
    Path(p).write_bytes(hdr + bytes(body))
    return p


# ---------------------------------------------------------------------------
# Pixel diff. Both TGAs are bottom-up BGR(A). We compute abs-diff per channel
# and produce (a) a highlight image (changed px in red over dim original) and
# (b) an abs-diff heatmap, both emitted TOP-DOWN RGB. Bounding box uses the
# NATURAL top-left origin (y=0 at top of image).
# ---------------------------------------------------------------------------
def pixel_diff(px_a, px_b, w, h, stride):
    """Return (changed_count, bbox_or_none, highlight_rgb, heat_rgb).
    bbox = (x, y, w, h) with y measured from the TOP of the image."""
    highlight = bytearray(w * h * 3)
    heat = bytearray(w * h * 3)
    changed = 0
    minx = miny = 1 << 30
    maxx = maxy = -1
    for row in range(h):
        # source rows are bottom-up; top-of-image is the last source row.
        src = (h - 1 - row) * w * stride
        dst = row * w * 3
        for x in range(w):
            s = src + x * stride
            d = dst + x * 3
            ba, ga, ra = px_a[s], px_a[s + 1], px_a[s + 2]
            bb, gb, rb = px_b[s], px_b[s + 1], px_b[s + 2]
            db = abs(ba - bb)
            dg = abs(ga - gb)
            dr = abs(ra - rb)
            dmax = db if db > dg else dg
            if dr > dmax:
                dmax = dr
            # heatmap: grayscale magnitude (top-down RGB).
            heat[d] = heat[d + 1] = heat[d + 2] = dmax
            if dmax != 0:
                changed += 1
                # highlight: pure red marker.
                highlight[d] = 255
                highlight[d + 1] = 0
                highlight[d + 2] = 0
                if x < minx:
                    minx = x
                if x > maxx:
                    maxx = x
                if row < miny:
                    miny = row
                if row > maxy:
                    maxy = row
            else:
                # dim the unchanged original (from A) as context.
                highlight[d] = ra >> 2
                highlight[d + 1] = ga >> 2
                highlight[d + 2] = ba >> 2
    bbox = None
    if changed:
        bbox = (minx, miny, maxx - minx + 1, maxy - miny + 1)
    return changed, bbox, highlight, heat


# ---------------------------------------------------------------------------
# Counter / pass-hint diff.
# ---------------------------------------------------------------------------
def _flatten_ints(obj, prefix=""):
    out = {}
    if isinstance(obj, dict):
        for k in sorted(obj.keys()):
            out.update(_flatten_ints(obj[k], "%s.%s" % (prefix, k) if prefix else str(k)))
    elif isinstance(obj, list):
        for i, v in enumerate(obj):
            out.update(_flatten_ints(v, "%s[%d]" % (prefix, i)))
    elif isinstance(obj, (int, float)) and not isinstance(obj, bool):
        out[prefix] = obj
    return out


def counter_deltas(man_a, man_b):
    """Diff numeric leaf counters under the counter roots. Returns a sorted list
    of (dotted_key, a, b, delta)."""
    fa, fb = {}, {}
    for root in _COUNTER_ROOTS:
        fa.update(_flatten_ints(man_a.get(root, {}), root))
        fb.update(_flatten_ints(man_b.get(root, {}), root))
    deltas = []
    for k in sorted(set(fa) | set(fb)):
        av = fa.get(k)
        bv = fb.get(k)
        if av != bv:
            d = None
            if isinstance(av, (int, float)) and isinstance(bv, (int, float)):
                d = bv - av
            deltas.append((k, av, bv, d))
    return deltas


def _pass_name(key):
    """Extract a human pass/counter label from a dotted counter key
    (e.g. 'pass_counters.terrainSolid.drawCalls' -> 'terrainSolid')."""
    parts = key.split(".")
    # drop the counter root; the next segment is usually the pass name.
    if len(parts) >= 2:
        return parts[1].split("[")[0]
    return key


def culprit_passes(deltas):
    """Order candidate passes by number of changed counters (most-changed
    first). Returns a de-duped list of pass names."""
    from collections import Counter
    tally = Counter(_pass_name(k) for k, *_ in deltas)
    return [name for name, _ in tally.most_common()]


# ---------------------------------------------------------------------------
def load_manifest(p):
    return json.loads(Path(p).read_text(encoding="utf-8"))


def resolve_tga(man, manifest_path):
    """Resolve tga_source; if the recorded absolute path is missing, try the
    manifest's own directory (captures + manifest usually ship together)."""
    src = man.get("tga_source")
    if not src:
        return None
    p = Path(src)
    if p.is_file():
        return p
    alt = Path(manifest_path).parent / p.name
    if alt.is_file():
        return alt
    return p  # return as-is; caller reports the miss


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("a", help="manifest A (baseline)")
    ap.add_argument("b", help="manifest B (candidate)")
    ap.add_argument("--out-dir", default=".",
                    help="artifact dir for diff image(s) + golden_report.json")
    ap.add_argument("--report", default=None,
                    help="report path (default <out-dir>/golden_report.json)")
    args = ap.parse_args()

    man_a = load_manifest(args.a)
    man_b = load_manifest(args.b)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    report_path = Path(args.report) if args.report else out_dir / "golden_report.json"

    ha = man_a.get("pixel_hash")
    hb = man_b.get("pixel_hash")
    pixel_mismatch = (ha != hb)

    report = {
        "schema": "GOLDEN_DIFF_REPORT_V1",
        "manifest_a": str(args.a),
        "manifest_b": str(args.b),
        "pixel_hash_a": ha,
        "pixel_hash_b": hb,
        "pixel_mismatch": pixel_mismatch,
        "pixel_diff": None,
        "counter_deltas": [],
        "culprit_passes": [],
    }

    # --- pass / counter hint (always computed; cheap, no TGA needed) -----------
    deltas = counter_deltas(man_a, man_b)
    report["counter_deltas"] = [
        {"counter": k, "a": av, "b": bv, "delta": d} for (k, av, bv, d) in deltas]
    culprits = culprit_passes(deltas)
    report["culprit_passes"] = culprits

    # --- pixel diff image + region (only when the hash actually differs) -------
    diff_note = None
    if pixel_mismatch:
        tga_a = resolve_tga(man_a, args.a)
        tga_b = resolve_tga(man_b, args.b)
        if not tga_a or not tga_b or not tga_a.is_file() or not tga_b.is_file():
            diff_note = "pixel_hash differs but a TGA source is missing (a=%s b=%s)" % (
                tga_a, tga_b)
        else:
            pa, wa, hgt_a, sa = read_tga(tga_a)
            pb, wb, hgt_b, sb = read_tga(tga_b)
            if (wa, hgt_a) != (wb, hgt_b):
                diff_note = "TGA dimensions differ: A=%dx%d B=%dx%d" % (
                    wa, hgt_a, wb, hgt_b)
            elif sa != sb:
                diff_note = "TGA channel stride differs: A=%d B=%d" % (sa, sb)
            else:
                changed, bbox, highlight, heat = pixel_diff(pa, pb, wa, hgt_a, sa)
                hi_path = write_image(highlight, wa, hgt_a, str(out_dir / "golden_diff"))
                heat_path = write_image(heat, wa, hgt_a, str(out_dir / "golden_diff_heat"))
                report["pixel_diff"] = {
                    "width": wa,
                    "height": hgt_a,
                    "changed_pixels": changed,
                    "total_pixels": wa * hgt_a,
                    "changed_region_xywh": list(bbox) if bbox else None,
                    "highlight_image": hi_path,
                    "heatmap_image": heat_path,
                }
    if diff_note:
        report["pixel_diff"] = {"note": diff_note}

    report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                           encoding="utf-8")

    # --- human summary --------------------------------------------------------
    print("[golden_diff_report] A=%s" % args.a)
    print("[golden_diff_report] B=%s" % args.b)
    print("[golden_diff_report] report -> %s" % report_path)

    pd = report["pixel_diff"]
    if not pixel_mismatch:
        print("[golden_diff_report] no pixel diff: pixel_hash identical (%s)" % ha)
    elif pd and "changed_pixels" in pd:
        bbox = pd["changed_region_xywh"]
        print("[golden_diff_report] %d/%d pixels changed in region (x=%d,y=%d,w=%d,h=%d)"
              % (pd["changed_pixels"], pd["total_pixels"],
                 bbox[0], bbox[1], bbox[2], bbox[3]))
        print("[golden_diff_report]   highlight: %s" % pd["highlight_image"])
        print("[golden_diff_report]   heatmap:   %s" % pd["heatmap_image"])
    elif pd and "note" in pd:
        print("[golden_diff_report] pixel_hash differs but no diff image: %s" % pd["note"])

    if deltas:
        print("[golden_diff_report] counter deltas (%d):" % len(deltas))
        for k, av, bv, d in deltas:
            ds = ("  (delta %+g)" % d) if d is not None else ""
            print("    %-52s %r -> %r%s" % (k, av, bv, ds))
        print("[golden_diff_report] likely culprit pass(es): %s"
              % ", ".join(culprits))
    else:
        print("[golden_diff_report] no pass/counter deltas")

    # one-line machine-friendly summary
    n_changed = (pd or {}).get("changed_pixels", 0) if pixel_mismatch else 0
    region = (pd or {}).get("changed_region_xywh") if pixel_mismatch else None
    print("[golden_diff_report] SUMMARY: %d pixels changed%s; counter deltas: %d; "
          "likely culprit: %s"
          % (n_changed,
             (" in region %s" % region) if region else "",
             len(deltas),
             culprits[0] if culprits else "(none)"))

    # exit 1 iff there is any observed difference (pixel or counter).
    return 1 if (pixel_mismatch or deltas) else 0


if __name__ == "__main__":
    sys.exit(main())
