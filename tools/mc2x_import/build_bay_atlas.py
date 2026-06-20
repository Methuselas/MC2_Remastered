#!/usr/bin/env python3
"""Derive the MC2X logistics-bay icon atlas (mcui_gn_mechicons.tga) from the
MC2X-native mc2x_mechicons.tga.

WHY: the purchase/bay screen loads data/art/mcui_gn_mechicons.tga and indexes
it as a grid of 25x30 cells (cols = atlasWidth/25). MC2X campaigns author
vehicle/infantry IconIndex 118-142, which only resolve on the 512-wide,
20-column MC2X atlas (mc2x_mechicons.tga) -- NOT the 256-wide retail mech
atlas. mc2x_mechicons.tga is opaque RGB (HUD usage); the bay needs
transparency, so we color-key near-black -> alpha 0.

This is a DEV/build tool (needs Pillow); it pre-generates the committed shim
asset tools/mc2x_import/shims/mc2x-compat/data/art/mcui_gn_mechicons.tga so the
runtime importer (which may be PyInstaller-frozen, no Pillow) just copies it
verbatim via copy_shims().

Re-run after the upstream mc2x_mechicons.tga changes:
  py -3 tools/mc2x_import/build_bay_atlas.py \
      --src <install>/data/art/mc2x_mechicons.tga \
      --out tools/mc2x_import/shims/mc2x-compat/data/art/mcui_gn_mechicons.tga
"""
import argparse
import os
import sys

# Pixels whose R+G+B sum is <= this are treated as background -> fully
# transparent. Pure black only; keeps dark interior icon pixels opaque.
BLACK_KEY_SUM = 16


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src", required=True,
                    help="source mc2x_mechicons.tga (512x512 RGB, MC2X art.fst)")
    ap.add_argument("--out", required=True,
                    help="output mcui_gn_mechicons.tga (512x512 RGBA, black-keyed)")
    args = ap.parse_args(argv)

    try:
        from PIL import Image
    except ImportError:
        print("ERROR: Pillow required for build_bay_atlas.py (dev tool).", file=sys.stderr)
        return 2

    im = Image.open(args.src).convert("RGBA")
    px = im.load()
    w, h = im.size
    keyed = 0
    for y in range(h):
        for x in range(w):
            r, g, b, _a = px[x, y]
            if r + g + b <= BLACK_KEY_SUM:
                px[x, y] = (r, g, b, 0)
                keyed += 1
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    im.save(args.out)
    print(f"wrote {args.out} ({w}x{h} RGBA, {keyed} px keyed transparent)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
