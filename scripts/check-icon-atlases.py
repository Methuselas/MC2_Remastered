#!/usr/bin/env python3
"""check-icon-atlases.py -- static size lint for GUI/HUD icon atlases.

Catches the icon-atlas size-mismatch class of bug (e.g. POAR mech-bay vehicle
icons rendering red, other campaigns rendering wrong-sized icons):

  - the engine's mech-bay roster path divides icon-cell V coords by the atlas's
    LOGICAL height; the disk-TGA loader (mclib/txmmgr.cpp) applies uvScale=4 to
    most TGAs (logical = physical/4), so a too-short / wrongly-scaled atlas pushes
    high icon indices off-atlas (red) or distorts in-range ones (wrong-sized).
  - a campaign/dep shipping a differently-sized copy of a shared atlas silently
    breaks just that campaign.

What it does (read-only): for every icon-atlas .tga in a deploy (base data/art +
each mods/<mod>/data/art), read the TGA header dimensions, compute the logical
size under the per-atlas uvScale, and FLAG:
  * copies of the same atlas that disagree in physical size (the wrong-size/red
    culprit -- e.g. cveg 256x256 vs mco-compat 256x512 mcui_gn_mechicons.tga),
  * the bay roster atlas (mcui_gn_mechicons.tga) when a copy is only 256 tall
    (~80 icon slots) -- red-risk for campaigns whose units use higher indices
    (vehicles), which need a 512-tall atlas,
  * a physical width that is not a clean multiple of the known cell width.

Usage:
  py -3 scripts/check-icon-atlases.py [--deploy <dir>] [--strict]

  --deploy  deploy root to scan (default: A:/Games/mc2-opengl/releases/mc2-win64-v0.4d-rc1)
  --strict  exit nonzero if any flag fires (for CI gating)

Exit 0 = no flags (or non-strict). Nonzero (with --strict) = flags present.
"""

import argparse
import os
import struct
import sys

DEFAULT_DEPLOY = r"A:/Games/mc2-opengl/releases/mc2-win64-v0.4d-rc1"

# Known icon-atlas cell layouts (from code/mechicon.cpp setIconVariables +
# docs/asset-pipeline.md 6a). cols=None means columns are derived at runtime
# (width/cellW) so we only check width-vs-cellW and consistency, not row count.
#   name : (cols, cellW, cellH, uvScale)
# uvScale is the V-axis divisor scale: mcui_gn_mechicons.tga is forced to 1 in
# txmmgr (native UI atlas, not upscaled); the rest follow the disk-TGA uvScale=4.
ATLAS_SPECS = {
    "mcui_gn_mechicons.tga": (10, 25, 30, 1),   # mech-bay roster (logistics)
    "mcui_med4.tga":         (None, 32, 38, 4), # in-mission HUD, 800-wide
    "mcui_low4.tga":         (None, 25, 30, 4), # in-mission HUD, 640-wide
    "mcui_high7.tga":        (None, 40, 48, 4), # in-mission HUD, hi-res
}

# Icon-atlas filename family (consistency-checked even without a known cell spec).
ATLAS_FAMILY_PREFIXES = ("mcui_",)
ATLAS_FAMILY_SUFFIXES = ("_mechicons.tga",)


def is_icon_atlas(fn):
    low = fn.lower()
    if not low.endswith(".tga"):
        return False
    return low.startswith(ATLAS_FAMILY_PREFIXES) or low.endswith(ATLAS_FAMILY_SUFFIXES)


def read_tga_dims(path):
    """Return (width, height, bpp) from a TGA header, or None on failure."""
    try:
        with open(path, "rb") as f:
            h = f.read(18)
        if len(h) < 18:
            return None
        w = struct.unpack("<H", h[12:14])[0]
        ht = struct.unpack("<H", h[14:16])[0]
        bpp = h[16]
        return (w, ht, bpp)
    except OSError:
        return None


def scan(deploy):
    """{atlas_name: [(location_label, abspath, (w,h,bpp))]}"""
    found = {}
    roots = []
    base_art = os.path.join(deploy, "data", "art")
    if os.path.isdir(base_art):
        roots.append(("base", base_art))
    mods_dir = os.path.join(deploy, "mods")
    if os.path.isdir(mods_dir):
        for mod in sorted(os.listdir(mods_dir)):
            art = os.path.join(mods_dir, mod, "data", "art")
            if os.path.isdir(art):
                roots.append((mod, art))
    for label, art in roots:
        for fn in os.listdir(art):
            if is_icon_atlas(fn):
                p = os.path.join(art, fn)
                dims = read_tga_dims(p)
                found.setdefault(fn.lower(), []).append((label, p, dims))
    return found


def main():
    ap = argparse.ArgumentParser(description="Static size lint for GUI/HUD icon atlases.")
    ap.add_argument("--deploy", default=DEFAULT_DEPLOY, help="deploy root to scan")
    ap.add_argument("--strict", action="store_true", help="nonzero exit if any flag fires")
    args = ap.parse_args()

    if not os.path.isdir(args.deploy):
        print(f"ERROR: deploy dir not found: {args.deploy}", file=sys.stderr)
        return 2

    found = scan(args.deploy)
    if not found:
        print(f"No icon atlases found under {args.deploy}", file=sys.stderr)
        return 2

    flags = []
    print(f"=== icon-atlas size check: {args.deploy} ===\n")
    for name in sorted(found):
        copies = found[name]
        spec = ATLAS_SPECS.get(name)
        cols, cellW, cellH, uvScale = spec if spec else (None, None, None, 4)
        # distinct physical sizes across copies
        sizes = {(d[0], d[1]) for (_l, _p, d) in copies if d}
        print(f"{name}  ({len(copies)} cop{'y' if len(copies)==1 else 'ies'})"
              + (f"  spec: {cols or '?'}col x {cellW}x{cellH}px, uvScale={uvScale}" if spec else "  (no cell spec)"))
        for label, _p, d in copies:
            if not d:
                print(f"    {label:<22} <unreadable TGA header>")
                flags.append(f"{name}@{label}: unreadable header")
                continue
            w, ht, bpp = d
            logical_w = w / uvScale
            logical_h = ht / uvScale
            note = ""
            if spec and cellW and (w % cellW) != 0 and (w not in (cols * cellW if cols else -1, 256)):
                note += " [W not a clean multiple of cellW]"
            print(f"    {label:<22} phys={w}x{ht} {bpp}bpp  logical(/{uvScale})={logical_w:.0f}x{logical_h:.0f}{note}")
        # FLAG: copies disagree in physical size
        if len(sizes) > 1:
            flags.append(f"{name}: copies disagree in size {sorted(sizes)} -- a campaign/dep ships a differently-sized atlas (wrong-size/red risk)")
            print(f"    !! FLAG: size disagreement across copies: {sorted(sizes)}")
        # FLAG: bay roster atlas too short (256 tall = ~80 slots; high indices/vehicles need 512)
        if name == "mcui_gn_mechicons.tga":
            for label, _p, d in copies:
                if d and d[1] <= 256:
                    flags.append(f"{name}@{label}: {d[0]}x{d[1]} only ~{(d[1]//cellH)*cols} slots -- red-risk if its campaign's units use higher icon indices (need 512-tall)")
                    print(f"    !! FLAG: {label} is {d[0]}x{d[1]} (only ~{(d[1]//cellH)*cols} icon slots) -- vehicles/high indices will be RED; needs 512-tall")
        print()

    print("=== summary ===")
    if flags:
        print(f"{len(flags)} flag(s):")
        for fl in flags:
            print(f"  - {fl}")
        if args.strict:
            return 1
    else:
        print("no flags.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
