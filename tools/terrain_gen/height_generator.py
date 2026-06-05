# tools/terrain_gen/height_generator.py
import numpy as np
from PIL import Image, ImageFilter
from opensimplex import OpenSimplex
from terrain_gen.terrain_recipe import TerrainRecipe


class HeightGenerator:
    def generate(self, recipe: TerrainRecipe) -> np.ndarray:
        """Return float32 ndarray [size, size] in [0, 1]."""
        N = recipe.size
        h = recipe.height
        gen1 = OpenSimplex(recipe.seed)
        gen2 = OpenSimplex(recipe.seed + 1)

        # fx/fy are normalized [0,1] coords. base_frequency ~4 gives macro hills.
        # Vectorised with opensimplex.noise2array (C-accelerated) -- the old per-pixel
        # python double loop was O(N^2) and took minutes at 1020x1020.
        coords = np.arange(N, dtype=np.float64) / N   # normalized [0,1)
        base = self._fbm_array(gen1, coords, h.octaves, h.persistence, h.lacunarity, h.base_frequency)
        if h.mountain_amount > 0.0:
            ridged = self._ridged_array(gen2, coords, h.base_frequency * 2.0)
            detail = self._fbm_array(gen2, coords, h.octaves, h.persistence, h.lacunarity, h.base_frequency)
            mountain = detail + ridged * h.ridged_amount
            height = base + mountain * h.mountain_amount * 0.4
        else:
            height = base

        # Map to [0, 1] via tanh.
        # mountain_amount increases the tanh contrast (scale), which monotonically
        # increases the std of the output — sharp peaks push toward 0/1 extremes.
        tanh_scale = 2.0 + h.mountain_amount * 3.0  # 2.0 (flat) → 5.0 (mountains)
        height = (np.tanh(height * tanh_scale) + 1.0) * 0.5

        # Plateau (flatten peaks slightly)
        if h.plateau_strength > 0:
            height = np.power(height, 1.0 + h.plateau_strength)
            mx2 = height.max()
            if mx2 > 0:
                height /= mx2

        # Fake erosion passes
        height = self._fake_erosion(height, h.erosion_passes)

        return height.astype(np.float32)

    def _fbm_array(self, gen, coords, octaves, persistence, lacunarity, freq):
        """Vectorised fBm over an NxN grid. coords = normalized [0,1) per axis.
        noise2array(x, y) returns shape (len(y), len(x)) = result[y, x]."""
        N = coords.shape[0]
        value = np.zeros((N, N), dtype=np.float64)
        amplitude = 1.0
        total_amp = 0.0
        f = freq
        for _ in range(octaves):
            value     += gen.noise2array(coords * f, coords * f) * amplitude
            total_amp += amplitude
            amplitude *= persistence
            f         *= lacunarity
        return value / total_amp

    def _ridged_array(self, gen, coords, freq):
        n = gen.noise2array(coords * freq, coords * freq)
        return 1.0 - np.abs(n)

    def _fake_erosion(self, height: np.ndarray, passes: int) -> np.ndarray:
        for _ in range(passes):
            gy, gx = np.gradient(height)
            slope = np.clip(np.sqrt(gx**2 + gy**2) * 6.0, 0.0, 1.0)
            h_uint8 = (np.clip(height, 0, 1) * 255).astype(np.uint8)
            h_pil   = Image.fromarray(h_uint8, mode='L')
            h_soft  = np.array(h_pil.filter(ImageFilter.GaussianBlur(radius=2.5))) / 255.0
            # steep areas keep original height; flat areas soften
            height = height * slope + h_soft * (1.0 - slope)
        return np.clip(height, 0.0, 1.0)
