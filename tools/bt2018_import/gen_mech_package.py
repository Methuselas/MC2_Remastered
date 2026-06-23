#!/usr/bin/env python3
"""BT2018-MECH-NODE-MANIFEST-1A — generate per-mech bt2018_mech_package.json.

First tenant of the asset-package layer: declares the MC2 semantic node -> source
joint map the importer consumes to resolve weapon firepoints + hit nodes (read-only
bone-world lookup; no FK/clip/shader/gameplay change). Default map is constant (all
BT mechs share the Atlas rig) but every file is per-mech, so it is override-capable —
edit the json (or DEFAULT_NODES here) for a mech whose rig differs, without engine code.

Writes MC2Conversions/<bt>/bt2018_mech_package.json and deploys the engine sidecar
<deploy>/<bt>.package.json (the importer reads <glbstem>.package.json next to the GLB).
Validates each source joint exists in that mech's GLB; WARNs (does not fail) on miss.

Usage:
  py -3 gen_mech_package.py --manifest MC2Conversions/conversions_manifest.json \
      --conversions MC2Conversions --deploy <release>/data/tgl
"""
import argparse
import json
import os
from pygltflib import GLTF2

# MC2 semantic node name -> source joint (Atlas rig). Override per-mech in the json.
DEFAULT_NODES = {
    "joint_torso":        "j_Spine2",
    # j_LHand / j_RHand are nodes in the GLB but the runtime bone list only
    # contains joints actually used by skinned meshes; hands are unskinned in
    # these rigs. j_LForearm / j_RForearm are in-skin and give a plausible
    # arm-tip fire point.
    "weapon_rightarm":    "j_RForearm",
    "weapon_leftarm":     "j_LForearm",
    "weapon_righttorso":  "j_RClavicle",
    "weapon_lefttorso":   "j_LClavicle",
    "weapon_centertorso": "j_Spine2",
    "cockpit":            "j_COCKPIT",
    "hit_left":           "j_LClavicle",
    "hit_right":          "j_RClavicle",
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--conversions", required=True)
    ap.add_argument("--deploy", required=True)
    args = ap.parse_args()

    with open(args.manifest, encoding="utf-8") as f:
        mechs = [r["btname"] for r in json.load(f)]

    total_missing = 0
    for bt in mechs:
        glb = os.path.join(args.conversions, f"{bt}.glb")
        if not os.path.isfile(glb):
            print(f"[pkg] SKIP {bt}: glb missing")
            continue
        g = GLTF2().load(glb)
        # Validate against the SKIN bone set, NOT every glTF node — the engine's
        # mc2skel::BuildSkeleton only records joints actually used by skinned
        # meshes (assimp_importer.cpp `e.names`). Nodes outside any skin
        # (e.g. j_LHand on these rigs) will pass a node-list check but fail at
        # runtime resolution.
        joints = set()
        for sk in (g.skins or []):
            for ji in sk.joints:
                n = g.nodes[ji]
                if n.name:
                    joints.add(n.name)
        nodes = dict(DEFAULT_NODES)
        missing = sorted({j for j in nodes.values() if j not in joints})
        if missing:
            total_missing += 1
            print(f"[pkg] WARN {bt}: source joints absent in rig: {missing}")
        pkg = {
            "schema": "bt2018-mech-package/1",
            "chassisId": bt,
            "sourceGlb": f"{bt}.glb",
            "nodes": nodes,
        }
        # source-of-truth copy next to the conversion
        asset_dir = os.path.join(args.conversions, bt)
        os.makedirs(asset_dir, exist_ok=True)
        text = json.dumps(pkg, indent=2)
        with open(os.path.join(asset_dir, "bt2018_mech_package.json"), "w", encoding="utf-8") as f:
            f.write(text)
        # engine sidecar: <deploy>/<bt>.package.json (read as <glbstem>.package.json)
        with open(os.path.join(args.deploy, f"{bt}.package.json"), "w", encoding="utf-8") as f:
            f.write(text)
        print(f"[pkg] {bt}: {len(nodes)} nodes" + (" (with warnings)" if missing else ""))

    print(f"\n[pkg] done: {len(mechs)} mechs, {total_missing} with missing-joint warnings")


if __name__ == "__main__":
    main()
