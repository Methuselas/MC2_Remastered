#!/usr/bin/env python3
"""pytest for control_map_tool.py: pattern determinism, validate error cases,
and (skip-if-missing) generate against a real Carver5 stock .pak."""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest
from PIL import Image

HERE = Path(__file__).resolve().parent
TOOL = HERE / "control_map_tool.py"
CARVER5_MISSIONS = Path("A:/Games/Carver5-feasibility/data/missions")


def run(*args) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(TOOL), *args],
        capture_output=True, text=True,
    )


# --- pattern determinism -----------------------------------------------------
def test_pattern_quadrant_deterministic(tmp_path):
    out1 = tmp_path / "a.png"
    out2 = tmp_path / "b.png"
    r1 = run("pattern", "--side", "60", "--out", str(out1), "--kind", "quadrant")
    r2 = run("pattern", "--side", "60", "--out", str(out2), "--kind", "quadrant")
    assert r1.returncode == 0, r1.stderr
    assert r2.returncode == 0, r2.stderr
    a1 = np.array(Image.open(out1))
    a2 = np.array(Image.open(out2))
    assert np.array_equal(a1, a2)


def test_pattern_quadrant_corners(tmp_path):
    out = tmp_path / "q.png"
    run("pattern", "--side", "60", "--out", str(out), "--kind", "quadrant")
    arr = np.array(Image.open(out).convert("RGBA"))
    side = arr.shape[0]
    # Row 0 == top per documented row-order convention.
    assert tuple(arr[0, 0]) == (255, 0, 0, 0)               # top-left: rock
    assert tuple(arr[0, side - 1]) == (0, 255, 0, 0)        # top-right: grass
    assert tuple(arr[side - 1, 0]) == (0, 0, 255, 0)        # bottom-left: dirt
    assert tuple(arr[side - 1, side - 1]) == (0, 0, 0, 255)  # bottom-right: concrete


def test_pattern_gradient_shape_and_range(tmp_path):
    out = tmp_path / "g.png"
    r = run("pattern", "--side", "80", "--out", str(out), "--kind", "gradient")
    assert r.returncode == 0, r.stderr
    arr = np.array(Image.open(out).convert("RGBA"))
    assert arr.shape == (80, 80, 4)
    # R decreases left->right, G increases left->right, each row identical.
    row = arr[0]
    assert row[0, 0] > row[-1, 0]
    assert row[0, 1] < row[-1, 1]
    assert np.array_equal(arr[0], arr[-1])  # same gradient every row


def test_pattern_checker_binary_channels(tmp_path):
    out = tmp_path / "c.png"
    r = run("pattern", "--side", "64", "--out", str(out), "--kind", "checker")
    assert r.returncode == 0, r.stderr
    arr = np.array(Image.open(out).convert("RGBA"))
    # Every texel is pure rock XOR pure grass, dirt/concrete unused.
    assert np.all(arr[..., 2] == 0)
    assert np.all(arr[..., 3] == 0)
    is_rock = arr[..., 0] == 255
    is_grass = arr[..., 1] == 255
    assert np.all(is_rock ^ is_grass)


def test_pattern_bad_kind_fails(tmp_path):
    out = tmp_path / "bad.png"
    r = run("pattern", "--side", "60", "--out", str(out), "--kind", "notarealkind")
    assert r.returncode != 0


def test_pattern_side_too_small_fails(tmp_path):
    out = tmp_path / "tiny.png"
    r = run("pattern", "--side", "1", "--out", str(out), "--kind", "quadrant")
    assert r.returncode != 0


# --- validate -----------------------------------------------------------------
def test_validate_correct_size_passes(tmp_path):
    png = tmp_path / "ok.png"
    run("pattern", "--side", "60", "--out", str(png), "--kind", "quadrant")
    r = run("validate", "--png", str(png), "--side", "60")
    assert r.returncode == 0, r.stdout + r.stderr
    assert "PASS" in r.stdout


def test_validate_wrong_size_fails(tmp_path):
    png = tmp_path / "wrong.png"
    run("pattern", "--side", "60", "--out", str(png), "--kind", "quadrant")
    r = run("validate", "--png", str(png), "--side", "80")
    assert r.returncode == 1
    assert "FAIL" in r.stdout


def test_validate_non_square_fails(tmp_path):
    png = tmp_path / "nonsquare.png"
    Image.new("RGBA", (60, 40), (0, 0, 0, 0)).save(png)
    r = run("validate", "--png", str(png))
    assert r.returncode == 1
    assert "FAIL" in r.stdout


def test_validate_wrong_mode_fails(tmp_path):
    png = tmp_path / "rgb.png"
    Image.new("RGB", (60, 60), (10, 20, 30)).save(png)
    r = run("validate", "--png", str(png))
    assert r.returncode == 1
    assert "FAIL" in r.stdout


def test_validate_missing_file_fails(tmp_path):
    r = run("validate", "--png", str(tmp_path / "nope.png"))
    assert r.returncode == 1


# --- sheet ----------------------------------------------------------------
def test_sheet_runs_on_pattern(tmp_path):
    png = tmp_path / "p.png"
    run("pattern", "--side", "60", "--out", str(png), "--kind", "quadrant")
    out = tmp_path / "sheet.png"
    r = run("sheet", "--png", str(png), "--out", str(out))
    assert r.returncode == 0, r.stderr
    assert out.is_file()


# --- install ----------------------------------------------------------------
def test_install_copies_to_beauty_dir(tmp_path):
    png = tmp_path / "src.png"
    run("pattern", "--side", "60", "--out", str(png), "--kind", "quadrant")
    deploy = tmp_path / "deploy"
    r = run("install", "--png", str(png), "--deploy", str(deploy), "--mission", "mc2_01")
    assert r.returncode == 0, r.stderr
    dest = deploy / "data" / "missions" / "mc2_01.beauty" / "control_map.png"
    assert dest.is_file()


# --- generate (skip if Carver5 stock data absent) ----------------------------
@pytest.mark.skipif(not (CARVER5_MISSIONS / "mc2_01.pak").is_file(),
                     reason="Carver5-feasibility stock mission data not present")
def test_generate_runs_on_mc2_01(tmp_path):
    out = tmp_path / "mc2_01_control_map.png"
    r = run("generate", "--pak", str(CARVER5_MISSIONS / "mc2_01.pak"), "--out", str(out))
    assert r.returncode == 0, r.stdout + r.stderr
    assert out.is_file()
    img = Image.open(out)
    assert img.mode == "RGBA"
    w, h = img.size
    assert w == h
    assert w in (60, 80, 100, 120)
    # every texel should classify to exactly one material (one-hot 0/255 channels)
    arr = np.array(img)
    total = arr.sum(axis=-1)
    assert np.all((total == 255) | (total == 0))


@pytest.mark.skipif(not (CARVER5_MISSIONS / "mc2_01.pak").is_file(),
                     reason="Carver5-feasibility stock mission data not present")
def test_generate_output_validates(tmp_path):
    out = tmp_path / "mc2_01_cm.png"
    run("generate", "--pak", str(CARVER5_MISSIONS / "mc2_01.pak"), "--out", str(out))
    r = run("validate", "--png", str(out), "--pak", str(CARVER5_MISSIONS / "mc2_01.pak"))
    assert r.returncode == 0, r.stdout + r.stderr
