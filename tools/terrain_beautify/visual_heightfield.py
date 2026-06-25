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
    derive_masks, detect_pyramid_islands, read_object_footprints, _dilate,
    WORLD_UNITS_PER_VERTEX, CLIFF_SLOPE_DEG,
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


def _box3(a: np.ndarray) -> np.ndarray:
    p = np.pad(a, 1, mode="edge")
    return (p[0:-2, 0:-2] + p[0:-2, 1:-1] + p[0:-2, 2:] +
            p[1:-1, 0:-2] + p[1:-1, 1:-1] + p[1:-1, 2:] +
            p[2:, 0:-2] + p[2:, 1:-1] + p[2:, 2:]) / 9.0


def _fine_mask(mask: np.ndarray, factor: int) -> np.ndarray:
    """Upsample a coarse bool mask to fine grid (nearest)."""
    N = mask.shape[0]; V = (N - 1) * factor + 1
    idx = np.clip((np.arange(V) + factor // 2) // factor, 0, N - 1)
    return mask[np.ix_(idx, idx)]


def _value_noise(V: int, cells: int, seed: int) -> np.ndarray:
    """Deterministic value noise in [-1,1] at the fine resolution (bilinear-upsampled
    random lattice)."""
    rs = np.random.RandomState(seed)
    g = ((rs.uniform(-1.0, 1.0, (cells, cells)) + 1.0) * 127.5).astype(np.uint8)
    img = np.asarray(Image.fromarray(g, "L").resize((V, V), Image.BILINEAR), dtype=np.float64)
    return img / 127.5 - 1.0


def reshape_visual(base: np.ndarray, elev: np.ndarray, factor: int,
                   region_fine: np.ndarray, protect_fine: np.ndarray,
                   corner_clamp: float, max_delta: float, passes: int,
                   erosion_amp: float = 6.0) -> np.ndarray:
    """De-pyramid: round the blocky ramps (iterative fine-grid smoothing) AND break
    the radial pyramid symmetry with multi-octave erosion noise (the "eroded rocky
    island" look — pure smoothing alone only makes islands MORE pyramid-like).
    Coarse-vertex movement clamped to `corner_clamp` ("don't let units float");
    protected (structural) cells pinned to the bilinear baseline; total deviation
    clamped to `max_delta`."""
    work = base.copy()
    V = base.shape[0]
    edit = region_fine & ~protect_fine

    # 1) round the ramps.
    for _ in range(passes):
        avg = _box3(work)
        work = np.where(edit, work + 0.6 * (avg - work), work)
        work[protect_fine] = base[protect_fine]

    # 2) erosion: 2-octave value noise, amplitude scaled by local steepness so flats
    #    stay calm and slopes get rocky break-up. Asymmetric -> lowers pyramid score.
    if erosion_amp > 0.0:
        gy, gx = np.gradient(work)
        steep = np.clip(np.sqrt(gx * gx + gy * gy) / (WORLD_UNITS_PER_VERTEX / factor) / 0.6, 0.0, 1.0)
        noise = 0.65 * _value_noise(V, max(8, V // 14), 1234) + 0.35 * _value_noise(V, max(16, V // 7), 5678)
        work = np.where(edit, work + noise * steep * erosion_amp, work)

    # 3) clamp corner movement + total deviation; pin protected.
    work[protect_fine] = base[protect_fine]
    cor = work[::factor, ::factor]
    cor_clamped = elev + np.clip(cor - elev, -corner_clamp, corner_clamp)
    # distribute the corner correction so we don't reintroduce facets: just set corners,
    # the surrounding fine verts already smooth toward them next time; acceptable for v1.
    work[::factor, ::factor] = cor_clamped
    work = base + np.clip(work - base, -max_delta, max_delta)
    work[protect_fine] = base[protect_fine]
    return work


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


def _pyramid_top_score(elev, water_mask, land_mask):
    pyr, _ = detect_pyramid_islands(elev, land_mask, water_mask)
    return float(pyr[0]["score"]) if pyr else 0.0


def bake(mission: str, missions_dir: Path, out_root: Path, factor: int,
         reshape: bool = False, corner_clamp: float = 0.0,
         max_delta: float = 40.0, passes: int = 24) -> dict:
    pak = missions_dir / f"{mission}.pak"
    if not pak.is_file():
        return {"mission": mission, "error": f"not found: {pak}"}
    md = locate_mapdata(read_packets(pak))
    if md is None:
        return {"mission": mission, "error": "no MapData packet"}
    _, side, blocks = md
    water_elev = read_water_elevation(missions_dir / f"{mission}.fit")
    layers = extract_layers(side, blocks, water_elev)
    elev = layers["elev"]   # (N,N) world units

    visual = upsample_corner_pinned(elev, factor)
    V = visual.shape[0]

    reshape_info = None
    if reshape:
        masks = derive_masks(elev, layers["water"], layers["overlay"])
        _pyr, pyr_mask = detect_pyramid_islands(elev, masks["land"], layers["water"])
        foot, _o, _ = read_object_footprints(read_packets(pak), side)
        protect = _dilate(layers["overlay"] | foot)
        # Reshape region: pyramid islands + steep land slopes (the blocky ramps).
        region = (pyr_mask | (masks["slope_deg"] > CLIFF_SLOPE_DEG)) & masks["land"]
        region_fine = _fine_mask(region, factor)
        protect_fine = _fine_mask(protect, factor)
        base = visual.copy()
        visual = reshape_visual(base, elev, factor, region_fine, protect_fine,
                                corner_clamp, max_delta, passes)
        score_before = _pyramid_top_score(elev, layers["water"], masks["land"])
        vis_coarse = visual[::factor, ::factor][:side, :side]
        score_after = _pyramid_top_score(vis_coarse, layers["water"], masks["land"])
        reshape_info = {
            "corner_clamp_wu": corner_clamp, "max_delta_wu": max_delta, "passes": passes,
            "reshape_region_cells": int(region.sum()),
            "pyramid_top_score_before": score_before,
            "pyramid_top_score_after": score_after,
            "max_corner_move_wu": float(np.abs(vis_coarse - elev).max()),
        }

    # Corner-pin proof: visual[i*factor, j*factor] vs elev (== for bilinear; <=
    # corner_clamp for reshape).
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
        "corner_pinned": not reshape,
        "max_corner_error_wu": max_corner_err,    # 0 for bilinear; <=corner_clamp for reshape
        "reshape": reshape_info,
        "elevation_wu": {"min": float(elev.min()), "max": float(elev.max())},
        "visual_delta_wu": {"max_abs": float(np.abs(delta).max()),
                            "mean_abs": float(np.abs(delta).mean())},
        "visual_height_file": f"visual_height_{factor}x.r32",
        "note": ("reshaped (de-pyramid) visual height; coarse corners clamped to "
                 "corner_clamp; protected pinned; render-only.") if reshape else
                ("boring bilinear bake; corners exact; render-only "
                 "(gameplay heightfield untouched)."),
    }
    (beauty / "visual_height_report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    return report


def main() -> int:
    ap = argparse.ArgumentParser(description="VISUAL-HEIGHTFIELD-BAKE-1 (offline, corner-pinned)")
    ap.add_argument("missions", nargs="*", default=["mc2_01", "mc2_24"])
    ap.add_argument("--missions-dir", default="A:/Games/Carver5-feasibility/data/missions")
    ap.add_argument("--out", default="tests/terrain/beautify")
    ap.add_argument("--factor", type=int, default=4)
    ap.add_argument("--reshape", action="store_true", help="de-pyramid reshape (else boring bilinear)")
    ap.add_argument("--corner-clamp", type=float, default=0.0, help="max coarse-vertex move (wu)")
    ap.add_argument("--max-delta", type=float, default=40.0, help="max visual deviation (wu)")
    ap.add_argument("--passes", type=int, default=24)
    args = ap.parse_args()
    missions_dir = Path(args.missions_dir)
    out_root = Path(args.out)
    rc = 0
    for m in args.missions:
        rep = bake(m, missions_dir, out_root, args.factor, args.reshape,
                   args.corner_clamp, args.max_delta, args.passes)
        if rep.get("error"):
            print(f"[visual-bake] {m}: ERROR {rep['error']}", file=sys.stderr); rc = 1; continue
        cap = (0.0 if not args.reshape else args.corner_clamp) + 1e-4
        ok = rep["max_corner_error_wu"] <= cap
        extra = ""
        if rep.get("reshape"):
            ri = rep["reshape"]
            extra = (f" pyr_score {ri['pyramid_top_score_before']:.2f}->{ri['pyramid_top_score_after']:.2f}"
                     f" corner_move<={ri['max_corner_move_wu']:.1f}wu")
        print(f"[visual-bake] {m}: coarse={rep['coarse_side']} -> visual={rep['visual_side']} "
              f"(x{rep['factor']}) corner_err={rep['max_corner_error_wu']:.3g} "
              f"{'PASS' if ok else 'FAIL'}  delta_max={rep['visual_delta_wu']['max_abs']:.2f}wu{extra} "
              f"-> {out_root / (m + '.beauty')}")
        if not ok:
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
