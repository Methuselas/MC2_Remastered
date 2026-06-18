#!/usr/bin/env python3
"""
classify_materials.py -- Keyword-classify material slots from an ASE file.

Reads an ASE file (and optional INI), extracts every material slot, guesses a
semantic material class using keyword rules, and writes a suggestions JSON that
a human can review and correct.

Usage:
    py -3 tools/classify_materials.py --ase PATH.ase [--ini PATH.ini] \
        --out OUTPUT.json [--asset-name NAME]

Stdlib only, no pip dependencies.

Review workflow
---------------
Every slot is emitted with "needs_review": true and "reviewed": false.
This tool NEVER auto-clears needs_review, even for high-confidence guesses.
After inspecting a slot, set manually in the output JSON:
    "reviewed": true, "approved_class": "<class>"
The "auto_confidence" field records the classifier's raw score for reference
only — it does not gate approval.
# After review, set: "reviewed": true, "approved_class": "<class>"
# The "needs_review": true field is always set by this tool (never auto-cleared).
"""
import argparse
import json
import os
import re
import sys

__version__ = '1.0.0'

# ---------------------------------------------------------------------------
# Keyword → class table (checked against lowercased material name AND texture)
# ---------------------------------------------------------------------------

# Order matters: first match wins.
KEYWORD_RULES = [
    ('corrugated_steel_painted', ['corr', 'rib', 'siding', 'steel', 'sheet', 'hangar']),
    ('painted_metal',            ['metal', 'door', 'pipe', 'rail', 'panel', 'tank']),
    ('bare_metal',               ['bare']),
    ('concrete',                 ['conc', 'cement', 'foundation', 'slab']),
    ('brick',                    ['brick', 'masonry']),
    ('roof',                     ['roof', 'shingle', 'tar']),
    ('glass',                    ['glass', 'window']),
    ('rubber',                   ['rubber', 'tire']),
    ('dirt_grime',               ['dirt', 'mud', 'soil']),
]

FALLBACK_CLASS = 'unknown'

# ---------------------------------------------------------------------------
# Per-class PBR defaults
# ---------------------------------------------------------------------------

CLASS_DEFAULTS = {
    'corrugated_steel_painted': {'metallic': 0.0,  'roughness': 0.70, 'tile_scale': 2.0},
    'painted_metal':            {'metallic': 0.05, 'roughness': 0.65, 'tile_scale': 1.0},
    'bare_metal':               {'metallic': 0.85, 'roughness': 0.50, 'tile_scale': 1.0},
    'concrete':                 {'metallic': 0.0,  'roughness': 0.85, 'tile_scale': 1.5},
    'brick':                    {'metallic': 0.0,  'roughness': 0.88, 'tile_scale': 1.0},
    'roof':                     {'metallic': 0.1,  'roughness': 0.80, 'tile_scale': 3.0},
    'glass':                    {'metallic': 0.0,  'roughness': 0.08, 'tile_scale': 1.0},
    'rubber':                   {'metallic': 0.0,  'roughness': 0.88, 'tile_scale': 1.0},
    'dirt_grime':               {'metallic': 0.0,  'roughness': 0.95, 'tile_scale': 1.0},
    'unknown':                  {'metallic': 0.0,  'roughness': 0.75, 'tile_scale': 1.0},
}


# ---------------------------------------------------------------------------
# Classification logic
# ---------------------------------------------------------------------------

