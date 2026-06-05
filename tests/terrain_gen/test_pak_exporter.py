# tests/terrain_gen/test_pak_exporter.py
import sys, os, struct, math
import numpy as np
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '../../tools'))

from terrain_gen.terrain_recipe import TerrainRecipe
from terrain_gen.height_generator import HeightGenerator
from terrain_gen.material_classifier import MaterialClassifier, TerrainType
from terrain_gen.pak_exporter import PakExporter

VERTEX_STRUCT = struct.Struct('<3f f I I I 4B')
NEUTRAL_LIGHT = 0x00CCCCCC


def _run(size=60):
    r = TerrainRecipe(size=size, seed=1)
    h = HeightGenerator().generate(r)
    masks = MaterialClassifier().classify(h, r)
    return r, h, masks, PakExporter().build_packet0(h, masks, r)


def test_packet0_size_60():
    r, h, masks, data = _run(size=60)
    assert len(data) == 60 * 60 * VERTEX_STRUCT.size


def test_packet0_size_100():
    r, h, masks, data = _run(size=100)
    assert len(data) == 100 * 100 * VERTEX_STRUCT.size


def test_elevation_encoded_correctly():
    r, h, masks, data = _run(size=60)
    v = VERTEX_STRUCT.unpack_from(data, 0)
    elevation_stored = v[3]
    expected_elev = float(h[0, 0]) * r.height.max_elevation
    assert abs(elevation_stored - expected_elev) < 0.5, \
        f"elevation mismatch: {elevation_stored} vs {expected_elev}"


def test_terrain_type_encoded():
    r, h, masks, data = _run(size=60)
    for y in range(60):
        for x in range(60):
            idx = y * 60 + x
            v = VERTEX_STRUCT.unpack_from(data, idx * VERTEX_STRUCT.size)
            stored_tt = v[6]
            expected_tt = int(masks.terrain_type[y, x])
            assert stored_tt == expected_tt, f"terrain_type mismatch at ({x},{y})"


def test_neutral_lighting_not_zero():
    r, h, masks, data = _run(size=60)
    v = VERTEX_STRUCT.unpack_from(data, 0)
    assert v[5] != 0, "localRGBLight must not be zero"
    assert v[5] == NEUTRAL_LIGHT, f"expected {NEUTRAL_LIGHT:#010x}, got {v[5]:#010x}"


def test_vertex_normals_unit_length():
    r, h, masks, data = _run(size=60)
    for i in range(min(100, 60 * 60)):
        v = VERTEX_STRUCT.unpack_from(data, i * VERTEX_STRUCT.size)
        nx, ny, nz = v[0], v[1], v[2]
        length = math.sqrt(nx*nx + ny*ny + nz*nz)
        assert abs(length - 1.0) < 0.01, f"normal not unit at vertex {i}: length={length:.4f}"


def test_patch_pak_same_size(tmp_path):
    N = 60
    pkt0_size = N * N * VERTEX_STRUCT.size
    first_pkt_offset = 16  # magic(4) + fpo(4) + 2 seek entries(8)
    fake_pak = (
        struct.pack('<I', 0xFEEDFACE) +
        struct.pack('<I', first_pkt_offset) +
        struct.pack('<i', first_pkt_offset) +
        struct.pack('<i', first_pkt_offset + pkt0_size) +
        bytes(pkt0_size)
    )
    template = tmp_path / "template.pak"
    template.write_bytes(fake_pak)

    r, h, masks, new_pkt0 = _run(size=60)
    out = tmp_path / "out.pak"
    PakExporter().patch_pak(str(template), str(out), new_pkt0)

    result = out.read_bytes()
    assert result[:first_pkt_offset] == fake_pak[:first_pkt_offset], "header changed"
    assert result[first_pkt_offset:first_pkt_offset + pkt0_size] == new_pkt0, "pkt0 not replaced"


def test_patch_pak_size_mismatch_raises(tmp_path):
    N = 60
    pkt0_size = N * N * VERTEX_STRUCT.size
    first_pkt_offset = 16
    fake_pak = (
        struct.pack('<I', 0xFEEDFACE) +
        struct.pack('<I', first_pkt_offset) +
        struct.pack('<i', first_pkt_offset) +
        struct.pack('<i', first_pkt_offset + pkt0_size) +
        bytes(pkt0_size)
    )
    template = tmp_path / "template.pak"
    template.write_bytes(fake_pak)

    # Build packet for wrong size (100 instead of 60)
    r2, h2, masks2, wrong_pkt0 = _run(size=100)
    import pytest
    with pytest.raises(ValueError, match="size mismatch"):
        PakExporter().patch_pak(str(template), str(tmp_path / "out.pak"), wrong_pkt0)
