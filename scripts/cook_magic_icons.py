#!/usr/bin/env python3
"""Install the Magic weapon icons from the known-good MC2-MagicExpansion mod.

The component detail box + list sample icons via setUVs(0,0, IconX*48, IconY*32)
(texel coords). The texture must be >= the component's footprint region; ballistic
weapons are up to 2x4 (96x128), so a 64x64 icon is too small and the art shrinks
into the corner. The proven-working MC2-MagicExpansion mod ships these icons at
128x128 (covers every footprint) -- copy those verbatim rather than re-deriving
sizes from the smaller archive art (which rendered tiny).

TGA art is gitignored, so this is the reproducible source of truth. Also deletes
each mod's .modindex-cache so the engine re-scans.

Usage:
  py -3 scripts/cook_magic_icons.py \
      --src "A:/Games/mc2-opengl/MC2-MagicExpansion/data/art" \
      --out mods/magic-ballistic-weapons/data/art [--out <install art> ...]
"""
import argparse, glob, os, shutil, sys

DEFAULT_SRC = r"A:/Games/mc2-opengl/MC2-MagicExpansion/data/art"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default=DEFAULT_SRC, help="known-good 128x128 icon source")
    ap.add_argument("--out", action="append", required=True)
    ap.add_argument("--names", nargs="*", help="restrict to these icon basenames (default: all mcl_mc_*.tga present in --out[0])")
    args = ap.parse_args()

    # which icons to refresh: those already shipped in the mod (keep names compbas references)
    targets = set()
    for f in glob.glob(os.path.join(args.out[0], "mcl_mc_*.tga")):
        targets.add(os.path.basename(f).lower())

    avail = {os.path.basename(p).lower(): p for p in glob.glob(os.path.join(args.src, "*.tga"))}

    copied = missing = 0
    for name in sorted(targets):
        src = avail.get(name)
        if not src:
            missing += 1
            print(f"  MISSING in source (left as-is): {name}")
            continue
        for d in args.out:
            shutil.copy(src, os.path.join(d, os.path.basename(src)))
        copied += 1

    for d in args.out:
        cache = os.path.join(os.path.dirname(os.path.dirname(d)), ".modindex-cache")
        if os.path.isfile(cache):
            os.remove(cache)
    print(f"copied {copied} known-good icon(s); {missing} not in source; caches invalidated")


if __name__ == "__main__":
    main()
