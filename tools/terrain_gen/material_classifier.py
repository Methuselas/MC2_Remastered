# tools/terrain_gen/material_classifier.py
from __future__ import annotations
from dataclasses import dataclass
from enum import IntEnum

import numpy as np
from PIL import Image, ImageFilter
from opensimplex import OpenSimplex

from terrain_gen.terrain_recipe import TerrainRecipe


class TerrainType(IntEnum):
    BLUE_WATER   = 0
    GREEN_WATER  = 1
    MUD          = 2
    MOSS         = 3
    DIRT         = 4
    ASH          = 5
    MOUNTAIN     = 6
    TUNDRA       = 7
    FOREST_FLOOR = 8
    GRASS        = 9
    CONCRETE     = 10
    CLIFF        = 11
    SLIMY        = 12
    NONE         = 20


@dataclass
class TerrainMasks:
    altitude:     np.ndarray   # float32 [0,1]
    slope:        np.ndarray   # float32 [0,1]
    curvature:    np.ndarray   # float32 [0,1]
    valley:       np.ndarray   # float32 [0,1]  high = low/flat areas
    noise:        np.ndarray   # float32 [0,1]  mid-freq variation
    terrain_type: np.ndarray   # uint8, TerrainType values


class MaterialClassifier:
    def classify(self, height: np.ndarray, recipe: TerrainRecipe) -> TerrainMasks:
        m = recipe.materials
        N = height.shape[0]

        altitude = height.astype(np.float32)

        # Slope from finite difference gradient
        gy, gx = np.gradient(height.astype(np.float64))
        slope_raw = np.sqrt(gx**2 + gy**2)
        slope = np.clip(slope_raw * 8.0, 0.0, 1.0).astype(np.float32)

        # Curvature (laplacian)
        lap = np.gradient(gx, axis=1) + np.gradient(gy, axis=0)
        curvature = np.clip(np.abs(lap) * 4.0, 0.0, 1.0).astype(np.float32)

        # Valley: inverted blurred altitude (high = low flat areas)
        h_pil  = Image.fromarray((np.clip(height, 0, 1) * 255).astype(np.uint8), mode='L')
        h_blur = np.array(h_pil.filter(ImageFilter.GaussianBlur(radius=max(1, min(N // 8, 48))))) / 255.0
        valley = np.clip(1.0 - h_blur.astype(np.float32), 0.0, 1.0)

        # Noise variation for dirt blending (vectorised; per-pixel python was O(N^2)).
        gen = OpenSimplex(recipe.seed + 2)
        coords = np.arange(N, dtype=np.float64) / N * m.dirt_noise_scale
        noise = gen.noise2array(coords, coords).astype(np.float32)
        noise = (noise - noise.min()) / (noise.max() - noise.min() + 1e-8)

        # TerrainType classification (priority: water < dirt < rock < snow)
        tt = np.full((N, N), TerrainType.GRASS, dtype=np.uint8)

        # Water
        tt[altitude < m.water_level] = TerrainType.GREEN_WATER

        # Low wet zones above water
        moss_mask = (altitude >= m.water_level) & (altitude < m.water_level + 0.05) & (valley > 0.6)
        tt[moss_mask] = TerrainType.MUD

        # Dirt in noisy intermediate zones
        dirt_mask = (altitude >= m.water_level) & (altitude < m.grass_lowland) & (noise > 0.5)
        tt[dirt_mask] = TerrainType.DIRT

        # Rock on steep slope
        rock_mask = (slope > m.rock_slope) & (altitude > m.water_level)
        tt[rock_mask] = TerrainType.MOUNTAIN

        # Cliff on very steep slope
        cliff_mask = slope > (m.rock_slope + 0.2)
        tt[cliff_mask] = TerrainType.CLIFF

        # Curvature-based rock
        curve_mask = (curvature > m.rock_curvature) & (altitude > m.grass_lowland)
        tt[curve_mask] = TerrainType.MOUNTAIN

        # Snow above snow line
        tt[altitude > m.snow_line] = TerrainType.TUNDRA

        # Steep + above snow line = cliff
        tt[(altitude > m.snow_line) & (slope > 0.3)] = TerrainType.CLIFF

        return TerrainMasks(
            altitude=altitude,
            slope=slope,
            curvature=curvature,
            valley=valley,
            noise=noise,
            terrain_type=tt,
        )
