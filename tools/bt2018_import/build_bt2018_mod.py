#!/usr/bin/env python3
"""Build the BT2018Mechs mod: chassis-ini overlays that swap stock MC2 mechs to the
imported BT2018 GLB models (BT names) via additive [Import] blocks. Non-destructive —
shadows stock inis at mods/BT2018Mechs/data/tgl/; stock files untouched; [TGLData]
fallback kept intact so missing GLBs degrade to stock ASE.

Assignment:
  * 1:1  — BT model whose name matches an MC2 chassis name -> that chassis.
  * mismatch — BT model with no same-name MC2 chassis -> a random UNUSED MC2 mech
    chassis (deterministic via --seed; re-map by hand later).

GLBs/TGAs already deploy to stock data/tgl and resolve via fall-through, so the mod
carries only inis. Activate with MC2_MOD_DEPS=BT2018Mechs.

Usage:
  py -3 build_bt2018_mod.py --manifest MC2Conversions/conversions_manifest.json \
      --stock-tgl <release>/data/tgl --mod-root <release>/mods/BT2018Mechs [--seed 1337]
"""
import argparse
import glob
import json
import os
import random
import re

IMPORT_BLOCK = '[Import]\nst Source = "{src}"\n'


def ensure_import(text, src):
    if "[Import]" in text:
        return re.sub(r'st\s+Source\s*=\s*"[^"]*"', f'st Source = "{src}"', text, count=1)
    block = IMPORT_BLOCK.format(src=src)
    out, injected = [], False
    for ln in text.splitlines(keepends=True):
        out.append(ln)
        if not injected and ln.strip().upper().startswith("FITINI"):
            out.append("\n" + block + "\n")
            injected = True
    if not injected:
        out.insert(0, block + "\n")
    return "".join(out)


def mc2_mech_chassis(stock_tgl):
    """Real MC2 mechs = base names that have a <name>leftarm.ini sibling."""
    names = set()
    for p in glob.glob(os.path.join(stock_tgl, "*leftarm.ini")):
        names.add(os.path.basename(p)[:-len("leftarm.ini")].lower())
    return names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--stock-tgl", required=True)
    ap.add_argument("--mod-root", required=True)
    ap.add_argument("--seed", type=int, default=1337)
    args = ap.parse_args()

    with open(args.manifest, encoding="utf-8") as f:
        bt_models = [r["btname"] for r in json.load(f)]
    chassis = mc2_mech_chassis(args.stock_tgl)

    assignments = {}          # chassis -> bt source
    used = set()
    mismatched = []
    for bt in bt_models:
        if bt.lower() in chassis:                 # 1:1 name match
            assignments[bt.lower()] = bt
            used.add(bt.lower())
        else:
            mismatched.append(bt)

    free = sorted(chassis - used)
    rng = random.Random(args.seed)
    rng.shuffle(free)
    overflow = []
    for bt in mismatched:
        if free:
            assignments[free.pop()] = bt
        else:
            overflow.append(bt)

    mod_tgl = os.path.join(args.mod_root, "data", "tgl")
    os.makedirs(mod_tgl, exist_ok=True)
    with open(os.path.join(args.mod_root, "mod.json"), "w", encoding="utf-8") as f:
        json.dump({
            "schema": "mc2-mod/1", "id": "BT2018Mechs", "name": "BT2018 Mech Imports",
            "version": "1.0.0", "type": "assets",
            "description": "Swaps MC2 chassis to imported BT2018 GLB models (retargeted "
                           "clips). 1:1 where names match; mismatched mapped onto random "
                           "unused MC2 mechs (re-map in ASSIGNMENTS).",
            "dependencies": [],
        }, f, indent=2)

    one_to_one, random_swaps = [], []
    for chas, src in sorted(assignments.items()):
        spath = os.path.join(args.stock_tgl, f"{chas}.ini")
        if not os.path.isfile(spath):
            continue
        with open(spath, "r", encoding="latin-1") as f:
            text = f.read()
        with open(os.path.join(mod_tgl, f"{chas}.ini"), "w", encoding="latin-1") as f:
            f.write(ensure_import(text, src))
        (one_to_one if chas == src.lower() else random_swaps).append(f"{chas} -> {src}")

    print(f"[mod] 1:1 matches ({len(one_to_one)}):")
    for s in one_to_one: print("   " + s)
    print(f"[mod] random swaps ({len(random_swaps)}):")
    for s in random_swaps: print("   " + s)
    if overflow:
        print(f"[mod] OVERFLOW (no free chassis, not wired): {overflow}")
    print(f"[mod] total overlays: {len(one_to_one) + len(random_swaps)} -> {args.mod_root}")


if __name__ == "__main__":
    main()
