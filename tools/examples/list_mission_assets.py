"""
list_mission_assets.py  —  inventory every asset referenced in a MC2 mission .fit file.

Usage:
    python list_mission_assets.py  mc2_01.fit  [--buildings-csv path/to/Buildings.csv]

Output (stdout):
    A sorted table of appearance-name → count for all Part-based objects
    (mechs, vehicles, buildings), plus a summary of tree-group distributions
    used by Forest blocks.

Buildings.csv lookup (optional but recommended for tree names):
    Pass --buildings-csv pointing at  data/art/Buildings.csv  (or gui/Buildings.csv)
    inside a deployed game dir.  Without it, Part objects fall back to fitID only
    and tree group→name resolution is skipped.

.fit format notes:
    • Header line starts with "FITini"
    • Blocks:  [BlockName]
    • Values:  <type-prefix> Key = Value   (prefixes: ul, l, f, b, st, c …)
    • [Parts] / NumParts  →  [Part1]..[PartN]  each with ObjectNumber + CSVFile
    • [Forest1]..[ForestM]  each with TreeType0..TreeType14  (% weights, sum ≈ 100)
      TreeType index k  →  forest group id  k+1  (1-based)
"""

import argparse
import csv
import os
import re
import sys
from collections import Counter, defaultdict


# ---------------------------------------------------------------------------
# Minimal FitIni parser
# ---------------------------------------------------------------------------

def parse_fitini(path):
    """Return dict of block_name → dict of key → raw_value_string."""
    blocks = {}
    current = None
    try:
        with open(path, "r", encoding="latin-1") as fh:
            for raw in fh:
                line = raw.strip()
                if not line:
                    continue
                if line.startswith("[") and line.endswith("]"):
                    current = line[1:-1]
                    blocks.setdefault(current, {})
                    continue
                if current is None:
                    continue
                # strip type prefix (ul, l, f, b, st, c …) before the key
                m = re.match(r"^(?:[a-z]{1,3}\s+)?(\w+)\s*=\s*(.*)", line)
                if m:
                    key, val = m.group(1), m.group(2).strip()
                    # strip surrounding quotes if present
                    if val.startswith('"') and val.endswith('"'):
                        val = val[1:-1]
                    blocks[current][key] = val
    except OSError as e:
        print(f"[error] cannot open {path}: {e}", file=sys.stderr)
        sys.exit(1)
    return blocks


# ---------------------------------------------------------------------------
# Buildings.csv loader
# ---------------------------------------------------------------------------

def load_buildings_csv(path):
    """
    Returns two dicts built from Buildings.csv:
        fit_id_to_name  : int fitID  → str appearance name  (e.g. 252 → "Birch1")
        forest_group    : int groupID → list[str] appearance names  (e.g. 3 → ["Birch1","Birch2","Birch3"])
    """
    fit_id_to_name = {}
    forest_group = defaultdict(list)

    if not path or not os.path.exists(path):
        return fit_id_to_name, forest_group

    with open(path, newline="", encoding="latin-1") as fh:
        reader = csv.reader(fh)
        for i, row in enumerate(reader):
            if i == 0:
                continue  # header
            if len(row) < 6:
                continue
            name        = row[0].strip()
            obj_type    = row[3].strip().upper()   # TREE / BUILDING / …
            try:
                fit_id  = int(row[4].strip())
            except ValueError:
                continue
            special     = row[5].strip().upper()   # TREE3, TREE5, …

            fit_id_to_name[fit_id] = name

            if obj_type == "TREE" and special.startswith("TREE"):
                try:
                    gid = int(special[4:])
                    forest_group[gid].append(name)
                except ValueError:
                    pass

    return fit_id_to_name, forest_group


# ---------------------------------------------------------------------------
# Mission analysis
# ---------------------------------------------------------------------------

