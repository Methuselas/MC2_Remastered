#!/usr/bin/env python3
"""build_asset_thumbnails.py -- editor asset-browser thumbnail cache (S18).

Headless thumbnail-cache generator: walks the editor object catalog
(data/art/Buildings.csv), and for every object that already has a legacy 2D
thumbnail (the `TGA Filename` column -> data/art/<tga>.tga) downscales it into a
PNG cache keyed by the stable asset id (the `File Name` column). Objects whose
only source is geometry (.tgl/.ase/GLB) have no 2D source and are recorded as
`needs-3d-render` (an offscreen-GL render pass, out of scope for this headless
tool -- the editor's MeshPreview3D does that in-app).

This is the "reuse legacy TGA thumbs first" half of S18. The editor's existing
AssetThumbnailCache is in-memory GL only (no disk cache); this produces the
on-disk PNG cache + an index of exactly which assets have a 2D thumb vs need a
3D render (the index quantifies the render-pass gap). Wiring the editor to read
the disk cache is a separate editor-build change ("render later").

cache layout : <out>/<asset_id>.png      (default <art-dir>/.thumbs)
index        : <out>/thumb_index.json    (id -> {tga, source, png, status})

On the stock deploy the object thumbnails are NOT loose 2D files -- they are
packed in data/objects/Object2.pak, and 452 of 479 catalog objects have no 2D
thumb at all (they need an offscreen 3D render). So on stock this tool mostly
produces a COVERAGE AUDIT; the PNG-cache path activates for any thumbnail that
IS present as a loose file (the common case for mods that ship loose thumb TGAs,
and verified by --self-test).

status values:
  cached            -- loose <tga>.tga found, downscaled to <out>/<id>.png
  unresolved-2d-ref -- tga named but no loose file (likely packed in Object2.pak,
                       or absent); advisory, not a failure -- a pak extractor or
                       the editor's offscreen renderer would resolve it
  needs-3d-render   -- no 2D thumb ref; needs an offscreen GL render (out of scope)
  decode-error      -- loose file exists but failed to decode (a real defect)
  skipped-no-pillow -- Pillow absent

Exit: 0 = ok, 1 = any decode-error (a present-but-broken thumb), 3 = bad env
(no CSV). --self-test returns 0/2.

Python 3 stdlib + optional Pillow. No emoji.
"""
from __future__ import annotations

import argparse
import csv
import json
import os
import sys
from pathlib import Path

PREFIX = "[asset-thumbs]"
DEFAULT_DEPLOY = Path(r"A:/Games/mc2-opengl/mc2-win64-v0.4")
TGA_SENTINELS = {"", "0", "none", "null"}

try:
    from PIL import Image
    Image.MAX_IMAGE_PIXELS = None
    HAVE_PIL = True
except ImportError:
    HAVE_PIL = False


def log(msg: str) -> None:
    print(f"{PREFIX} {msg}", file=sys.stderr, flush=True)


def _col_index(header, *names):
    """Index of the first header column matching any of names (ci). Exact match
    is preferred across all names before falling back to a prefix match, so an
    earlier column that merely starts with the name cannot shadow an exact one."""
    low = [h.strip().lower() for h in header]
    wanted = [n.lower() for n in names]
    for n in wanted:                       # pass 1: exact
        for i, h in enumerate(low):
            if h == n:
                return i
    for n in wanted:                       # pass 2: prefix fallback
        for i, h in enumerate(low):
            if h.startswith(n):
                return i
    return None


def read_catalog(csv_path: Path):
    """Yield (asset_id, tga_value) from Buildings.csv. tga_value '' if none."""
    with open(csv_path, newline="", encoding="utf-8", errors="replace") as f:
        rdr = csv.reader(f)
        header = next(rdr)
        fi = _col_index(header, "file name", "filename")
        ti = _col_index(header, "tga filename", "tga")
        if fi is None:
            raise ValueError("Buildings.csv has no 'File Name' column")
        rows = []
        for raw in rdr:
            if not raw or fi >= len(raw):
                continue
            asset_id = raw[fi].strip()
            if not asset_id:
                continue
            tga = raw[ti].strip() if (ti is not None and ti < len(raw)) else ""
            rows.append((asset_id, tga))
    rows.sort(key=lambda r: r[0].lower())  # deterministic order
    return rows


