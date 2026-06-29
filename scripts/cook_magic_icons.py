#!/usr/bin/env python3
"""Cook the Magic weapon icons to 64x64 full-art TGAs (matching stock energy icons).

MC2 component icons are authored 64x64 with the art filling the canvas (the stock
energy-weapon icons are 64x64). The detail/info box and the component list both
expect that size; a larger texture (e.g. 128) gets sampled at the smaller region
and renders tiny in the corner. So: resize each archive source (128) to 64x64,
full art, keeping the original MCL_MC_* filename the compbas references.

TGA art is gitignored, so this is the reproducible source of truth. Also deletes
the mod's .modindex-cache so the engine re-scans (new/changed files otherwise
stay invisible).

Usage:
  py -3 scripts/cook_magic_icons.py \
      --src "A:/Games/Magic_MC2_archive/4. Weapons/Art-weapons" \
      --out mods/magic-ballistic-weapons/data/art [--out <install art> ...]
"""
import argparse, glob, os, sys
try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow required: py -3 -m pip install pillow")

SIZE = 64
DEFAULT_SRC = r"A:/Games/Magic_MC2_archive/4. Weapons/Art-weapons"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default=DEFAULT_SRC)
    ap.add_argument("--out", action="append", required=True)
    args = ap.parse_args()

    srcs = sorted(glob.glob(os.path.join(args.src, "*.tga")))
    if not srcs:
        sys.exit(f"no .tga in {args.src}")
    for d in args.out:
        os.makedirs(d, exist_ok=True)

    n = 0
    for src in srcs:
        im = Image.open(src).convert("RGBA").resize((SIZE, SIZE), Image.LANCZOS)
        name = os.path.basename(src)
        for d in args.out:
            im.save(os.path.join(d, name), compression=None)
        n += 1

    for d in args.out:
        cache = os.path.join(os.path.dirname(os.path.dirname(d)), ".modindex-cache")
        if os.path.isfile(cache):
            os.remove(cache)
    print(f"cooked {n} icon(s) -> {SIZE}x{SIZE} full-art; caches invalidated")


if __name__ == "__main__":
    main()