def classify(mat_name, tex_name):
    """
    Returns (class_name, confidence, reason).

    Confidence rules:
      Multiple keywords match (name OR tex): 0.85
      Single keyword match in texture name:  0.75
      Single keyword match in material name: 0.60
      No match (fallback):                   0.30
    """
    name_lc = mat_name.lower()
    tex_lc  = tex_name.lower()

    for cls, keywords in KEYWORD_RULES:
        name_hits = [kw for kw in keywords if kw in name_lc]
        tex_hits  = [kw for kw in keywords if kw in tex_lc]
        all_hits  = list(dict.fromkeys(name_hits + tex_hits))  # deduped, order preserved

        if not all_hits:
            continue

        # High confidence: keyword hit in BOTH material name AND texture (even same kw),
        # OR multiple distinct keywords hit in either source.
        both_sources = bool(name_hits) and bool(tex_hits)
        multi_kw     = len(all_hits) >= 2
        if both_sources or multi_kw:
            confidence = 0.85
            sources = []
            if name_hits:
                sources.append("material name '{}'".format(mat_name))
            if tex_hits:
                sources.append("texture '{}'".format(tex_name))
            reason = "keywords {} in {} → {}".format(
                all_hits, ' and '.join(sources), cls)
        elif tex_hits:
            confidence = 0.75
            reason = "texture '{}' matches keyword '{}' → {}".format(
                tex_name, tex_hits[0], cls)
        else:
            confidence = 0.60
            reason = "material name '{}' matches keyword '{}' → {}".format(
                mat_name, name_hits[0], cls)

        return cls, confidence, reason

    # No match
    reason = "no keyword matched for name='{}' tex='{}' → {}".format(
        mat_name, tex_name, FALLBACK_CLASS)
    return FALLBACK_CLASS, 0.30, reason


def build_profile_defaults(cls):
    d = CLASS_DEFAULTS.get(cls, CLASS_DEFAULTS['unknown'])
    return {
        'metallic':    d['metallic'],
        'roughness':   d['roughness'],
        'tile_scale':  d['tile_scale'],
        'normal_tex':  None,
        'orm_tex':     None,
        'paint_color': None,
        'wear':        0.0,
    }


# ---------------------------------------------------------------------------
# INI parser (FITini format — reused from ase_to_glb.py)
# ---------------------------------------------------------------------------

def parse_fitini(path):
    sections = {}
    current = None
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith('//') or line in ('FITini', 'FITend'):
                continue
            m = re.match(r'^\[([^\]]+)\]$', line)
            if m:
                current = m.group(1)
                sections.setdefault(current, {})
                continue
            if current is None:
                continue
            m = re.match(r'^(\w+)\s+(\w[\w\s]*?)\s*=\s*(.+)$', line)
            if m:
                key = m.group(2).strip()
                val = m.group(3).strip().strip('"')
                sections[current][key] = val
    return sections


# ---------------------------------------------------------------------------
# ASE material-list parser
# ---------------------------------------------------------------------------

class _RawMat:
    """One material or sub-material slot from an ASE file."""
    __slots__ = ('name', 'bitmap', 'submaterials')

    def __init__(self):
        self.name         = ''
        self.bitmap       = ''   # basename of diffuse texture (may be empty)
        self.submaterials = []   # list[_RawMat], populated for multi-sub-object mats


def _parse_mat_block(lines, start):
    """
    Parse a *MATERIAL or *SUBMATERIAL block starting at `start` (the line
    AFTER the opening '{').  Returns (mat, next_i).
    """
    mat = _RawMat()
    i = start
    n = len(lines)
    depth = 1

    while i < n and depth > 0:
        l = lines[i].strip()
        i += 1
        depth += l.count('{') - l.count('}')
        if depth <= 0:
            break

        # Material name
        mm = re.match(r'^\*MATERIAL_NAME\s+"([^"]*)"', l)
        if mm:
            mat.name = mm.group(1)
            continue

        # Bitmap inside MAP_DIFFUSE (we just grab whatever *BITMAP we see at this depth)
        mm = re.match(r'^\*BITMAP\s+"([^"]*)"', l)
        if mm and not mat.bitmap:
            raw_path = mm.group(1).replace('\\', '/')
            mat.bitmap = os.path.basename(raw_path)
            continue

        # Sub-material block
        mm = re.match(r'^\*SUBMATERIAL\s+\d+\s+\{', l)
        if mm:
            sub, i = _parse_mat_block(lines, i)
            mat.submaterials.append(sub)
            # _parse_mat_block consumed the closing '}', so depth was already
            # decremented — we need to account for that.
            depth -= 1
            continue

    return mat, i


