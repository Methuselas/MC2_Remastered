"""tests/test_classify.py -- Unit tests for asset_cook_classify."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from asset_cook_classify import CookClass, classify_appearance, can_emit_override


def _write_ini(tmp_path: Path, name: str, lines: list) -> None:
    (tmp_path / f"{name}.ini").write_text("\n".join(lines) + "\n", encoding="utf-8")


def test_animated_prop_classified(tmp_path):
    _write_ini(tmp_path, "artilleryturret", [
        'AppearanceName = "artilleryturret"',
        '   st AnimationNodeId = "turret"',
    ])
    cook_class, reason = classify_appearance("artilleryturret", tmp_path)
    assert cook_class is CookClass.NODE_ANIMATED_PROP
    assert "turret" in reason


def test_static_prop_classified(tmp_path):
    _write_ini(tmp_path, "building", [
        'AppearanceName = "building"',
        'MeshName = "building.msh"',
    ])
    cook_class, reason = classify_appearance("building", tmp_path)
    assert cook_class is CookClass.STATIC_RENDER_ONLY


def test_none_animation_id_is_static(tmp_path):
    _write_ini(tmp_path, "smalltree", [
        'AppearanceName = "smalltree"',
        '   st AnimationNodeId = "NONE"',
    ])
    cook_class, reason = classify_appearance("smalltree", tmp_path)
    assert cook_class is CookClass.STATIC_RENDER_ONLY


def test_missing_ini_is_unknown_unsafe(tmp_path):
    cook_class, reason = classify_appearance("nonexistent_prop", tmp_path)
    assert cook_class is CookClass.UNKNOWN_UNSAFE
    assert "no ini" in reason.lower()


def test_can_emit_override():
    assert can_emit_override(CookClass.STATIC_RENDER_ONLY) is True
    assert can_emit_override(CookClass.NODE_ANIMATED_PROP) is False
    assert can_emit_override(CookClass.UNKNOWN_UNSAFE) is False
    assert can_emit_override(CookClass.SKELETAL_OR_ANIMATED_MESH) is False
    assert can_emit_override(CookClass.VFX_MESH_UNSUPPORTED) is False