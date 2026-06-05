#!/usr/bin/env python3
"""tools/asset_cook/tglmeshdump_to_glb.py — Track G path B: stock meshdump -> glb.

Input: a meshdump.json from the asset-viewer `--export-tgl-meshdump` (MeshData in
the workbench's GL space). Output: a .glb authored so the OVERRIDE engine importer
(assimp_importer.cpp axisMap0 `(-x,-y,z)` + auto-ground + UV `1-v`) round-trips it
BACK to the stock GL geometry — i.e. the cooked building renders matching stock.

Inversion applied (so importer undoes it):
  position: (px,py,pz) -> (-px, -py, pz)      [axisMap0 is its own inverse on x,y]
  normal:   (nx,ny,nz) -> (-nx, -ny, nz)
  uv:       (u, v)     -> (u, 1 - v)          [importer re-applies 1-v]

Ground (importer GROUND=2) re-bases Y to 0 — desirable for a building. So XZ
footprint is exact; Y is re-grounded.

  py -3 tools/asset_cook/tglmeshdump_to_glb.py <meshdump.json> <out.glb>
"""
from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

import numpy as np


def build_glb(dump: dict) -> bytes:
    bin_parts: list[bytes] = []
    offset = 0
    bufferViews, accessors, primitives = [], [], []

    def add_view(data: bytes) -> int:
        nonlocal offset
        # 4-byte align
        pad = (-len(data)) % 4
        idx = len(bufferViews)
        bufferViews.append({"buffer": 0, "byteOffset": offset, "byteLength": len(data)})
        bin_parts.append(data + b"\x00" * pad)
        offset += len(data) + pad
        return idx

    for sm in dump["submeshes"]:
        verts = np.array(sm["verts"], dtype=np.float64)  # [N,8] px..nz,u,v
        if verts.size == 0:
            continue
        pos = verts[:, 0:3].copy()
        nrm = verts[:, 3:6].copy()
        uv = verts[:, 6:8].copy()
        # axis/uv inversion so the importer reconstructs stock GL space
        pos[:, 0] *= -1.0; pos[:, 1] *= -1.0
        nrm[:, 0] *= -1.0; nrm[:, 1] *= -1.0
        uv[:, 1] = 1.0 - uv[:, 1]
        idx = np.array(sm["idx"], dtype=np.uint32)

        pos32 = pos.astype(np.float32)
        pv = add_view(pos32.tobytes())
        accessors.append({"bufferView": pv, "componentType": 5126, "count": len(pos32),
                          "type": "VEC3", "min": pos32.min(axis=0).tolist(),
                          "max": pos32.max(axis=0).tolist()})
        a_pos = len(accessors) - 1
        nv = add_view(nrm.astype(np.float32).tobytes())
        accessors.append({"bufferView": nv, "componentType": 5126, "count": len(nrm), "type": "VEC3"})
        a_nrm = len(accessors) - 1
        uvv = add_view(uv.astype(np.float32).tobytes())
        accessors.append({"bufferView": uvv, "componentType": 5126, "count": len(uv), "type": "VEC2"})
        a_uv = len(accessors) - 1
        iv = add_view(idx.tobytes())
        accessors.append({"bufferView": iv, "componentType": 5125, "count": len(idx), "type": "SCALAR"})
        a_idx = len(accessors) - 1

        mat = len(primitives)
        primitives.append({"attributes": {"POSITION": a_pos, "NORMAL": a_nrm, "TEXCOORD_0": a_uv},
                           "indices": a_idx, "mode": 4, "material": mat})

    binblob = b"".join(bin_parts)
    materials = [{"name": (sm.get("textureName") or "NULLTXM")} for sm in dump["submeshes"] if sm["verts"]]
    gltf = {
        "asset": {"version": "2.0", "generator": "trackg tglmeshdump_to_glb.v1"},
        "scenes": [{"nodes": [0]}], "scene": 0,
        "nodes": [{"mesh": 0, "name": Path(dump.get("tgl", "asset")).stem}],
        "meshes": [{"primitives": primitives}],
        "materials": materials,
        "buffers": [{"byteLength": len(binblob)}],
        "bufferViews": bufferViews,
        "accessors": accessors,
    }
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
    if len(sys.argv) != 3:
        print(__doc__); return 2
    dump = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
    glb = build_glb(dump)
    Path(sys.argv[2]).write_bytes(glb)
    nverts = sum(len(s["verts"]) for s in dump["submeshes"])
    print(f"GLB {sys.argv[2]}  {len(dump['submeshes'])} prim, {nverts} verts, {len(glb)} bytes "
          f"(axis-inverted for importer round-trip)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
