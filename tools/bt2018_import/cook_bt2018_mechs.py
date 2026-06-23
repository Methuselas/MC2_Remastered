#!/usr/bin/env python3
"""BT2018 mech cook: produce deployable GLBs from the raw FBX2glTF source GLBs.

The 64 atlas_ animation clips ship in every mech's GLB authored as ABSOLUTE local
rotations for the ATLAS skeleton bind. A mech with a different bind (e.g. the
Marauder's deep reverse-joint legs) has its bind erased by rotationOnly sampling,
which inverts the legs under animation. This cook stage retargets every clip into
each mech's own bind frame (target_bind * conj(atlas_bind) * clip) so the shared
clips animate correctly on every chassis.

Source GLBs in BattleTech_2018_Dump are NEVER modified; retargeted copies are
written straight into the deploy data/tgl directory under their in-engine names.

The Atlas itself is the reference skeleton, so its retarget is a mathematical
no-op (atlas_bind == atlas_bind) — safe and included for completeness.

Usage:
  py -3 tools/bt2018_import/cook_bt2018_mechs.py \
      --dump  A:/Games/mc2-opengl/BattleTech_2018_Dump \
      --deploy A:/Games/mc2-opengl/releases/mc2-win64-v0.5.0/data/tgl
  (optional: --only MadCat,Archer   to cook a subset)
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "scripts"))
from bt2018_retarget_clips import retarget_file  # noqa: E402

# source GLB (relative to --dump)  ->  in-engine deploy name (<Name>.glb)
# Extend this as more BT2018 mechs are imported.
MANIFEST = [
    ("Archer/archer_fbx2gltf.glb",          "Archer"),
    ("Archer_BHA/archer_bha_fbx2gltf.glb",  "Archer_BHA"),
    ("Atlas/atlas_fbx2gltf.glb",            "Atlas"),      # reference: retarget = identity
    ("Awesome/awesome_fbx2gltf.glb",        "Awesome"),
    ("Banshee/banshee_fbx2gltf.glb",        "Banshee"),
    ("MadCat/marauder_fbx2gltf.glb",        "MadCat"),
]

ATLAS_REF = "Atlas/atlas_fbx2gltf.glb"  # canonical skeleton the clips were authored against


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump", required=True, help="BattleTech_2018_Dump root")
    ap.add_argument("--deploy", required=True, help="target data/tgl directory")
    ap.add_argument("--only", default="", help="comma list of deploy names to cook (default: all)")
    args = ap.parse_args()

    atlas_ref = os.path.join(args.dump, ATLAS_REF)
    if not os.path.isfile(atlas_ref):
        sys.exit(f"FATAL: atlas reference not found: {atlas_ref}")
    only = {s.strip() for s in args.only.split(",") if s.strip()}

    cooked, skipped = 0, 0
    for src_rel, name in MANIFEST:
        if only and name not in only:
            continue
        src = os.path.join(args.dump, src_rel)
        if not os.path.isfile(src):
            print(f"[cook] SKIP {name}: source missing ({src})")
            skipped += 1
            continue
        out = os.path.join(args.deploy, f"{name}.glb")
        retarget_file(src, atlas_ref, out)
        cooked += 1

    print(f"[cook] done: cooked={cooked} skipped={skipped}")


if __name__ == "__main__":
    main()
