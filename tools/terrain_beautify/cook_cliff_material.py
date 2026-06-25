#!/usr/bin/env python3
"""Cook a Poly-Haven-style cliff GLTF asset into the terrain material-normal array
layer 5 (MAT_LAYER_MARBLE_CLIFF) as mat5_normal.tga.

The terrain normal array (terrtxm2.cpp:2456 buildTerrainNormalArray) loads
mat<N>_normal.tga as RGBA: RGB = tangent-space normal, A = displacement. Slots 0-4
required, 5-8 optional. This produces mat5_normal.tga at the same size as mat0
(2048^2 RGBA) so the array build accepts it, from a <name>_2k.gltf asset that ships
a normal (nor_gl) + ARM (AO/rough/metal); displacement is derived from the ARM AO
channel (dark in crevices = low) since these assets carry no height map.

Run:
  python cook_cliff_material.py <asset.gltf.zip> [--out-textures DIR ...] [--size 2048]
"""
from __future__ import annotations

import argparse
import sys
import tempfile
import zipfile
from pathlib import Path

import numpy as np
from PIL import Image


def cook(zip_path: Path, size: int) -> np.ndarray:
    with tempfile.TemporaryDirectory(dir=str(Path.cwd())) as td:
        with zipfile.ZipFile(zip_path) as z:
            z.extractall(td)
        tex = Path(td) / "textures"
        nor = next(tex.glob("*nor_gl*"), None) or next(tex.glob("*normal*"), None)
        arm = next(tex.glob("*arm*"), None)
        if not nor:
            raise FileNotFoundError("no *nor_gl*/*normal* texture in asset")
        rgb = np.asarray(Image.open(nor).convert("RGB").resize((size, size), Image.LANCZOS))
        if arm:
            ao = np.asarray(Image.open(arm).convert("RGB").resize((size, size), Image.LANCZOS))[..., 0]
        else:
            ao = np.full((size, size), 200, np.uint8)   # neutral-ish displacement
        return np.dstack([rgb, ao]).astype(np.uint8)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("asset_zip")
    ap.add_argument("--out-textures", action="append", default=[
        "release_assets/0.4-testing/data/textures",
    ], help="dir(s) to write mat5_normal.tga (repeatable)")
    ap.add_argument("--size", type=int, default=2048)
    ap.add_argument("--slot", type=int, default=5, help="mat<slot>_normal.tga")
    args = ap.parse_args()

    out = cook(Path(args.asset_zip), args.size)
    rgb_mean = out[..., :3].reshape(-1, 3).mean(0).round(0)
    a = out[..., 3]
    for d in args.out_textures:
        dst = Path(d) / f"mat{args.slot}_normal.tga"
        dst.parent.mkdir(parents=True, exist_ok=True)
        Image.fromarray(out, "RGBA").save(dst)
        print(f"[cook-cliff] wrote {dst} ({dst.stat().st_size // 1024} KB) "
              f"normal_mean={rgb_mean} disp(min/max/mean)={a.min()}/{a.max()}/{a.mean():.0f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
