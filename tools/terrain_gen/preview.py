# tools/terrain_gen/preview.py
"""
make_preview: 256x256 thumbnail of a burnin image.
make_contact_sheet: 3x2 debug grid with labelled panels.
"""
from __future__ import annotations

import numpy as np
from PIL import Image, ImageDraw

from terrain_gen.material_classifier import TerrainMasks, TerrainType


_THUMB = 256

_TYPE_COLORS: dict[int, tuple[int, int, int]] = {
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
    TerrainType.SLIMY:        (80,  100, 90),
    TerrainType.NONE:         (30,  30,  30),
}


def make_preview(burnin: Image.Image) -> Image.Image:
    """Return 256x256 RGB thumbnail."""
    return burnin.resize((_THUMB, _THUMB), Image.LANCZOS)


def make_contact_sheet(
    height: np.ndarray,
    masks: TerrainMasks,
    burnin: Image.Image,
) -> Image.Image:
    """3x2 debug grid: height | slope | curvature | valley | terrain_type | burnin."""

    def gray_thumb(arr: np.ndarray) -> Image.Image:
        img = Image.fromarray((np.clip(arr, 0, 1) * 255).astype(np.uint8), mode='L')
        return img.resize((_THUMB, _THUMB), Image.BICUBIC).convert('RGB')

    def type_thumb(tt: np.ndarray) -> Image.Image:
        rgb = np.zeros((*tt.shape, 3), dtype=np.uint8)
        for val, color in _TYPE_COLORS.items():
            rgb[tt == val] = color
        return Image.fromarray(rgb, mode='RGB').resize((_THUMB, _THUMB), Image.NEAREST)

    panels = [
        ("height",       gray_thumb(height)),
        ("slope",        gray_thumb(masks.slope)),
        ("curvature",    gray_thumb(masks.curvature)),
        ("valley",       gray_thumb(masks.valley)),
        ("terrain_type", type_thumb(masks.terrain_type)),
        ("burnin",       burnin.resize((_THUMB, _THUMB), Image.LANCZOS)),
    ]

    COLS, ROWS = 3, 2
    LABEL_H = 18
    sheet = Image.new('RGB', (_THUMB * COLS, (_THUMB + LABEL_H) * ROWS), (30, 30, 30))
    draw  = ImageDraw.Draw(sheet)

    for i, (label, thumb) in enumerate(panels):
        col, row = i % COLS, i // COLS
        x = col * _THUMB
        y = row * (_THUMB + LABEL_H)
        sheet.paste(thumb, (x, y + LABEL_H))
        draw.text((x + 4, y + 2), label, fill=(220, 220, 220))

    return sheet
