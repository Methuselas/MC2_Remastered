#!/usr/bin/env python3
"""
mc2 mod parity checker

Compares an old working flat-install data/ tree against what a selected
mod + its dependencies would overlay on top of the base install.

Usage:
    python parity_check.py
        --flat     <old-flat-install-root>   e.g. A:/Games/mc2-opengl/MC2-Exodus
        --base     <new-base-install-root>   e.g. A:/Games/mc2-opengl/mc2-win64-0.4c
        --mod      <mod-id>                  e.g. darkrain
        [--mods-dir <mods-dir>]              default: <base>/mods
        [--verbose]

Reports:
  MISSING    file in old flat data/ but not resolved by base+overlay
  SHADOW     file in mod overlay that shadows a base data/ file
  DUP        file appears in multiple mods in the active mod set
  EXTRA      file in mod overlay not present in old flat data/
  REF-MISS   path referenced inside a FIT/CSV/INI but not resolved
"""

import os, sys, json, hashlib, re, argparse
from pathlib import Path

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def normalize(p: str) -> str:
    return p.replace("\\", "/").lower()

def md5(path: str) -> str:
    h = hashlib.md5()
    try:
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(65536), b""):
                h.update(chunk)
        return h.hexdigest()
    except OSError:
        return ""

def build_index(root: str) -> dict[str, str]:
    """Walk root/data/ and return {normalized_rel_key: absolute_path}."""
    data_dir = os.path.join(root, "data")
    index: dict[str, str] = {}
    if not os.path.isdir(data_dir):
        return index
    for dirpath, _, filenames in os.walk(data_dir):
        for fn in filenames:
            abs_path = os.path.join(dirpath, fn)
            rel = os.path.relpath(abs_path, root)
            key = normalize(rel)
            index[key] = abs_path
    return index

def read_mod_json(mod_dir: str) -> dict:
    json_path = os.path.join(mod_dir, "mod.json")
    try:
        with open(json_path, "r", encoding="utf-8") as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError):
        return {}

def build_mod_overlay(mods_dir: str, mod_id: str) -> dict[str, tuple[str, str]]:
    """
    Return {normalized_key: (absolute_path, owning_mod_id)} for active mod
    plus its declared dependencies. Active mod wins over deps (first-wins
    because we index active first).
    """
    overlay: dict[str, tuple[str, str]] = {}
    dups: dict[str, list[str]] = {}

    def index_one(mid: str):
        mod_data = os.path.join(mods_dir, mid, "data")
        if not os.path.isdir(mod_data):
            print(f"  [warn] mod '{mid}' has no data/ folder, skipping")
            return
        for dirpath, _, filenames in os.walk(mod_data):
            for fn in filenames:
                abs_path = os.path.join(dirpath, fn)
                rel = os.path.relpath(abs_path, os.path.join(mods_dir, mid))
                key = normalize(rel)
                if key in overlay:
                    dups.setdefault(key, [overlay[key][1]]).append(mid)
                else:
                    overlay[key] = (abs_path, mid)

    mod_dir = os.path.join(mods_dir, mod_id)
    meta = read_mod_json(mod_dir)
    deps = meta.get("dependencies", [])

    index_one(mod_id)          # active mod first (highest priority)
    for dep in deps:
        index_one(dep)         # deps fill gaps

    return overlay, dups

# ---------------------------------------------------------------------------
# Reference scanner
# ---------------------------------------------------------------------------

REF_EXTENSIONS = {".fit", ".csv", ".ini", ".abl"}

