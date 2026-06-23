#!/usr/bin/env python3
"""Re-extract ALL albedo variants from each converted GLB and deploy as TGAs.

Skin variants (_BlackWidow/_BHA/_SLDF) embed BOTH a skin albedo (e.g.
chrTxrMech_warhammer-Widow-alb.png) AND the base (chrTxrMech_warhammer-Base-alb.png).
The engine derives the runtime tga name (deriveName = lowercase stem + .tga) from the
mesh MATERIAL's image, which for a skin is the skin texture. Deploying every non-blip
albedo guarantees the engine finds whichever its material references (no more black mesh).

Data-only: no FBX2glTF, no retarget, no rebuild.

Usage:
  py -3 extract_textures.py --manifest MC2Conversions/conversions_manifest.json \
      --conversions MC2Conversions --deploy <release>/data/tgl
"""
import argparse, io, json, os
from PIL import Image
from pygltflib import GLTF2


def img_bytes(g, im):
    bv = g.bufferViews[im.bufferView]
    blob = g.binary_blob()
    off = bv.byteOffset or 0
    return blob[off:off + bv.byteLength]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--conversions", required=True)
    ap.add_argument("--deploy", required=True)
    args = ap.parse_args()
    with open(args.manifest, encoding="utf-8") as f:
        mechs = [r["btname"] for r in json.load(f)]

    total = 0
    for bt in mechs:
        glb = os.path.join(args.conversions, f"{bt}.glb")
        if not os.path.isfile(glb):
            continue
        g = GLTF2().load(glb)
        wrote = []
        for im in (g.images or []):
            if not im.name:
                continue
            n = im.name.lower()
            if "alb" not in n or "blip" in n:
                continue
            tga = os.path.splitext(im.name)[0].lower() + ".tga"
            img = Image.open(io.BytesIO(img_bytes(g, im))).convert("RGBA")
            # Deploy to data/tgl/ AND data/tgl/128/. resetPaintScheme (mech3d.cpp) does a
            # fileExists() pre-check on data/tgl/<ObjectTextureSize>/<name> that does NOT
            # strip the size subdir; without the 128/ copy the texture is skipped -> black
            # mesh. (Runtime File::open strips, but the pre-check gates first.)
            img.save(os.path.join(args.deploy, tga))
            size_dir = os.path.join(args.deploy, "128")
            os.makedirs(size_dir, exist_ok=True)
            img.save(os.path.join(size_dir, tga))
            wrote.append(tga)
            total += 1
        print(f"[tex] {bt}: {wrote}")
    print(f"\n[tex] done: {total} albedo tgas deployed")


if __name__ == "__main__":
    main()
