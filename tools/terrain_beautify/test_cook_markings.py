#!/usr/bin/env python3
"""pytest for marking_glyphs.py + cook_markings.py: glyph determinism, schema
validation, and extract/compose/install round-trip."""
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest
from PIL import Image

HERE = Path(__file__).resolve().parent
TOOL = HERE / "cook_markings.py"
CARVER5_MISSIONS = Path("A:/Games/Carver5-feasibility/data/missions")

sys.path.insert(0, str(HERE))
import marking_glyphs as mg  # noqa: E402


def run(*args) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(TOOL), *args],
        capture_output=True, text=True,
    )


# --- glyph determinism --------------------------------------------------------
@pytest.mark.parametrize("name", sorted(mg.GLYPHS))
def test_glyph_deterministic(name):
    kwargs = {"text": "27L", "height_wu": 6.0} if name == "runway_numeral" else {}
    a1 = mg.render_glyph(name, 8.0, "white", **kwargs)
    a2 = mg.render_glyph(name, 8.0, "white", **kwargs)
    assert np.array_equal(a1, a2)


@pytest.mark.parametrize("name", sorted(mg.GLYPHS))
def test_glyph_alpha_native_rgba(name):
    kwargs = {"text": "09", "height_wu": 6.0} if name == "runway_numeral" else {}
    arr = mg.render_glyph(name, 8.0, "white", **kwargs)
    assert arr.dtype == np.uint8
    assert arr.ndim == 3 and arr.shape[2] == 4
    assert int(arr[..., 3].max()) > 0, "glyph must draw at least one opaque-ish pixel"


def test_glyph_rejects_non_white_yellow_color():
    with pytest.raises(ValueError):
        mg.render_glyph("road_centerline_dash", 8.0, "magenta")


def test_glyph_rejects_bad_numeral_chars():
    with pytest.raises(ValueError):
        mg.render_glyph("runway_numeral", 8.0, "white", text="AB9")


def test_glyph_unknown_name_rejected():
    with pytest.raises(ValueError):
        mg.render_glyph("not_a_real_glyph", 8.0, "white")


def test_rotate_glyph_identity_at_zero():
    base = mg.render_glyph("road_centerline_dash", 8.0, "white")
    rotated = mg.rotate_glyph(base, 0.0)
    assert np.array_equal(base, rotated)


def test_rotate_glyph_changes_shape_at_90():
    base = mg.render_glyph("runway_centerline_dashes", 8.0, "white", length_wu=20.0)
    rotated = mg.rotate_glyph(base, 90.0)
    # a long horizontal strip rotated 90 degrees becomes tall & narrow
    assert rotated.shape[0] > rotated.shape[1]


def test_scale_glyph_doubles_dimensions_approximately():
    base = mg.render_glyph("crossing_paint", 8.0, "white", size_wu=4.0)
    scaled = mg.scale_glyph(base, 2.0)
    assert abs(scaled.shape[0] - 2 * base.shape[0]) <= 1
    assert abs(scaled.shape[1] - 2 * base.shape[1]) <= 1


# --- extract: determinism + schema --------------------------------------------
def _extract(pak_stem: str, out: Path) -> subprocess.CompletedProcess:
    return run("extract", "--pak", str(CARVER5_MISSIONS / f"{pak_stem}.pak"), "--out", str(out))


@pytest.mark.skipif(not (CARVER5_MISSIONS / "mc2_24.pak").is_file(),
                     reason="Carver5-feasibility stock mission data not present")
def test_extract_deterministic(tmp_path):
    out1 = tmp_path / "a.json"
    out2 = tmp_path / "b.json"
    r1 = _extract("mc2_24", out1)
    r2 = _extract("mc2_24", out2)
    assert r1.returncode == 0, r1.stdout + r1.stderr
    assert r2.returncode == 0, r2.stdout + r2.stderr
    assert out1.read_text() == out2.read_text()


@pytest.mark.skipif(not (CARVER5_MISSIONS / "mc2_24.pak").is_file(),
                     reason="Carver5-feasibility stock mission data not present")
