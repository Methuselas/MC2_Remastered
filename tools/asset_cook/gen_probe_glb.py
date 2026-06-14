#!/usr/bin/env python3
"""tools/asset_cook/gen_probe_glb.py — Animated prop GLB probe v1.

Generates a minimal probe .glb for a single animated prop, where ALL geometry
is placed under a node named --turret-node (matching the INI AnimationNodeId).
This proves the importer node-name contract without requiring per-node geometry
split (Gap 1 in animated-prop-cook-recon.md).

Accept criteria:
  - runtime GetNodeNameId(AnimationNodeId) returns >= 0 (node found)
  - runtime SetNodeRotation applies yaw to that node
  - geometry visible (not black), not frozen at T-pose
  - stock path unaffected when probe not installed

Usage:
  py -3 tools/asset_cook/gen_probe_glb.py
      <meshdump.json>
      <out.glb>
      [--turret-node Artillery_Turret]
      [--axes x,-z,y]

The axes default matches tglmeshdump_to_glb.py calibrated round-trip.
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

import numpy as np


def parse_axes(spec: str):
    cols = {"x": 0, "y": 1, "z": 2}
    plan = []
    for tok in spec.split(","):
        tok = tok.strip()
        sign = -1.0 if tok[0] == "-" else 1.0
        plan.append((cols[tok[-1]], sign))

    def fn(a):
        out = a.copy()
        for i, (src, sign) in enumerate(plan):
            out[:, i] = sign * a[:, src]
        return out
    return fn


def build_probe_glb(dump: dict, turret_node: str, axes: str = "x,-z,y") -> bytes:
    """Build a single-node GLB with all geometry under `turret_node`.

    Uses the same axis mapping and UV pass-through as tglmeshdump_to_glb.py so
    the geometry round-trips correctly through assimp_importer.cpp axisMap0.
    """
    pos_xform = parse_axes(axes)

    bin_parts: list[bytes] = []
    offset = 0
    bufferViews: list[dict] = []
    accessors: list[dict] = []
    primitives: list[dict] = []

    def add_view(data: bytes) -> int:
        nonlocal offset
        pad = (-len(data)) % 4
        idx = len(bufferViews)
        bufferViews.append({"buffer": 0, "byteOffset": offset, "byteLength": len(data)})
        bin_parts.append(data + b"\x00" * pad)
        offset += len(data) + pad
        return idx

    images: list[dict] = []
    textures: list[dict] = []
    materials: list[dict] = []

    for sm in dump["submeshes"]:
        verts_raw = sm.get("verts") or []
        if not verts_raw:
            continue

        verts = np.array(verts_raw, dtype=np.float64)
        pos = pos_xform(verts[:, 0:3])
        nrm = pos_xform(verts[:, 3:6])
        uv = verts[:, 6:8].copy()
        idx = np.array(sm["idx"], dtype=np.uint32)

        pos32 = pos.astype(np.float32)
        pv = add_view(pos32.tobytes())
        accessors.append({
            "bufferView": pv, "componentType": 5126,
            "count": len(pos32), "type": "VEC3",
            "min": pos32.min(axis=0).tolist(),
            "max": pos32.max(axis=0).tolist(),
        })
        a_pos = len(accessors) - 1

        nv = add_view(nrm.astype(np.float32).tobytes())
        accessors.append({
            "bufferView": nv, "componentType": 5126,
            "count": len(nrm), "type": "VEC3",
        })
        a_nrm = len(accessors) - 1

        uvv = add_view(uv.astype(np.float32).tobytes())
        accessors.append({
            "bufferView": uvv, "componentType": 5126,
            "count": len(uv), "type": "VEC2",
        })
        a_uv = len(accessors) - 1

        iv = add_view(idx.tobytes())
        accessors.append({
            "bufferView": iv, "componentType": 5125,
            "count": len(idx), "type": "SCALAR",
        })
        a_idx = len(accessors) - 1

        mat_idx = len(materials)
        tn = sm.get("textureName") or ""
        if tn and tn.upper() != "NULLTXM":
            img_idx = len(images)
            images.append({"uri": tn})
            tex_idx = len(textures)
            textures.append({"source": img_idx})
            is_alpha = tn[:2].lower() == "a_"
            materials.append({
                "name": Path(tn).stem,
                "alphaMode": "MASK" if is_alpha else "OPAQUE",
                "pbrMetallicRoughness": {"baseColorTexture": {"index": tex_idx}},
            })
        else:
            materials.append({"name": "NULLTXM"})

        primitives.append({
            "attributes": {"POSITION": a_pos, "NORMAL": a_nrm, "TEXCOORD_0": a_uv},
            "indices": a_idx,
            "mode": 4,
            "material": mat_idx,
        })

    if not primitives:
        raise ValueError("no geometry found in meshdump")

    binblob = b"".join(bin_parts)

    # Single node: all geometry under the turret node.
    # ValidateScene: node name <= 24 chars, no duplicates, >= 1 mesh.
    if len(turret_node) > 24:
        raise ValueError(f"turret_node name too long (>24): {turret_node!r}")

    gltf: dict = {
        "asset": {"version": "2.0", "generator": "mc2-animated-prop-probe-v1"},
        "scenes": [{"nodes": [0]}],
        "scene": 0,
        "nodes": [{"name": turret_node, "mesh": 0}],
        "meshes": [{"name": turret_node, "primitives": primitives}],
        "materials": materials,
        "buffers": [{"byteLength": len(binblob)}],
        "bufferViews": bufferViews,
        "accessors": accessors,
    }
    if images:
        gltf["images"] = images
        gltf["textures"] = textures

    js = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    js += b" " * ((-len(js)) % 4)
    binpad = binblob + b"\x00" * ((-len(binblob)) % 4)
    total = 12 + 8 + len(js) + 8 + len(binpad)

    out = bytearray()
    out += b"glTF" + struct.pack("<II", 2, total)
    out += struct.pack("<I", len(js)) + b"JSON" + js
    out += struct.pack("<I", len(binpad)) + b"BIN\x00" + binpad
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("meshdump", type=Path, help="<asset>.meshdump.json")
    ap.add_argument("out_glb", type=Path, help="output .glb path")
    ap.add_argument("--turret-node", default="Artillery_Turret",
                    help="node name matching INI AnimationNodeId (default: Artillery_Turret)")
    ap.add_argument("--axes", default="x,-z,y",
                    help="axis mapping (default: x,-z,y, calibrated for assimp_importer round-trip)")
    args = ap.parse_args()

    dump = json.loads(args.meshdump.read_text(encoding="utf-8"))
    glb = build_probe_glb(dump, args.turret_node, args.axes)
    args.out_glb.parent.mkdir(parents=True, exist_ok=True)
    args.out_glb.write_bytes(glb)

    nverts = sum(len(s.get("verts") or []) for s in dump["submeshes"])
    nprims = sum(1 for s in dump["submeshes"] if s.get("verts"))
    print(f"probe GLB: {args.out_glb}")
    print(f"  turret-node: {args.turret_node!r}")
    print(f"  primitives:  {nprims}  verts: {nverts}  bytes: {len(glb)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
