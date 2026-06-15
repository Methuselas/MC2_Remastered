#!/usr/bin/env python3
"""mc2weapon — MechCommander 2 weapon viewer / modder tool.

VIEWER (this slice): read every component from compbas.csv, resolve each weapon's
FX binding via effects.csv, and present a unified weapon catalog (human table or
--json). Editor commands (set / new / validate, writing a loose mods/<id>/ overlay)
build on this same data model — see docs/weapon-tool-design.md.

Data model (recon: docs/weapon-tool-design.md):
  compbas.csv  — one row per component; masterID = row's first column. Weapon stats:
                 damage, heat, recycle(cooldown s), Weight(tons), crit-hits(slots),
                 Range bracket (short/medium/long), Special FX ID, Missile type, Fields.
  effects.csv  — FX table indexed by Special FX ID (skip 1 header line, then row N =
                 FX id N): effectName / muzzleFlash / hitEffect / missEffect / objNum.

Stats are CSV TEXT — no engine/pak needed. compbas.csv is packed in v0.4 but a loose
mods/<id>/data/objects/compbas.csv overlay works (the editor writes that). Weapons have
NO 3D mesh: their visual is a procedural bolt (texture + gosFX), so "model" = bolt
texture + FX, not a glb. See the design doc.
"""

import argparse
import csv
import json
import os
import sys

WEAPON_TYPES = {"energyweapon", "ballisticweapon", "missileweapon"}

# compbas.csv columns are matched by header text (prefix/contains) so the tool
# survives column drift. Each logical field -> a predicate over the header cell.
COMPBAS_FIELDS = [
    ("masterID", "component table"),
    ("type", "type"),
    ("name", "name"),
    ("slots", "crit hits"),
    ("recycle", "recycle"),
    ("heat", "heat"),
    ("tons", "weight"),
    ("damage", "damage"),
    ("br", "br"),
    ("rp", "rp"),
    ("range", "range"),
    ("missileType", "missile type"),
    ("fields", "fields"),
    ("fxid", "special fx id"),
    ("ammoMasterId", "ammo master id"),
]

# Candidate locations for the loose source CSVs (first existing wins).
COMPBAS_CANDIDATES = [
    "data/objects/compbas.csv",
    "A:/Games/mc2-opengl-src/mc2srcdata/objects/compbas.csv",
    "A:/Games/mc2-opengl/mc2-win64-v0.4/data/objects/compbas.csv",
    "A:/Games/mc2-opengl/mc2srcdata-fresh/objects/compbas.csv",
]
EFFECTS_CANDIDATES = [
    "data/objects/effects.csv",
    "A:/Games/mc2-opengl/mc2-win64-v0.4/data/objects/effects.csv",
    "A:/Games/mc2-opengl-src/mc2srcdata/objects/effects.csv",
]


def _find(path_arg, candidates, what):
    if path_arg:
        if not os.path.isfile(path_arg):
            sys.exit(f"mc2weapon: --{what} not found: {path_arg}")
        return path_arg
    for c in candidates:
        if os.path.isfile(c):
            return c
    sys.exit(f"mc2weapon: could not locate {what}.csv; pass --{what} <path>. "
             f"Tried: {', '.join(candidates)}")


def _col_index(header, needle):
    """First header cell whose lowercased text starts with / contains needle."""
    nl = needle.lower()
    for i, h in enumerate(header):
        hl = h.strip().lower()
        if hl.startswith(nl) or nl in hl:
            return i
    return -1


def load_compbas(path):
    with open(path, newline="", encoding="latin-1") as f:
        rows = list(csv.reader(f))
    if not rows:
        sys.exit(f"mc2weapon: empty compbas: {path}")
    header = rows[0]
    idx = {field: _col_index(header, needle) for field, needle in COMPBAS_FIELDS}
    comps = []
    for r in rows[1:]:
        if not r or not r[0].strip():
            continue

        def get(field):
            i = idx.get(field, -1)
            return r[i].strip() if 0 <= i < len(r) else ""

        comps.append({field: get(field) for field, _ in COMPBAS_FIELDS})
    return comps, idx, header


def load_effects(path):
    """Returns dict: fxid (int) -> {name, muzzle, hit, miss, objNum, weaponName}.
    Loader semantics: skip 1 header line, then data row position == FX id."""
    with open(path, newline="", encoding="latin-1") as f:
        rows = list(csv.reader(f))
    fx = {}
    for fxid, r in enumerate(rows[1:]):  # row 0 = header; row 1 -> fxid 0
        if not r:
            continue
        def cell(i):
            return r[i].strip() if i < len(r) else ""
        fx[fxid] = {
            "name": cell(1), "muzzle": cell(2), "hit": cell(3),
            "objNum": cell(4), "miss": cell(5),
            "weaponName": cell(6) if len(r) > 6 else "",
        }
    return fx