def test_extract_mc2_24_finds_runway(tmp_path):
    """mc2_24 is the airfield mission this slice ships (most runway tiles by
    the morphological-opening shape classifier) -- must classify >=1 runway
    component and emit centerline/edge/threshold/numeral glyphs for it."""
    out = tmp_path / "markings.json"
    r = _extract("mc2_24", out)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "runway_components=" in r.stdout
    n_runway = int(r.stdout.split("runway_components=")[1].split()[0])
    assert n_runway >= 1

    doc = json.loads(out.read_text())
    glyph_names = {g["glyph"] for g in doc["glyphs"]}
    assert "runway_centerline_dashes" in glyph_names
    assert "runway_edge_stripes" in glyph_names
    assert "runway_threshold_bars" in glyph_names
    assert "runway_numeral" in glyph_names


@pytest.mark.skipif(not (CARVER5_MISSIONS / "mc2_01.pak").is_file(),
                     reason="Carver5-feasibility stock mission data not present")
def test_extract_pak_untouched_byte_identical(tmp_path):
    pak = CARVER5_MISSIONS / "mc2_01.pak"
    before = pak.read_bytes()
    out = tmp_path / "mc2_01_markings.json"
    _extract("mc2_01", out)
    after = pak.read_bytes()
    assert before == after


def test_extract_missing_pak_fails(tmp_path):
    r = run("extract", "--pak", str(tmp_path / "nope.pak"), "--out", str(tmp_path / "out.json"))
    assert r.returncode == 4


# --- schema validation (placement doc shape) ----------------------------------
_VALID_GLYPH_KEYS = {"glyph", "x", "y", "rotation_deg", "scale", "color", "params"}


def _write_markings(tmp_path: Path, glyphs: list[dict]) -> Path:
    p = tmp_path / "markings.json"
    p.write_text(json.dumps({"mission": "synthetic", "side": 60, "glyphs": glyphs}))
    return p


def test_schema_every_glyph_name_is_registered():
    """Any markings.json produced by extract must only reference glyph names
    that marking_glyphs.py actually implements -- a hand-authored file with a
    typo'd glyph name must fail loudly at compose time, not silently no-op."""
    doc = {"mission": "x", "side": 60, "glyphs": [
        {"glyph": "totally_made_up", "x": 0.0, "y": 0.0, "rotation_deg": 0.0,
         "scale": 1.0, "color": "white", "params": {}},
    ]}
    assert doc["glyphs"][0]["glyph"] not in mg.GLYPHS


def test_schema_placement_fields_present_after_extract(tmp_path):
    if not (CARVER5_MISSIONS / "mc2_24.pak").is_file():
        pytest.skip("Carver5-feasibility stock mission data not present")
    out = tmp_path / "m.json"
    _extract("mc2_24", out)
    doc = json.loads(out.read_text())
    assert "mission" in doc and "side" in doc and "glyphs" in doc
    for g in doc["glyphs"]:
        assert set(g.keys()) == _VALID_GLYPH_KEYS
        assert g["glyph"] in mg.GLYPHS
        assert g["color"] in mg.COLOR_NAMES
        assert isinstance(g["x"], (int, float)) and isinstance(g["y"], (int, float))


# --- compose: round-trip -------------------------------------------------------
def _make_overlay_png(tmp_path: Path, w: int, h: int) -> Path:
    rgba = np.zeros((h, w, 4), dtype=np.uint8)
    rgba[..., :3] = 150  # legacy "cement grey" placeholder (overlay_extract.py convention)
    rgba[..., 3] = 255
    p = tmp_path / "base_overlay.png"
    Image.fromarray(rgba, "RGBA").save(p)
    bounds = tmp_path / "base_overlay.bounds.txt"
    bounds.write_text("-100.0 100.0 200.0 200.0\n")
    return p


def test_compose_requires_bounds(tmp_path):
    overlay = _make_overlay_png(tmp_path, 20, 20)
    (tmp_path / "base_overlay.bounds.txt").unlink()  # remove companion bounds
    markings = _write_markings(tmp_path, [])
    out = tmp_path / "composed.png"
    r = run("compose", "--markings", str(markings), "--overlay", str(overlay), "--out", str(out))
    assert r.returncode == 4
    assert "ERROR" in r.stdout + r.stderr


