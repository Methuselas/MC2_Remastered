#!/usr/bin/env python3
"""VISUAL-HEIGHTFIELD-BAKE-1: high-res VISUAL terrain heightfield (render-only).

Bakes a stock mission's coarse heightfield into a higher-resolution VISUAL height
layer that the renderer can later sample to displace terrain vertices for a
smoother/less-blocky silhouette — WITHOUT touching the gameplay heightfield
(pathing/grounding/LOS stay on the original coarse grid; see
VISUAL-HEIGHTFIELD-SIDECAR-RECON-1).

This first bake is deliberately BORING + SAFE: a factor-x bilinear upsample with a
CORNER-PINNED guarantee — every coarse vertex appears verbatim in the visual grid
(max corner error = 0). No aggressive reshaping yet (that's the later
PYRAMID-ISLAND-VISUAL-RESHAPE slice, which will use the protected mask). No .pak
write, no engine change.

Grid math (corner-pinned): coarse side N (N-1 cells). Visual side V = (N-1)*F + 1,
so visual[i*F, j*F] == coarse[i, j] exactly; the new interior verts bilerp between.
World extent is unchanged (finer stride within the same 128-wu cells), so the
visual grid is coincident with the coarse grid at every F-th vertex.

Outputs (in <out>/<mission>.beauty/):
    visual_height_<F>x.r32      float32 [V*V] row-major, world-unit elevations
    visual_height_preview.png   grayscale normalized
    visual_delta_heatmap.png    |visual - nearest-coarse| (the smoothing introduced)
    visual_height_report.json   dims + corner-error proof + delta stats

Run:
  python visual_heightfield.py <mission> [--missions-dir DIR] [--out DIR] [--factor 4]
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mission_terrain_analyzer import (  # noqa: E402
    read_packets, locate_mapdata, extract_layers, read_water_elevation,
    WORLD_UNITS_PER_VERTEX,
)


def upsample_corner_pinned(elev: np.ndarray, factor: int) -> np.ndarray:
    """Bilinear upsample with exact corner preservation.

    Returns a (V,V) array, V=(N-1)*factor+1, where out[i*factor, j*factor] ==
    elev[i, j] bit-exactly (bilinear weight is exactly 1 at coarse vertices)."""
    N = elev.shape[0]
    V = (N - 1) * factor + 1
    coords = np.arange(V, dtype=np.float64) / factor          # coarse-space coords
    i0 = np.clip(np.floor(coords).astype(np.int64), 0, N - 2)
    fr = coords - i0                                          # 0 at coarse verts
    # Separable bilinear: interp along rows then cols.
    e = elev.astype(np.float64)
    # rows: out_rows[v, c] = (1-fr[v])*e[i0[v],c] + fr[v]*e[i0[v]+1,c]
    rows = (1.0 - fr)[:, None] * e[i0, :] + fr[:, None] * e[i0 + 1, :]
    # cols: out[v, w] = (1-fr[w])*rows[v,i0[w]] + fr[w]*rows[v,i0[w]+1]
    out = (1.0 - fr)[None, :] * rows[:, i0] + fr[None, :] * rows[:, i0 + 1]
    return out


def nearest_coarse(elev: np.ndarray, factor: int) -> np.ndarray:
    """The blocky reference: each visual cell = its containing coarse value."""
    N = elev.shape[0]
    V = (N - 1) * factor + 1
    idx = np.clip((np.arange(V) + factor // 2) // factor, 0, N - 1)
    return elev[np.ix_(idx, idx)]


def _save_gray(a: np.ndarray, path: Path):
    lo, hi = float(a.min()), float(a.max())
    n = np.zeros(a.shape, np.uint8) if hi - lo < 1e-9 else ((a - lo) / (hi - lo) * 255).astype(np.uint8)
    Image.fromarray(n, mode="L").save(path)


def bake(mission: str, missions_dir: Path, out_root: Path, factor: int) -> dict:
    pak = missions_dir / f"{mission}.pak"
    if not pak.is_file():
        return {"mission": mission, "error": f"not found: {pak}"}
    md = locate_mapdata(read_packets(pak))
    if md is None:
        return {"mission": mission, "error": "no MapData packet"}
    _, side, blocks = md
    water_elev = read_water_elevation(missions_dir / f"{mission}.fit")
    elev = extract_layers(side, blocks, water_elev)["elev"]   # (N,N) world units

    visual = upsample_corner_pinned(elev, factor)
    V = visual.shape[0]

    # Corner-pin proof: visual[i*factor, j*factor] must equal elev exactly.
    corners = visual[::factor, ::factor]
    max_corner_err = float(np.abs(corners - elev).max())

    delta = visual - nearest_coarse(elev, factor)             # smoothing introduced

    beauty = out_root / f"{mission}.beauty"
    beauty.mkdir(parents=True, exist_ok=True)
    visual.astype("<f4").tofile(beauty / f"visual_height_{factor}x.r32")
    _save_gray(visual, beauty / "visual_height_preview.png")
    _save_gray(np.abs(delta), beauty / "visual_delta_heatmap.png")

    report = {
        "mission": mission,
        "coarse_side": int(side),
        "factor": factor,
        "visual_side": int(V),
        "world_units_per_vertex_coarse": WORLD_UNITS_PER_VERTEX,
        "world_units_per_vertex_visual": WORLD_UNITS_PER_VERTEX / factor,
        "corner_pinned": True,
        "max_corner_error_wu": max_corner_err,         # MUST be 0.0
        "elevation_wu": {"min": float(elev.min()), "max": float(elev.max())},
        "visual_delta_wu": {"max_abs": float(np.abs(delta).max()),
                            "mean_abs": float(np.abs(delta).mean())},
        "visual_height_file": f"visual_height_{factor}x.r32",
        "note": "boring bilinear bake; corners exact; no reshape yet; render-only "
                "(gameplay heightfield untouched).",
    }
    (beauty / "visual_height_report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    return report


def main() -> int:
    ap = argparse.ArgumentParser(description="VISUAL-HEIGHTFIELD-BAKE-1 (offline, corner-pinned)")
    ap.add_argument("missions", nargs="*", default=["mc2_01", "mc2_24"])
    ap.add_argument("--missions-dir", default="A:/Games/Carver5-feasibility/data/missions")
    ap.add_argument("--out", default="tests/terrain/beautify")
    ap.add_argument("--factor", type=int, default=4)
    args = ap.parse_args()
    missions_dir = Path(args.missions_dir)
    out_root = Path(args.out)
    rc = 0
    for m in args.missions:
        rep = bake(m, missions_dir, out_root, args.factor)
        if rep.get("error"):
            print(f"[visual-bake] {m}: ERROR {rep['error']}", file=sys.stderr); rc = 1; continue
        ok = rep["max_corner_error_wu"] == 0.0
        print(f"[visual-bake] {m}: coarse={rep['coarse_side']} -> visual={rep['visual_side']} "
              f"(x{rep['factor']}) corner_err={rep['max_corner_error_wu']:.3g} "
              f"{'PASS' if ok else 'FAIL'}  delta_max={rep['visual_delta_wu']['max_abs']:.2f}wu "
              f"-> {out_root / (m + '.beauty')}")
        if not ok:
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