def is_weapon(comp):
    return comp.get("type", "").strip().lower() in WEAPON_TYPES


def build_catalog(compbas_path, effects_path, weapons_only=True):
    comps, _, _ = load_compbas(compbas_path)
    fx = load_effects(effects_path)
    out = []
    for c in comps:
        if weapons_only and not is_weapon(c):
            continue
        entry = dict(c)
        entry["isWeapon"] = is_weapon(c)
        fxid_s = c.get("fxid", "")
        try:
            fxid = int(float(fxid_s))
        except (ValueError, TypeError):
            fxid = None
        entry["fxResolved"] = fx.get(fxid) if fxid is not None else None
        out.append(entry)
    return out, fx


def cmd_list(args):
    cat, _ = build_catalog(args.compbas, args.effects, weapons_only=not args.all)
    if args.json:
        print(json.dumps(cat, indent=2))
        return
    hdr = f"{'ID':>3}  {'Name':<28} {'Type':<15} {'Dmg':>6} {'Heat':>5} " \
          f"{'Recyc':>6} {'Range':<7} {'Tons':>5} {'Slot':>4} {'FX':>3} {'FX name':<18}"
    print(hdr)
    print("-" * len(hdr))
    for e in cat:
        fxname = (e["fxResolved"] or {}).get("name", "") if e["fxResolved"] else ""
        print(f"{e['masterID']:>3}  {e['name'][:28]:<28} {e['type'][:15]:<15} "
              f"{e['damage']:>6} {e['heat']:>5} {e['recycle']:>6} "
              f"{e['range'][:7]:<7} {e['tons']:>5} {e['slots']:>4} {e['fxid']:>3} "
              f"{fxname[:18]:<18}")
    print(f"\n{len(cat)} {'component' if args.all else 'weapon'}(s).")


def cmd_show(args):
    cat, fx = build_catalog(args.compbas, args.effects, weapons_only=False)
    q = args.weapon.strip().lower()
    hit = None
    for e in cat:
        if e["masterID"] == args.weapon or e["name"].strip().lower() == q:
            hit = e
            break
    if hit is None:
        # substring fallback
        matches = [e for e in cat if q in e["name"].lower()]
        if len(matches) == 1:
            hit = matches[0]
        elif len(matches) > 1:
            print("Ambiguous; matches:")
            for m in matches:
                print(f"  [{m['masterID']}] {m['name']}")
            return
    if hit is None:
        sys.exit(f"mc2weapon: no component matching '{args.weapon}'")
    if args.json:
        print(json.dumps(hit, indent=2))
        return
    print(f"[{hit['masterID']}] {hit['name']}  ({hit['type']})")
    for k in ("damage", "heat", "recycle", "range", "tons", "slots", "br", "rp",
              "missileType", "fields", "ammoMasterId"):
        print(f"  {k:<13}: {hit[k]}")
    print(f"  {'fxid':<13}: {hit['fxid']}")
    r = hit["fxResolved"]
    if r:
        print(f"  FX -> trail={r['name']}  muzzle={r['muzzle']}  hit={r['hit']}  "
              f"miss={r['miss']}  objNum={r['objNum']}")
    else:
        print(f"  FX -> (Special FX ID {hit['fxid']} not found in effects.csv)")


def main(argv=None):
    p = argparse.ArgumentParser(prog="mc2weapon",
                                description="MC2 weapon viewer / modder tool")
    p.add_argument("--compbas", help="path to compbas.csv (auto-detected if omitted)")
    p.add_argument("--effects", help="path to effects.csv (auto-detected if omitted)")
    sub = p.add_subparsers(dest="cmd", required=True)

    pl = sub.add_parser("list", help="list weapons (or --all components) with stats + FX")
    pl.add_argument("--all", action="store_true", help="include non-weapon components")
    pl.add_argument("--json", action="store_true", help="emit JSON")
    pl.set_defaults(func=cmd_list)

    ps = sub.add_parser("show", help="show one component's full stats + resolved FX")
    ps.add_argument("weapon", help="masterID or name (substring ok)")
    ps.add_argument("--json", action="store_true", help="emit JSON")
    ps.set_defaults(func=cmd_show)

    args = p.parse_args(argv)
    args.compbas = _find(args.compbas, COMPBAS_CANDIDATES, "compbas")
    args.effects = _find(args.effects, EFFECTS_CANDIDATES, "effects")
    args.func(args)


if __name__ == "__main__":
    main()
