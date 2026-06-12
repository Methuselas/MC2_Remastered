#!/usr/bin/env python3
"""residency_slim.py - residency-aware slim deploy for data/tgl/128 (v1).

The blind fst_repack_drop.py dropped EVERY data/tgl/128/*.tga, including the
CPU_RGBA_REQUIRED mech/vehicle paint textures (*rgb) -> mechs rendered
untextured. This tool drops ONLY the tgas that are provably safe to remove
(GPU-sampled / static-prop, which load from the BC7 .ktx2 via route-2 +
the bdactor gate fix), and KEEPS every CPU-required source. Fails before
touching anything if it cannot classify a texture (unless --allow-unknown).

SCOPE v1: operates ONLY on <deploy>/data/tgl/128. data/art, terrain splat
mats (data/textures/mat*), and burnin colormaps are structurally outside /128
and are never touched here.

Classification (per data/tgl/128/*.tga):
  KEEP  cpu_required   name matches *rgb.tga  (mech/vehicle paint, gos_LockTexture)
  KEEP  no_ktx2        no same-stem .ktx2 exists (no GPU substitute)
  DROP  gpu_ktx2       same-stem .ktx2 exists AND not a KEEP rule
  unknown              (none under these rules; bucket reserved for future scope)

On --apply: repack tgl.fst dropping only the DROP set's data/tgl/128/*.tga
entries, and move the DROP-set loose tgas into <128>/_tga_slim_backup/.
--restore undoes both. Default action is --dry-run (prints the validator line,
changes nothing).
"""
import argparse
import os
import re
import struct
import sys

# Reuse the proven fst parse/rebuild from fst_repack_drop.py.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fst_repack_drop as fstlib  # parse(), ENTRY_SIZE

BACKUP_DIR = "_tga_slim_backup"
RGB_RE = re.compile(r"rgb\.tga$", re.IGNORECASE)


def classify(d128):
    """Return (drop_basenames:set, buckets:dict)."""
    tgas = [f for f in os.listdir(d128) if f.lower().endswith(".tga")]
    ktx_stems = {os.path.splitext(f)[0].lower()
                 for f in os.listdir(d128) if f.lower().endswith(".ktx2")}
    drop, keep_cpu, keep_noktx, unknown = set(), [], [], []
    for t in tgas:
        stem = os.path.splitext(t)[0]
        if RGB_RE.search(t):                       # mech/vehicle paint (CPU_RGBA_REQUIRED)
            keep_cpu.append(t)
        elif stem.lower() not in ktx_stems:        # no GPU substitute
            keep_noktx.append(t)
        else:                                      # has ktx2, not paint -> GPU-sampled, safe
            drop.add(t)
    return drop, {
        "drop": drop, "keep_cpu_required": keep_cpu,
        "keep_no_ktx2": keep_noktx, "unknown": unknown,
    }


def repack_fst(src_fst, dst_fst, drop_paths_lower):
    data = open(src_fst, "rb").read()
    version, entries = fstlib.parse(data)
    kept = [e for e in entries if e["name"].lower().replace("\\", "/") not in drop_paths_lower]
    dropped = len(entries) - len(kept)
    cur = 8 + len(kept) * fstlib.ENTRY_SIZE
    out = bytearray(struct.pack("<II", version, len(kept)))
    table, blob = bytearray(), bytearray()
    for e in kept:
        table += struct.pack("<IIII", cur, e["size"], e["real"], e["hash"])
        table += e["name_field"]; blob += e["payload"]; cur += e["size"]
    out += table + blob
    open(dst_fst, "wb").write(out)
    return len(entries), len(kept), dropped


def main():
    ap = argparse.ArgumentParser(description="residency-aware slim deploy for data/tgl/128 (v1)")
    ap.add_argument("deploy_root")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--dry-run", action="store_true", help="classify + print only (default)")
    g.add_argument("--apply", action="store_true", help="repack fst + move DROP tgas to backup")
    g.add_argument("--restore", action="store_true", help="undo a prior --apply")
    ap.add_argument("--allow-unknown", action="store_true")
    args = ap.parse_args()

    d128 = os.path.join(args.deploy_root, "data", "tgl", "128")
    fst = os.path.join(args.deploy_root, "tgl.fst")
    bk = os.path.join(d128, BACKUP_DIR)
    if not os.path.isdir(d128):
        print(f"FAIL: {d128} not found"); sys.exit(2)

    if args.restore:
        pre = fst + ".preslim"
        if os.path.isfile(pre):
            os.replace(pre, fst); print(f"[slim] restored tgl.fst")
        n = 0
        if os.path.isdir(bk):
            for f in os.listdir(bk):
                os.replace(os.path.join(bk, f), os.path.join(d128, f)); n += 1
            os.rmdir(bk)
        print(f"[slim] restored {n} tgas"); return

    drop, b = classify(d128)
    def sz(names): return sum(os.path.getsize(os.path.join(d128, n)) for n in names)
    drop_bytes = sz(drop)
    print("[slim] VALIDATOR:")
    print(f"  drop_count               = {len(drop)}")
    print(f"  drop_bytes               = {drop_bytes/1048576:.1f} MB")
    print(f"  keep_cpu_required_count  = {len(b['keep_cpu_required'])}")
    print(f"  keep_no_ktx2_count       = {len(b['keep_no_ktx2'])}")
    print(f"  keep_terrain_splat_count = 0   (out of /128 scope)")
    print(f"  keep_colormap_count      = 0   (out of /128 scope)")
    print(f"  unknown_count            = {len(b['unknown'])}")
    if b["unknown"] and not args.allow_unknown:
        print("FAIL: unknown textures present; re-run with --allow-unknown to override")
        sys.exit(1)

    if not args.apply:
        print("[slim] dry-run only (use --apply to repack/move).")
        return

    # --apply: residency-aware fst repack + loose move.
    drop_fst_keys = {f"data/tgl/128/{n.lower()}" for n in drop}
    if not os.path.isfile(fst + ".preslim"):
        import shutil; shutil.copyfile(fst, fst + ".preslim")
    tot, kept, dropped = repack_fst(fst + ".preslim", fst, drop_fst_keys)
    print(f"[slim] tgl.fst {tot} -> {kept} entries ({dropped} dropped)")
    os.makedirs(bk, exist_ok=True)
    for n in drop:
        os.replace(os.path.join(d128, n), os.path.join(bk, n))
    print(f"[slim] moved {len(drop)} loose tgas to {BACKUP_DIR}/ ; kept "
          f"{len(b['keep_cpu_required'])} cpu + {len(b['keep_no_ktx2'])} no-ktx2")


if __name__ == "__main__":
    main()
