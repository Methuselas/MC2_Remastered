#!/usr/bin/env python3
"""BT2018 clip retarget: convert Atlas-skeleton absolute local rotations into
the target mesh's bind frame.  Root cause: atlas_ clips author ABSOLUTE local
rotations for the Atlas bind pose; a mesh (e.g. Marauder) with a different bind
(deep pre-bent legs) has its bind erased by rotationOnly sampling -> legs reverse.

Fix per rotation keyframe (standard same-topology retarget):
    q_corrected = q_targetBind * conj(q_atlasBind) * q_key
so a clip key equal to the atlas bind maps to the target bind (correct rest), and
motion is applied as a delta from the atlas bind on top of the target bind.

Usage:
  py -3 bt2018_retarget_clips.py <target.glb> <atlas_ref.glb> <out.glb>
"""
import sys
import numpy as np
from pygltflib import GLTF2

FLOAT = 5126


def quat_mul(a, b):
    # Hamilton product, [x,y,z,w]
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return np.array([
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz,
    ], dtype=np.float64)


def quat_conj(q):
    return np.array([-q[0], -q[1], -q[2], q[3]], dtype=np.float64)


def node_bind_rot(gltf, node):
    r = gltf.nodes[node].rotation
    return np.array(r if r else [0.0, 0.0, 0.0, 1.0], dtype=np.float64)


def name_to_index(gltf):
    return {n.name: i for i, n in enumerate(gltf.nodes) if n.name}


def accessor_view(gltf, blob, acc_idx):
    acc = gltf.accessors[acc_idx]
    assert acc.componentType == FLOAT and acc.type == "VEC4", \
        f"accessor {acc_idx} not FLOAT VEC4 ({acc.componentType},{acc.type})"
    bv = gltf.bufferViews[acc.bufferView]
    base = (bv.byteOffset or 0) + (acc.byteOffset or 0)
    stride = bv.byteStride or 16
    assert stride == 16, f"unexpected byteStride {stride}"
    n = acc.count
    arr = np.frombuffer(blob, dtype=np.float32, count=n * 4, offset=base)
    return arr.reshape(n, 4), base, n


def retarget_file(target_path, atlas_path, out_path):
    """Retarget all rotation clips in target_path from the Atlas-skeleton bind
    into target_path's own bind frame; write to out_path. Returns (channels, keys)."""
    tgt = GLTF2().load(target_path)
    atl = GLTF2().load(atlas_path)
    blob = bytearray(tgt.binary_blob())

    atl_names = name_to_index(atl)
    tgt_names_rev = {i: n.name for i, n in enumerate(tgt.nodes)}

    n_chan = 0
    n_keys = 0
    missing = []
    bone_report = {}
    for anim in tgt.animations:
        for ch in anim.channels:
            if ch.target.path != "rotation":
                continue
            node_idx = ch.target.node
            bone = tgt_names_rev.get(node_idx, "")
            if bone not in atl_names:
                missing.append(bone)
                continue
            q_mbind = node_bind_rot(tgt, node_idx)
            q_abind = node_bind_rot(atl, atl_names[bone])
            pre = quat_mul(q_mbind, quat_conj(q_abind))  # q_targetBind * conj(q_atlasBind)

            sampler = anim.samplers[ch.sampler]
            data, base, count = accessor_view(tgt, blob, sampler.output)
            new = np.empty_like(data)
            for k in range(count):
                q = data[k].astype(np.float64)
                qc = quat_mul(pre, q)
                norm = np.linalg.norm(qc)
                if norm > 0:
                    qc = qc / norm
                new[k] = qc.astype(np.float32)
            # write back into blob
            raw = new.astype("<f4").tobytes()
            blob[base:base + len(raw)] = raw
            n_chan += 1
            n_keys += count
            if bone not in bone_report:
                # angle between first key before/after, for sanity
                bone_report[bone] = count

    tgt.set_binary_blob(bytes(blob))
    tgt.save(out_path)
    print(f"[retarget] {out_path}: channels={n_chan} keys={n_keys} bones={len(bone_report)}")
    if missing:
        print(f"[retarget] WARN no atlas bind for (left as-is): {sorted(set(missing))}")
    return n_chan, n_keys


if __name__ == "__main__":
    retarget_file(sys.argv[1], sys.argv[2], sys.argv[3])
