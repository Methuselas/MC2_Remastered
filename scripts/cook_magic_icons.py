#!/usr/bin/env python3
"""Cook the Magic mod component icons to the EXACT pixel size the mech-bay UI samples.

The mechlab/purchase UI draws a component icon with
    icon.setUVs(0, 0, IconX*48, IconY*32)   (componentlistbox.cpp:57)
i.e. it samples the texture in TEXEL coords from (0,0) to (IconX*48, IconY*32),
where IconX/IconY are the component's grid footprint (compbas cols 28/29) and the
cell is 48x32 px. So each icon TGA must be exactly IconX*48 x IconY*32 pixels --
a uniformly-resized 64x64 (or the 128x128 originals) sample the wrong region and
render the icon at the wrong size.

This reads the mod's compbas.csv, computes each shipped icon's target size as
max(IconX)*48 x max(IconY)*32 over the components that reference it (so an icon
shared by differently-sized components never over-samples), and resizes the
128x128 source art (from the archive) to that exact size.

TGA art is gitignored, so this script is the reproducible source of truth.

Usage:
  py -3 scripts/cook_magic_icons.py \
      --src "A:/Games/Magic_MC2_archive/4. Weapons/Art-weapons" \
      --compbas mods/magic-ballistic-weapons/data/objects/compbas.csv \
      --out mods/magic-ballistic-weapons/data/art \
      [--out <install art dir> ...]
"""
import argparse, csv, glob, os, sys
try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow required: py -3 -m pip install pillow")

CELL_W, CELL_H = 48, 32
DEFAULT_SRC = r"A:/Games/Magic_MC2_archive/4. Weapons/Art-weapons"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default=DEFAULT_SRC, help="128px source art folder")
    ap.add_argument("--compbas", required=True, help="mod compbas.csv (for IconX/IconY)")
    ap.add_argument("--out", action="append", required=True, help="dest data/art (repeatable)")
    args = ap.parse_args()

    # icon-basename(lower) -> (maxIconX, maxIconY) over components using it
    want = {}
    for r in csv.reader(open(args.compbas, newline="")):
        if not r or not r[0].strip().isdigit() or len(r) < 29:
            continue
        icon = r[23].strip()
        if not icon or icon == "0":
            continue
        try:
            ix, iy = int(r[27]), int(r[28])
        except ValueError:
            continue
        if ix <= 0 or iy <= 0:
            continue
        key = os.path.splitext(icon)[0].lower()
        w, h = want.get(key, (0, 0))
        want[key] = (max(w, ix), max(h, iy))

    # source art available (lower basename -> path)
    srcs = {os.path.splitext(os.path.basename(p))[0].lower(): p
            for p in glob.glob(os.path.join(args.src, "*.tga"))}

    for d in args.out:
        os.makedirs(d, exist_ok=True)

    cooked = skipped = 0
    for key, (ix, iy) in sorted(want.items()):
        sp = srcs.get(key)
        if not sp:
            skipped += 1   # icon resolves from FST (not mod-shipped) -- leave it
            continue
        tw, th = ix * CELL_W, iy * CELL_H
        im = Image.open(sp).convert("RGBA").resize((tw, th), Image.LANCZOS)
        name = os.path.basename(sp)
        for d in args.out:
            im.save(os.path.join(d, name), compression=None)
        cooked += 1
        print(f"  {name}: {ix}x{iy} cells -> {tw}x{th}px")
    print(f"cooked {cooked} icon(s); {skipped} FST-resolved icon(s) left untouched")


if __name__ == "__main__":
    main()