def parse_ase_materials(path):
    """
    Return list of _RawMat for every *MATERIAL block in the *MATERIAL_LIST.
    """
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        lines = [l.rstrip('\n').rstrip('\r') for l in f]

    materials = []
    n = len(lines)
    i = 0

    while i < n:
        l = lines[i].strip()
        i += 1
        if l != '*MATERIAL_LIST {':
            continue

        # Inside MATERIAL_LIST
        while i < n:
            l2 = lines[i].strip()
            if l2 == '}':
                i += 1
                break
            mm = re.match(r'^\*MATERIAL\s+\d+\s+\{', l2)
            if mm:
                i += 1  # move past the opening '{'
                mat, i = _parse_mat_block(lines, i)
                materials.append(mat)
                continue
            i += 1
        break  # only one MATERIAL_LIST expected

    return materials


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def build_slots(materials):
    """Flatten materials (handling sub-materials) into slot dicts."""
    slots = []

    for parent_idx, mat in enumerate(materials):
        if mat.submaterials:
            # Multi-sub-object: each sub-material is a separate slot
            for sub_idx, sub in enumerate(mat.submaterials):
                cls, conf, reason = classify(sub.name, sub.bitmap)
                slot = {
                    'slot':             '{}.{}'.format(parent_idx, sub_idx),
                    'material_name':    sub.name,
                    'legacy_texture':   sub.bitmap or None,
                    'guessed_class':    cls,
                    'auto_confidence':  round(conf, 4),
                    'needs_review':     True,
                    'reviewed':         False,
                    'reason':           reason,
                    'profile_defaults': build_profile_defaults(cls),
                }
                slots.append(slot)
        else:
            cls, conf, reason = classify(mat.name, mat.bitmap)
            slot = {
                'slot':             parent_idx,
                'material_name':    mat.name,
                'legacy_texture':   mat.bitmap or None,
                'guessed_class':    cls,
                'auto_confidence':  round(conf, 4),
                'needs_review':     True,
                'reviewed':         False,
                'reason':           reason,
                'profile_defaults': build_profile_defaults(cls),
            }
            slots.append(slot)

    return slots


def main():
    ap = argparse.ArgumentParser(
        description='Classify ASE material slots into semantic PBR types.')
    ap.add_argument('--ase',        required=True,  help='Input ASE file')
    ap.add_argument('--ini',        default=None,   help='Optional FITini file for asset-class hint')
    ap.add_argument('--out',        required=True,  help='Output .material_suggestions.json')
    ap.add_argument('--asset-name', default=None,   help='Asset name (defaults to ASE stem)')
    args = ap.parse_args()

    ase_path = args.ase
    if not os.path.isfile(ase_path):
        print('ERROR: ASE not found: {}'.format(ase_path), file=sys.stderr)
        sys.exit(1)

    asset_name = args.asset_name or os.path.splitext(os.path.basename(ase_path))[0].lower()

    # Optional INI — extract asset class hint if present
    ini_asset_class = None
    if args.ini and os.path.isfile(args.ini):
        ini = parse_fitini(args.ini)
        # TGLData has FileName0 = the base LOD name; asset class not directly encoded,
        # but presence of TGLDamage hints at a building.
        if 'TGLDamage' in ini:
            ini_asset_class = 'building'
        elif 'TGLData' in ini:
            ini_asset_class = 'prop'

    materials = parse_ase_materials(ase_path)
    if not materials:
        print('WARNING: no *MATERIAL_LIST found in {}'.format(ase_path), file=sys.stderr)

    slots = build_slots(materials)

    doc = {
        'schema_version': '1.0',
        'generated_by':   'classify_materials.py',
        'asset':          asset_name,
        'slots':          slots,
    }
    if ini_asset_class:
        doc['ini_asset_class'] = ini_asset_class

    out_path = args.out
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, 'w', encoding='utf-8') as f:
        json.dump(doc, f, indent=2)
        f.write('\n')

    print(json.dumps(doc, indent=2))
    print('\n-- wrote {} slot(s) to {}'.format(len(slots), out_path), file=sys.stderr)


if __name__ == '__main__':
    main()
