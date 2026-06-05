# tools/terrain_gen/biome_presets.py
from dataclasses import dataclass
from typing import Tuple

RGB = Tuple[int, int, int]


@dataclass
class Palette:
    grass:        RGB
    dry_grass:    RGB
    dirt:         RGB
    rock:         RGB
    dark_rock:    RGB
    snow:         RGB
    mud:          RGB
    water:        RGB
    forest_floor: RGB


@dataclass
class BiomePreset:
    palette:   Palette
    height:    dict   # partial overrides for HeightParams fields
    materials: dict   # partial overrides for MaterialParams fields


BIOMES: dict[str, BiomePreset] = {
    "temperate_hills": BiomePreset(
        palette=Palette(
            grass=(122, 138, 69),
            dry_grass=(160, 144, 80),
            dirt=(138, 112, 85),
            rock=(112, 112, 112),
            dark_rock=(72, 68, 64),
            snow=(220, 225, 228),
            mud=(90, 80, 64),
            water=(40, 80, 120),
            forest_floor=(62, 79, 50),
        ),
        height=dict(base_frequency=3.5, ridged_amount=0.35, mountain_amount=0.4, erosion_passes=2),
        materials=dict(snow_line=0.82, rock_slope=0.55, grass_lowland=0.4),
    ),
    "rocky_badlands": BiomePreset(
        palette=Palette(
            grass=(120, 128, 80),
            dry_grass=(160, 140, 80),
            dirt=(154, 136, 96),
            rock=(122, 112, 104),
            dark_rock=(90, 82, 72),
            snow=(200, 198, 188),
            mud=(100, 90, 72),
            water=(50, 80, 100),
            forest_floor=(80, 90, 60),
        ),
        height=dict(base_frequency=5.0, ridged_amount=0.6, mountain_amount=0.55, erosion_passes=4),
        materials=dict(snow_line=0.88, rock_slope=0.40, grass_lowland=0.25),
    ),
    "snow_mountain": BiomePreset(
        # Desolate frozen-rock biome: NOT a white snowfield. The dominant ground is
        # barren grey-brown frozen dirt/rock; snow only caps the very highest peaks
        # (~<=1%). Vegetation palette entries are recoloured to bleak greys so the
        # majority "grass" classification reads as barren tundra, not green.
        palette=Palette(
            grass=(118, 114, 104),       # barren frozen ground (grey-tan)
            dry_grass=(132, 124, 110),
            dirt=(96, 88, 76),
            rock=(112, 108, 102),
            dark_rock=(74, 70, 66),
            snow=(220, 224, 228),
            mud=(82, 78, 70),
            water=(58, 74, 92),
            forest_floor=(92, 90, 84),   # no real forest here; keep barren
        ),
        height=dict(base_frequency=4.5, ridged_amount=0.7, mountain_amount=0.7, erosion_passes=3),
        materials=dict(snow_line=0.98, rock_slope=0.30, grass_lowland=0.18),
    ),
    "swamp_forest": BiomePreset(
        palette=Palette(
            grass=(58, 78, 42),
            dry_grass=(80, 88, 56),
            dirt=(72, 68, 56),
            rock=(88, 90, 80),
            dark_rock=(60, 64, 50),
            snow=(180, 185, 175),
            mud=(90, 80, 64),
            water=(42, 60, 42),
            forest_floor=(58, 78, 48),
        ),
        height=dict(base_frequency=2.5, ridged_amount=0.1, mountain_amount=0.15, erosion_passes=2),
        materials=dict(snow_line=0.95, rock_slope=0.65, grass_lowland=0.55, water_level=0.15),
    ),
    "desert": BiomePreset(
        palette=Palette(
            grass=(192, 168, 120),
            dry_grass=(200, 180, 130),
            dirt=(176, 144, 112),
            rock=(154, 136, 108),
            dark_rock=(122, 104, 80),
            snow=(220, 210, 190),
            mud=(138, 120, 90),
            water=(80, 100, 120),
            forest_floor=(100, 115, 70),
        ),
        height=dict(base_frequency=3.0, ridged_amount=0.3, mountain_amount=0.25, erosion_passes=1),
        materials=dict(snow_line=0.92, rock_slope=0.60, grass_lowland=0.30, water_level=0.04),
    ),
}
