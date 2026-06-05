#!/usr/bin/env python3
# tools/terrain_gen/terrain_gen.py
"""
MC2 terrain generator -- Phase A visual output only.

Usage:
  python terrain_gen.py recipe.json --out outdir/

Outputs:
  {name}.burnin.tga    24-bit RGB TGA (MC2 colormap)
  {name}.recipe.json   resolved recipe with biome params applied
  {name}.preview.png   256x256 thumbnail
  contact_sheet.png    3x2 debug grid
  height.png           grayscale height
  slope.png            grayscale slope
  altitude.png         grayscale altitude
  valley.png           grayscale valley
  terrain_type.png     colorized terrain type map
"""
import sys
import os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import argparse
from pathlib import Path
import numpy as np
from PIL import Image

from terrain_gen.terrain_recipe import TerrainRecipe
from terrain_gen.biome_presets import BIOMES
from terrain_gen.height_generator import HeightGenerator
from terrain_gen.material_classifier import MaterialClassifier, TerrainType
from terrain_gen.burnin_renderer import BurninRenderer
from terrain_gen.preview import make_preview, make_contact_sheet

try:
    from terrain_gen.pak_exporter import PakExporter
    _PAK_AVAILABLE = True
except ImportError:
    _PAK_AVAILABLE = False


_TYPE_COLORS: dict = {
    TerrainType.BLUE_WATER:   (40,  80,  200),
    TerrainType.GREEN_WATER:  (60,  140, 80),
    TerrainType.MUD:          (100, 80,  60),
    TerrainType.MOSS:         (80,  110, 60),
    TerrainType.DIRT:         (160, 130, 90),
    TerrainType.ASH:          (150, 145, 140),
    TerrainType.MOUNTAIN:     (130, 120, 110),
    TerrainType.TUNDRA:       (200, 210, 220),
    TerrainType.FOREST_FLOOR: (50,  80,  40),
    TerrainType.GRASS:        (100, 150, 60),
    TerrainType.CONCRETE:     (160, 160, 155),
    TerrainType.CLIFF:        (90,  85,  80),
    TerrainType.NONE:         (30,  30,  30),
}


def _save_tga(img: Image.Image, path: Path) -> None:
    img.save(str(path), format='TGA')


def _save_gray(arr: np.ndarray, path: Path) -> None:
    Image.fromarray((np.clip(arr, 0, 1) * 255).astype(np.uint8), mode='L').save(str(path))


def _save_terrain_type(tt: np.ndarray, path: Path) -> None:
    rgb = np.zeros((*tt.shape, 3), dtype=np.uint8)
    for val, color in _TYPE_COLORS.items():
        rgb[tt == val] = color
    Image.fromarray(rgb, mode='RGB').save(str(path))


def main() -> None:
    parser = argparse.ArgumentParser(description='MC2 terrain generator')
    parser.add_argument('recipe', help='Path to recipe JSON file')
    parser.add_argument('--out', default='terrain_out', help='Output directory')
    parser.add_argument('--template-pak', help='Template .pak for Packet 0 patching (Phase B)')
    args = parser.parse_args()

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    print(f"Loading recipe: {args.recipe}")
    recipe = TerrainRecipe.from_json(args.recipe)
    print(f"  size={recipe.size}  biome={recipe.biome}  seed={recipe.seed}")
    recipe.apply_biome()

    print("Generating height...")
    height = HeightGenerator().generate(recipe)

    print("Classifying materials...")
    masks = MaterialClassifier().classify(height, recipe)

    print("Rendering burnin...")
    burnin = BurninRenderer().render(masks, recipe, BIOMES[recipe.biome])

    name = recipe.name
    print(f"Saving to {out}/")

    _save_tga(burnin, out / f"{name}.burnin.tga")
    print(f"  {name}.burnin.tga  ({burnin.size[0]}x{burnin.size[1]})")

    make_preview(burnin).save(str(out / f"{name}.preview.png"))
    print(f"  {name}.preview.png")

    make_contact_sheet(height, masks, burnin).save(str(out / "contact_sheet.png"))
    print(f"  contact_sheet.png")

    _save_gray(height,         out / "height.png")
    _save_gray(masks.slope,    out / "slope.png")
    _save_gray(masks.altitude, out / "altitude.png")
    _save_gray(masks.valley,   out / "valley.png")
    _save_terrain_type(masks.terrain_type, out / "terrain_type.png")
    print("  height.png  slope.png  altitude.png  valley.png  terrain_type.png")

    recipe.to_json(out / f"{name}.recipe.json")
    print(f"  {name}.recipe.json")

    if args.template_pak:
        if not _PAK_AVAILABLE:
            print("WARNING: --template-pak provided but pak_exporter not available (Phase B). Skipping.")
        else:
            print(f"Patching pak from template: {args.template_pak}")
            pkt0 = PakExporter().build_packet0(height, masks, recipe)
            PakExporter().patch_pak(args.template_pak, str(out / f"{name}.pak"), pkt0)
            print(f"  {name}.pak")

    print("Done.")


if __name__ == '__main__':
    main()
