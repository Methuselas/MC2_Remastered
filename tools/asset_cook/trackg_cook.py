#!/usr/bin/env python3
"""tools/asset_cook/trackg_cook.py — Track G offline static-prop cook driver.

Subcommands:
  stage   source.glb -> cooked.glb (convention-frozen) + staged.json geometry fragment

`stage` computes geometry (bounds/pivot/counts) in MC2 runtime-importer space by
REPLICATING mclib/assimp_importer.cpp's default-env transform — NOT the workbench
GlbMeshLoader (which uses a different convention; see the R0 convention note). The
cooked glb for an already-default-correct source (e.g. bigbox) is a passthrough copy;
meshopt optimization is a later add. Material *discovery* here is names + alphaClass
only; KTX2 texture cook is G2.

Frozen runtime convention (assimp_importer.cpp, default env):
  axisMap(0): X=-x, Y=-y, Z=z      (assimp_importer.cpp:60-69)
  toMC2Pos:   (X, Y + YOFF(0), Z)  (:71)
  auto-ground GROUND=2: dy = -minBox.y, translate Y so base sits at 0 (:544)

Does NOT cook textures, build GL state, write models.json, or touch the engine.

Usage:
  py -3 tools/asset_cook/trackg_cook.py stage <source.glb> <out_dir> \
       --id bigbox --class staticprop --appearance hangar
"""
from __future__ import annotations

import argparse
import json
import math
import shutil
import struct
import sys
from pathlib import Path

import numpy as np

# ---- glTF accessor decode -------------------------------------------------

_COMP = {5120: ("b", 1), 5121: ("B", 1), 5122: ("h", 2), 5123: ("H", 2),
         5125: ("I", 4), 5126: ("f", 4)}
_NCOMP = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}


def read_glb(path: Path):
    d = path.read_bytes()
    if d[:4] != b"glTF":
        raise ValueError(f"{path}: not a GLB (magic {d[:4]!r})")
    off, js, binblob = 12, None, b""
    while off < len(d):
        clen, ctype = struct.unpack_from("<I4s", d, off)
        off += 8
        chunk = d[off:off + clen]
        off += clen
        if ctype == b"JSON":
            js = json.loads(chunk)
        elif ctype == b"BIN\x00":
            binblob = chunk
    if js is None:
        raise ValueError(f"{path}: no JSON chunk")
    return js, binblob


def accessor_array(js, binblob, idx) -> np.ndarray:
    acc = js["accessors"][idx]
    bv = js["bufferViews"][acc["bufferView"]]
    comp, csize = _COMP[acc["componentType"]]
    n = _NCOMP[acc["type"]]
    count = acc["count"]
    base = bv.get("byteOffset", 0) + acc.get("byteOffset", 0)
    stride = bv.get("byteStride", 0) or (csize * n)
    out = np.empty((count, n), dtype=np.float64)
    for i in range(count):
        vals = struct.unpack_from("<" + comp * n, binblob, base + i * stride)
        out[i] = vals
    return out


def node_matrix(node) -> np.ndarray:
    if "matrix" in node:  # column-major 16
        return np.array(node["matrix"], dtype=np.float64).reshape(4, 4).T
    m = np.eye(4)
    if "scale" in node:
        m = np.diag([*node["scale"], 1.0]) @ m
    if "rotation" in node:
        x, y, z, w = node["rotation"]
        r = np.array([
            [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), 0],
            [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), 0],
            [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), 0],
            [0, 0, 0, 1]])
        m = r @ m
    if "translation" in node:
        t = np.eye(4); t[:3, 3] = node["translation"]
        m = t @ m
    return m


# ---- runtime importer transform replica -----------------------------------

def axis_map0(p: np.ndarray) -> np.ndarray:
    """assimp_importer.cpp axisMap(0): X=-x, Y=-y, Z=z."""
    out = p.copy()
    out[:, 0] = -p[:, 0]
    out[:, 1] = -p[:, 1]
    out[:, 2] = p[:, 2]
    return out


