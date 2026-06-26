#!/usr/bin/env python3
"""GAEA-HEIGHT-IMPORT-1: import a Gaea-authored terrain into the MC2 pipeline.

Reads a Gaea raw export, normalizes + downsamples to the MC2 coarse gameplay grid,
classifies materials, and emits a minimal MC2 map (<name>.pak / .elev.r32 / .fit /
.burnin.tga) by reusing the existing generator (PakExporter.build_packet0 +
patch_pak — which PATCHES a stock template .pak, so the target grid must match the
template grid). The full-res normalized height is also saved as the render-only
visual-height layer (feeds the visual_height_4x arc).

HEIGHT SOURCE: a single-channel float .r32 (size = res*res*4) is the real height.
If only a 3-channel COLOR .r32 is available (size = res*res*3*4, e.g. Gaea's Combine
output), this uses its LUMINANCE as a PLACEHOLDER height + the RGB as the colormap —
clearly flagged. Re-export a dedicated Height .r32 (Mountain node) for real terrain.

Run:
  python import_gaea_height.py --recipe recipes/gaea_test_01.json
  python import_gaea_height.py --height-file X.r32 --res 512 --size 120 \
        --template-pak <stock 120-grid .pak> --name gaea_test_01 [--max-elev 280] ...
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from terrain_gen.terrain_recipe import TerrainRecipe, HeightParams, MaterialParams  # noqa: E402
from terrain_gen.material_classifier import MaterialClassifier                       # noqa: E402
from terrain_gen.pak_exporter import PakExporter                                     # noqa: E402
from terrain_gen.terrain_gen import _write_fit                                       # noqa: E402


def read_gaea(height_file: Path, res: int):
    """Return (height01[res,res], color01[res,res,3] or None, source_label)."""
    raw = np.fromfile(height_file, dtype="<f4")
    if raw.size == res * res:
        h = raw.reshape(res, res).astype(np.float64)
        return h, None, "height (1-channel .r32)"
    if raw.size == res * res * 3:
        rgb = raw.reshape(res, res, 3).astype(np.float64)
        lum = rgb @ np.array([0.299, 0.587, 0.114])
        return lum, np.clip(rgb, 0, 1), "COLOR-LUMINANCE PLACEHOLDER (re-export a Height .r32!)"
    raise ValueError(f"{height_file}: {raw.size} floats != res^2 ({res*res}) or res^2*3 "
                     f"({res*res*3}); wrong --res or not a Gaea raw export")


def _resize01(a: np.ndarray, n: int) -> np.ndarray:
    return np.clip(np.asarray(Image.fromarray(a.astype(np.float32), "F").resize((n, n), Image.BILINEAR)), 0, 1)


def run(cfg: dict, out_root: Path) -> dict:
    res = int(cfg["height_resolution"])
    size = int(cfg["size"])
    name = cfg["name"]
    h_raw, color, src = read_gaea(Path(cfg["height_file"]), res)

    # normalize -> [0,1], orientation guard (Gaea image-Y-down vs MC2 row-major).
    hn = (h_raw - h_raw.min()) / (h_raw.max() - h_raw.min() + 1e-9)
    if cfg.get("flip_y", True):
        hn = np.flipud(hn)
        if color is not None:
            color = np.flipud(color)

    coarse = _resize01(hn, size)                      # gameplay grid
    water_level = float(np.percentile(coarse, cfg.get("water_percentile", 30)))

    recipe = TerrainRecipe(
        version=1, name=name, size=size, biome=cfg.get("biome", "temperate_hills"), seed=1,
        height=HeightParams(min_elevation=float(cfg["height"]["min_elevation"]),
                            max_elevation=float(cfg["height"]["max_elevation"])),
        materials=MaterialParams(water_level=water_level),
    )

    masks = MaterialClassifier().classify(coarse.astype(np.float32), recipe)

    out = out_root / name
    out.mkdir(parents=True, exist_ok=True)
    exp = PakExporter()
    pkt0 = exp.build_packet0(coarse.astype(np.float32), masks, recipe)
    exp.patch_pak(cfg["template_pak"], str(out / f"{name}.pak"), pkt0)

    elev = (coarse * recipe.height.max_elevation + recipe.height.min_elevation).astype("<f4")
    elev.tofile(str(out / f"{name}.elev.r32"))
    _write_fit(out / f"{name}.fit", f"{name}.burnin", recipe)

    # colormap (burnin): use the Gaea color directly if present, else hillshade of height.
    burnin_res = recipe.burnin_resolution()
    if color is not None:
        Image.fromarray((color * 255).astype(np.uint8), "RGB").resize(
            (burnin_res, burnin_res), Image.LANCZOS).save(str(out / f"{name}.burnin.tga"))
        cmap_src = "gaea color"
    else:
        g = (_resize01(hn, burnin_res) * 255).astype(np.uint8)
        Image.fromarray(np.stack([g, g, g], -1), "RGB").save(str(out / f"{name}.burnin.tga"))
        cmap_src = "height greyscale"

    # full-res visual-height layer (render-only; feeds visual_height arc).
    vis = (_resize01(hn, res) * recipe.height.max_elevation + recipe.height.min_elevation).astype("<f4")
    vis.tofile(str(out / f"{name}.visual_height.r32"))
    # preview
    pg = (_resize01(hn, 256) * 255).astype(np.uint8)
    Image.fromarray(np.stack([pg, pg, pg], -1), "RGB").save(str(out / f"{name}.preview.png"))

    rep = {
        "name": name, "height_source": src, "colormap_source": cmap_src,
        "gaea_res": res, "mc2_grid": size, "template_pak": cfg["template_pak"],
        "elevation_wu": {"min": recipe.height.min_elevation, "max": recipe.height.max_elevation},
        "water_level_norm": water_level,
        "water_elevation_wu": water_level * recipe.height.max_elevation + recipe.height.min_elevation,
        "flip_y": cfg.get("flip_y", True),
        "outputs": [f"{name}.pak", f"{name}.elev.r32", f"{name}.fit",
                    f"{name}.burnin.tga", f"{name}.visual_height.r32", f"{name}.preview.png"],
        "out_dir": str(out),
    }
    (out / f"{name}.import_report.json").write_text(json.dumps(rep, indent=2), encoding="utf-8")
    return rep


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--recipe", help="gaea recipe json (overrides individual flags)")
    ap.add_argument("--height-file"); ap.add_argument("--res", type=int, default=512)
    ap.add_argument("--size", type=int, default=120)
    ap.add_argument("--template-pak"); ap.add_argument("--name", default="gaea_test_01")
    ap.add_argument("--biome", default="temperate_hills")
    ap.add_argument("--max-elev", type=float, default=280.0)
    ap.add_argument("--min-elev", type=float, default=0.0)
    ap.add_argument("--water-pct", type=float, default=30.0)
    ap.add_argument("--no-flip", action="store_true")
    ap.add_argument("--out", default="tests/terrain/gaea")
    a = ap.parse_args()

    if a.recipe:
        cfg = json.loads(Path(a.recipe).read_text())
    else:
        if not (a.height_file and a.template_pak):
            ap.error("need --recipe OR (--height-file and --template-pak)")
        cfg = {"name": a.name, "height_file": a.height_file, "height_resolution": a.res,
               "size": a.size, "template_pak": a.template_pak, "biome": a.biome,
               "flip_y": not a.no_flip, "water_percentile": a.water_pct,
               "height": {"min_elevation": a.min_elev, "max_elevation": a.max_elev}}
    rep = run(cfg, Path(a.out))
    print(f"[gaea-import] {rep['name']}: src={rep['height_source']} "
          f"gaea{rep['gaea_res']}->mc2 {rep['mc2_grid']}^2  water={rep['water_elevation_wu']:.0f}wu "
          f"cmap={rep['colormap_source']} -> {rep['out_dir']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
