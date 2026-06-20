#!/usr/bin/env python3
"""
pull_compat_pilot_roster.py -- pull pilot roster CSV (+ referenced .fit files)
from a source MC2X/MCO install into the matching compat mod's data layer.

PROBLEM: MC2X/MCO campaigns that don't ship their own pilots.csv fall through
to base STOCK pilots.csv (only 52 entries).  Custom campaign pilots then fail
to resolve at runtime (LogisticsData::initPilots can't find them), so the
deployment screen is empty or crashes.

FIX: Inject pilots.csv (+ any referenced .fit files not already in the base)
into the compat mod's data/objects/ (CSV) and data/missions/profiles/ (.fits).
Campaigns using that compat inherit it via the VFS deps layer:
  active mod > mc2x-compat > base

SURGICAL: ONLY the pilot roster CSV + its referenced .fit files are written.
The full misc.fst is NOT overlaid (doing so regresses logistics — same class
of breakage as buildings.csv/object2.pak overlay, per transfer_mc2x_campaign.py
comments).

Usage:
  py -3 scripts/pull_compat_pilot_roster.py \\
      --source   A:/Games/mc2-opengl/MC2X-CVE-G \\
      --compat   A:/Games/mc2-opengl/releases/mc2-win64-v0.4d-rc1/mods/mc2x-compat \\
      --base-fst A:/Games/mc2-opengl/releases/mc2-win64-v0.4d-rc1/misc.fst \\
      [--pilots-csv-name mc2xpilots.csv]   # if source uses a non-standard name
      [--dry-run]

For MCO (loose data, no misc.fst):
  py -3 scripts/pull_compat_pilot_roster.py \\
      --source   A:/Games/mc2-opengl/MechCommanderOmnitech \\
      --compat   A:/Games/mc2-opengl/releases/mc2-win64-v0.4d-rc1/mods/mco-compat \\
      --base-fst A:/Games/mc2-opengl/releases/mc2-win64-v0.4d-rc1/misc.fst
"""
import argparse
import os
import shutil
import struct
import sys
import zlib

# ---------------------------------------------------------------------------
# Paths the engine uses (from mclib/paths.cpp):
#   objectPath  = "data/objects/"         -- pilots.csv lives here
#   warriorPath = "data/missions/profiles/" -- pilot .fit files live here
# Source installs store .fits under "data/missions/warriors/" (same content).
# ---------------------------------------------------------------------------

PILOTS_CSV_DEST  = os.path.join("data", "objects", "pilots.csv")
PILOT_FIT_SUBDIR = os.path.join("data", "missions", "profiles")

# Candidate subdirs under source data/ where pilot .fit files may live
WARRIOR_SUBDIRS  = [
    os.path.join("data", "missions", "warriors"),
    os.path.join("data", "missions", "profiles"),
]


# ---------------------------------------------------------------------------
# FST helpers (minimal, robust -- handles both CADDECAF zlib and FADDECAF
# installs that mis-report LZW but actually contain zlib, as seen in base misc.fst)
# ---------------------------------------------------------------------------

def _fst_entries(fst_path):
    """Yield (offset, comp_sz, real_sz, name, data) for each FST entry."""
    with open(fst_path, "rb") as f:
        data = f.read()
    _magic, count = struct.unpack_from("<II", data, 0)
    for i in range(count):
        base = 8 + i * 266
        off, comp, real, _h = struct.unpack_from("<IIII", data, base)
        name_raw = data[base + 16: base + 266]
        null = name_raw.find(b"\x00")
        name = name_raw[:null].decode("latin-1", "replace") if null >= 0 else ""
        yield off, comp, real, name, data


