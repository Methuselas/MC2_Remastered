#!/usr/bin/env python3
"""Cook the Magic ballistic-weapon component icons into the mod.

The archived Magic weapon art ships at 128x128, but MC2 weapon-component icon
cells are 64x64 (stock weapon icons are 64x64). At 128 the engine samples only
the top-left 64x64 of the texture, so the gun renders quarter-size in the corner
with the transparent remainder showing as empty bars. This downscales every icon
to 64x64 (uncompressed 32-bit TGA) into the mod's data/art.

TGA art is gitignored (.gitignore: *.tga), so this script is the reproducible
source-of-truth: re-run it after a fresh checkout/deploy to regenerate the icons.

Usage:
  py -3 scripts/cook_magic_icons.py \
      --src "A:/Games/Magic_MC2_archive/4. Weapons/Art-weapons" \
      --out mods/magic-ballistic-weapons/data/art \
      [--out <additional install path> ...]
"""
import argparse
import glob
import os
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow required: py -3 -m pip install pillow")

DEFAULT_SRC = r"A:/Games/Magic_MC2_archive/4. Weapons/Art-weapons"
CELL = 64  # MC2 weapon-component icon cell size


def main():
    ap = argparse.ArgumentParser(description="Resize Magic weapon icons to 64x64 TGA.")
    ap.add_argument("--src", default=DEFAULT_SRC, help="source 128x128 art folder")
    ap.add_argument("--out", action="append", required=True,
                    help="destination data/art folder (repeatable for repo + install)")
    args = ap.parse_args()

    srcs = sorted(glob.glob(os.path.join(args.src, "*.tga")))
    if not srcs:
        sys.exit(f"no .tga found in {args.src}")
    for d in args.out:
        os.makedirs(d, exist_ok=True)

    n = 0
    for p in srcs:
        im = Image.open(p).convert("RGBA").resize((CELL, CELL), Image.LANCZOS)
        name = os.path.basename(p)
        for d in args.out:
            im.save(os.path.join(d, name), compression=None)  # uncompressed TGA
        n += 1
    print(f"cooked {n} icon(s) -> {CELL}x{CELL} into {len(args.out)} location(s)")


if __name__ == "__main__":
    main()
