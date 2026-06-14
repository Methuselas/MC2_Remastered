#!/usr/bin/env python3
"""
build_object2.py -- build a complete MechCommander Omnitech object2.pak for the
mco-compat dependency mod, sourced from the USER's own MCO install.

WHY THIS EXISTS
---------------
MCO missions (e.g. Wolf Dragoons) place movers by ObjectNumber, which is a packet
index ("FitID") into data/objects/object2.pak. The authoritative name->FitID map is
data/art/buildings.csv (col "FitID"). In MCO that map references FitIDs up to ~2692,
but the object2.pak that ships in the install only has 1188 packets and contains NO
mech packets at all (every MECH row in buildings.csv has FitID >= 1188). So movers
like Atlas (1203) / Catapult (1213) fail with
    ObjectTypeManager.load: can't create object
because seekPacket(FitID) is out of range.

This tool reconstructs the full object2.pak the missions expect:
  * slots 0..1187      -> copied verbatim from the user's shipped object2.pak
  * high non-mech rows -> the loose data/objects/<Name>.fit from the user's install
  * high mech rows     -> a small generated BattleMechType stub FIT (we author it;
                          the real mech data stays in the user's <name>.csv, which the
                          engine reads via ProfileName at load time)
  * unused gap slots   -> NUL packets

NO MCO CONTENT IS REDISTRIBUTED: the recipe here is just the slot layout + a 12-line
mech stub template. All real bytes are read from the user's MCO install at build time.

PacketFile format (see mclib/packet.cpp / packet.h):
  int0 = 0xFEEDFACE (version, no checksum)
  int1 = firstPacketOffset = (2 + numPackets) * 4
  seekTable[i] (numPackets ints, starting at byte 8):
      entry = (offset & 0x1FFFFFFF) | (storageType << 29)
      storageType: 0=RAW 2=LZD 4=ZLIB 7=NUL
  then packet payloads concatenated, in slot order.
"""
import argparse
import os
import struct
import sys

PACKET_FILE_VERSION = 0xFEEDFACE
TYPE_SHIFT = 29
OFFSET_MASK = (1 << TYPE_SHIFT) - 1
RAW, LZD, ZLIB, NUL = 0, 2, 4, 7

# BATTLEMECH_TYPE discriminator (objtype.cpp switch, ObjectTypeNum=2 in shipped mech packets)
MECH_OBJECT_TYPE_NUM = 2


def read_pak(path):
    """Return (numPackets, [(storageType, payload_bytes_or_None)]) for a PacketFile."""
    d = open(path, "rb").read()
    first = struct.unpack_from("<I", d, 4)[0]
    n = first // 4 - 2
    tab = []
    for i in range(n):
        e = struct.unpack_from("<I", d, 8 + i * 4)[0]
        tab.append((e >> TYPE_SHIFT, e & OFFSET_MASK))
    slots = []
    for i in range(n):
        t, off = tab[i]
        nxt = len(d) if i + 1 == n else tab[i + 1][1]
        if t == NUL:
            slots.append((NUL, b""))
        else:
            slots.append((t, d[off:nxt]))
    return n, slots


def write_pak(path, slots):
    """slots: list of (storageType, payload_bytes). NUL payloads must be b''."""
    n = len(slots)
    first_offset = (2 + n) * 4
    table = []
    payloads = []
    cursor = first_offset
    for t, payload in slots:
        if t == NUL:
            # NUL: zero-length, offset points at current cursor (>0 so seekPacket==NO_ERR)
            table.append((cursor & OFFSET_MASK) | (NUL << TYPE_SHIFT))
        else:
            table.append((cursor & OFFSET_MASK) | (t << TYPE_SHIFT))
            payloads.append(payload)
            cursor += len(payload)
    with open(path, "wb") as f:
        f.write(struct.pack("<I", PACKET_FILE_VERSION))
        f.write(struct.pack("<I", first_offset))
        for e in table:
            f.write(struct.pack("<I", e))
        for p in payloads:
            f.write(p)


def mech_stub(name):
    """Authored BattleMechType template; real data lives in the user's <name>.csv."""
    return (
        "FITini\r\n\r\n"
        "[ObjectClass]\r\n"
        "l ObjectTypeNum = %d\r\n\r\n" % MECH_OBJECT_TYPE_NUM +
        "[ObjectType]\r\n"
        'st Name = "%s"\r\n' % name +
        "l ExplosionObject = 41\r\n"
        "l DestroyedObject = 101\r\n"
        "f ExtentRadius = 12\r\n"
        'st AppearanceName = "%s"\r\n\r\n' % name +
        "[MechProfile]\r\n"
        'st ProfileName = "%s"\r\n' % name +
        "l MoveType = 0\r\n"
        "b NormMech = TRUE\r\n"
        "f destructdamage = 45\r\n"
        "f explosiondamage = 20\r\n"
        "f explosionradius = 80\r\n\r\n"
        "FITEnd\r\n"
    ).encode("latin1")


