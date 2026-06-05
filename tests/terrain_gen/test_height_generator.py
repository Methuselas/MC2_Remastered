# tests/terrain_gen/test_height_generator.py
import sys, os
import numpy as np
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '../../tools'))

from terrain_gen.terrain_recipe import TerrainRecipe
from terrain_gen.height_generator import HeightGenerator


def _recipe(size=60, seed=42, **kw) -> TerrainRecipe:
    r = TerrainRecipe(size=size, seed=seed)
    for k, v in kw.items():
        setattr(r.height, k, v)
    return r


def test_output_shape():
    h = HeightGenerator().generate(_recipe(size=60))
    assert h.shape == (60, 60)


def test_output_range():
    h = HeightGenerator().generate(_recipe(size=60))
    assert h.min() >= 0.0
    assert h.max() <= 1.0


def test_dtype_float32():
    h = HeightGenerator().generate(_recipe(size=60))
    assert h.dtype == np.float32


def test_deterministic():
    r = _recipe(size=60, seed=123)
    h1 = HeightGenerator().generate(r)
    h2 = HeightGenerator().generate(r)
    np.testing.assert_array_equal(h1, h2)


def test_different_seeds_differ():
    h1 = HeightGenerator().generate(_recipe(size=60, seed=1))
    h2 = HeightGenerator().generate(_recipe(size=60, seed=2))
    assert not np.allclose(h1, h2)


def test_mountain_amount_increases_variance():
    low  = HeightGenerator().generate(_recipe(size=60, seed=7, mountain_amount=0.0))
    high = HeightGenerator().generate(_recipe(size=60, seed=7, mountain_amount=1.0))
    assert high.std() > low.std()


def test_erosion_smooths_gradients():
    r0 = _recipe(size=60, seed=5, erosion_passes=0)
    r4 = _recipe(size=60, seed=5, erosion_passes=4)
    h0 = HeightGenerator().generate(r0)
    h4 = HeightGenerator().generate(r4)
    gy0, gx0 = np.gradient(h0.astype(float))
    gy4, gx4 = np.gradient(h4.astype(float))
    grad0 = np.sqrt(gx0**2 + gy0**2)
    grad4 = np.sqrt(gx4**2 + gy4**2)
    assert grad4.mean() < grad0.mean()
