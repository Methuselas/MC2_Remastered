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
        height = np.zeros((N, N), dtype=np.float64)
        for y in range(N):
            for x in range(N):
                fx = x / N
                fy = y / N
                base = self._fbm(gen1, fx, fy, h.octaves, h.persistence, h.lacunarity, h.base_frequency)
                if h.mountain_amount > 0.0:
                    ridged = self._ridged(gen2, fx, fy, h.base_frequency * 2.0)
                    detail = self._fbm(gen2, fx, fy, h.octaves, h.persistence, h.lacunarity, h.base_frequency)
                    mountain = detail + ridged * h.ridged_amount
                    height[y, x] = base + mountain * h.mountain_amount * 0.4
                else:
                    height[y, x] = base

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

    def _fbm(self, gen, x, y, octaves, persistence, lacunarity, freq):
        value = 0.0
        amplitude = 1.0
        total_amp = 0.0
        for _ in range(octaves):
            value     += gen.noise2(x * freq, y * freq) * amplitude
            total_amp += amplitude
            amplitude *= persistence
            freq      *= lacunarity
        return value / total_amp

    def _ridged(self, gen, x, y, freq):
        n = gen.noise2(x * freq, y * freq)
        return 1.0 - abs(n)

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
