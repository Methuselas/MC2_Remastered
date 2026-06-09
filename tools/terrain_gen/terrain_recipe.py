# tools/terrain_gen/terrain_recipe.py
from __future__ import annotations
import json
from dataclasses import dataclass, field, asdict
from pathlib import Path
from .biome_presets import BIOMES


@dataclass
class HeightParams:
    generator:        str   = "layered_noise"
    base_frequency:   float = 4.0   # applied to normalized [0,1] coords; 2-8 = macro hills
    octaves:          int   = 6
    persistence:      float = 0.5
    lacunarity:       float = 2.0
    ridged_amount:    float = 0.5
    mountain_amount:  float = 0.55
    erosion_passes:   int   = 3
    plateau_strength: float = 0.1
    min_elevation:    float = 0.0
    max_elevation:    float = 420.0


@dataclass
class MaterialParams:
    water_level:      float = 0.08
    snow_line:        float = 0.78
    rock_slope:       float = 0.55
    rock_curvature:   float = 0.4
    grass_lowland:    float = 0.35
    dirt_noise:       float = 0.25
    dirt_noise_scale: float = 5.0   # noise frequency on normalized [0,1] coords


@dataclass
class BurninParams:
    ao_strength:     float = 0.35
    slope_shading:   float = 0.25
    grain_scale:     float = 0.5
    color_variation: float = 0.12


@dataclass
class TerrainRecipe:
    version:   int           = 1
    name:      str           = "unnamed"
    size:      int           = 120
    biome:     str           = "temperate_hills"
    seed:      int           = 12345
    height:    HeightParams   = field(default_factory=HeightParams)
    materials: MaterialParams = field(default_factory=MaterialParams)
    burnin:    BurninParams   = field(default_factory=BurninParams)

    def __post_init__(self):
        if not (60 <= self.size <= 2048):
            raise ValueError(f"size {self.size} out of range [60, 2048]")
        if self.size % 20 != 0:
            raise ValueError(f"size {self.size} must be a multiple of 20 (verticesBlockSide=20)")
        if self.biome not in BIOMES:
            raise ValueError(f"unknown biome '{self.biome}'; valid: {list(BIOMES)}")

    def burnin_resolution(self) -> int:
        # Historical engine-matched colormap resolution:
        #   (vertices / verticesBlockSide) * 256
        # The renderer may cap final output, currently 4096² max, because
        # high-frequency detail comes from normal/detail maps rather than enormous
        # baked colormaps. (verticesBlockSide=20, tile=256; 20*12.8=256, so this
        # equals size*12.8.)
        return (self.size // 20) * 256

    def apply_biome(self) -> None:
        """Overlay biome preset defaults onto height/material params, but NEVER
        clobber fields the recipe explicitly overrode (see _overrides, populated by
        from_json). Lets the editor's Map Generator dialog set height/water/etc."""
        ov = getattr(self, '_overrides', {})
        preset = BIOMES[self.biome]
        for k, v in preset.height.items():
            if k not in ov.get('height', ()):
                setattr(self.height, k, v)
        for k, v in preset.materials.items():
            if k not in ov.get('materials', ()):
                setattr(self.materials, k, v)

    def to_json(self, path: Path | str) -> None:
        with open(path, 'w') as f:
            json.dump(asdict(self), f, indent=2)

    @classmethod
    def from_json(cls, path: Path | str) -> TerrainRecipe:
        with open(path) as f:
            d = json.load(f)
        height    = HeightParams(**d.get('height', {}))
        materials = MaterialParams(**d.get('materials', {}))
        burnin    = BurninParams(**d.get('burnin', {}))
        r = cls(
            version=d.get('version', 1),
            name=d.get('name', 'unnamed'),
            size=d['size'],
            biome=d.get('biome', 'temperate_hills'),
            seed=d.get('seed', 12345),
            height=height,
            materials=materials,
            burnin=burnin,
        )
        # Record explicitly-provided height/materials keys so apply_biome() won't
        # overwrite them (editor Map Generator dialog sends these as overrides).
        r._overrides = {
            'height':    set(d.get('height', {}).keys()),
            'materials': set(d.get('materials', {}).keys()),
        }
        return r
