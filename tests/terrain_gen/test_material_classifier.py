# tests/terrain_gen/test_material_classifier.py
import sys, os
import numpy as np
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '../../tools'))

from terrain_gen.terrain_recipe import TerrainRecipe
from terrain_gen.material_classifier import MaterialClassifier, TerrainType, TerrainMasks


def _flat_height(N=60, value=0.5):
    return np.full((N, N), value, dtype=np.float32)


def test_masks_match_height_shape():
    h = _flat_height(60, 0.5)
    r = TerrainRecipe(size=60)
    masks = MaterialClassifier().classify(h, r)
    for name in ('altitude', 'slope', 'curvature', 'valley', 'terrain_type'):
        arr = getattr(masks, name)
        assert arr.shape == (60, 60), f"{name} wrong shape: {arr.shape}"


def test_altitude_mask_matches_height():
    h = _flat_height(60, 0.7)
    r = TerrainRecipe(size=60)
    masks = MaterialClassifier().classify(h, r)
    np.testing.assert_allclose(masks.altitude, h, atol=0.01)


def test_high_altitude_gives_snow_or_mountain():
    h = _flat_height(60, 0.95)   # above any snow_line
    r = TerrainRecipe(size=60)
    r.materials.snow_line = 0.8
    masks = MaterialClassifier().classify(h, r)
    dominant = np.bincount(masks.terrain_type.ravel()).argmax()
    assert dominant in (TerrainType.TUNDRA, TerrainType.MOUNTAIN)


def test_low_flat_gives_grass():
    h = _flat_height(60, 0.3)
    r = TerrainRecipe(size=60)
    r.materials.water_level = 0.1
    r.materials.snow_line   = 0.8
    r.materials.grass_lowland = 0.2  # 0.3 is above threshold → no dirt mask
    masks = MaterialClassifier().classify(h, r)
    dominant = np.bincount(masks.terrain_type.ravel()).argmax()
    assert dominant == TerrainType.GRASS


def test_below_water_level_gives_water():
    h = _flat_height(60, 0.03)
    r = TerrainRecipe(size=60)
    r.materials.water_level = 0.10
    masks = MaterialClassifier().classify(h, r)
    dominant = np.bincount(masks.terrain_type.ravel()).argmax()
    assert dominant in (TerrainType.BLUE_WATER, TerrainType.GREEN_WATER)


def test_terrain_type_values_in_range():
    h = np.random.default_rng(0).random((60, 60)).astype(np.float32)
    r = TerrainRecipe(size=60)
    masks = MaterialClassifier().classify(h, r)
    assert masks.terrain_type.min() >= 0
    assert masks.terrain_type.max() <= 20


def test_slope_zero_on_flat():
    h = _flat_height(60, 0.5)
    r = TerrainRecipe(size=60)
    masks = MaterialClassifier().classify(h, r)
    assert masks.slope.max() < 0.01


def test_terrain_masks_dtype():
    h = _flat_height(60, 0.5)
    r = TerrainRecipe(size=60)
    masks = MaterialClassifier().classify(h, r)
    assert masks.terrain_type.dtype == np.uint8
    assert masks.slope.dtype == np.float32
