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


def _height_from_normal(rgb: np.ndarray) -> np.ndarray:
    """Integrate a tangent-space normal map (NormalGL) into a relative height
    field via Poisson-style frequency-domain solve of div(grad) = div(slope).

    marble_cliff ships no displacement map, so the ONLY sources of relief are
    the pack's own normal + ARM-AO. AO captures crevice occlusion but is soft;
    integrating the normal recovers the mid/high-frequency groove structure that
    makes a cliff face read as layered rock. Returns float height in [0,1]."""
    nx = rgb[..., 0].astype(np.float32) / 127.5 - 1.0
    ny = rgb[..., 1].astype(np.float32) / 127.5 - 1.0
    nz = np.maximum(rgb[..., 2].astype(np.float32) / 127.5 - 1.0, 1e-3)
    # slope = -n.xy / n.z  (gradient of height the normal implies)
    p = -nx / nz
    q = -ny / nz
    h, w = p.shape
    fx = np.fft.fftfreq(w).reshape(1, w).astype(np.float32)
    fy = np.fft.fftfreq(h).reshape(h, 1).astype(np.float32)
    wx = 2.0 * np.pi * fx
    wy = 2.0 * np.pi * fy
    denom = (wx * wx) + (wy * wy)
    denom[0, 0] = 1.0
    # div of slope in freq domain, solved for height
    P = np.fft.fft2(p)
    Q = np.fft.fft2(q)
    H = (-1j * wx * P - 1j * wy * Q) / denom
    H[0, 0] = 0.0
    height = np.real(np.fft.ifft2(H)).astype(np.float32)
    height -= height.min()
    mx = height.max()
    if mx > 1e-6:
        height /= mx
    return height


def cook(zip_path: Path, size: int) -> np.ndarray:
    with tempfile.TemporaryDirectory(dir=str(Path.cwd())) as td:
        with zipfile.ZipFile(zip_path) as z:
            z.extractall(td)
        tex = Path(td) / "textures"
        nor = next(tex.glob("*nor_gl*"), None) or next(tex.glob("*normal*"), None)
        arm = next(tex.glob("*arm*"), None)
        disp = (next(tex.glob("*disp*"), None) or next(tex.glob("*height*"), None))
        if not nor:
            raise FileNotFoundError("no *nor_gl*/*normal* texture in asset")
        rgb = np.asarray(Image.open(nor).convert("RGB").resize((size, size), Image.LANCZOS))
        if disp is not None:
            # Prefer a real displacement/height map when the pack ships one.
            a = np.asarray(Image.open(disp).convert("L").resize((size, size), Image.LANCZOS)).astype(np.float32) / 255.0
        else:
            # No height map (marble_cliff): synthesize relief from the pack's OWN
            # data -- integrate the normal for groove structure, modulate with
            # ARM-AO for crevice darkening. Both are marble_cliff's own maps.
            hfromN = _height_from_normal(rgb)
            if arm is not None:
                ao = np.asarray(Image.open(arm).convert("RGB").resize((size, size), Image.LANCZOS))[..., 0].astype(np.float32) / 255.0
            else:
                ao = np.full((size, size), 0.78, np.float32)
            # weight normal-integrated height (structure) with AO (occlusion).
            a = 0.6 * hfromN + 0.4 * ao
            a -= a.min()
            mx = a.max()
            if mx > 1e-6:
                a /= mx
        disp8 = np.clip(a * 255.0, 0, 255).astype(np.uint8)
        return np.dstack([rgb, disp8]).astype(np.uint8)


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