def make_thumbnail(src_tga: Path, dst_png: Path, size: int) -> bool:
    """Downscale src TGA into dst PNG (size x size box). Return True on success."""
    with Image.open(src_tga) as im:
        im = im.convert("RGBA")
        im.thumbnail((size, size))
        dst_png.parent.mkdir(parents=True, exist_ok=True)
        im.save(dst_png, format="PNG")
    return True


def build(csv_path: Path, art_dir: Path, out_dir: Path, size: int):
    rows = read_catalog(csv_path)
    entries = []
    counts = {"cached": 0, "needs-3d-render": 0, "unresolved-2d-ref": 0,
              "decode-error": 0, "skipped-no-pillow": 0}
    for asset_id, tga in rows:
        rec = {"id": asset_id, "tga": tga, "source": None, "png": None, "status": None}
        if tga.lower() in TGA_SENTINELS:
            rec["status"] = "needs-3d-render"
        else:
            # basename both CSV-derived values before using them as filenames:
            # a malicious/garbled cell ("../x", "C:/y") cannot escape art/out dirs.
            # Real thumb stems / ids are plain names, so this is transparent.
            tga_name = os.path.basename(tga)
            id_name = os.path.basename(asset_id)
            src = art_dir / f"{tga_name}.tga"
            rec["source"] = str(src).replace("\\", "/")
            if not src.is_file():
                # named but not loose -- likely packed in Object2.pak (advisory)
                rec["status"] = "unresolved-2d-ref"
            elif not HAVE_PIL:
                rec["status"] = "skipped-no-pillow"
            else:
                dst = out_dir / f"{id_name}.png"
                try:
                    make_thumbnail(src, dst, size)
                    rec["png"] = str(dst).replace("\\", "/")
                    rec["status"] = "cached"
                except Exception as exc:  # noqa: BLE001  present-but-broken thumb
                    rec["status"] = "decode-error"
                    rec["source"] = f"{rec['source']} (decode failed: {exc})"
        counts[rec["status"]] = counts.get(rec["status"], 0) + 1
        entries.append(rec)
    return entries, counts


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Editor asset-browser thumbnail cache generator.")
    ap.add_argument("--csv", default=str(DEFAULT_DEPLOY / "data" / "art" / "Buildings.csv"))
    ap.add_argument("--art-dir", default=str(DEFAULT_DEPLOY / "data" / "art"))
    ap.add_argument("--out", default=None, help="cache dir (default: <art-dir>/.thumbs)")
    ap.add_argument("--size", type=int, default=64, help="thumbnail box size px (default 64)")
    ap.add_argument("--utc", default="", help="generated_utc stamp (default empty = byte-stable index)")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()

    csv_path = Path(args.csv)
    art_dir = Path(args.art_dir)
    out_dir = Path(args.out) if args.out else art_dir / ".thumbs"
    if not csv_path.is_file():
        log(f"FATAL: catalog CSV not found: {csv_path}")
        return 3
    if not HAVE_PIL:
        log("NOTE: Pillow absent -- no PNGs rendered; index still records statuses")

    entries, counts = build(csv_path, art_dir, out_dir, args.size)
    for r in entries:
        if r["status"] != "needs-3d-render" or args.verbose:
            if r["status"] != "cached" or args.verbose:
                log(f'{r["status"]:18s} {r["id"]}'
                    + (f'  <- {r["tga"]}.tga' if r["tga"] else ''))

    log(f"catalog={len(entries)} cached={counts['cached']} "
        f"needs-3d-render={counts['needs-3d-render']} "
        f"unresolved-2d-ref={counts['unresolved-2d-ref']} "
        f"decode-error={counts['decode-error']} "
        f"skipped-no-pillow={counts['skipped-no-pillow']}")

    index = {
        "schema": "mc2-thumb-index/1",
        "generated_utc": args.utc,
        "size": args.size,
        "csv": str(csv_path).replace("\\", "/"),
        "counts": counts,
        "entries": entries,
    }
    idx_path = out_dir / "thumb_index.json"
    idx_path.parent.mkdir(parents=True, exist_ok=True)
    idx_path.write_text(json.dumps(index, indent=2), encoding="utf-8")
    log(f"index written: {idx_path}")

    return 1 if counts["decode-error"] else 0


# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------

def self_test() -> int:
    import tempfile
    import shutil
    tmp = Path(tempfile.mkdtemp(prefix="assetthumbs_selftest_"))
    fails = []
    try:
        art = tmp / "art"
        art.mkdir()
        out = tmp / "thumbs"
        # catalog: one reuse (has tga + file), one broken ref (tga but no file),
        # two no-tga (needs 3d -- sentinels '' and '0').
        csv_path = art / "Buildings.csv"
        csv_path.write_text(
            "File Name,Group ID,TGA Filename (if any),Mission Briefing scale\n"
            "HasThumb,1,thumb_a,1\n"
            "PackedRef,1,packed_tga,1\n"
            "Corrupt,1,corrupt_tga,1\n"
            "MeshOnly,1,,1\n"
            "MeshOnly2,1,0,1\n", encoding="utf-8")

        if HAVE_PIL:
            Image.new("RGBA", (256, 256), (10, 20, 30, 255)).save(art / "thumb_a.tga")
        (art / "corrupt_tga.tga").write_bytes(b"this is not a valid TGA file")

        entries, counts = build(csv_path, art, out, 64)
        by = {e["id"]: e for e in entries}

        def chk(label, cond):
            print(f"  [{'ok' if cond else 'FAIL'}] {label}")
            if not cond:
                fails.append(label)

        chk("5 catalog rows", len(entries) == 5)
        chk("MeshOnly -> needs-3d-render", by["MeshOnly"]["status"] == "needs-3d-render")
        chk("MeshOnly2 (tga='0') -> needs-3d-render", by["MeshOnly2"]["status"] == "needs-3d-render")
        chk("PackedRef -> unresolved-2d-ref", by["PackedRef"]["status"] == "unresolved-2d-ref")
        chk("entries sorted by id", [e["id"] for e in entries] ==
            sorted((e["id"] for e in entries), key=str.lower))

        if HAVE_PIL:
            chk("HasThumb -> cached", by["HasThumb"]["status"] == "cached")
            chk("Corrupt -> decode-error", by["Corrupt"]["status"] == "decode-error")
            png = out / "HasThumb.png"
            chk("png written", png.is_file())
            if png.is_file():
                with Image.open(png) as im:
                    chk("downscaled to <=64", max(im.size) <= 64)
            rc = main(["--csv", str(csv_path), "--art-dir", str(art), "--out", str(out)])
            chk("exit 1 on decode-error", rc == 1)
            # index byte-stability
            i1 = (out / "thumb_index.json").read_bytes()
            main(["--csv", str(csv_path), "--art-dir", str(art), "--out", str(out)])
            i2 = (out / "thumb_index.json").read_bytes()
            chk("index byte-stable across runs", i1 == i2)
        else:
            chk("HasThumb -> skipped-no-pillow", by["HasThumb"]["status"] == "skipped-no-pillow")
            print("  [skip] Pillow absent -- PNG render not exercised")
    finally:
        shutil.rmtree(str(tmp), ignore_errors=True)

    if fails:
        print(f"SELF-TEST FAIL: {', '.join(fails)}")
        return 2
    print("SELF-TEST PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
