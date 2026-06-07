#!/usr/bin/env python3
"""
gen_presets.py — Pre-bake flat terrain presets for the MC2 Mission Editor.

Generates a flat (zero-elevation) elev.r32 + biome-coloured burnin.tga for
every biome × map-size combination and stores them under:

    terrain_gen_presets/<biome>_<sizeN>/genmap.elev.r32
    terrain_gen_presets/<biome>_<sizeN>/genmap.burnin.tga
    terrain_gen_presets/<biome>_<sizeN>/genmap.preview.png

Because height is a constant-zero array (all terrain at sea level) we can
skip the expensive opensimplex HeightGenerator pass entirely -- just run
MaterialClassifier + BurninRenderer, which are fast even at 1k resolution.

Usage:
    py -3 tools/terrain_gen/gen_presets.py
    py -3 tools/terrain_gen/gen_presets.py --sizes 1020 --out terrain_gen_presets
    py -3 tools/terrain_gen/gen_presets.py --biomes swamp_forest desert --sizes 120 1020

The editor reads these via the "Load Preset" button in the Map Generator
dialog, which copies the pair to terrain_gen_out/ and loads without running
terrain_gen.py.
"""
import sys
import os
import argparse
import time
from pathlib import Path

# Python automatically inserts the script's directory (tools/terrain_gen/) into
# sys.path[0] when running this file.  That makes 'terrain_gen.py' (the generator
# script that lives in the same directory) shadow the terrain_gen/ *package* when
# we 'from terrain_gen.xxx import ...'.  Fix: replace the script dir entry with
# its parent (tools/) so Python resolves terrain_gen as the package directory.
_here  = Path(__file__).resolve().parent   # .../tools/terrain_gen/
_tools = _here.parent                       # .../tools/
if sys.path and Path(sys.path[0]).resolve() == _here:
    sys.path[0] = str(_tools)
else:
    sys.path.insert(0, str(_tools))

import numpy as np
from PIL import Image

from terrain_gen.biome_presets import BIOMES, BiomePreset
from terrain_gen.terrain_recipe import TerrainRecipe
from terrain_gen.material_classifier import MaterialClassifier
from terrain_gen.burnin_renderer import BurninRenderer
from terrain_gen.preview import make_preview

# ---------------------------------------------------------------------------
# Inventory
# ---------------------------------------------------------------------------

ALL_BIOMES: list[str] = list(BIOMES.keys())
# 0=60, 1=80, 2=100, 3=120, 4=260, 5=520, 6=1020  (matches EditorData.cpp)
ALL_SIZES:  list[int] = [60, 80, 100, 120, 260, 520, 1020]


# ---------------------------------------------------------------------------
# Flat preset generator
# ---------------------------------------------------------------------------

def generate_flat_preset(biome: str, size_n: int, out_dir: Path) -> None:
    """Generate one flat-terrain preset pair and write to out_dir."""
    out_dir.mkdir(parents=True, exist_ok=True)

    t0 = time.time()

    # Build a minimal recipe (height params don't matter — we skip HeightGenerator)
    recipe_json = (
        '{"version":1,"name":"genmap",'
        f'"size":{size_n},"biome":"{biome}","seed":1,'
        '"height":{"max_elevation":0.0,"min_elevation":0.0,"mountain_amount":0.0,"ridged_amount":0.0},'
        '"materials":{"grass_lowland":0.8,"snow_line":1.0,"water_level":0.0}}'
    )
    tmp_recipe = out_dir / "genmap.recipe.json"
    tmp_recipe.write_text(recipe_json, encoding='latin-1')
    recipe = TerrainRecipe.from_json(str(tmp_recipe))
    recipe.apply_biome()
    # Force flat-friendly material params regardless of biome defaults
    recipe.materials.water_level = 0.0    # no water on flat ground
    recipe.materials.snow_line   = 1.0    # no snow on flat ground
    recipe.materials.grass_lowland = 0.8  # mostly grass/ground

    # Flat height field: constant 0 in [0,1] range.
    # MaterialClassifier uses altitude thresholds against this value.
    # At altitude=0 everything is "lowland" -> grass / biome ground.
    height = np.zeros((size_n, size_n), dtype=np.float32)

    # Material classification and burnin
    masks  = MaterialClassifier().classify(height, recipe)
    burnin = BurninRenderer().render(masks, recipe, BIOMES[biome])

    # --- elev.r32: raw float32 world-unit elevations, all at min_elevation ---
    # initTerrainFromTGA reads: setVertexHeight(vertNum, *(ptr+vertNum))
    # For a flat map at elevation 0 wu we want all values = 0.
    elev = np.zeros((size_n, size_n), dtype='<f4')
    elev.tofile(str(out_dir / "genmap.elev.r32"))

    # --- burnin.tga ---
    burnin.save(str(out_dir / "genmap.burnin.tga"), format='TGA')

    # --- preview.png ---
    make_preview(burnin).save(str(out_dir / "genmap.preview.png"))

    dt = time.time() - t0
    print(f"  [{biome:20s} {size_n:5d}x{size_n}]  -> {out_dir}  ({dt:.1f}s)")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(description="Pre-bake flat terrain presets for MC2 editor")
    parser.add_argument('--biomes', nargs='+', default=ALL_BIOMES,
                        help=f"Biomes to generate (default: all). Choices: {ALL_BIOMES}")
    parser.add_argument('--sizes',  nargs='+', type=int, default=ALL_SIZES,
                        help=f"Map sizes (vertex count) to generate (default: all). Choices: {ALL_SIZES}")
    parser.add_argument('--out', default='terrain_gen_presets',
                        help='Output root directory (default: terrain_gen_presets)')
    args = parser.parse_args()

    out_root = Path(args.out)

    # Validate
    bad_biomes = [b for b in args.biomes if b not in ALL_BIOMES]
    if bad_biomes:
        print(f"ERROR: unknown biome(s): {bad_biomes}. Valid: {ALL_BIOMES}", file=sys.stderr)
        sys.exit(1)
    bad_sizes = [s for s in args.sizes if s not in ALL_SIZES]
    if bad_sizes:
        print(f"ERROR: unknown size(s): {bad_sizes}. Valid: {ALL_SIZES}", file=sys.stderr)
        sys.exit(1)

    total = len(args.biomes) * len(args.sizes)
    print(f"Generating {total} flat presets -> {out_root}/")
    print(f"  Biomes: {args.biomes}")
    print(f"  Sizes:  {args.sizes}")
    print()

    t_all = time.time()
    done  = 0
    for biome in args.biomes:
        for size_n in args.sizes:
            subdir = out_root / f"{biome}_{size_n}"
            generate_flat_preset(biome, size_n, subdir)
            done += 1

    print()
    print(f"Done: {done}/{total} presets in {time.time()-t_all:.1f}s  ->  {out_root}/")
    print()
    print("To use in the editor: Map Generator dialog -> 'Load Preset'")
    print("(copies preset files to terrain_gen_out/ and loads without running Python)")


if __name__ == '__main__':
    main()
