#!/usr/bin/env python3
"""normalize_broadleaf_glb.py — pre-bake external Blender/GLTF tree assets into
MC2-importer-ready GLBs.

WHY: MC2's assimp importer (mclib/assimp_importer.cpp:59) reads MESH-LOCAL
vertices (mesh->mVertices) and DROPS the node->world transform (it only
decomposes the node's pivot translation, never the rotation/scale). External
Blender GLTF exports carry a per-node quaternion that converts Blender Z-up to
the scene's Y-up plus a uniform node scale. Dropped at runtime => the mesh-local
geometry is Z-up (lies on its side) AND at its un-scaled size (~2x stock).

Separately, the runtime derives the MC2 texture NAME from the baseColor IMAGE
filename (DeriveMC2TextureName, assimp_importer.cpp:172), while the cook names
cooked KTX2 by MATERIAL name. The two disagree (image 'maple_branch_color-...'
vs material 'MapleFoliage') => NULLTXM => black tree. We rename each baseColor
image to its material name so the runtime-derived name == the cook's KTX2 stem.

WHAT THIS DOES (raw GLB surgery, no trimesh re-export to avoid PBR loss):
  1. Bake node ROTATION (quaternion) into POSITION + NORMAL accessors so the
     mesh-local frame is upright (Y-up) after the runtime axisMap0(-x,-y,z).
     Node SCALE/TRANSLATION are intentionally DROPPED (raw size kept) and a
     single user --scale (default 0.5) is applied to hit ~stock size.
  2. Set node TRS to identity (defensive; runtime ignores it anyway).
  3. Recompute POSITION accessor min/max (glTF requires it).
  4. Rename each material's baseColor image to the (sanitized) material name so
     runtime texture binding matches the cook's material-name KTX2 output.

Buffer bytes are edited in place (VEC3 float32, same layout) so all bufferViews
/ textures / indices stay valid. Output GLB is byte-compatible except POSITION
/NORMAL float payloads, node TRS, accessor min/max, and image names.

Usage:
  python normalize_broadleaf_glb.py <in.glb> <out.glb> [--scale 0.5]
"""
import argparse
import json
import struct
import sys
import numpy as np


def quat_to_mat3(q):
    # glTF quaternion order: [x, y, z, w]
    x, y, z, w = q
    n = (x * x + y * y + z * z + w * w) ** 0.5
    if n == 0:
        return np.eye(3)
    x, y, z, w = x / n, y / n, z / n, w / n
    return np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w)],
        [2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y)],
    ], dtype=np.float64)


def read_glb(path):
    d = bytearray(open(path, "rb").read())
    assert d[:4] == b"glTF", "not a GLB"
    ver, total = struct.unpack("<II", d[4:12])
    off = 12
    json_bytes = None
    bin_off = None
    bin_len = 0
    while off < total:
        clen, ctype = struct.unpack("<II", d[off:off + 8])
        off += 8
        if ctype == 0x4E4F534A:  # 'JSON'
            json_bytes = bytes(d[off:off + clen])
        elif ctype == 0x004E4942:  # 'BIN\0'
            bin_off = off
            bin_len = clen
        off += clen
    js = json.loads(json_bytes)
    return d, js, bin_off, bin_len


def accessor_floats(d, js, bin_off, acc_idx, ncomp):
    acc = js["accessors"][acc_idx]
    bv = js["bufferViews"][acc["bufferView"]]
    base = bin_off + bv.get("byteOffset", 0) + acc.get("byteOffset", 0)
    count = acc["count"]
    stride = bv.get("byteStride", ncomp * 4)
    out = np.empty((count, ncomp), dtype=np.float32)
    for i in range(count):
        o = base + i * stride
        out[i] = struct.unpack_from("<" + "f" * ncomp, d, o)
    return out, base, stride