def parse_buildings(path):
    """Yield (name, type_upper, fitid) for each data row of buildings.csv."""
    raw = open(path, "rb").read().replace(b"\x00", b"").decode("latin1")
    for ln, line in enumerate(raw.splitlines()):
        if ln == 0:
            continue  # header
        cols = line.split(",")
        if len(cols) < 5:
            continue
        name = cols[0].strip()
        typ = cols[3].strip().upper()
        fid_s = cols[4].strip()
        if not name or not fid_s.lstrip("-").isdigit():
            continue
        yield name, typ, int(fid_s)


def find_loose_fit(objects_dir, name):
    """Case-insensitive lookup of <name>.fit in the loose objects dir."""
    target = (name + ".fit").lower()
    for fn in os.listdir(objects_dir):
        if fn.lower() == target:
            return os.path.join(objects_dir, fn)
    return None


def build_object2(source, out, low_cut=1188):
    objects_dir = os.path.join(source, "data", "objects")
    base_pak = os.path.join(objects_dir, "object2.pak")
    buildings = os.path.join(source, "data", "art", "buildings.csv")
    for p in (base_pak, buildings):
        if not os.path.isfile(p):
            sys.exit("missing required source file: %s" % p)

    base_n, base_slots = read_pak(base_pak)
    print("shipped object2.pak: %d packets" % base_n)

    rows = list(parse_buildings(buildings))
    max_fit = max(f for _, _, f in rows)
    total = max(max_fit + 1, base_n)
    print("buildings.csv: %d rows, max FitID %d -> building %d-slot pak" % (len(rows), max_fit, total))

    # start with NUL everywhere, then fill
    slots = [(NUL, b"") for _ in range(total)]

    # 1) low slots verbatim from shipped pak
    low_kept = 0
    for i in range(min(low_cut, base_n)):
        slots[i] = base_slots[i]
        low_kept += 1

    # 2) high rows from buildings.csv
    n_mech = n_fit = n_missing = 0
    missing = []
    for name, typ, fid in rows:
        if fid < low_cut:
            continue  # covered by shipped low slots
        if fid >= total:
            continue
        if typ == "MECH":
            slots[fid] = (RAW, mech_stub(name))
            n_mech += 1
        else:
            fp = find_loose_fit(objects_dir, name)
            if fp:
                slots[fid] = (RAW, open(fp, "rb").read())
                n_fit += 1
            else:
                n_missing += 1
                missing.append("%s(%s,%d)" % (name, typ, fid))

    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    write_pak(out, slots)
    print("wrote %s" % out)
    print("  low verbatim: %d   high mech-stub: %d   high loose-fit: %d   missing: %d"
          % (low_kept, n_mech, n_fit, n_missing))
    if missing:
        print("  MISSING (NUL slots, may stub at runtime):", ", ".join(missing[:20]),
              "..." if len(missing) > 20 else "")

    # 3) roundtrip self-check: reopen + verify a few mech slots parse
    out_n, out_slots = read_pak(out)
    assert out_n == total, "roundtrip packet count mismatch %d != %d" % (out_n, total)
    checks = {1203: "Atlas", 1213: "Catapult", 1286: "ShaYu", 2027: "Infantry", 2066: "PoweredArmor"}
    for fid, nm in checks.items():
        if fid < out_n:
            t, payload = out_slots[fid]
            ok = payload and (nm.lower().encode() in payload.lower() or b"objectclass" in payload.lower())
            print("  check slot %d (%s): %s (%d bytes)" % (fid, nm, "OK" if ok else "??", len(payload)))
    print("roundtrip OK: %d packets" % out_n)


def main():
    ap = argparse.ArgumentParser(description="Build mco-compat object2.pak from a user's MCO install.")
    ap.add_argument("--source", required=True, help="MCO install root (has data/objects + data/art)")
    ap.add_argument("--out", required=True, help="output object2.pak path")
    ap.add_argument("--low-cut", type=int, default=1188,
                    help="slots below this come verbatim from the shipped pak (default 1188)")
    args = ap.parse_args()
    build_object2(args.source, args.out, args.low_cut)


if __name__ == "__main__":
    main()
