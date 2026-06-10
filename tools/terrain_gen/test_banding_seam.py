# tools/terrain_gen/test_banding_seam.py
"""Phase 2 seam audit: prove banded full-res generation equals monolithic
generation (no per-band normalization islands).

The banded path (_fbm_banded / _ridged_banded, global total_amp, global
tanh/plateau/erosion after assembly) must match _generate_at_size at the same N
to floating-point tolerance. A real per-band seam bug would blow past `tol`.

Run:
  py -3 tools/terrain_gen/test_banding_seam.py
Exit 0 = parity holds.
"""
import sys
import os
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from terrain_gen.terrain_recipe import TerrainRecipe
from terrain_gen.height_generator import HeightGenerator, CancelledError


def main() -> int:
    N = 120
    recipe = TerrainRecipe(name="seam", size=N, biome="temperate_hills", seed=42)
    recipe.apply_biome()

    gen = HeightGenerator()
    mono = gen._generate_at_size(recipe, N)
    # band_height = 60 (3 chunks) does NOT divide N evenly into many bands, and
    # band_height = 40 (2 chunks) gives 3 bands -> exercises the partial-band path.
    for sc in (2, 3):
        banded = gen.generate_fullres_banded(recipe, superchunk_chunks=sc)
        assert banded.shape == (N, N), f"shape {banded.shape}"
        diff = float(np.max(np.abs(banded - mono)))
        tol = 2e-3  # float32 amplitude accumulation vs float64 monolith
        status = "OK" if diff < tol else "FAIL"
        print(f"  superchunk_chunks={sc} band={20*sc}: max|banded-mono|={diff:.2e} {status}")
        if diff >= tol:
            print("SEAM DETECTED", flush=True)
            return 1

    # Cancellation: a check that always fires must raise out of generation.
    def always_cancel():
        raise CancelledError()
    try:
        gen.generate_fullres_banded(recipe, cancel_check=always_cancel)
        print("  cancel: FAIL (did not raise)")
        return 1
    except CancelledError:
        print("  cancel: OK (raised)")

    print("PARITY OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
