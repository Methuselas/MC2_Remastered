#!/usr/bin/env python3
"""
build_mco_compat.py -- assemble the `mco-compat` dependency mod that lets
MechCommander Omnitech content packs (Wolf Dragoons, Volstand, Clan Eagle,
MercStar, Desert Fox, Day of Heroes, ...) run on this engine.

Like the MC2X importer, this ships only the recipe/logic; every real byte is
read from the USER's own installs at build time (no content redistribution).

mco-compat supplies exactly the three things MCO content needs that the MC2
base lacks -- and DELIBERATELY NOTHING ELSE:

  1. data/objects/object2.pak  -- a complete object table covering buildings.csv
     FitIDs up to ~2692 (the shipped MCO pak only has 1188 packets and ZERO mech
     packets). Built by build_object2.py: low slots verbatim from the user's MCO
     pak, high non-mech slots from the user's loose .fit, high mech slots from a
     generated BattleMechType stub whose ProfileName points at the user's <name>.csv.

  2. data/missions/{orders,miscfunc,corebrain}.abx  -- the ABL libraries the engine
     compiles at every mission start (mission.cpp ~2462). MCO warrior brains call
     library routines like `magicAttack` that live in corebrain. orders/miscfunc
     come from the user's MCO install. corebrain is special: MCO's own 2015
     corebrain uses a `range` type this engine's ABL compiler rejects ("Incompatible
     types range"), so we take the MC2-era corebrain (from --corebrain) which also
     defines magicAttack and compiles cleanly. TODO(engine): teach the ABL compiler
     MCO's `range` type to drop the --corebrain dependency.

  3. data/tgl/**  -- MCO mech appearance INIs + shapes (~302MB). Non-stock MCO mechs
     (chimera, awesome, firestarter, ...) have no appearance/shape in the MC2 base,
     so mech_recipe_build dereferences null and crashes silently at load. Mirrored
     from the user's MCO data/tgl.

WHY NOT "extract everything diff from stock"?  Tried it (2026-06-14): mirroring all
~992MB of MCO data/ that differs from the base REGRESSED every campaign. MCO's
data/art/buildings.csv overlaying the base re-routed logistics init into packets it
mis-parsed -> silent crash at load; MCO's data/missions stock content / other files
caused a play-time freeze. The FOCUSED set above makes all 6 tested campaign starts
PASS; adding the rest only introduced conflicts. Same lesson as the MC2X
delta-vs-oracle: ship the proven minimal set, don't get clever.

Verified (smoke, MC2_MOD_DEPS=mco-compat): outreach (Wolf Dragoons) / zhukov (Volstand)
/ doh_0 (Day of Heroes) / cfv2_mission1_escort (Clan Eagle) / galatea (MercStar) /
desfox1 (Desert Fox) campaign starts all PASS. The player selects campaign +
compatibility layer at launch; the launcher exports MC2_MOD_DEPS so no per-campaign
mod.json is needed.
"""
import argparse
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from build_object2 import build_object2  # noqa: E402

# ABL libraries taken verbatim from the user's MCO install (corebrain is overridden).
CORE_ABX_FROM_MCO = ["orders.abx", "miscfunc.abx"]


def copy_file(src, dst, label):
    if not os.path.isfile(src):
        print("  WARN: missing %s (%s)" % (label, src))
        return False
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)
    print("  %s: %s (%d bytes)" % (label, os.path.basename(dst), os.path.getsize(dst)))
    return True


def mirror_tree(src, dst, label):
    if not os.path.isdir(src):
        print("  WARN: missing dir %s (%s)" % (label, src))
        return 0
    os.makedirs(dst, exist_ok=True)
    n = 0
    for root, _, files in os.walk(src):
        rel = os.path.relpath(root, src)
        outdir = os.path.join(dst, rel) if rel != "." else dst
        os.makedirs(outdir, exist_ok=True)
        for f in files:
            shutil.copy2(os.path.join(root, f), os.path.join(outdir, f))
            n += 1
    print("  %s: %d files" % (label, n))
    return n


def main():
    ap = argparse.ArgumentParser(description="Build the mco-compat dependency mod from a user's MCO install.")
    ap.add_argument("--source", required=True, help="MCO install root (MechCommanderOmnitech)")
    ap.add_argument("--out", required=True, help="mco-compat mod dir to write (…/mods/mco-compat)")
    ap.add_argument("--corebrain", required=True,
                    help="compatible corebrain.abx (MC2-era; defines magicAttack, compiles cleanly). "
                         "e.g. <MC2-Exodus>/data/missions/corebrain.abx")
    ap.add_argument("--skip-tgl", action="store_true",
                    help="skip the ~302MB data/tgl mirror (objects/ABL only; non-stock MCO mechs will "
                         "crash at load — for fast iteration only)")
    args = ap.parse_args()

    src = args.source
    out = args.out
    if not os.path.isdir(os.path.join(src, "data", "objects")):
        sys.exit("not an MCO install (no data/objects): %s" % src)

    print("[1/4] object2.pak (covers buildings.csv FitIDs incl. mech stubs)")
    build_object2(src, os.path.join(out, "data", "objects", "object2.pak"))

    print("[2/4] ABL libraries from MCO install")
    for f in CORE_ABX_FROM_MCO:
        copy_file(os.path.join(src, "data", "missions", f),
                  os.path.join(out, "data", "missions", f), "abx")

    print("[3/4] corebrain.abx (compatible source)")
    copy_file(args.corebrain, os.path.join(out, "data", "missions", "corebrain.abx"), "corebrain")

    if args.skip_tgl:
        print("[4/4] data/tgl  -- SKIPPED (--skip-tgl)")
    else:
        print("[4/4] data/tgl mirror (appearances + shapes, large)")
        mirror_tree(os.path.join(src, "data", "tgl"), os.path.join(out, "data", "tgl"), "tgl")

    mod_json = os.path.join(out, "mod.json")
    if not os.path.isfile(mod_json):
        with open(mod_json, "w") as f:
            f.write('{ "id": "mco-compat", "name": "MCO Compatibility Base", "type": "dependency", "version": "1.0" }\n')
        print("wrote mod.json")

    cache = os.path.join(out, ".modindex-cache")
    if os.path.exists(cache):
        os.remove(cache)
    print("done: %s" % out)


if __name__ == "__main__":
    main()
