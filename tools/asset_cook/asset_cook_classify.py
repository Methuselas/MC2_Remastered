"""tools/asset_cook/asset_cook_classify.py -- Asset classification layer.

Classifies a TGL appearance as STATIC_RENDER_ONLY, NODE_ANIMATED_PROP, or
UNKNOWN_UNSAFE based on the appearance INI file.

Not a CLI -- imported by cook_all_stock.py and tests.
"""
from __future__ import annotations

import re
from enum import Enum
from pathlib import Path


class CookClass(Enum):
    STATIC_RENDER_ONLY = "STATIC_RENDER_ONLY"
    NODE_ANIMATED_PROP = "NODE_ANIMATED_PROP"
    SKELETAL_OR_ANIMATED_MESH = "SKELETAL_OR_ANIMATED_MESH"  # reserved, not yet detected
    VFX_MESH_UNSUPPORTED = "VFX_MESH_UNSUPPORTED"            # reserved, not yet detected
    UNKNOWN_UNSAFE = "UNKNOWN_UNSAFE"


_ANIM_NODE_RE = re.compile(r'\s*st\s+AnimationNodeId\s*=\s*"([^"]+)"', re.IGNORECASE)


def _find_ini(asset_id: str, tgl_dir: Path):
    """Case-insensitive INI lookup: exact stem, lowercase stem, then glob."""
    exact = tgl_dir / f"{asset_id}.ini"
    if exact.exists():
        return exact
    lower = tgl_dir / f"{asset_id.lower()}.ini"
    if lower.exists():
        return lower
    for p in tgl_dir.glob("*.ini"):
        if p.stem.lower() == asset_id.lower():
            return p
    return None


def classify_appearance(asset_id: str, tgl_dir: Path):
    """Return (CookClass, reason_str).

    Reads <tgl_dir>/<asset_id>.ini (case-insensitive find).
    - INI not found                          -> UNKNOWN_UNSAFE, "no ini found"
    - AnimationNodeId present and != "NONE"  -> NODE_ANIMATED_PROP
    - Otherwise                              -> STATIC_RENDER_ONLY

    SKELETAL_OR_ANIMATED_MESH and VFX_MESH_UNSUPPORTED are reserved enum
    members; no INI heuristic detects them yet.
    """
    ini_path = _find_ini(asset_id, tgl_dir)
    if ini_path is None:
        return CookClass.UNKNOWN_UNSAFE, "no ini found"

    try:
        text = ini_path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        return CookClass.UNKNOWN_UNSAFE, f"ini read error: {exc}"

    for line in text.splitlines():
        m = _ANIM_NODE_RE.match(line)
        if m:
            value = m.group(1).strip()
            if value.upper() != "NONE" and value != "":
                return CookClass.NODE_ANIMATED_PROP, f"AnimationNodeId={value}"

    return CookClass.STATIC_RENDER_ONLY, "static prop"


def can_emit_override(cook_class: CookClass) -> bool:
    """Return True only when it is safe to emit a renderOnly static override."""
    return cook_class is CookClass.STATIC_RENDER_ONLY