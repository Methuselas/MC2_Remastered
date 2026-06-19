#!/usr/bin/env python3
"""
tools/extract_veg_cards.py — extract vegetation atlas frames into loose card PNGs.

Usage:
    py -3 tools/extract_veg_cards.py [--atlas PATH] [--out DIR]

Defaults:
    atlas  = data/textures/vegetation_atlas_v2.png   (relative to CWD)
    out    = data/vegetation/cards/

Atlas layout (vegetation_atlas_v2.png):
    2048 x 1024 px, RGBA
    Row 0 (y=0..511)   frames 0-3 : grass_01..grass_04
    Row 1 (y=512..1023) frames 4-7 : fern_01..fern_04

Output card names:
    grass_01.png .. grass_04.png
    fern_01.png  .. fern_04.png

Requires: Pillow  (pip install pillow)

TODO: optionally cook each card to .ktx2 via texconv + ktx2ktx2 for fast load.
"""

import argparse
import os
import sys

def main():
    parser = argparse.ArgumentParser(description="Extract vegetation atlas into card PNGs")
    parser.add_argument("--atlas", default=os.path.join("data", "textures", "vegetation_atlas_v2.png"),
                        help="Source atlas PNG")
    parser.add_argument("--out", default=os.path.join("data", "vegetation", "cards"),
                        help="Output directory for card PNGs")
    args = parser.parse_args()

    try:
        from PIL import Image
    except ImportError:
        print("ERROR: Pillow not available. Install with: pip install pillow", file=sys.stderr)
        sys.exit(1)

    atlas_path = args.atlas
    out_dir = args.out

    if not os.path.isfile(atlas_path):
        print(f"ERROR: Atlas not found: {atlas_path}", file=sys.stderr)
        sys.exit(1)

    atlas = Image.open(atlas_path)
    w, h = atlas.size
    print(f"Atlas: {atlas_path}  {w}x{h}  mode={atlas.mode}")

    COLS, ROWS = 4, 2
    if w % COLS != 0 or h % ROWS != 0:
        print(f"ERROR: Atlas size {w}x{h} not evenly divisible by {COLS}x{ROWS}", file=sys.stderr)
        sys.exit(1)

    fw, fh = w // COLS, h // ROWS
    print(f"Frame size: {fw}x{fh}")

    os.makedirs(out_dir, exist_ok=True)

    # Row 0: grass variants, Row 1: fern/shrub variants
    card_names = [
        ["grass_01", "grass_02", "grass_03", "grass_04"],
        ["fern_01",  "fern_02",  "fern_03",  "fern_04"],
    ]

    saved = []
    for row in range(ROWS):
        for col in range(COLS):
            x0 = col * fw
            y0 = row * fh
            card = atlas.crop((x0, y0, x0 + fw, y0 + fh))
            name = card_names[row][col]
            out_path = os.path.join(out_dir, name + ".png")
            card.save(out_path)
            saved.append(out_path)
            print(f"  wrote {out_path}  ({fw}x{fh})")

    print(f"\nExtracted {len(saved)} cards to {out_dir}")
    print("NOTE: KTX2 cook deferred. To cook: texconv + ktx2ktx2 per card (TODO).")


if __name__ == "__main__":
    main()
