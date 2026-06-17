#!/usr/bin/env python3
"""
asset_baseline.py -- Reads MC2 ASE + FITini, outputs baseline JSON for validation.

Usage:
    python tools/asset_baseline.py --ase PATH.ase --ini PATH.ini --out docs/baseline-flea.json

Stdlib only: no pip dependencies.
"""
import argparse
import json
import os
import re
import sys


# ---------------------------------------------------------------------------
# INI parser (FITini format: type-prefixed keys, e.g. "st FileName0 = ...")
# ---------------------------------------------------------------------------

def parse_fitini(path):
    """Parse FITini file into {section: {key_without_prefix: value_string}}."""
    sections = {}
    current = None
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith('//') or line in ('FITini', 'FITend'):
                continue
            # Section header
            m = re.match(r'^\[([^\]]+)\]$', line)
            if m:
                current = m.group(1)
                sections.setdefault(current, {})
                continue
            if current is None:
                continue
            # Key = Value -- strip type prefix (st/f/l/b/etc.)
            m = re.match(r'^(\w+)\s+(\w[\w\s]*?)\s*=\s*(.+)$', line)
            if m:
                # m.group(1) = type prefix, m.group(2) = key, m.group(3) = value
                key = m.group(2).strip()
                val = m.group(3).strip().strip('"')
                sections[current][key] = val
    return sections


def ini_int(sections, section, key, default=0):
    try:
        return int(sections.get(section, {}).get(key, default))
    except (ValueError, TypeError):
        return default


def ini_float(sections, section, key, default=0.0):
    try:
        return float(sections.get(section, {}).get(key, default))
    except (ValueError, TypeError):
        return default


def ini_str(sections, section, key, default=''):
    return sections.get(section, {}).get(key, default)


# ---------------------------------------------------------------------------
# ASE parser
# ---------------------------------------------------------------------------

