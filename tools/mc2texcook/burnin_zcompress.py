#!/usr/bin/env python3
"""BURNIN-DISK-COMPRESS cook tool.

Shrinks the terrain burn-in colormaps on disk by storing each
<name>.burnin.tga (5120x5120 RGBA, ~108MB) as a zlib-compressed
<name>.burnin.z sidecar. The engine (mclib/terrtxm2.cpp) inflates the
sidecar back to the EXACT original .tga file bytes on load and feeds them
into the existing TGA parser, so the result is fully lossless and
byte-identical to loading the .tga directly.

Sidecar format (.burnin.z):
    bytes [0..7]  : little-endian uint64 uncompressedSize (== original .tga size)
    bytes [8..]   : zlib.compress(raw .tga file bytes, level 9)

We compress the WHOLE .tga file bytes (header + pixels) — simplest, and it
reuses the engine's TGA parser on inflate.

Usage:
    python burnin_zcompress.py <file_or_dir> [more ...]

  * A file argument ending in .burnin.tga is cooked directly.
  * A directory argument is scanned for *.burnin.tga (non-recursive by
    default; pass --recursive to recurse).

The original .tga is NOT deleted — verify identical-render, then delete the
.tga yourself to realize the disk win.
"""

import argparse
import os
import struct
import sys
import zlib


def cook_one(tga_path: str, level: int = 9) -> bool:
    if not os.path.isfile(tga_path):
        print(f"[skip] not a file: {tga_path}")
        return False

    with open(tga_path, "rb") as f:
        raw = f.read()

    uncompressed_size = len(raw)
    compressed = zlib.compress(raw, level)

    z_path = tga_path[:-4] + ".z" if tga_path.lower().endswith(".tga") else tga_path + ".z"
    with open(z_path, "wb") as f:
        f.write(struct.pack("<Q", uncompressed_size))  # 8-byte LE uint64 header
        f.write(compressed)

    z_size = 8 + len(compressed)
    ratio = uncompressed_size / z_size if z_size else 0.0
    mb = 1024.0 * 1024.0
    print(
        f"[cook] {os.path.basename(tga_path)}: "
        f"{uncompressed_size/mb:.1f} MB tga -> {z_size/mb:.1f} MB .z "
        f"(ratio {ratio:.2f}x)  -> {z_path}"
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
    ap = argparse.ArgumentParser(description="Compress *.burnin.tga -> *.burnin.z (lossless zlib sidecar).")
    ap.add_argument("paths", nargs="+", help="*.burnin.tga files and/or directories to scan")
    ap.add_argument("--recursive", action="store_true", help="recurse into directories")
    ap.add_argument("--level", type=int, default=9, help="zlib compression level (default 9)")
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
        if cook_one(t, args.level):
            n += 1
            total_in += sz
            zp = t[:-4] + ".z"
            try:
                total_out += os.path.getsize(zp)
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
