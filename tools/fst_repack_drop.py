#!/usr/bin/env python3
"""fst_repack_drop.py - rebuild a MechCommander .fst FastFile archive with some
member entries removed, without recompressing (compressed payload blobs are
copied verbatim; only the entry table offsets are rebuilt).

Format (ground truth: mclib/ffile.h, tools/mod_install/fst_listing.py):
  offset 0  u32 version   (0xCADDECAF zlib / 0xFADDECAF LZSS)
  offset 4  u32 numFiles
  offset 8  FILEENTRY[numFiles], 266 bytes each, #pragma pack(1):
              u32 offset, u32 size(compressed), u32 realSize, u32 hash,
              char name[250]  (NUL-terminated, lowercase, forward slashes)
  then the (compressed) payloads at their offsets.

Use: drop every data/tgl/128/*.tga member (each has a .ktx2 the engine can
CPU-decode via route-2 MC2_TEXMGR_KTX_PRIMARY=1), so the redundant .tga can be
removed from both the loose tree and the archive.
"""
import struct
import sys

ENTRY_SIZE = 266
NAME_OFF = 16
NAME_LEN = 250


def parse(data):
    version, num = struct.unpack_from("<II", data, 0)
    entries = []
    for i in range(num):
        b = 8 + i * ENTRY_SIZE
        off, size, real, h = struct.unpack_from("<IIII", data, b)
        name_field = data[b + NAME_OFF:b + NAME_OFF + NAME_LEN]
        name = name_field.split(b"\0")[0].decode("latin1")
        entries.append({
            "name": name, "off": off, "size": size, "real": real,
            "hash": h, "name_field": name_field,
            "payload": bytes(data[off:off + size]),
        })
    return version, entries


def drop_pred(name):
    n = name.lower().replace("\\", "/")
    return n.startswith("data/tgl/128/") and n.endswith(".tga")


def repack(src, dst, dry_run=False):
    data = open(src, "rb").read()
    version, entries = parse(data)
    kept = [e for e in entries if not drop_pred(e["name"])]
    dropped = len(entries) - len(kept)
    dropped_bytes = sum(e["size"] for e in entries if drop_pred(e["name"]))
    print(f"[fst_repack] {src}: {len(entries)} entries -> {len(kept)} kept, "
          f"{dropped} dropped ({dropped_bytes/1048576:.1f} MB compressed payload)")
    if dry_run:
        return len(entries), len(kept)
    table_size = len(kept) * ENTRY_SIZE
    cur = 8 + table_size
    out = bytearray(struct.pack("<II", version, len(kept)))
    table = bytearray()
    blob = bytearray()
    for e in kept:
        table += struct.pack("<IIII", cur, e["size"], e["real"], e["hash"])
        table += e["name_field"]
        blob += e["payload"]
        cur += e["size"]
    out += table + blob
    open(dst, "wb").write(out)
    print(f"[fst_repack] wrote {dst} ({len(out)/1048576:.1f} MB, was "
          f"{len(data)/1048576:.1f} MB)")
    return len(entries), len(kept)


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("usage: fst_repack_drop.py <src.fst> <dst.fst> [--dry-run]")
        sys.exit(2)
    repack(sys.argv[1], sys.argv[2], "--dry-run" in sys.argv)