def extract_refs(path: str) -> list[str]:
    """Heuristically extract data/ path references from text-format game files."""
    refs = []
    try:
        with open(path, "r", encoding="latin-1", errors="replace") as f:
            text = f.read()
        # Match anything that looks like data/... (case-insensitive)
        for m in re.finditer(r'data[/\\][^\s\'"<>|,;\r\n]{3,}', text, re.IGNORECASE):
            refs.append(normalize(m.group(0)))
    except OSError:
        pass
    return refs

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--flat",     required=True, help="Old flat-install root")
    ap.add_argument("--base",     required=True, help="New base-install root")
    ap.add_argument("--mod",      required=True, help="Mod ID to check (e.g. darkrain)")
    ap.add_argument("--mods-dir", default=None,  help="Path to mods/ dir (default: <base>/mods)")
    ap.add_argument("--verbose",  action="store_true")
    args = ap.parse_args()

    flat_root  = os.path.normpath(args.flat)
    base_root  = os.path.normpath(args.base)
    mods_dir   = args.mods_dir or os.path.join(base_root, "mods")
    mod_id     = args.mod
    verbose    = args.verbose

    print(f"[parity] flat   = {flat_root}")
    print(f"[parity] base   = {base_root}")
    print(f"[parity] mod    = {mod_id}")
    print(f"[parity] mods   = {mods_dir}")
    print()

    print("[parity] indexing flat install data/ ...")
    flat_index = build_index(flat_root)
    print(f"         {len(flat_index)} files")

    print("[parity] indexing base install data/ ...")
    base_index = build_index(base_root)
    print(f"         {len(base_index)} files")

    print(f"[parity] building mod overlay for '{mod_id}' ...")
    overlay, dups = build_mod_overlay(mods_dir, mod_id)
    print(f"         {len(overlay)} files in overlay, {len(dups)} duplicate keys")
    print()

    # Effective resolution: overlay wins, then base, else missing.
    def resolve(key: str) -> str | None:
        if key in overlay:
            return overlay[key][0]
        if key in base_index:
            return base_index[key]
        return None

    missing:  list[tuple[str, str]] = []   # (key, flat_path) in flat but not resolved
    shadow:   list[tuple[str, str, str]] = []  # (key, mod, base_path) shadowed by mod
    extra:    list[tuple[str, str]] = []   # (key, mod) in mod but not in flat
    dup_list: list[tuple[str, list[str]]] = sorted(dups.items())

    for key, flat_path in sorted(flat_index.items()):
        resolved = resolve(key)
        if resolved is None:
            missing.append((key, flat_path))
        elif verbose and resolved != flat_path and md5(resolved) != md5(flat_path):
            # File exists but content differs from flat install.
            pass  # counted below as shadowed if from overlay

    for key, (abs_path, mid) in sorted(overlay.items()):
        if key in base_index:
            shadow.append((key, mid, base_index[key]))
        if key not in flat_index:
            extra.append((key, mid))

    # Reference scanning: look for unresolved data/ refs in FIT/CSV/INI/ABL files.
    ref_missing: list[tuple[str, str, str]] = []   # (source_key, ref_key, source_abs)
    scan_sources = list(overlay.items()) + [(k, (v, "base")) for k, v in base_index.items()]
    ext_ok = REF_EXTENSIONS
    for key, (abs_path, mid) in scan_sources:
        if Path(key).suffix.lower() not in ext_ok:
            continue
        for ref in extract_refs(abs_path):
            if resolve(ref) is None:
                ref_missing.append((key, ref, abs_path))

    # ---------------------------------------------------------------------------
    # Report
    # ---------------------------------------------------------------------------
    print("=" * 70)
    print(f"MISSING  ({len(missing)})  — in flat data/ but not resolved by base+overlay")
    print("=" * 70)
    for key, fp in missing[:200]:
        print(f"  MISSING  {key}")
        if verbose:
            print(f"           flat={fp}")
    if len(missing) > 200:
        print(f"  ... {len(missing)-200} more")
    print()

    print("=" * 70)
    print(f"SHADOW   ({len(shadow)})  — mod file shadows a base data/ file")
    print("=" * 70)
    for key, mid, base_path in shadow[:200]:
        mod_path, _ = overlay[key]
        same = md5(mod_path) == md5(base_path)
        tag = "SAME" if same else "DIFF"
        print(f"  SHADOW [{tag}] [{mid}]  {key}")
        if verbose:
            print(f"           mod ={mod_path}")
            print(f"           base={base_path}")
    if len(shadow) > 200:
        print(f"  ... {len(shadow)-200} more")
    print()

    print("=" * 70)
    print(f"DUP      ({len(dup_list)})  — key in multiple mods in active set")
    print("=" * 70)
    for key, mids in dup_list[:100]:
        print(f"  DUP  {key}  owners={mids}")
    print()

    print("=" * 70)
    print(f"EXTRA    ({len(extra)})  — in mod overlay but not in old flat data/")
    print("=" * 70)
    for key, mid in extra[:200]:
        print(f"  EXTRA  [{mid}]  {key}")
    if len(extra) > 200:
        print(f"  ... {len(extra)-200} more")
    print()

    print("=" * 70)
    print(f"REF-MISS ({len(ref_missing)})  — references inside FIT/CSV/INI/ABL that don't resolve")
    print("=" * 70)
    seen_refs: set[str] = set()
    for src_key, ref_key, _ in ref_missing[:500]:
        if ref_key not in seen_refs:
            seen_refs.add(ref_key)
            print(f"  REF-MISS  {ref_key}")
            if verbose:
                print(f"            from {src_key}")
    print()

    print("=" * 70)
    total_issues = len(missing) + len(dup_list) + len(ref_missing)
    print(f"SUMMARY: {len(missing)} missing, {len(shadow)} shadows, {len(dup_list)} dups, "
          f"{len(extra)} extras, {len(ref_missing)} ref-misses")
    print(f"         Total actionable issues: {total_issues}")
    print("=" * 70)


if __name__ == "__main__":
    main()
