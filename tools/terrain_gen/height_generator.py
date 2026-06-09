# tools/terrain_gen/height_generator.py
import numpy as np
from PIL import Image, ImageFilter
from opensimplex import OpenSimplex
from terrain_gen.terrain_recipe import TerrainRecipe


class HeightGenerator:
    # Standard MC2 dimensions for banded generation
    CHUNK_SIZE = 20
    SUPERCHUNK_CHUNKS = 3

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

    def generate_fullres_banded(self, recipe: TerrainRecipe, progress=None, superchunk_chunks: int = 3) -> np.ndarray:
        """Generate full-resolution height via row bands (no upscaling).

        Processes height in bands of size (chunk_size * superchunk_chunks) rows.
        Uses global x coordinates and band-local y coordinates.
        Applies tanh/plateau/erosion globally after all bands are combined.

        Args:
            recipe: Terrain recipe
            progress: optional callable(pct, stage, msg)
            superchunk_chunks: Number of chunks per band (default 3 = 60 rows)
        Returns:
            float32 ndarray [N, N] in [0, 1]
        """
        N = recipe.size
        h = recipe.height
        band_height = self.CHUNK_SIZE * superchunk_chunks

        if progress:
            progress(1, "height", f"full-res banding (bands of {band_height} rows)")

        gen1 = OpenSimplex(recipe.seed)
        gen2 = OpenSimplex(recipe.seed + 1)

        # Generate base fBm banded
        if progress:
            progress(5, "height", "base noise")
        base = self._fbm_banded(gen1, N, h.octaves, h.persistence, h.lacunarity, h.base_frequency,
                               band_height, progress, label="base", pct0=5, pct1=35)

        # Generate mountain component if needed
        if h.mountain_amount > 0.0:
            if progress:
                progress(35, "height", "ridged noise")
            ridged = self._ridged_banded(gen2, N, h.base_frequency * 2.0, band_height,
                                        progress, pct0=35, pct1=48)

            if progress:
                progress(48, "height", "detail noise")
            detail = self._fbm_banded(gen2, N, h.octaves, h.persistence, h.lacunarity, h.base_frequency,
                                     band_height, progress, label="detail", pct0=48, pct1=65)

            mountain = detail + ridged * h.ridged_amount
            height = base + mountain * h.mountain_amount * 0.4
        else:
            height = base

        # Apply global transformations on full height array
        if progress:
            progress(66, "height", "tanh mapping")
        tanh_scale = 2.0 + h.mountain_amount * 3.0
        height = (np.tanh(height * tanh_scale) + 1.0) * 0.5

        # Plateau shaping
        if h.plateau_strength > 0:
            if progress:
                progress(70, "height", "plateau shaping")
            height = np.power(height, 1.0 + h.plateau_strength)
            mx2 = height.max()
            if mx2 > 0:
                height /= mx2

        # Erosion passes
        if h.erosion_passes > 0:
            if progress:
                progress(75, "height", f"erosion {h.erosion_passes} passes")
            height = self._fake_erosion(height, h.erosion_passes, progress)

        return height.astype(np.float32)

    def _fbm_banded(self, gen, N: int, octaves: int, persistence: float, lacunarity: float, freq: float,
                    band_height: int, progress=None, label: str = "fbm", pct0: int = 0, pct1: int = 100) -> np.ndarray:
        """Vectorised fBm processed band-by-band. Uses global x, band-local y coordinates.

        Args:
            gen: OpenSimplex generator
            N: Total grid dimension
            octaves, persistence, lacunarity, freq: fBm parameters
            band_height: Height of each band (rows)
            progress: optional callback
            label: Label for progress reporting (e.g., "base" or "detail")
            pct0, pct1: Progress range for this stage
        Returns:
            float32 [N, N] fBm array
        """
        xcoords = np.arange(N, dtype=np.float64) / N
        value = np.zeros((N, N), dtype=np.float32)

        amplitude = np.float32(1.0)
        total_amp = np.float32(0.0)
        f = freq

        total_bands = (N + band_height - 1) // band_height
        total_steps = max(1, octaves * total_bands)
        step = 0

        for octave in range(octaves):
            for band_idx, y0 in enumerate(range(0, N, band_height)):
                y1 = min(y0 + band_height, N)
                band_rows = y1 - y0
                ycoords = np.arange(y0, y1, dtype=np.float64) / N

                # Generate noise for this band
                band_noise = gen.noise2array(xcoords * f, ycoords * f).astype(np.float32)
                value[y0:y1, :] += band_noise * amplitude

                step += 1
                if progress:
                    pct = pct0 + int((pct1 - pct0) * step / total_steps)
                    progress(pct, "height", f"{label} octave {octave + 1}/{octaves} band {band_idx + 1}/{total_bands}")

            total_amp += amplitude
            amplitude *= np.float32(persistence)
            f *= lacunarity

        return value / total_amp

    def _ridged_banded(self, gen, N: int, freq: float, band_height: int,
                       progress=None, pct0: int = 0, pct1: int = 100) -> np.ndarray:
        """Ridged noise processed band-by-band."""
        xcoords = np.arange(N, dtype=np.float64) / N
        value = np.zeros((N, N), dtype=np.float32)

        total_bands = (N + band_height - 1) // band_height

        for band_idx, y0 in enumerate(range(0, N, band_height)):
            y1 = min(y0 + band_height, N)
            ycoords = np.arange(y0, y1, dtype=np.float64) / N

            band_noise = gen.noise2array(xcoords * freq, ycoords * freq).astype(np.float32)
            value[y0:y1, :] = 1.0 - np.abs(band_noise)

            if progress:
                pct = pct0 + int((pct1 - pct0) * (band_idx + 1) / total_bands)
                progress(pct, "height", f"ridged band {band_idx + 1}/{total_bands}")

        return value

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
        base = self._fbm_array(gen1, coords, h.octaves, h.persistence, h.lacunarity, h.base_frequency, progress, label="base")

        if h.mountain_amount > 0.0:
            if progress:
                progress(30, "height", "ridged noise")
            ridged = self._ridged_array(gen2, coords, h.base_frequency * 2.0)
            if progress:
                progress(40, "height", "detail noise")
            detail = self._fbm_array(gen2, coords, h.octaves, h.persistence, h.lacunarity, h.base_frequency, progress, label="detail")
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

    def _fbm_array(self, gen, coords, octaves, persistence, lacunarity, freq, progress=None, label="fbm"):
        """Vectorised fBm over an NxN grid. coords = normalized [0,1) per axis.
        noise2array(x, y) returns shape (len(y), len(x)) = result[y, x]."""
        N = coords.shape[0]
        value = np.zeros((N, N), dtype=np.float64)
        amplitude = 1.0
        total_amp = 0.0
        f = freq
        for i in range(octaves):
            if progress and i % max(1, octaves // 3) == 0:  # Report every ~3 octaves
                progress(int(5 + 30 * i / max(1, octaves)), "height", f"{label} octave {i + 1}/{octaves}")
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