def write_floats(d, base, stride, arr):
    ncomp = arr.shape[1]
    for i in range(arr.shape[0]):
        o = base + i * stride
        struct.pack_into("<" + "f" * ncomp, d, o, *[float(x) for x in arr[i]])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("infile")
    ap.add_argument("outfile")
    ap.add_argument("--scale", type=float, default=0.5,
                    help="uniform scale baked into verts (default 0.5 = halve)")
    ap.add_argument("--no-flip-x", action="store_true",
                    help="skip the 180-deg X flip (default: flip ON). The flip puts "
                         "the base at maxBox.y so the importer's default "
                         "MC2_GLTF_GROUND=2 grounds the base (not the canopy) => upright.")
    args = ap.parse_args()
    # 180-deg rotation about X: (x,y,z)->(x,-y,-z). Even parity (winding kept).
    FLIP = np.diag([1.0, -1.0, -1.0]) if not args.no_flip_x else np.eye(3)

    d, js, bin_off, bin_len = read_glb(args.infile)
    if bin_off is None:
        print("FAIL: GLB has no BIN chunk (external buffer unsupported)", file=sys.stderr)
        return 1

    # Map mesh index -> node (TRS). One node per mesh in these assets.
    mesh_node = {}
    for n in js.get("nodes", []):
        if "mesh" in n:
            mesh_node[n["mesh"]] = n

    for mi, mesh in enumerate(js["meshes"]):
        node = mesh_node.get(mi)
        q = node.get("rotation", [0, 0, 0, 1]) if node else [0, 0, 0, 1]
        R = (FLIP @ quat_to_mat3(q)) * float(args.scale)  # flip * rotation * user-scale (drop node scale/trans)
        Rn = FLIP @ quat_to_mat3(q)                        # normals: flip * rotation only
        for pr in mesh["primitives"]:
            attrs = pr["attributes"]
            # POSITION
            pacc = attrs["POSITION"]
            pos, pbase, pstride = accessor_floats(d, js, bin_off, pacc, 3)
            pos2 = (R @ pos.T.astype(np.float64)).T.astype(np.float32)
            write_floats(d, pbase, pstride, pos2)
            a = js["accessors"][pacc]
            a["min"] = [float(x) for x in pos2.min(axis=0)]
            a["max"] = [float(x) for x in pos2.max(axis=0)]
            # NORMAL
            if "NORMAL" in attrs:
                nacc = attrs["NORMAL"]
                nrm, nbase, nstride = accessor_floats(d, js, bin_off, nacc, 3)
                nrm2 = (Rn @ nrm.T.astype(np.float64)).T
                ln = np.linalg.norm(nrm2, axis=1, keepdims=True)
                ln[ln == 0] = 1.0
                nrm2 = (nrm2 / ln).astype(np.float32)
                write_floats(d, nbase, nstride, nrm2)

    # Identity node TRS (defensive; runtime drops it anyway).
    for n in js.get("nodes", []):
        if "mesh" in n:
            n.pop("matrix", None)
            n["rotation"] = [0, 0, 0, 1]
            n["scale"] = [1, 1, 1]
            n["translation"] = [0, 0, 0]

    # Rename each material's baseColor image to the material name so the runtime
    # DeriveMC2TextureName (image-filename based) yields the SAME stem the cook
    # uses (material-name based) => texture binds instead of NULLTXM (black).
    images = js.get("images", [])
    textures = js.get("textures", [])
    for m in js.get("materials", []):
        bct = m.get("pbrMetallicRoughness", {}).get("baseColorTexture")
        if not bct:
            continue
        src = textures[bct["index"]].get("source")
        if src is None:
            continue
        images[src]["name"] = m.get("name", images[src].get("name", "tex"))

    # Re-serialize JSON chunk; pad to 4 bytes with spaces. BIN chunk unchanged.
    new_json = json.dumps(js, separators=(",", ":")).encode("utf-8")
    while len(new_json) % 4 != 0:
        new_json += b" "
    bin_chunk_start = None
    # Rebuild GLB: header + JSON chunk + BIN chunk (copied from original bytes).
    # Extract original BIN payload.
    # bin_off points at payload start; the chunk header is 8 bytes before it.
    orig_bin_payload = bytes(d[bin_off:bin_off + bin_len])
    # pad bin to 4 (already is in source)
    bin_pad = orig_bin_payload
    while len(bin_pad) % 4 != 0:
        bin_pad += b"\x00"

    out = bytearray()
    out += b"glTF"
    out += struct.pack("<I", 2)
    total = 12 + 8 + len(new_json) + 8 + len(bin_pad)
    out += struct.pack("<I", total)
    out += struct.pack("<I", len(new_json)) + struct.pack("<I", 0x4E4F534A) + new_json
    out += struct.pack("<I", len(bin_pad)) + struct.pack("<I", 0x004E4942) + bin_pad
    open(args.outfile, "wb").write(out)
    print(f"NORMALIZED {args.infile} -> {args.outfile} scale={args.scale} "
          f"meshes={len(js['meshes'])} images_renamed={sum(1 for m in js.get('materials',[]) if m.get('pbrMetallicRoughness',{}).get('baseColorTexture'))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