class AseParser:
    def __init__(self, path):
        self.path = path
        self.node_names = []
        self.vertex_count = 0
        self.triangle_count = 0
        self.material_count = 0
        self.texture_names = []
        self.vertices = []  # for bbox
        self._parse()

    def _parse(self):
        with open(self.path, 'r', encoding='utf-8', errors='replace') as f:
            lines = [l.rstrip('\n').rstrip('\r') for l in f]

        i = 0
        n = len(lines)
        in_mesh_normals = False

        while i < n:
            line = lines[i].strip()

            # Node names (all object types)
            m = re.match(r'^\*NODE_NAME\s+"([^"]*)"', line)
            if m:
                name = m.group(1)
                if name not in self.node_names:
                    self.node_names.append(name)
                i += 1
                continue

            # Material count
            m = re.match(r'^\*MATERIAL_COUNT\s+(\d+)', line)
            if m:
                self.material_count = max(self.material_count, int(m.group(1)))
                i += 1
                continue

            # Texture bitmap paths
            m = re.match(r'^\*BITMAP\s+"([^"]*)"', line)
            if m:
                bm = os.path.basename(m.group(1).replace('\\', '/'))
                if bm and bm not in self.texture_names:
                    self.texture_names.append(bm)
                i += 1
                continue

            # Mesh vertex count
            m = re.match(r'^\*MESH_NUMVERTEX\s+(\d+)', line)
            if m:
                self.vertex_count += int(m.group(1))
                i += 1
                continue

            # Mesh face count
            m = re.match(r'^\*MESH_NUMFACES\s+(\d+)', line)
            if m:
                self.triangle_count += int(m.group(1))
                i += 1
                continue

            # Vertex positions (for bbox) -- skip inside MESH_NORMALS
            if line.startswith('*MESH_NORMALS'):
                in_mesh_normals = True
                i += 1
                continue
            # End of MESH_NORMALS block (heuristic: closing brace at same indent)
            if in_mesh_normals and line == '}':
                in_mesh_normals = False
                i += 1
                continue

            if not in_mesh_normals:
                m = re.match(r'^\*MESH_VERTEX\s+\d+\s+([\-\d\.eE+]+)\s+([\-\d\.eE+]+)\s+([\-\d\.eE+]+)', line)
                if m:
                    try:
                        x, y, z = float(m.group(1)), float(m.group(2)), float(m.group(3))
                        self.vertices.append((x, y, z))
                    except ValueError:
                        pass
                    i += 1
                    continue

            i += 1

    def bbox(self):
        if not self.vertices:
            return {'min': [0.0, 0.0, 0.0], 'max': [0.0, 0.0, 0.0]}
        xs = [v[0] for v in self.vertices]
        ys = [v[1] for v in self.vertices]
        zs = [v[2] for v in self.vertices]
        return {
            'min': [min(xs), min(ys), min(zs)],
            'max': [max(xs), max(ys), max(zs)],
        }


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def build_baseline(ase_path, ini_path, out_path):
    print(f"Parsing ASE: {ase_path}")
    ase = AseParser(ase_path)
    print(f"  nodes={len(ase.node_names)}, verts={ase.vertex_count}, tris={ase.triangle_count}, mats={ase.material_count}")

    print(f"Parsing INI: {ini_path}")
    ini = parse_fitini(ini_path)

    # Build LODs from TGLData section
    lods = []
    for idx in range(10):
        fn = ini_str(ini, 'TGLData', f'FileName{idx}')
        if not fn:
            break
        dist = ini_float(ini, 'TGLData', f'Distance{idx}', 0.0)
        lods.append({'filename': fn, 'distance': dist})

    shadow_name = ini_str(ini, 'TGLData', 'ShadowName', '')

    # Hardpoints
    num_smoke   = ini_int(ini, 'Nodes', 'NumSmoke', 0)
    num_jumpjet = ini_int(ini, 'Nodes', 'NumJumpJet', 0)
    num_weapon  = ini_int(ini, 'Nodes', 'NumWeapon', 0)
    num_feet    = ini_int(ini, 'Nodes', 'NumFeet', 0)

    smoke_nodes = []
    for i in range(num_smoke):
        name = ini_str(ini, f'SmokeNode{i}', 'SmokeNodeName', '')
        if name:
            smoke_nodes.append({'name': name})

    jumpjet_nodes = []
    for i in range(num_jumpjet):
        name = ini_str(ini, f'JumpJetNode{i}', 'JumpNodeName', '')
        if name:
            jumpjet_nodes.append({'name': name})

    weapon_nodes = []
    for i in range(num_weapon):
        name = ini_str(ini, f'WeaponNode{i}', 'WeaponNodeName', '')
        wtype = ini_int(ini, f'WeaponNode{i}', 'WeaponType', 0)
        if name:
            weapon_nodes.append({'name': name, 'type': wtype})

    foot_nodes = []
    for i in range(num_feet):
        name = ini_str(ini, f'FootNode{i}', 'FootNodeName', '')
        if name:
            foot_nodes.append({'name': name})

    asset_name = lods[0]['filename'] if lods else os.path.splitext(os.path.basename(ase_path))[0]

    baseline = {
        'asset_name': asset_name,
        'source_ase': os.path.abspath(ase_path),
        'source_ini': os.path.abspath(ini_path),
        'vertex_count': ase.vertex_count,
        'triangle_count': ase.triangle_count,
        'material_slots': ase.material_count,
        'texture_names': ase.texture_names,
        'node_names': ase.node_names,
        'bbox': ase.bbox(),
        'lods': lods,
        'shadow_name': shadow_name,
        'hardpoints': {
            'weapon': weapon_nodes,
            'smoke': smoke_nodes,
            'jumpjet': jumpjet_nodes,
            'foot': foot_nodes,
        },
    }

    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, 'w', encoding='utf-8') as f:
        json.dump(baseline, f, indent=2)
    print(f"Wrote baseline: {out_path}")
    return baseline


def main():
    p = argparse.ArgumentParser(description='Generate MC2 asset baseline JSON from ASE + FITini.')
    p.add_argument('--ase', required=True, help='Path to .ase file')
    p.add_argument('--ini', required=True, help='Path to .ini (FITini) file')
    p.add_argument('--out', required=True, help='Output JSON path')
    args = p.parse_args()

    if not os.path.isfile(args.ase):
        print(f"ERROR: ASE not found: {args.ase}", file=sys.stderr)
        sys.exit(1)
    if not os.path.isfile(args.ini):
        print(f"ERROR: INI not found: {args.ini}", file=sys.stderr)
        sys.exit(1)

    build_baseline(args.ase, args.ini, args.out)


if __name__ == '__main__':
    main()
