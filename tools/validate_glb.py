#!/usr/bin/env python3
"""
validate_glb.py -- Validates a GLB + mcasset.json sidecar against a baseline JSON.

Usage:
    python tools/validate_glb.py --glb PATH.glb --sidecar PATH.mcasset.json --baseline docs/baseline-flea.json

If --glb is omitted or missing, validates the sidecar alone (node names, hardpoints).
Exit 0 = all HARD checks pass. Exit 1 = at least one HARD check failed.

Stdlib only: no pip dependencies.
"""
import argparse
import json
import os
import struct
import sys


PASS = 'PASS'
WARN = 'WARN'
FAIL = 'FAIL'

results = []


def report(status, check, msg):
    line = f"{status} {check}: {msg}"
    results.append((status, line))
    print(line)


# ---------------------------------------------------------------------------
# GLB reader (stdlib, no pygltflib)
# ---------------------------------------------------------------------------

def read_glb_json(glb_path):
    """Returns (gltf_dict, error_string). On error, gltf_dict is None."""
    with open(glb_path, 'rb') as f:
        data = f.read()

    if len(data) < 12:
        return None, "file too small to be GLB"

    magic, version, total_len = struct.unpack_from('<III', data, 0)
    if magic != 0x46546C67:  # 'glTF'
        return None, f"bad GLB magic: 0x{magic:08X}"
    if version != 2:
        return None, f"unsupported GLB version: {version}"

    # Chunk 0: JSON
    if len(data) < 20:
        return None, "no JSON chunk header"
    chunk0_len, chunk0_type = struct.unpack_from('<II', data, 12)
    if chunk0_type != 0x4E4F534A:  # JSON
        return None, f"chunk 0 is not JSON type: 0x{chunk0_type:08X}"

    json_bytes = data[20:20 + chunk0_len]
    try:
        gltf = json.loads(json_bytes.decode('utf-8'))
    except (json.JSONDecodeError, UnicodeDecodeError) as e:
        return None, f"JSON parse error: {e}"

    return gltf, None


# ---------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------

def check_node_name_length(node_names, max_len=23):
    """HARD: all node names <= max_len chars."""
    violations = [n for n in node_names if len(n) > max_len]
    if violations:
        report(FAIL, 'node_name_length',
               f"{len(violations)} nodes exceed {max_len} chars: {violations[:5]}")
        return False
    report(PASS, 'node_name_length',
           f"all {len(node_names)} nodes <= {max_len} chars")
    return True


def check_hardpoint_nodes_present(sidecar_hardpoints, glb_node_names):
    """HARD: all hardpoint node names from sidecar exist in GLB node names."""
    hp_names = set()
    for hp_type, hps in sidecar_hardpoints.items():
        for hp in hps:
            n = hp.get('name') or hp.get('SmokeNodeName') or hp.get('JumpNodeName') or hp.get('WeaponNodeName') or hp.get('FootNodeName')
            if n:
                hp_names.add(n)

    glb_set = set(glb_node_names)
    missing = [n for n in hp_names if n not in glb_set]
    if missing:
        report(FAIL, 'node_names_match',
               f"{len(missing)} hardpoint nodes missing from GLB: {missing}")
        return False
    report(PASS, 'node_names_match',
           f"all {len(hp_names)} hardpoint nodes found in GLB")
    return True


def check_material_slot_count(gltf_mat_count, baseline_mat_count):
    """HARD: material slot count matches baseline exactly."""
    if gltf_mat_count != baseline_mat_count:
        report(FAIL, 'material_slot_count',
               f"{gltf_mat_count} slots (baseline: {baseline_mat_count})")
        return False
    report(PASS, 'material_slot_count',
           f"{gltf_mat_count} slots (baseline: {baseline_mat_count})")
    return True