def _decompress(off, comp, real, data):
    """Decompress FST entry; handles both zlib and uncompressed."""
    raw = data[off: off + comp]
    if comp == real:
        return raw
    # Try zlib first (handles both CADDECAF and FADDECAF-zlib quirk)
    try:
        return zlib.decompress(raw)
    except Exception:
        pass
    # Fallback: try importing the project's fst module for LZW
    try:
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                        "..", "tools", "mc2x_import"))
        import fst as fst_mod
        return fst_mod.lzw_decomp(raw, real)
    except Exception as e:
        raise RuntimeError(f"Cannot decompress FST entry (comp={comp} real={real}): {e}")


# ---------------------------------------------------------------------------
# Core logic
# ---------------------------------------------------------------------------

def _norm(name):
    """Normalise path to forward-slash lowercase for key comparisons."""
    return name.replace("\\", "/").lower()


def load_base_pilots(base_fst_path):
    """Return set of lowercased pilot names from base stock misc.fst pilots.csv."""
    if not os.path.isfile(base_fst_path):
        print(f"  WARN: base misc.fst not found at {base_fst_path}; skipping base dedup")
        return set()
    for off, comp, real, name, data in _fst_entries(base_fst_path):
        if _norm(name) == "data/objects/pilots.csv":
            content = _decompress(off, comp, real, data)
            return set(l.strip().lower() for l in content.decode("latin-1").splitlines() if l.strip())
    print(f"  WARN: pilots.csv not found in {base_fst_path}")
    return set()


def find_pilots_csv_in_fst(fst_path, csv_name_hint=None):
    """
    Return (pilot_names: list[str], raw_content: bytes) from the source misc.fst.
    csv_name_hint: if the source uses a non-standard name (e.g. 'mc2xpilots.csv'),
    pass the bare filename.  If None, try 'pilots.csv' and 'mc2xpilots.csv'.
    """
    candidates = set()
    if csv_name_hint:
        candidates.add("data/objects/" + csv_name_hint.lower())
    else:
        candidates.update({"data/objects/pilots.csv", "data/objects/mc2xpilots.csv"})

    for off, comp, real, name, data in _fst_entries(fst_path):
        if _norm(name) in candidates:
            content = _decompress(off, comp, real, data)
            lines = content.decode("latin-1").splitlines()
            pilots = [l.strip() for l in lines if l.strip()]
            return pilots, content
    return None, None


def find_pilots_csv_loose(source_root):
    """Return (pilot_names: list[str], raw_bytes: bytes) from loose data/objects/pilots.csv."""
    path = os.path.join(source_root, "data", "objects", "pilots.csv")
    if not os.path.isfile(path):
        return None, None
    with open(path, "rb") as f:
        raw = f.read()
    lines = raw.decode("latin-1").splitlines()
    pilots = [l.strip() for l in lines if l.strip()]
    return pilots, raw


def find_pilot_fit_in_fst(fst_path, pilot_name):
    """Return decompressed bytes of <pilot>.fit from the given FST, or None."""
    target_bare = pilot_name.lower() + ".fit"
    for off, comp, real, name, data in _fst_entries(fst_path):
        n = _norm(name)
        if os.path.basename(n) == target_bare:
            return _decompress(off, comp, real, data)
    return None


def find_pilot_fit_loose(source_root, pilot_name):
    """Return (path, bytes) of <pilot>.fit from source loose data/, or (None, None)."""
    target_bare = pilot_name.lower() + ".fit"
    for subdir in WARRIOR_SUBDIRS:
        # Case-insensitive scan of the directory
        dirpath = os.path.join(source_root, subdir)
        if not os.path.isdir(dirpath):
            continue
        for fn in os.listdir(dirpath):
            if fn.lower() == target_bare:
                fp = os.path.join(dirpath, fn)
                with open(fp, "rb") as f:
                    return fp, f.read()
    return None, None


