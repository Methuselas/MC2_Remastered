# tools/terrain_gen/burnin_renderer.py
from __future__ import annotations

import numpy as np
from PIL import Image, ImageFilter
from opensimplex import OpenSimplex

from terrain_gen.terrain_recipe import TerrainRecipe
from terrain_gen.material_classifier import TerrainMasks
from terrain_gen.biome_presets import BiomePreset


class BurninRenderer:
    def render(self, masks: TerrainMasks, recipe: TerrainRecipe, preset: BiomePreset) -> Image.Image:
        res = recipe.burnin_resolution()
        b   = recipe.burnin
        p   = preset.palette
        m   = recipe.materials

        def up(arr: np.ndarray) -> np.ndarray:
            img = Image.fromarray((np.clip(arr, 0, 1) * 255).astype(np.uint8), mode='L')
            return np.array(img.resize((res, res), Image.BICUBIC)) / 255.0

        alt    = up(masks.altitude)
        slope  = up(masks.slope)
        curv   = up(masks.curvature)
        valley = up(masks.valley)
        noise  = up(masks.noise)

        # Continuous blend weights (each pixel is a weighted mix of palette entries)
        w_snow  = np.clip((alt - m.snow_line) / 0.12, 0, 1)
        w_rock  = np.clip((slope - (m.rock_slope - 0.15)) / 0.15, 0, 1) * (1.0 - w_snow)
        w_rock += np.clip((curv - (m.rock_curvature - 0.15)) / 0.15, 0, 1) * (1.0 - w_snow - w_rock)
        w_rock  = np.clip(w_rock, 0, 1)
        w_water = np.clip((m.water_level + 0.04 - alt) / 0.04, 0, 1)
        w_grass = np.clip(1.0 - w_snow - w_rock - w_water, 0, 1)
        w_dirt  = w_grass * np.clip(noise * 2.0 - 0.5, 0, 1) * (1.0 - np.clip(alt / (m.grass_lowland + 1e-8), 0, 1))
        w_grass = np.clip(w_grass - w_dirt, 0, 1)
        w_mud   = w_grass * valley * 0.3
        w_grass = np.clip(w_grass - w_mud, 0, 1)
        w_forest = w_grass * np.clip((m.grass_lowland - alt) / (m.grass_lowland + 0.01), 0, 1) * 0.4
        w_grass = np.clip(w_grass - w_forest, 0, 1)

        def to_f(rgb):
            return np.array([v / 255.0 for v in rgb], dtype=np.float64)

        color = (
            to_f(p.grass)        * w_grass[..., None]  +
            to_f(p.dirt)         * w_dirt[..., None]   +
            to_f(p.mud)          * w_mud[..., None]    +
            to_f(p.forest_floor) * w_forest[..., None] +
            to_f(p.rock)         * w_rock[..., None] * 0.6 +
            to_f(p.dark_rock)    * w_rock[..., None] * 0.4 +
            to_f(p.snow)         * w_snow[..., None]   +
            to_f(p.water)        * w_water[..., None]
        )
        color = np.clip(color, 0, 1)

        # Ambient occlusion
        alt_pil  = Image.fromarray((alt * 255).astype(np.uint8), mode='L')
        alt_blur = np.array(alt_pil.filter(ImageFilter.GaussianBlur(radius=max(1, res // 20)))) / 255.0
        ao = np.clip(alt - alt_blur, 0, 1)
        color *= (1.0 - b.ao_strength * ao)[..., None]

        # Slope shading
        color *= (1.0 - b.slope_shading * slope)[..., None]

        # Erosion streaks
        streak = valley * slope * 0.3
        color = color * (1.0 - streak[..., None])

        # Large color variation (vectorised; the per-pixel python loop over res*res
        # = ~1.6M noise2 calls dominated generation time).
        gen = OpenSimplex(recipe.seed + 3)
        vc = np.arange(res, dtype=np.float64) / res * 4.0
        var_noise = gen.noise2array(vc, vc).astype(np.float32)
        var_noise = (var_noise - var_noise.min()) / (var_noise.max() - var_noise.min() + 1e-8)
        color = color * (1.0 - b.color_variation * 0.5 + var_noise[..., None] * b.color_variation)

        # Fine grain
        rng = np.random.default_rng(recipe.seed)
        grain = (rng.random((res, res)) - 0.5) * b.grain_scale * 0.05
        color += grain[..., None]

        color = np.clip(color * 255, 0, 255).astype(np.uint8)
        return Image.fromarray(color, mode='RGB')
