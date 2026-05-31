#!/usr/bin/env python3
"""BURNIN-DISK-JPEG cook tool.

Shrinks the terrain burn-in colormaps on disk by storing each
<name>.burnin.tga (5120x5120, 24-bit BGR, ~75MB) as a lossy JPEG
<name>.burnin.jpg (quality 90, ~5-9MB). The engine (mclib/terrtxm2.cpp)
decodes the JPEG with stb_image on load and packs the RGB pixels into the
ColorMap buffer in the SAME byte order (B,G,R,A) the .burnin.tga path
produces, so terrain ground color is unchanged modulo JPEG quantization.

This is LOSSY but acceptable: the burn-in colormap is a low-visibility
terrain ground texture, heavily tiled and minified at runtime.

Pillow reads the .burnin.tga (handling its BGR channel order and TGA
orientation) into a top-down logical RGB image, then writes JPEG. stb_image
on the engine side likewise yields top-down RGB, so orientation matches the
engine's post-flipTopToBottom ColorMap with no extra flip.

Usage:
    python burnin_jpeg.py <file_or_dir> [more ...] [--quality 90] [--recursive]

  * A file argument ending in .burnin.tga is cooked directly.
  * A directory argument is scanned for *.burnin.tga.

The original .tga is NOT deleted — verify identical-render, then delete the
.tga yourself to realize the disk win.
"""

import argparse
import os
import sys

from PIL import Image

# 5120x5120 burn-in maps exceed Pillow's default decompression-bomb guard.
Image.MAX_IMAGE_PIXELS = None


def cook_one(tga_path: str, quality: int = 90) -> bool:
    if not os.path.isfile(tga_path):
        print(f"[skip] not a file: {tga_path}")
        return False

    jpg_path = (tga_path[:-4] + ".jpg") if tga_path.lower().endswith(".tga") else tga_path + ".jpg"

    with Image.open(tga_path) as im:
        im.convert("RGB").save(jpg_path, "JPEG", quality=quality)

    orig = os.path.getsize(tga_path)
    jpg = os.path.getsize(jpg_path)
    ratio = orig / jpg if jpg else 0.0
    mb = 1024.0 * 1024.0
    print(
        f"[cook] {os.path.basename(tga_path)}: "
        f"{orig/mb:.1f} MB tga -> {jpg/mb:.1f} MB .jpg "
        f"(ratio {ratio:.2f}x, q{quality})  -> {jpg_path}"
    )
    return True


def gather(paths, recursive):
    out = []
    for p in paths:
        if os.path.isdir(p):
            if recursive:
                for root, _dirs, files in os.walk(p):
                    for fn in files:
                        if fn.lower().endswith(".burnin.tga"):
                            out.append(os.path.join(root, fn))
            else:
                for fn in os.listdir(p):
                    if fn.lower().endswith(".burnin.tga"):
                        out.append(os.path.join(p, fn))
        else:
            out.append(p)
    return sorted(set(out))


def main(argv):
    ap = argparse.ArgumentParser(description="Compress *.burnin.tga -> *.burnin.jpg (lossy JPEG sidecar).")
    ap.add_argument("paths", nargs="+", help="*.burnin.tga files and/or directories to scan")
    ap.add_argument("--recursive", action="store_true", help="recurse into directories")
    ap.add_argument("--quality", type=int, default=90, help="JPEG quality (default 90)")
    args = ap.parse_args(argv)

    targets = gather(args.paths, args.recursive)
    if not targets:
        print("no *.burnin.tga targets found")
        return 1

    total_in = total_out = 0
    n = 0
    for t in targets:
        try:
            sz = os.path.getsize(t)
        except OSError:
            sz = 0
        if cook_one(t, args.quality):
            n += 1
            total_in += sz
            jp = (t[:-4] + ".jpg") if t.lower().endswith(".tga") else t + ".jpg"
            try:
                total_out += os.path.getsize(jp)
            except OSError:
                pass

    if n:
        mb = 1024.0 * 1024.0
        ratio = total_in / total_out if total_out else 0.0
        print(
            f"\n[total] {n} file(s): {total_in/mb:.1f} MB -> {total_out/mb:.1f} MB "
            f"(overall {ratio:.2f}x, saved {(total_in-total_out)/mb:.1f} MB)"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