def pilot_fit_exists_in_compat(compat_root, pilot_name):
    """True if the compat mod already ships <pilot>.fit in its profiles dir."""
    target = os.path.join(compat_root, PILOT_FIT_SUBDIR, pilot_name.lower() + ".fit")
    # Also check case-insensitively
    profiles_dir = os.path.join(compat_root, PILOT_FIT_SUBDIR)
    if not os.path.isdir(profiles_dir):
        return False
    target_bare = pilot_name.lower() + ".fit"
    return any(fn.lower() == target_bare for fn in os.listdir(profiles_dir))


def pull_pilot_roster(source_root, compat_root, base_fst_path,
                      pilots_csv_name=None, dry_run=False):
    """
    Main entry point.  Returns dict with stats for reporting.
    """
    result = {
        "source": source_root,
        "compat": compat_root,
        "pilots_csv_written": False,
        "pilots_csv_path": None,
        "pilots_csv_differs_from_base": None,
        "total_in_roster": 0,
        "custom_vs_base": 0,
        "fits_pulled": 0,
        "fits_already_in_compat": 0,
        "fits_missing_in_source": 0,
        "roster_head": [],
        "fits_pulled_names": [],
        "source_misc_fst": None,
        "skipped_identical_to_base": False,
    }

    # ------------------------------------------------------------------
    # 1. Load base stock pilot set (for dedup / identity check)
    # ------------------------------------------------------------------
    base_pilots = load_base_pilots(base_fst_path)
    print(f"  Base stock pilots.csv: {len(base_pilots)} entries")

    # ------------------------------------------------------------------
    # 2. Find pilots.csv in source (FST first, then loose)
    # ------------------------------------------------------------------
    misc_fst = os.path.join(source_root, "misc.fst")
    roster_lines = None
    roster_raw = None

    if os.path.isfile(misc_fst):
        result["source_misc_fst"] = misc_fst
        print(f"  Found misc.fst: {misc_fst}")
        roster_lines, roster_raw = find_pilots_csv_in_fst(misc_fst, pilots_csv_name)
        if roster_lines is None:
            print("  WARN: pilots.csv not found in misc.fst; falling back to loose")

    if roster_lines is None:
        roster_lines, roster_raw = find_pilots_csv_loose(source_root)
        if roster_lines is None:
            print("  ERROR: No pilots.csv found in source (FST or loose)")
            return result
        print(f"  Found loose pilots.csv: {os.path.join(source_root, 'data', 'objects', 'pilots.csv')}")

    result["total_in_roster"] = len(roster_lines)
    result["roster_head"] = roster_lines[:8]

    # ------------------------------------------------------------------
    # 3. Identity check: if roster == base stock, skip (no custom pilots)
    # ------------------------------------------------------------------
    roster_set = set(l.lower() for l in roster_lines)
    custom_pilots = roster_set - base_pilots
    result["custom_vs_base"] = len(custom_pilots)
    result["pilots_csv_differs_from_base"] = bool(custom_pilots) or (len(roster_set) != len(base_pilots))

    if not result["pilots_csv_differs_from_base"]:
        print("  Roster is IDENTICAL to base stock — no custom pilots. Skipping write.")
        result["skipped_identical_to_base"] = True
        return result

    print(f"  Roster: {len(roster_lines)} pilots, {len(custom_pilots)} new vs base stock")

    # ------------------------------------------------------------------
    # 4. Write pilots.csv to compat data/objects/
    # ------------------------------------------------------------------
    dest_csv = os.path.join(compat_root, PILOTS_CSV_DEST)
    print(f"  Writing pilots.csv -> {dest_csv}")
    if not dry_run:
        os.makedirs(os.path.dirname(dest_csv), exist_ok=True)
        # Always write as pilots.csv (canonical name the engine looks for)
        with open(dest_csv, "wb") as f:
            f.write(roster_raw)
    result["pilots_csv_written"] = True
    result["pilots_csv_path"] = dest_csv

    # ------------------------------------------------------------------
    # 5. For each pilot in roster, pull its .fit if not in compat already
    # ------------------------------------------------------------------
    profiles_dest = os.path.join(compat_root, PILOT_FIT_SUBDIR)

    for pilot in roster_lines:
        pilot = pilot.strip()
        if not pilot:
            continue

        if pilot_fit_exists_in_compat(compat_root, pilot):
            result["fits_already_in_compat"] += 1
            continue

        # Try to pull from source FST
        fit_bytes = None
        if os.path.isfile(misc_fst):
            fit_bytes = find_pilot_fit_in_fst(misc_fst, pilot)

        # If not in misc.fst, try other FSTs and loose data
        if fit_bytes is None:
            _, fit_bytes = find_pilot_fit_loose(source_root, pilot)

        if fit_bytes is None:
            # Try mission.fst (MC2X stores .fits there under data/missions/warriors/)
            mission_fst = os.path.join(source_root, "mission.fst")
            if os.path.isfile(mission_fst):
                fit_bytes = find_pilot_fit_in_fst(mission_fst, pilot)

        if fit_bytes is None:
            print(f"  MISSING: {pilot}.fit not found in source")
            result["fits_missing_in_source"] += 1
            continue

        dest_fit = os.path.join(profiles_dest, pilot.lower() + ".fit")
        if not dry_run:
            os.makedirs(profiles_dest, exist_ok=True)
            with open(dest_fit, "wb") as f:
                f.write(fit_bytes)
        result["fits_pulled"] += 1
        result["fits_pulled_names"].append(pilot)

    return result


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        description="Pull pilot roster (pilots.csv + .fit files) from a source "
                    "MC2X/MCO install into the matching compat mod layer.")
    ap.add_argument("--source", required=True,
                    help="Source install root (has misc.fst and/or data/)")
    ap.add_argument("--compat", required=True,
                    help="Target compat mod root (e.g. …/mods/mc2x-compat)")
    ap.add_argument("--base-fst", required=True,
                    help="Base deploy misc.fst (for stock pilot dedup)")
    ap.add_argument("--pilots-csv-name", default=None,
                    help="Bare filename of the pilots CSV in source misc.fst if "
                         "non-standard (e.g. mc2xpilots.csv). Default: auto-detect.")
    ap.add_argument("--dry-run", action="store_true",
                    help="Report what would be done without writing any files")
    args = ap.parse_args()

    print(f"\n=== pull_compat_pilot_roster ===")
    print(f"  source : {args.source}")
    print(f"  compat : {args.compat}")
    print(f"  base   : {args.base_fst}")
    if args.dry_run:
        print("  DRY-RUN mode — no files will be written")
    print()

    r = pull_pilot_roster(
        source_root=args.source,
        compat_root=args.compat,
        base_fst_path=args.base_fst,
        pilots_csv_name=args.pilots_csv_name,
        dry_run=args.dry_run,
    )

    print()
    print("=== RESULT ===")
    if r["skipped_identical_to_base"]:
        print("  SKIP: roster identical to base stock — nothing written")
    else:
        print(f"  pilots.csv written : {r['pilots_csv_written']}")
        if r["pilots_csv_path"]:
            sz = os.path.getsize(r["pilots_csv_path"]) if os.path.isfile(r["pilots_csv_path"]) else "(dry-run)"
            print(f"  pilots.csv path    : {r['pilots_csv_path']}  ({sz} bytes)")
        print(f"  differs from base  : {r['pilots_csv_differs_from_base']}")
        print(f"  roster size        : {r['total_in_roster']} pilots")
        print(f"  custom vs base     : {r['custom_vs_base']}")
        print(f"  .fit already in compat : {r['fits_already_in_compat']}")
        print(f"  .fit pulled        : {r['fits_pulled']}")
        print(f"  .fit missing       : {r['fits_missing_in_source']}")
        print(f"  roster head        : {r['roster_head']}")
        if r["fits_pulled_names"]:
            print(f"  first 10 fits pulled: {r['fits_pulled_names'][:10]}")
    print()


if __name__ == "__main__":
    main()