def check_texture_refs(sidecar_mat_map, search_dirs):
    """WARN only: texture files exist on disk."""
    all_ok = True
    for entry in sidecar_mat_map:
        tex = entry.get('texture', '')
        if not tex:
            continue
        found = any(os.path.isfile(os.path.join(d, tex)) for d in search_dirs)
        if not found:
            report(WARN, 'texture_refs',
                   f"{tex} not found in search dirs: {search_dirs}")
            all_ok = False
    if all_ok and sidecar_mat_map:
        texnames = [e.get('texture', '') for e in sidecar_mat_map]
        report(PASS, 'texture_refs', f"all textures found: {texnames}")
    return True  # WARN only, never hard-fails


def check_vertex_count(glb_vert_count, baseline_vert_count, tolerance=0.05):
    """WARN only: vertex count within 5% of baseline."""
    if baseline_vert_count == 0:
        report(WARN, 'vertex_count', "baseline vertex_count is 0, skipping")
        return True
    ratio = abs(glb_vert_count - baseline_vert_count) / baseline_vert_count
    if ratio > tolerance:
        report(WARN, 'vertex_count',
               f"{glb_vert_count} vs baseline {baseline_vert_count} ({ratio*100:.1f}% delta > {tolerance*100:.0f}%)")
    else:
        report(PASS, 'vertex_count',
               f"{glb_vert_count} vs baseline {baseline_vert_count} ({ratio*100:.1f}% delta)")
    return True


def check_bbox(gltf, baseline_bbox, tolerance=0.01):
    """WARN only: GLB POSITION accessor min/max within tolerance of baseline bbox."""
    if not baseline_bbox:
        return True

    # Find POSITION accessor min/max from first mesh primitive
    accessors = gltf.get('accessors', [])
    meshes = gltf.get('meshes', [])
    if not meshes or not accessors:
        report(WARN, 'bbox', "no meshes/accessors in GLB, skipping bbox check")
        return True

    pos_accessor = None
    for prim in meshes[0].get('primitives', []):
        attrs = prim.get('attributes', {})
        if 'POSITION' in attrs:
            idx = attrs['POSITION']
            if idx < len(accessors):
                pos_accessor = accessors[idx]
                break

    if pos_accessor is None:
        report(WARN, 'bbox', "POSITION accessor not found in GLB")
        return True

    acc_min = pos_accessor.get('min')
    acc_max = pos_accessor.get('max')
    if not acc_min or not acc_max:
        report(WARN, 'bbox', "POSITION accessor has no min/max")
        return True

    bmin = baseline_bbox.get('min', [0, 0, 0])
    bmax = baseline_bbox.get('max', [0, 0, 0])

    # Compute extents in each axis to normalize tolerance
    extents = [max(abs(bmax[i] - bmin[i]), 1e-6) for i in range(3)]
    errors = [abs(acc_min[i] - bmin[i]) / extents[i] for i in range(3)] + \
             [abs(acc_max[i] - bmax[i]) / extents[i] for i in range(3)]
    max_err = max(errors)
    if max_err > tolerance:
        report(WARN, 'bbox',
               f"max normalized bbox error {max_err:.4f} > {tolerance} (after axis transform)")
    else:
        report(PASS, 'bbox', f"max normalized error {max_err:.4f} within {tolerance}")
    return True


# ---------------------------------------------------------------------------
# Count vertices from GLB POSITION accessors
# ---------------------------------------------------------------------------

def count_glb_vertices(gltf):
    accessors = gltf.get('accessors', [])
    meshes = gltf.get('meshes', [])
    total = 0
    seen = set()
    for mesh in meshes:
        for prim in mesh.get('primitives', []):
            attrs = prim.get('attributes', {})
            if 'POSITION' in attrs:
                idx = attrs['POSITION']
                if idx not in seen and idx < len(accessors):
                    total += accessors[idx].get('count', 0)
                    seen.add(idx)
    return total


def count_glb_materials(gltf):
    return len(gltf.get('materials', []))