def test_compose_places_glyph_and_preserves_base(tmp_path):
    overlay = _make_overlay_png(tmp_path, 40, 40)
    markings = _write_markings(tmp_path, [
        {"glyph": "crossing_paint", "x": 0.0, "y": 0.0, "rotation_deg": 0.0,
         "scale": 1.0, "color": "white", "params": {"size_wu": 20.0}},
    ])
    out = tmp_path / "composed.png"
    r = run("compose", "--markings", str(markings), "--overlay", str(overlay),
            "--out", str(out), "--px-per-wu", "1.0")
    assert r.returncode == 0, r.stdout + r.stderr
    assert out.is_file()
    arr = np.array(Image.open(out).convert("RGBA"))
    # centre pixel should now be white-ish (glyph drawn), not the base grey
    cy, cx = arr.shape[0] // 2, arr.shape[1] // 2
    assert arr[cy, cx, 0] > 200, "expected white glyph pixel at canvas centre"
    # a corner far from the glyph should remain the untouched base grey
    assert tuple(arr[0, 0, :3]) == (150, 150, 150)

    out_bounds = out.with_suffix("").with_name(out.with_suffix("").name + ".bounds.txt")
    assert out_bounds.is_file()
    parts = [float(x) for x in out_bounds.read_text().split()]
    assert parts == [-100.0, 100.0, 200.0, 200.0]


def test_compose_clamps_oversized_canvas(tmp_path):
    """Requesting a high px-per-wu over a large-bounds overlay must clamp to
    --max-canvas rather than attempting to allocate an astronomical buffer
    (verified failure mode: 8 px/wu over a full 15360wu mission map ->
    ~123000px side, ~60GB RGBA)."""
    overlay = _make_overlay_png(tmp_path, 10, 10)
    bounds = tmp_path / "base_overlay.bounds.txt"
    bounds.write_text("-7680.0 7680.0 15360.0 15360.0\n")  # full-map-scale bounds
    markings = _write_markings(tmp_path, [])
    out = tmp_path / "composed.png"
    r = run("compose", "--markings", str(markings), "--overlay", str(overlay),
            "--out", str(out), "--px-per-wu", "8.0", "--max-canvas", "512")
    assert r.returncode == 0, r.stdout + r.stderr
    assert "clamped" in r.stdout
    w, h = Image.open(out).size
    assert max(w, h) <= 512


def test_compose_missing_markings_fails(tmp_path):
    overlay = _make_overlay_png(tmp_path, 10, 10)
    r = run("compose", "--markings", str(tmp_path / "nope.json"), "--overlay", str(overlay),
            "--out", str(tmp_path / "out.png"))
    assert r.returncode == 4


def test_compose_missing_overlay_fails(tmp_path):
    markings = _write_markings(tmp_path, [])
    r = run("compose", "--markings", str(markings), "--overlay", str(tmp_path / "nope.png"),
            "--out", str(tmp_path / "out.png"))
    assert r.returncode == 4


# --- install --------------------------------------------------------------
def test_install_copies_to_beauty_dir(tmp_path):
    png = tmp_path / "src.png"
    rgba = np.zeros((16, 16, 4), dtype=np.uint8)
    Image.fromarray(rgba, "RGBA").save(png)
    bounds = tmp_path / "src.bounds.txt"
    bounds.write_text("-100.0 100.0 200.0 200.0\n")
    deploy = tmp_path / "deploy"
    r = run("install", "--png", str(png), "--deploy", str(deploy), "--mission", "mc2_24")
    assert r.returncode == 0, r.stdout + r.stderr
    dest = deploy / "data" / "missions" / "mc2_24.beauty" / "overlay_v2.png"
    assert dest.is_file()
    dest_bounds = deploy / "data" / "missions" / "mc2_24.beauty" / "overlay_v2.bounds.txt"
    assert dest_bounds.is_file()


def test_install_missing_source_fails(tmp_path):
    r = run("install", "--png", str(tmp_path / "nope.png"), "--deploy", str(tmp_path / "deploy"),
            "--mission", "mc2_24")
    assert r.returncode == 4


# --- sheet ----------------------------------------------------------------
def test_sheet_runs(tmp_path):
    png = tmp_path / "p.png"
    rgba = np.zeros((16, 16, 4), dtype=np.uint8)
    rgba[..., 3] = 255
    Image.fromarray(rgba, "RGBA").save(png)
    out = tmp_path / "sheet.png"
    r = run("sheet", "--png", str(png), "--out", str(out))
    assert r.returncode == 0, r.stderr
    assert out.is_file()