def analyse(fit_path, buildings_csv=None):
    blocks = parse_fitini(fit_path)
    fit_id_to_name, forest_group = load_buildings_csv(buildings_csv)

    # --- Part-based objects (mechs, vehicles, buildings, turrets …) ---

    num_parts = 0
    try:
        num_parts = int(blocks.get("Parts", {}).get("NumParts", 0))
    except ValueError:
        pass

    part_names = []  # list of (appearance_name, fit_id)
    for i in range(1, num_parts + 1):
        blk = blocks.get(f"Part{i}", {})
        csv_file = blk.get("CSVFile", "").strip()
        try:
            fit_id = int(blk.get("ObjectNumber", -1))
        except ValueError:
            fit_id = -1

        if csv_file:
            name = csv_file
        elif fit_id >= 0 and fit_id in fit_id_to_name:
            name = fit_id_to_name[fit_id]
        else:
            name = f"<fitID {fit_id}>" if fit_id >= 0 else "<unknown>"

        part_names.append((name, fit_id))

    part_counts = Counter(n for n, _ in part_names)

    # --- Forest blocks (tree distribution) ---

    forest_blocks = sorted(
        [k for k in blocks if re.match(r"^Forest\d+$", k)],
        key=lambda k: int(k[6:])
    )

    # For each forest, collect which tree groups have non-zero weight
    forest_summaries = []
    for fb in forest_blocks:
        blk = blocks[fb]
        used = {}  # groupId → weight
        for k in range(15):  # FOREST_TYPES = 15
            key = f"TreeType{k}"
            try:
                weight = float(blk.get(key, 0))
            except ValueError:
                weight = 0.0
            if weight > 0:
                gid = k + 1
                used[gid] = weight
        if used:
            forest_summaries.append((fb, used))

    return part_counts, forest_summaries, forest_group, num_parts, len(forest_blocks)


# ---------------------------------------------------------------------------
# Formatting
# ---------------------------------------------------------------------------

def print_report(fit_path, part_counts, forest_summaries, forest_group,
                 num_parts, num_forests):
    print(f"Mission: {os.path.basename(fit_path)}")
    print(f"  Parts (objects):  {num_parts}")
    print(f"  Forest blocks:    {num_forests}")
    print()

    # --- Part objects ---
    if part_counts:
        print("=== Spawned objects (Parts) ===")
        col = max(len(n) for n in part_counts) + 2
        for name, count in sorted(part_counts.items(), key=lambda x: -x[1]):
            print(f"  {name:<{col}}  {count:>4}x")
        print(f"  {'TOTAL':<{col}}  {sum(part_counts.values()):>4}x")
    else:
        print("  (no Parts found)")
    print()

    # --- Forest / tree distribution ---
    if forest_summaries:
        print("=== Forest tree distributions ===")
        for fb, used in forest_summaries:
            total_w = sum(used.values())
            parts_str = []
            for gid, weight in sorted(used.items()):
                pct = weight / total_w * 100 if total_w else 0
                names = forest_group.get(gid)
                if names:
                    nstr = "/".join(names)
                else:
                    nstr = f"group {gid}"
                parts_str.append(f"group{gid}({pct:.0f}%={nstr})")
            print(f"  {fb}: {', '.join(parts_str)}")

        # Aggregate: which tree groups appear at all
        all_groups = Counter()
        for _, used in forest_summaries:
            for gid in used:
                all_groups[gid] += 1
        print()
        print("  Tree groups used across all forests:")
        for gid, cnt in sorted(all_groups.items()):
            names = forest_group.get(gid, [])
            nstr = ", ".join(names) if names else f"<group {gid}>"
            print(f"    group {gid:>2}  ({cnt:>2} forest(s))  ->  {nstr}")
    else:
        print("  (no Forest blocks found)")
    print()


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description="List all assets referenced in a MC2 mission .fit file.")
    ap.add_argument("fit", help="Path to mission .fit file")
    ap.add_argument(
        "--buildings-csv",
        metavar="PATH",
        default=None,
        help="Path to data/art/Buildings.csv for name resolution (optional)",
    )
    args = ap.parse_args()

    # Auto-detect Buildings.csv from common deploy layouts relative to the .fit
    csv_path = args.buildings_csv
    if csv_path is None:
        fit_dir = os.path.dirname(os.path.abspath(args.fit))
        candidates = [
            os.path.join(fit_dir, "..", "..", "art", "Buildings.csv"),
            os.path.join(fit_dir, "..", "..", "art", "gui", "Buildings.csv"),
            os.path.join(fit_dir, "..", "art", "Buildings.csv"),
        ]
        for c in candidates:
            if os.path.exists(c):
                csv_path = os.path.normpath(c)
                print(f"[info] using Buildings.csv: {csv_path}", file=sys.stderr)
                break

    result = analyse(args.fit, csv_path)
    print_report(args.fit, *result)


if __name__ == "__main__":
    main()
