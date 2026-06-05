# tests/terrain_gen/test_terrain_recipe.py
import pytest
from terrain_gen.terrain_recipe import TerrainRecipe
from terrain_gen.biome_presets import BIOMES


def test_default_recipe_fields():
    r = TerrainRecipe()
    assert r.size == 120
    assert r.biome == "temperate_hills"
    assert r.seed == 12345
    assert r.version == 1


def test_size_must_be_multiple_of_20():
    with pytest.raises(ValueError, match="multiple of 20"):
        TerrainRecipe(size=110)


def test_size_must_be_in_range():
    with pytest.raises(ValueError, match="range"):
        TerrainRecipe(size=40)
    with pytest.raises(ValueError, match="range"):
        TerrainRecipe(size=3000)


def test_valid_sizes_accepted():
    for s in (60, 80, 100, 120, 1000, 1020, 2000):
        r = TerrainRecipe(size=s)
        assert r.size == s


def test_json_roundtrip(tmp_path):
    r = TerrainRecipe(size=100, biome="snow_mountain", seed=999)
    path = tmp_path / "recipe.json"
    r.to_json(path)
    r2 = TerrainRecipe.from_json(path)
    assert r2.size == 100
    assert r2.biome == "snow_mountain"
    assert r2.seed == 999


def test_apply_biome_overrides_defaults():
    r = TerrainRecipe(size=120, biome="snow_mountain")
    r.apply_biome()
    assert r.materials.snow_line < 0.6   # snow_mountain has lower snow line than temperate


def test_burnin_resolution_fixed_1280():
    # MVP: always 1280 for MC2 classic parity
    for size in (60, 80, 100, 120, 1000):
        assert TerrainRecipe(size=size).burnin_resolution() == 1280


def test_all_biomes_present():
    for name in ("temperate_hills", "rocky_badlands", "snow_mountain", "swamp_forest", "desert"):
        assert name in BIOMES
        p = BIOMES[name].palette
        assert len(p.grass) == 3    # RGB tuple


def test_palette_values_in_range():
    for preset in BIOMES.values():
        for field in ("grass", "dry_grass", "dirt", "rock", "dark_rock", "snow", "mud", "water", "forest_floor"):
            rgb = getattr(preset.palette, field)
            assert all(0 <= v <= 255 for v in rgb), f"{field} out of range: {rgb}"