def stage(args) -> int:
    src = Path(args.source)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    js, binblob = read_glb(src)

    # gather world-space positions per primitive (compose node transforms)
    prim_positions: list[np.ndarray] = []
    materials_discovered = []
    nonidentity = False
    for node in js.get("nodes", []):
        if "mesh" not in node:
            continue
        M = node_matrix(node)
        if not np.allclose(M, np.eye(4)):
            nonidentity = True
        mesh = js["meshes"][node["mesh"]]
        for pr in mesh["primitives"]:
            if pr.get("mode", 4) != 4:
                continue  # non-triangle prim
            pos = accessor_array(js, binblob, pr["attributes"]["POSITION"])
            ph = np.hstack([pos, np.ones((len(pos), 1))])
            world = (M @ ph.T).T[:, :3]
            prim_positions.append(world)
            # tri count
            if "indices" in pr:
                tris = js["accessors"][pr["indices"]]["count"] // 3
            else:
                tris = len(pos) // 3
            # material discovery (names + alphaClass only; ktx2 is G2)
            mat_idx = pr.get("material")
            mname, alpha = "NULLTXM", 0
            if mat_idx is not None:
                mat = js["materials"][mat_idx]
                raw = mat.get("name", f"mat{mat_idx}")
                mname = "".join(c.lower() if c.isalnum() else "_" for c in raw)
                if mat.get("alphaMode") in ("MASK", "BLEND"):
                    alpha = 1
            materials_discovered.append(
                {"textureName": ("a_" + mname if alpha and not mname.startswith("a_") else mname),
                 "alphaClass": alpha, "verts": len(pos), "tris": tris})

    if not prim_positions:
        print(f"FAIL {src}: no triangulated geometry")
        return 1

    allpos = np.vstack(prim_positions)
    mc2 = axis_map0(allpos)
    mc2[:, 1] += args.yoff  # YOFF default 0

    pre_min = mc2.min(axis=0)
    # auto-ground GROUND=2: dy = -minBox.y
    dy = -pre_min[1] if args.ground == 2 else (-mc2.max(axis=0)[1] if args.ground == 1 else 0.0)
    mc2[:, 1] += dy

    bmin = mc2.min(axis=0)
    bmax = mc2.max(axis=0)
    center = (bmin + bmax) / 2.0
    radius = float(np.max(np.linalg.norm(mc2 - center, axis=1)))  # bounding-sphere about bbox center

    verts = sum(m["verts"] for m in materials_discovered)
    tris = sum(m["tris"] for m in materials_discovered)
    submeshes = len(materials_discovered)

    geometry = {
        "source": args.source_rel or src.name,
        "cooked": src.name,
        "convention": {"axis": 0, "vflip": True, "importer": "assimp_importer.v1"},
        "scale": 1.0,
        "bounds": {"min": [round(float(v), 4) for v in bmin],
                   "max": [round(float(v), 4) for v in bmax],
                   "radius": round(radius, 4)},
        "pivot": [0.0, 0.0, 0.0],
        "counts": {"verts": verts, "tris": tris, "submeshes": submeshes},
        "lods": [],
    }
    staged = {
        "asset": {"id": args.id, "class": args._class,
                  "appearanceName": args.appearance,
                  "replaces": f"{args._class}:{args.appearance}"},
        "geometry": geometry,
        "materials_discovered": [{"slot": i, **{k: m[k] for k in ("textureName", "alphaClass")}}
                                 for i, m in enumerate(materials_discovered)],
        "warnings": (["non-identity node transform composed"] if nonidentity else []),
    }

    # cook glb = passthrough (already default-env-correct); meshopt is a later add
    cooked = out_dir / src.name
    if cooked.resolve() != src.resolve():
        shutil.copyfile(src, cooked)
    (out_dir / "staged.json").write_text(json.dumps(staged, indent=2), encoding="utf-8")
    print(f"STAGED {src.name} -> {out_dir}/  bounds={geometry['bounds']['min']}..{geometry['bounds']['max']} "
          f"r={geometry['bounds']['radius']} verts={verts} tris={tris} submeshes={submeshes}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Track G offline static-prop cook driver.")
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("stage", help="source.glb -> cooked glb + staged.json geometry fragment")
    s.add_argument("source", type=str)
    s.add_argument("out_dir", type=str)
    s.add_argument("--id", required=True)
    s.add_argument("--class", dest="_class", required=True, choices=["staticprop", "tree"])
    s.add_argument("--appearance", required=True)
    s.add_argument("--source-rel", default=None, help="source path recorded in manifest (rel to deploy root)")
    s.add_argument("--ground", type=int, default=2, help="MC2_GLTF_GROUND (default 2)")
    s.add_argument("--yoff", type=float, default=0.0, help="MC2_GLTF_YOFF (default 0)")
    s.set_defaults(func=stage)
    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
