# tools/terrain_gen/height_generator.py
import numpy as np
from PIL import Image, ImageFilter
from opensimplex import OpenSimplex
from terrain_gen.terrain_recipe import TerrainRecipe


class HeightGenerator:
    def generate(self, recipe: TerrainRecipe, progress=None) -> np.ndarray:
        """Return float32 ndarray [size, size] in [0, 1].

        If recipe._height_work_size is set, generates at lower resolution then upscales.
        progress: optional callable(pct, stage, msg) for progress updates.
        """
        final_N = recipe.size
        work_N = int(getattr(recipe, "_height_work_size", final_N))

        # Generate at working resolution if specified and smaller than final
        if work_N > 0 and work_N < final_N:
            work_N = max(60, min(work_N, final_N))
            work_N = (work_N // 20) * 20  # Snap to block boundary

            if progress:
                progress(1, "height", f"working at {work_N} then upscaling to {final_N}")

            height = self._generate_at_size(recipe, work_N, progress)

            # Upscale to final resolution using high-quality BICUBIC interpolation
            if progress:
                progress(60, "height", "upscaling")

            img = Image.fromarray((np.clip(height, 0, 1) * 65535).astype(np.uint16), mode="I;16")
            img = img.resize((final_N, final_N), Image.BICUBIC)
            out = np.asarray(img, dtype=np.float32) / np.float32(65535.0)

            if progress:
                progress(64, "height", "upscale complete")

            return np.clip(out, 0, 1).astype(np.float32)

        # Full resolution generation
        return self._generate_at_size(recipe, final_N, progress)

    def _generate_at_size(self, recipe: TerrainRecipe, N: int, progress=None) -> np.ndarray:
        """Generate height field at specified resolution N."""
        h = recipe.height
        gen1 = OpenSimplex(recipe.seed)
        gen2 = OpenSimplex(recipe.seed + 1)

        if progress:
            progress(2, "height", "initializing")

        # fx/fy are normalized [0,1] coords. base_frequency ~4 gives macro hills.
        # Vectorised with opensimplex.noise2array (C-accelerated) -- the old per-pixel
        # python double loop was O(N^2) and took minutes at 1020x1020.
        coords = np.arange(N, dtype=np.float64) / N   # normalized [0,1)

        if progress:
            progress(5, "height", "base noise")
        base = self._fbm_array(gen1, coords, h.octaves, h.persistence, h.lacunarity, h.base_frequency, progress)

        if h.mountain_amount > 0.0:
            if progress:
                progress(30, "height", "ridged noise")
            ridged = self._ridged_array(gen2, coords, h.base_frequency * 2.0)
            if progress:
                progress(40, "height", "detail noise")
            detail = self._fbm_array(gen2, coords, h.octaves, h.persistence, h.lacunarity, h.base_frequency, progress)
            mountain = detail + ridged * h.ridged_amount
            height = base + mountain * h.mountain_amount * 0.4
        else:
            height = base

        # Map to [0, 1] via tanh.
        # mountain_amount increases the tanh contrast (scale), which monotonically
        # increases the std of the output — sharp peaks push toward 0/1 extremes.
        if progress:
            progress(48, "height", "tanh mapping")
        tanh_scale = 2.0 + h.mountain_amount * 3.0  # 2.0 (flat) → 5.0 (mountains)
        height = (np.tanh(height * tanh_scale) + 1.0) * 0.5

        # Plateau (flatten peaks slightly)
        if h.plateau_strength > 0:
            if progress:
                progress(52, "height", "plateau shaping")
            height = np.power(height, 1.0 + h.plateau_strength)
            mx2 = height.max()
            if mx2 > 0:
                height /= mx2

        # Fake erosion passes
        if h.erosion_passes > 0:
            if progress:
                progress(55, "height", f"erosion {h.erosion_passes} passes")
            height = self._fake_erosion(height, h.erosion_passes, progress)

        return height.astype(np.float32)

    def _fbm_array(self, gen, coords, octaves, persistence, lacunarity, freq, progress=None):
        """Vectorised fBm over an NxN grid. coords = normalized [0,1) per axis.
        noise2array(x, y) returns shape (len(y), len(x)) = result[y, x]."""
        N = coords.shape[0]
        value = np.zeros((N, N), dtype=np.float64)
        amplitude = 1.0
        total_amp = 0.0
        f = freq
        for i in range(octaves):
            if progress and i % max(1, octaves // 3) == 0:  # Report every ~3 octaves
                progress(int(5 + 30 * i / max(1, octaves)), "height", f"octave {i + 1}/{octaves}")
            value     += gen.noise2array(coords * f, coords * f) * amplitude
            total_amp += amplitude
            amplitude *= persistence
            f         *= lacunarity
        return value / total_amp

    def _ridged_array(self, gen, coords, freq):
        n = gen.noise2array(coords * freq, coords * freq)
        return 1.0 - np.abs(n)

    def _fake_erosion(self, height: np.ndarray, passes: int, progress=None) -> np.ndarray:
        for i in range(passes):
            if progress:
                progress(55 + int(5 * i / max(1, passes)), "height", f"erosion pass {i + 1}/{passes}")
            gy, gx = np.gradient(height)
            slope = np.clip(np.sqrt(gx**2 + gy**2) * 6.0, 0.0, 1.0)
            h_uint8 = (np.clip(height, 0, 1) * 255).astype(np.uint8)
            h_pil   = Image.fromarray(h_uint8, mode='L')
            h_soft  = np.array(h_pil.filter(ImageFilter.GaussianBlur(radius=2.5))) / 255.0
            # steep areas keep original height; flat areas soften
            height = height * slope + h_soft * (1.0 - slope)
        return np.clip(height, 0.0, 1.0)
