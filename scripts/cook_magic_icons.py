#!/usr/bin/env python3
"""Cook Magic mod component icons to MC2's actual convention.

The mech-bay/list/mechlopedia draw a component icon with:
    icon.resize( IconX*48, IconY*32 )
    icon.setUVs ( 0, 0, IconX*48, IconY*32 )       (componentlistbox.cpp:56-57)
i.e. it samples the TOP-LEFT (IconX*48 x IconY*32) texel region of the texture.
Stock/FST icons are 128x128 POWER-OF-TWO canvases with the art placed in that
top-left region. Non-power-of-two textures (96x64 etc.) get padded by the GPU and
mis-sample -> the art shrinks into a corner (this was the ballistic/missile bug).

So each icon must be a 128x128 pow2 TGA with the weapon art scaled to fill the
top-left IconX*48 x IconY*32 region, transparent elsewhere. Because the sampled
region depends on the COMPONENT's footprint, an icon shared by components with
different footprints needs separate files -> we emit ONE file per component
(mcl_cmp_<id>.tga) and rewrite that component's Logistics Icon 1 to point at it.

Reads/writes the mod compbas.csv (updates icon col 24) and writes the TGAs.
TGA art is gitignored, so this script is the reproducible source of truth.

Usage:
  py -3 scripts/cook_magic_icons.py \
      --src "A:/Games/Magic_MC2_archive/4. Weapons/Art-weapons" \
      --compbas mods/magic-ballistic-weapons/data/objects/compbas.csv \
      --out mods/magic-ballistic-weapons/data/art [--out <install art> ...]
"""
import argparse, csv, glob, io, os, sys
try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow required: py -3 -m pip install pillow")

CANVAS = 128           # power-of-two canvas (matches stock icons)
CELL_W, CELL_H = 48, 32
DEFAULT_SRC = r"A:/Games/Magic_MC2_archive/4. Weapons/Art-weapons"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default=DEFAULT_SRC)
    ap.add_argument("--compbas", required=True)
    ap.add_argument("--out", action="append", required=True)
    args = ap.parse_args()

    srcs = {os.path.splitext(os.path.basename(p))[0].lower(): p
            for p in glob.glob(os.path.join(args.src, "*.tga"))}
    for d in args.out:
        os.makedirs(d, exist_ok=True)

    rows = list(csv.reader(open(args.compbas, newline="")))
    cooked = skipped = 0
    for r in rows:
        if not r or not r[0].strip().isdigit() or len(r) < 29:
            continue
        cid = int(r[0])
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
        sp = srcs.get(key)
        if not sp:
            skipped += 1            # FST-resolved (energy etc.) -- leave untouched
            continue
        rw, rh = ix * CELL_W, iy * CELL_H        # sampled region in texels
        art = Image.open(sp).convert("RGBA").resize((rw, rh), Image.LANCZOS)
        canvas = Image.new("RGBA", (CANVAS, CANVAS), (0, 0, 0, 0))
        canvas.alpha_composite(art, (0, 0))      # top-left region
        name = f"mcl_cmp_{cid}.tga"
        for d in args.out:
            canvas.save(os.path.join(d, name), compression=None)
        r[23] = name                             # Logistics Icon 1 -> per-component file
        cooked += 1

    out = io.StringIO()
    csv.writer(out, lineterminator="\n").writerows(rows)
    open(args.compbas, "w", newline="").write(out.getvalue())
    print(f"cooked {cooked} per-component 128x128 icon(s); {skipped} FST-resolved left untouched")


if __name__ == "__main__":
    main()