def collect_glb_node_names(gltf):
    return [n.get('name', '') for n in gltf.get('nodes', []) if n.get('name')]


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    p = argparse.ArgumentParser(description='Validate GLB + sidecar against baseline JSON.')
    p.add_argument('--glb', help='Path to .glb file (optional — sidecar-only mode if missing)')
    p.add_argument('--sidecar', required=True, help='Path to .mcasset.json sidecar')
    p.add_argument('--baseline', required=True, help='Path to baseline JSON (from asset_baseline.py)')
    args = p.parse_args()

    # Load baseline
    if not os.path.isfile(args.baseline):
        print(f"ERROR: baseline not found: {args.baseline}", file=sys.stderr)
        sys.exit(1)
    with open(args.baseline, 'r', encoding='utf-8') as f:
        baseline = json.load(f)

    # Load sidecar
    if not os.path.isfile(args.sidecar):
        print(f"ERROR: sidecar not found: {args.sidecar}", file=sys.stderr)
        sys.exit(1)
    with open(args.sidecar, 'r', encoding='utf-8') as f:
        sidecar = json.load(f)

    # Load GLB if available
    gltf = None
    glb_available = False
    if args.glb and os.path.isfile(args.glb):
        gltf, err = read_glb_json(args.glb)
        if err:
            print(f"ERROR reading GLB: {err}", file=sys.stderr)
            sys.exit(1)
        glb_available = True
    elif args.glb:
        print(f"NOTE: GLB not found at {args.glb} — sidecar-only validation mode")

    hard_pass = True

    # --- Checks that can run from sidecar alone ---

    # Build node names for sidecar-based checks
    sidecar_hp = sidecar.get('hardpoints', {})
    hp_names = []
    for hp_type, hps in sidecar_hp.items():
        for hp in hps:
            n = hp.get('name') or hp.get('SmokeNodeName') or hp.get('JumpNodeName') or hp.get('WeaponNodeName') or hp.get('FootNodeName')
            if n:
                hp_names.append(n)

    # Node name length check (applies to sidecar hardpoint names too)
    if not check_node_name_length(hp_names, max_len=23):
        hard_pass = False

    # --- GLB-dependent checks ---
    if glb_available:
        glb_node_names = collect_glb_node_names(gltf)

        # Hardpoint names present in GLB
        if not check_hardpoint_nodes_present(sidecar_hp, glb_node_names):
            hard_pass = False

        # Also check all GLB node names for length
        if not check_node_name_length(glb_node_names, max_len=23):
            hard_pass = False

        # Material slot count (HARD)
        glb_mat_count = count_glb_materials(gltf)
        baseline_mat_count = baseline.get('material_slots', 0)
        if not check_material_slot_count(glb_mat_count, baseline_mat_count):
            hard_pass = False

        # Vertex count (WARN)
        glb_vert_count = count_glb_vertices(gltf)
        baseline_vert = baseline.get('vertex_count', 0)
        check_vertex_count(glb_vert_count, baseline_vert)

        # Bbox (WARN)
        vb = sidecar.get('validation_baseline', {})
        if 'bbox' in vb:
            check_bbox(gltf, vb['bbox'])
        elif 'bbox' in baseline:
            check_bbox(gltf, baseline['bbox'])
    else:
        report(WARN, 'glb_checks', "GLB not available — skipping GLB-dependent checks")

    # Texture refs (WARN)
    mat_map = sidecar.get('material_map', [])
    search_dirs = []
    glb_dir = os.path.dirname(os.path.abspath(args.glb)) if args.glb else ''
    if glb_dir:
        search_dirs.append(glb_dir)
    check_texture_refs(mat_map, search_dirs)

    # Summary
    print()
    hard_fails = [r for s, r in results if s == FAIL]
    warns = [r for s, r in results if s == WARN]
    passes = [r for s, r in results if s == PASS]
    print(f"Summary: {len(passes)} PASS, {len(warns)} WARN, {len(hard_fails)} FAIL")

    if hard_pass:
        print("EXIT 0 (all hard checks passed)")
        sys.exit(0)
    else:
        print("EXIT 1 (hard check(s) failed)")
        sys.exit(1)


if __name__ == '__main__':
    main()
