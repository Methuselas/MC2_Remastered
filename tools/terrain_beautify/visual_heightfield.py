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
    visual_damp.r32             (--reauth only) float32 [N*N] object-proximity damp
                                0=no displacement (on/near buildings) .. 1=full

TERRAIN-REAUTH-UNPIN-1 adds `--reauth`: terrain-AWARE, corner-UNpinned re-authoring
(user ruling: "UN PIN THE CORNERS. KEEP THE SHAPE"). Taubin lambda|mu band-pass
smoothing on the 4x grid flattens coarse-cell faceting and rounds pyramid facet
edges into curves, while the LANDFORM is kept by construction + constraint:
  - Taubin pair passes are volume-preserving band-pass (landform band ~untouched);
  - coarse local extrema (peaks/pits) may move at most --shape-tolerance of their
    local relief; residual loss is re-injected as smooth Gaussian fields
    (multi-point / neighborhood-aware, NOT a per-vertex clamp);
  - total deviation from the bilinear baseline is soft-bounded (C1 tanh limiter)
    by --max-drift;
  - roads/overlay + building footprints stay pinned to the bilinear baseline
    (feathered, crease-free).
Gameplay height (MapData::blocks[]) is NEVER touched: render-only sidecar for the
MC2_TERRAIN_VISUAL_DISPLACE consumer. The companion visual_damp.r32 is the engine
Half-B seed: static object-proximity displacement fade (units/buildings stand on
true gameplay height).

CLIFF-SMOOTH-1 extends --reauth for giant TERRACED cliff faces (hard 100wu+
steps in silhouette, mc2_17-class). Two problems block the base recipe there:
(a) --max-drift 24 << step size, and (b) extrema-preservation pins the
staircase (every terrace edge is a coarse extremum). Fixes, all render-only:
  - SLOPE-ADAPTIVE DRIFT: max-drift becomes a FIELD. Flats keep the scalar
    --max-drift; the cliff mask (one-sided coarse slope > --cliff-slope-deg,
    dilated one ring, feathered on the fine grid) allows --cliff-drift.
  - CLIFF MELT: Taubin is band-pass BY DESIGN (the landform band passes), so
    terrace treads survive it. On the cliff weight only, plain weighted
    diffusion (--cliff-melt-passes) relaxes the staircase toward the harmonic
    (continuous-face) surface; flats stay calm (weight ~ 0).
  - EXTREMA RELAXATION (documented): extrema ON the cliff mask that are NOT
    regional summits/pits (7x7 window) are exempted from re-injection — it is
    the STEPS between peaks that melt; true peaks stay pinned.
  - CLIFF-NESS: under --mountainify, detail on the cliff mask is biased toward
    gradient-aligned striation (value noise directionally blurred ALONG the
    downhill direction, then ridged) — erosion runs DOWN the face instead of
    isotropic bumps. Modest amplitude, still drift-bounded + water-excluded.

Run:
  python visual_heightfield.py <mission> [--missions-dir DIR] [--out DIR] [--factor 4]
  python visual_heightfield.py mc2_17 --reauth [--shape-tolerance 0.10]
         [--max-drift 24] [--reauth-passes 150] [--objfade-radius-wu 256]
         [--mountainify] [--mountainify-amp 14] [--seed 1337]
         [--cliff-drift 112] [--cliff-slope-deg 30] [--cliff-melt-passes 300]
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


# --- TERRAIN-REAUTH-UNPIN-1: corner-UNpinned landform-preserving re-authoring ---

def _filter3(a: np.ndarray, reduce_fn, iterations: int) -> np.ndarray:
    """Iterated 3x3 morphological max/min filter (edge-padded, numpy-only)."""
    n = a.shape[0]
    for _ in range(iterations):
        p = np.pad(a, 1, mode="edge")
        a = reduce_fn.reduce([p[dr:dr + n, dc:dc + n]
                              for dr in range(3) for dc in range(3)])
    return a


def _crease_energy(a: np.ndarray) -> float:
    """Mean squared 3-point second difference (row+col): faceting/crease metric.
    A bilinear upsample concentrates ALL of its curvature at coarse gridlines
    (creases); a rounded surface spreads (and shrinks) it."""
    sr = np.zeros_like(a)
    sc = np.zeros_like(a)
    sr[1:-1, :] = a[2:, :] - 2 * a[1:-1, :] + a[:-2, :]
    sc[:, 1:-1] = a[:, 2:] - 2 * a[:, 1:-1] + a[:, :-2]
    return float((sr * sr + sc * sc).mean())


def detect_coarse_extrema(elev: np.ndarray, min_prominence: float = 4.0,
                          relief_win_iters: int = 3) -> list:
    """Peaks and pits of the coarse landform, each with its LOCAL relief (7x7 by
    default) so the shape tolerance scales with how prominent the feature is.

    Flat-top mesas / terraces satisfy `elev >= nmax` for MANY connected cells; a
    representative-per-connected-component dedup keeps ONE guard point per
    feature (the raw per-cell mask floods terrace edges and makes the Gaussian
    corrections fight each other). Returns [{r, c, kind, h0, relief,
    is_regional}] — is_regional = the extremum is ALSO the extreme of its 7x7
    relief window (a true summit / basin floor, not a terrace-step edge with
    higher/lower ground a few cells away). CLIFF-SMOOTH-1 uses this to relax
    terrace-step extrema on the cliff mask while keeping real peaks pinned."""
    from mission_terrain_analyzer import label_components
    n = elev.shape[0]
    p = np.pad(elev, 1, mode="edge")
    neigh = [p[dr:dr + n, dc:dc + n]
             for dr in range(3) for dc in range(3) if (dr, dc) != (1, 1)]
    nmax = np.maximum.reduce(neigh)
    nmin = np.minimum.reduce(neigh)
    rmax = _filter3(elev, np.maximum, relief_win_iters)
    rmin = _filter3(elev, np.minimum, relief_win_iters)
    relief = rmax - rmin
    peaks = (elev >= nmax) & ((elev - rmin) >= min_prominence)
    pits = (elev <= nmin) & ((rmax - elev) >= min_prominence)
    out = []
    for kind, mask in (("peak", peaks), ("pit", pits)):
        labels, count = label_components(mask)
        for lid in range(1, count + 1):
            ys, xs = np.where(labels == lid)
            comp = elev[ys, xs]
            i = int(np.argmax(comp)) if kind == "peak" else int(np.argmin(comp))
            r, c = int(ys[i]), int(xs[i])
            regional = (bool(elev[r, c] >= rmax[r, c] - 1e-6) if kind == "peak"
                        else bool(elev[r, c] <= rmin[r, c] + 1e-6))
            out.append({"r": r, "c": c, "kind": kind, "h0": float(elev[r, c]),
                        "relief": float(relief[r, c]), "is_regional": regional})
    return out


def _smoothstep(edge0: float, edge1: float, x: np.ndarray) -> np.ndarray:
    t = np.clip((x - edge0) / max(1e-9, (edge1 - edge0)), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def _ridged_fbm(V: int, base_cells: int, seed: int, octaves: int = 5,
                gain: float = 0.5, lacunarity: float = 2.0) -> np.ndarray:
    """Deterministic ridged multifractal in [0,1] (numpy value-noise octaves,
    ridged = (1-|n|)^2 so crests form at noise zero-crossings)."""
    total = np.zeros((V, V), np.float64)
    amp, freq, norm = 1.0, float(base_cells), 0.0
    for o in range(octaves):
        n = _value_noise(V, max(4, int(round(freq))), seed + 101 * o)
        r = (1.0 - np.abs(n)) ** 2
        total += amp * r
        norm += amp
        amp *= gain
        freq *= lacunarity
    return total / norm


def mountain_rock_mask(work: np.ndarray, elev: np.ndarray, factor: int,
                       water_coarse=None, flat_deg: float = 10.0,
                       steep_deg: float = 35.0, low_frac: float = 0.35,
                       high_frac: float = 0.65) -> np.ndarray:
    """Rock-channel weight on the fine grid — same slope+altitude heuristic as
    control_map_tool.classify_weights (steep OR high -> rock), recomputed inline
    at fine resolution from the current visual surface. Water (and a feathered
    band around it) is excluded."""
    spacing = WORLD_UNITS_PER_VERTEX / factor
    gy, gx = np.gradient(work)
    slope_deg = np.degrees(np.arctan(np.hypot(gx, gy) / spacing))
    lo, hi = float(elev.min()), float(elev.max())
    elev_norm = np.clip((work - lo) / max(1e-9, hi - lo), 0.0, 1.0)
    rock = np.maximum(_smoothstep(flat_deg, steep_deg, slope_deg),
                      _smoothstep(low_frac, high_frac, elev_norm))
    if water_coarse is not None and water_coarse.any():
        wf = _fine_mask(_dilate(water_coarse), factor).astype(np.float64)
        for _ in range(factor):
            wf = _box3(wf)
        rock *= np.clip(1.0 - wf, 0.0, 1.0)
        rock[_fine_mask(water_coarse, factor)] = 0.0
    return rock


# --- CLIFF-SMOOTH-1: terraced-cliff melting + gradient-aligned striation -----

def onesided_step(elev: np.ndarray) -> np.ndarray:
    """Per-cell max |elevation jump| to any 4-neighbor (wu). The true riser
    height of a 1-cell terrace step (central differences halve it)."""
    d_r = np.abs(np.diff(elev, axis=0))          # (N-1, N) riser between rows
    d_c = np.abs(np.diff(elev, axis=1))          # (N, N-1)
    step = np.zeros_like(elev)
    step[:-1, :] = np.maximum(step[:-1, :], d_r)
    step[1:, :] = np.maximum(step[1:, :], d_r)
    step[:, :-1] = np.maximum(step[:, :-1], d_c)
    step[:, 1:] = np.maximum(step[:, 1:], d_c)
    return step


def coarse_cliff_mask(elev: np.ndarray, water_coarse=None,
                      cliff_slope_deg: float = 30.0,
                      dilate_iters: int = 1) -> np.ndarray:
    """Cliff mask on the coarse grid from ONE-SIDED max slope.

    np.gradient central differences HALVE the apparent slope of a single-cell
    100wu riser (50wu/cell = 21deg -> invisible to a 30deg threshold), which is
    exactly the terraced-staircase geometry this slice targets. One-sided
    forward diffs see the true 100/128 = 38deg step. Water is excluded both
    before and after the ring dilation (the melt must never touch the water
    plane)."""
    slope_deg = np.degrees(np.arctan(onesided_step(elev) / WORLD_UNITS_PER_VERTEX))
    m = slope_deg > cliff_slope_deg
    if water_coarse is not None:
        m &= ~water_coarse
    for _ in range(dilate_iters):
        m = _dilate(m)
    if water_coarse is not None:
        m &= ~water_coarse
    return m


def cliff_weight_fine(cliff_coarse: np.ndarray, factor: int) -> np.ndarray:
    """Feathered [0,1] cliff weight on the fine grid: 1.0 on the (dilated)
    coarse cliff mask, box-feathered ~2 coarse cells wide so the slope-adaptive
    drift cap has no crease at the mask boundary (same pattern as the
    structural-pin feather)."""
    w = _fine_mask(cliff_coarse, factor).astype(np.float64)
    for _ in range(2 * factor):
        w = _box3(w)
    w = np.clip(w, 0.0, 1.0)
    w[_fine_mask(cliff_coarse, factor)] = 1.0
    return w


def _bilinear_sample(a: np.ndarray, r: np.ndarray, c: np.ndarray) -> np.ndarray:
    """Bilinear gather at fractional (row, col) positions (edge-clamped)."""
    r = np.clip(r, 0.0, a.shape[0] - 1.0)
    c = np.clip(c, 0.0, a.shape[1] - 1.0)
    r0 = np.floor(r).astype(np.int64)
    c0 = np.floor(c).astype(np.int64)
    r1 = np.minimum(r0 + 1, a.shape[0] - 1)
    c1 = np.minimum(c0 + 1, a.shape[1] - 1)
    fr = r - r0
    fc = c - c0
    return (a[r0, c0] * (1 - fr) * (1 - fc) + a[r1, c0] * fr * (1 - fc)
            + a[r0, c1] * (1 - fr) * fc + a[r1, c1] * fr * fc)


def striation_field(surface: np.ndarray, seed: int, iters: int = 24,
                    blur_step: float = 1.5) -> np.ndarray:
    """Gradient-aligned ridged striation, zero-centered ~[-1,1].

    Erosion runs DOWN a cliff face: gullies/spurs are elongated along the fall
    line and vary ACROSS it. Build: high-frequency value noise, directionally
    blurred ALONG the local downhill direction of `surface` (features smear
    downslope), then ridged ((1-|n|)^2 -> crests at zero crossings). Tuning
    (iters=24, step=1.5, lattice V/4) gives ~6.6x across/down gradient-energy
    anisotropy on a uniform ramp. Deterministic from `seed` (offset so it
    never aliases the isotropic mountainify octaves)."""
    V = surface.shape[0]
    gy, gx = np.gradient(surface)
    mag = np.hypot(gx, gy) + 1e-9
    dy, dx = gy / mag * blur_step, gx / mag * blur_step
    n = _value_noise(V, max(8, V // 4), seed + 9001)
    rr, cc = np.mgrid[0:V, 0:V].astype(np.float64)
    rp, cp = rr + dy, cc + dx
    rm, cm = rr - dy, cc - dx
    for _ in range(iters):
        n = 0.5 * n + 0.25 * (_bilinear_sample(n, rp, cp)
                              + _bilinear_sample(n, rm, cm))
    n = n / (np.abs(n).max() + 1e-9)
    r = (1.0 - np.abs(n)) ** 2
    return (r - r.mean()) * 2.0


def reauth_visual(elev: np.ndarray, factor: int, protect_coarse,
                  shape_tolerance: float = 0.10, max_drift: float = 24.0,
                  passes: int = 60, lam: float = 0.5, mu: float = -0.52,
                  min_prominence: float = 4.0,
                  mountainify_amp: float = 0.0, seed: int = 1337,
                  water_coarse=None,
                  cliff_drift: float = 0.0, cliff_slope_deg: float = 30.0,
                  cliff_melt_passes: int = 300):
    """Corner-UNpinned, landform-preserving re-authoring of the visual surface.

    1) Taubin lambda|mu smoothing on the fine grid: a volume-preserving BAND-PASS
       (per pass-pair transfer f(k) = (1-lambda*k)(1-mu*k)); coarse-cell faceting
       (high k) is attenuated hard, the landform band (low k) passes ~unchanged.
       This is the "flatten faceting + soften pyramid edges" half. All coarse
       vertices are free to move (UNpinned).
    2) C1 soft drift bound: |out - bilinear| smoothly limited to max_drift via
       tanh (a hard clip would re-introduce facets at the clip isolines).
    3) Extrema re-injection (multi-point, neighborhood-aware): each coarse
       peak/pit may move at most shape_tolerance * local_relief; any excess loss
       is restored with a smooth Gaussian correction field (sigma ~ 1.25 coarse
       cells), 2 rounds for overlapping features.
    4) Feathered structural pin: overlay (roads) + building footprints ride the
       bilinear baseline; feather width ~1 coarse cell so no crease at the pin
       boundary.

    MOUNTAINIFY (mountainify_amp > 0): feature-ADDING pass between (2) and (3).
    Ridged multifractal detail (deterministic from `seed`), amplitude-modulated
    by the rock-channel mask (steep OR high — control_map_tool classifier logic
    recomputed inline on the fine grid), zero-centered so ridges add and ravines
    carve. Flat valleys get ~nothing; steep faces get ridge/spur/talus break-up.
    Still bounded by max_drift (re-limited) and still subject to the extrema
    guarantee + structural pin ("same mountain, more mountain-like").

    CLIFF-SMOOTH-1 (cliff_drift > 0): terraced-cliff melting. See module
    docstring — cliff-weighted diffusion between (1) and (2), a slope-adaptive
    drift FIELD in (2)/(2b), non-regional extrema on the cliff mask exempted
    from re-injection in (3), and gradient-aligned striation blended into the
    mountainify detail on the cliff weight.

    Returns (work, info) — info carries the shape-fidelity metrics."""
    base = upsample_corner_pinned(elev, factor)
    V = base.shape[0]
    work = base.copy()

    # 1) Taubin band-pass pairs.
    for _ in range(passes):
        work += lam * (_box3(work) - work)
        work += mu * (_box3(work) - work)

    # 1b) CLIFF-SMOOTH-1 melt: Taubin is band-pass BY DESIGN (the landform band
    #     passes ~unchanged), so a giant terraced face — whose treads live in
    #     the landform band — survives it. On the cliff weight only, run plain
    #     weighted diffusion: the staircase relaxes toward the harmonic
    #     (continuous-face) surface. Flats have weight ~0 and stay calm.
    cliff_mask = None
    cliff_w = None
    if cliff_drift > 0.0:
        cliff_mask = coarse_cliff_mask(elev, water_coarse=water_coarse,
                                       cliff_slope_deg=cliff_slope_deg)
        if cliff_mask.any():
            cliff_w = cliff_weight_fine(cliff_mask, factor)
            melt_step = 0.55 * cliff_w
            for _ in range(cliff_melt_passes):
                work += melt_step * (_box3(work) - work)

    # 2) soft drift bound. With CLIFF-SMOOTH-1 the bound is a FIELD: scalar
    #    max_drift on flats, ramping (feathered) to cliff_drift on the cliff
    #    mask so the melt can actually move terrace treads ~half a step.
    drift_cap = None
    eff_drift = max_drift
    if max_drift > 0:
        if cliff_w is not None and cliff_drift > max_drift:
            drift_cap = max_drift + (cliff_drift - max_drift) * cliff_w
            eff_drift = cliff_drift
            work = base + drift_cap * np.tanh((work - base) / drift_cap)
        else:
            dev = work - base
            work = base + max_drift * np.tanh(dev / max_drift)

    # facet-flattening proof point: crease energy of the SMOOTHED surface,
    # before any feature-ADDING detail (ridges are legitimate new curvature and
    # would pollute this metric).
    crease_smooth = _crease_energy(work)

    # 2b) MOUNTAINIFY: synthesize mountain character where the terrain is
    #     steep/high, then re-apply the drift bound to the composite.
    minfo = None
    if mountainify_amp > 0.0:
        rock = mountain_rock_mask(work, elev, factor, water_coarse=water_coarse)
        detail = (_ridged_fbm(V, max(6, V // 16), seed) - 0.5) * 2.0  # [-1,1]
        stria_rms_on_cliff = None
        if cliff_w is not None:
            # CLIFF-SMOOTH-1 cliff-ness: on the cliff weight, bias the synthesis
            # toward gradient-aligned striation (erosion runs DOWN the face)
            # instead of isotropic bumps; modest amplitude (0.75x).
            stria = striation_field(work, seed)
            detail = (1.0 - cliff_w) * detail + (0.75 * cliff_w) * stria
        add = mountainify_amp * rock * detail
        work += add
        if max_drift > 0:
            cap = drift_cap if drift_cap is not None else max_drift
            work = base + cap * np.tanh((work - base) / cap)
        rock_cells = rock > 0.35
        if cliff_w is not None:
            cliff_cells = cliff_w > 0.5
            if cliff_cells.any():
                stria_rms_on_cliff = float(np.sqrt((add[cliff_cells] ** 2).mean()))
        minfo = {
            "seed": seed,
            "amp_wu": mountainify_amp,
            "rock_area_frac": float(rock_cells.mean()),
            "detail_rms_on_rock_wu": (float(np.sqrt((add[rock_cells] ** 2).mean()))
                                      if rock_cells.any() else 0.0),
            "striation_rms_on_cliff_wu": stria_rms_on_cliff,
        }

    # 3) extrema re-injection: shrinking-sigma rounds so nearby guard points stop
    #    fighting (a wide correction for A can push neighbour B out of ITS budget;
    #    each round narrows the support and re-measures honestly).
    #    CLIFF-SMOOTH-1 relaxation (documented): extrema ON the cliff mask that
    #    are NOT regional summits/pits are terrace-step edges — re-injecting
    #    them would rebuild the exact staircase the melt removed. They are
    #    exempted; true peaks/pits (is_regional) stay pinned everywhere.
    all_extrema = detect_coarse_extrema(elev, min_prominence=min_prominence)
    relaxed = []
    if cliff_mask is not None and cliff_mask.any():
        extrema = []
        for e in all_extrema:
            if cliff_mask[e["r"], e["c"]] and not e["is_regional"]:
                relaxed.append(e)
            else:
                extrema.append(e)
    else:
        extrema = all_extrema
    for sigma in (1.25 * factor, 0.9 * factor, 0.6 * factor, 0.4 * factor):
        win = max(2, int(3 * sigma))
        yy, xx = np.mgrid[-win:win + 1, -win:win + 1]
        kern = np.exp(-(xx * xx + yy * yy) / (2.0 * sigma * sigma))
        fld = np.zeros_like(work)
        any_fix = False
        for e in extrema:
            fr, fc = e["r"] * factor, e["c"] * factor
            tol = max(0.5, shape_tolerance * e["relief"]) * 0.75  # keep margin
            err = e["h0"] - work[fr, fc]
            excess = err - np.clip(err, -tol, tol)
            if excess == 0.0:
                continue
            any_fix = True
            r0, r1 = max(0, fr - win), min(V, fr + win + 1)
            c0, c1 = max(0, fc - win), min(V, fc + win + 1)
            fld[r0:r1, c0:c1] += excess * kern[r0 - fr + win:r1 - fr + win,
                                               c0 - fc + win:c1 - fc + win]
        if not any_fix:
            break
        work += fld

    # 4) feathered structural pin.
    if protect_coarse is not None and protect_coarse.any():
        pf = _fine_mask(protect_coarse, factor).astype(np.float64)
        for _ in range(factor):
            pf = _box3(pf)
        pf = np.clip(pf, 0.0, 1.0)
        pf[_fine_mask(protect_coarse, factor)] = 1.0
        work = pf * base + (1.0 - pf) * work

    # --- honest post-everything metrics (the PASS-gate inputs) ---
    corners = work[::factor, ::factor]
    corner_move = np.abs(corners - elev)
    ev, cv = elev.ravel(), corners.ravel()
    corr = float(np.corrcoef(ev, cv)[0, 1]) if ev.std() > 1e-9 else 1.0
    viol = 0
    worst_frac = 0.0
    max_move = 0.0
    for e in extrema:
        tol = max(0.5, shape_tolerance * e["relief"])
        mv = abs(float(work[e["r"] * factor, e["c"] * factor]) - e["h0"])
        max_move = max(max_move, mv)
        worst_frac = max(worst_frac, mv / max(1e-9, e["relief"]))
        if mv > tol + 1e-6:
            viol += 1
    drift = np.abs(work - base)
    cliff_info = None
    if cliff_drift > 0.0:
        cliff_info = {
            "cliff_drift_wu": cliff_drift,
            "cliff_slope_deg": cliff_slope_deg,
            "melt_passes": cliff_melt_passes,
            "coarse_cliff_cells": (int(cliff_mask.sum())
                                   if cliff_mask is not None else 0),
            "coarse_cliff_frac": (float(cliff_mask.mean())
                                  if cliff_mask is not None else 0.0),
            "relaxed_extrema": len(relaxed),
        }
        if cliff_w is not None:
            core = cliff_w > 0.5
            if core.any():
                dcl = drift[core]
                cliff_info["drift_on_cliff_wu"] = {
                    "mean": float(dcl.mean()),
                    "p99": float(np.percentile(dcl, 99)),
                    "max": float(dcl.max())}
    info = {
        "mode": "reauth",
        "shape_tolerance": shape_tolerance,
        "max_drift_wu": max_drift,
        "effective_max_drift_wu": eff_drift,
        "passes": passes,
        "taubin_lambda": lam,
        "taubin_mu": mu,
        "min_prominence_wu": min_prominence,
        "landform_correlation": corr,
        "extrema": {"count": len(all_extrema), "enforced": len(extrema),
                    "relaxed_on_cliff": len(relaxed), "violations": viol,
                    "worst_move_frac_of_relief": worst_frac,
                    "max_move_wu": max_move},
        "cliff": cliff_info,
        "corner_move_wu": {"max": float(corner_move.max()),
                           "mean": float(corner_move.mean())},
        "facet_crease_energy": {"bilinear_base": _crease_energy(base),
                                "smoothed": crease_smooth,
                                "reauth": _crease_energy(work)},
        "drift_vs_bilinear_wu": {"mean": float(drift.mean()),
                                 "p99": float(np.percentile(drift, 99)),
                                 "max": float(drift.max())},
        "mountainify": minfo,
    }
    return work, info


def build_object_damp(side: int, foot: np.ndarray, radius_wu: float = 256.0,
                      core_dilate: int = 1) -> np.ndarray:
    """Static object-proximity displacement damp map (engine Half-B seed).

    0.0 on/next to building footprints (displacement fully OFF -> units and
    buildings stand on true gameplay height), smoothstep ramp to 1.0 at
    radius_wu away. Distance = 4-connected (manhattan) cell distance from the
    dilated footprint core; numpy-only."""
    core = foot.copy()
    for _ in range(core_dilate):
        core = _dilate(core)
    radius_cells = max(1, int(np.ceil(radius_wu / WORLD_UNITS_PER_VERTEX)))
    dist = np.full((side, side), np.float64(radius_cells + 1))
    dist[core] = 0.0
    cur = core.copy()
    for k in range(1, radius_cells + 1):
        nxt = _dilate(cur)
        dist[nxt & ~cur] = k
        cur = nxt
    t = np.clip(dist / radius_cells, 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)   # smoothstep


# TERRAIN-LOD-GEOMORPH-1: coarse-band vertex strides the renderer decimates to
# (mclib/terrain.cpp LOD_STEPS[1:]). One max-reduced level is emitted per stride.
MIP_STRIDES = (2, 4, 5, 10, 20)


def _sliding_max(a: np.ndarray, halfw: int, axis: int) -> np.ndarray:
    """Centered sliding-window max along `axis` (edge-padded, window 2*halfw+1)."""
    from numpy.lib.stride_tricks import sliding_window_view
    pad = [(0, 0), (0, 0)]
    pad[axis] = (halfw, halfw)
    p = np.pad(a, pad, mode="edge")
    return sliding_window_view(p, 2 * halfw + 1, axis=axis).max(axis=-1)


def build_max_mips(visual: np.ndarray, factor: int, side: int) -> dict:
    """Max-preserving reduction levels for the coarse-band silhouette fix.

    For each renderer LOD stride N the level stores, AT EVERY coarse vertex
    (side x side, full resolution -- no decimation, so stitch lookups at any
    stride-aligned index are well-defined), the MAX of the FINAL fine bake over
    the Voronoi footprint that vertex represents on the stride-N lattice:
    +/- N/2 coarse cells = +/- N*factor/2 fine samples. A coarse band whose
    vertex Z reads its own level can no longer drop ridge maxima that fall
    between surviving vertices (silhouette-LOSS root cause,
    TERRAIN-LOD-GEOMORPH-RECON-1 sec 3). Built from the FINAL `visual` array
    (post-reshape / post-reauth+mountainify) so silhouette maxes track whatever
    surface actually ships -- it composes with --reauth automatically. The mips
    sidecar MUST be regenerated whenever the fine bake changes."""
    levels = []
    stats = []
    corners = visual[::factor, ::factor][:side, :side]
    for s in MIP_STRIDES:
        halfw = (s * factor) // 2                     # +/- s/2 coarse cells in fine units
        m = _sliding_max(_sliding_max(visual, halfw, 0), halfw, 1)
        mip = m[::factor, ::factor][:side, :side]     # sample at coarse verts
        lift = mip - corners
        levels.append(mip.astype("<f4"))
        stats.append({"stride": s, "window_fine": 2 * halfw + 1,
                      "max_lift_wu": float(lift.max()),
                      "mean_lift_wu": float(lift.mean())})
    return {"blob": np.concatenate([lv.ravel() for lv in levels]),
            "strides": list(MIP_STRIDES), "stats": stats}


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
         max_delta: float = 40.0, passes: int = 24,
         reauth: bool = False, shape_tolerance: float = 0.10,
         max_drift: float = 24.0, reauth_passes: int = 60,
         objfade_radius_wu: float = 256.0,
         mountainify_amp: float = 0.0, seed: int = 1337,
         mips: bool = True,
         cliff_drift: float = 0.0, cliff_slope_deg: float = 30.0,
         cliff_melt_passes: int = 300) -> dict:
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
    reauth_info = None
    damp = None
    if reauth:
        # TERRAIN-REAUTH-UNPIN-1: corner-UNpinned landform-preserving re-auth.
        foot, _o, _ = read_object_footprints(read_packets(pak), side)
        protect = _dilate(layers["overlay"] | foot)
        visual, reauth_info = reauth_visual(
            elev, factor, protect, shape_tolerance=shape_tolerance,
            max_drift=max_drift, passes=reauth_passes,
            mountainify_amp=mountainify_amp, seed=seed,
            water_coarse=layers["water"],
            cliff_drift=cliff_drift, cliff_slope_deg=cliff_slope_deg,
            cliff_melt_passes=cliff_melt_passes)
        # pyramid score before/after on the coarse-sampled landform (same metric
        # the reshape path reports; reauth should LOWER it a bit -- rounded facets
        # weaken the radial-monotonic pyramid signature -- while corr stays high).
        masks = derive_masks(elev, layers["water"], layers["overlay"])
        reauth_info["pyramid_top_score_before"] = _pyramid_top_score(
            elev, layers["water"], masks["land"])
        reauth_info["pyramid_top_score_after"] = _pyramid_top_score(
            visual[::factor, ::factor][:side, :side], layers["water"], masks["land"])
        # Half-B companion: static object-proximity damp (buildings).
        reauth_info["objfade_radius_wu"] = objfade_radius_wu
        damp = build_object_damp(side, foot, radius_wu=objfade_radius_wu)
    elif reshape:
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
    # corner_clamp for reshape; UNpinned but drift-bounded for reauth).
    corners = visual[::factor, ::factor]
    max_corner_err = float(np.abs(corners - elev).max())

    delta = visual - nearest_coarse(elev, factor)             # smoothing introduced

    # TERRAIN-LOD-GEOMORPH-1: max-preserving mip levels, built from the FINAL
    # fine bake (post-reshape / post-any-future-reauth) so silhouette maxes
    # track whatever surface actually ships. Emitted as a sibling sidecar the
    # engine appends to the binding-26 SSBO; absent file = legacy behavior.
    mips_info = None
    if mips:
        mm = build_max_mips(visual, factor, side)
        mips_info = {"file": "visual_height_mips.r32",
                     "strides": mm["strides"],
                     "grid_side": int(side),
                     "levels": mm["stats"]}

    beauty = out_root / f"{mission}.beauty"
    beauty.mkdir(parents=True, exist_ok=True)
    visual.astype("<f4").tofile(beauty / f"visual_height_{factor}x.r32")
    if mips:
        mm["blob"].astype("<f4").tofile(beauty / "visual_height_mips.r32")
    _save_gray(visual, beauty / "visual_height_preview.png")
    _save_gray(np.abs(delta), beauty / "visual_delta_heatmap.png")
    if damp is not None:
        damp.astype("<f4").tofile(beauty / "visual_damp.r32")
        _save_gray(damp, beauty / "visual_damp_preview.png")

    if reauth:
        note = ("REAUTH (TERRAIN-REAUTH-UNPIN-1): corner-UNpinned Taubin band-pass "
                "smoothing; landform kept (extrema within shape_tolerance of local "
                "relief, drift tanh-bounded); overlay/footprints feather-pinned; "
                "visual_damp.r32 = static object displacement fade; render-only.")
        if reauth_info.get("cliff"):
            note += (" CLIFF-SMOOTH-1: slope-adaptive drift field (cliff mask "
                     "allows cliff_drift), terrace melt diffusion, non-regional "
                     "cliff extrema relaxed (true peaks pinned), gradient-aligned "
                     "striation under mountainify.")
    elif reshape:
        note = ("reshaped (de-pyramid) visual height; coarse corners clamped to "
                "corner_clamp; protected pinned; render-only.")
    else:
        note = ("boring bilinear bake; corners exact; render-only "
                "(gameplay heightfield untouched).")
    report = {
        "mission": mission,
        "coarse_side": int(side),
        "factor": factor,
        "visual_side": int(V),
        "world_units_per_vertex_coarse": WORLD_UNITS_PER_VERTEX,
        "world_units_per_vertex_visual": WORLD_UNITS_PER_VERTEX / factor,
        "corner_pinned": not (reshape or reauth),
        "max_corner_error_wu": max_corner_err,    # 0 bilinear; <=corner_clamp reshape; <=~max_drift reauth
        "reshape": reshape_info,
        "reauth": reauth_info,
        "elevation_wu": {"min": float(elev.min()), "max": float(elev.max())},
        "visual_delta_wu": {"max_abs": float(np.abs(delta).max()),
                            "mean_abs": float(np.abs(delta).mean())},
        "visual_height_file": f"visual_height_{factor}x.r32",
        "visual_damp_file": ("visual_damp.r32" if damp is not None else None),
        "max_mips": mips_info,
        "note": note,
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
    # TERRAIN-REAUTH-UNPIN-1 knobs (own the mode; --corner-clamp is the seed only)
    ap.add_argument("--reauth", action="store_true",
                    help="corner-UNpinned landform-preserving re-auth (Taubin band-pass)")
    ap.add_argument("--shape-tolerance", type=float, default=0.10,
                    help="max extremum move as fraction of local relief")
    ap.add_argument("--max-drift", type=float, default=24.0,
                    help="soft bound on |visual - bilinear| (wu, tanh limiter)")
    ap.add_argument("--reauth-passes", type=int, default=150, help="Taubin pass pairs")
    ap.add_argument("--objfade-radius-wu", type=float, default=256.0,
                    help="static object damp fade radius (visual_damp.r32)")
    ap.add_argument("--mountainify", action="store_true",
                    help="feature-ADDING ridged detail on rock-channel regions "
                         "(implies --reauth)")
    ap.add_argument("--mountainify-amp", type=float, default=14.0,
                    help="mountainify detail amplitude (wu) at full rock weight")
    ap.add_argument("--seed", type=int, default=1337, help="mountainify noise seed")
    # CLIFF-SMOOTH-1 knobs (active with --reauth; 0 disables)
    ap.add_argument("--cliff-drift", type=float, default=112.0,
                    help="drift bound ON the cliff mask (wu); flats keep "
                         "--max-drift. 0 disables CLIFF-SMOOTH-1 entirely")
    ap.add_argument("--cliff-slope-deg", type=float, default=30.0,
                    help="one-sided coarse slope threshold for the cliff mask")
    ap.add_argument("--cliff-melt-passes", type=int, default=300,
                    help="cliff-weighted diffusion passes (terrace melt)")
    # TERRAIN-LOD-GEOMORPH-1 knob
    ap.add_argument("--no-mips", action="store_true",
                    help="skip TERRAIN-LOD-GEOMORPH-1 max-mip sidecar emission")
    args = ap.parse_args()
    if args.mountainify:
        args.reauth = True
    if args.reauth and args.reshape:
        print("[visual-bake] --reauth and --reshape are mutually exclusive", file=sys.stderr)
        return 2
    missions_dir = Path(args.missions_dir)
    out_root = Path(args.out)
    rc = 0
    for m in args.missions:
        rep = bake(m, missions_dir, out_root, args.factor, args.reshape,
                   args.corner_clamp, args.max_delta, args.passes,
                   reauth=args.reauth, shape_tolerance=args.shape_tolerance,
                   max_drift=args.max_drift, reauth_passes=args.reauth_passes,
                   objfade_radius_wu=args.objfade_radius_wu,
                   mountainify_amp=(args.mountainify_amp if args.mountainify else 0.0),
                   seed=args.seed, mips=not args.no_mips,
                   cliff_drift=args.cliff_drift,
                   cliff_slope_deg=args.cliff_slope_deg,
                   cliff_melt_passes=args.cliff_melt_passes)
        if rep.get("error"):
            print(f"[visual-bake] {m}: ERROR {rep['error']}", file=sys.stderr); rc = 1; continue
        extra = ""
        if rep.get("reauth"):
            ra = rep["reauth"]
            # reauth PASS = landform kept (corr + extrema) AND corners actually
            # unpinned AND drift inside the soft bound (+tanh slack).
            # corner cap: extrema re-injection may locally exceed the tanh soft
            # bound while restoring a peak (that is the guarantee WORKING) ->
            # allow 25% headroom over the EFFECTIVE drift (CLIFF-SMOOTH-1: the
            # cliff mask allows cliff_drift; flats keep max_drift).
            eff = float(ra.get("effective_max_drift_wu", args.max_drift))
            ok = (ra["extrema"]["violations"] == 0
                  and ra["landform_correlation"] >= 0.99
                  and rep["max_corner_error_wu"] <= eff * 1.25
                  and rep["max_corner_error_wu"] > 0.05)
            ce = ra["facet_crease_energy"]
            n_enf = ra["extrema"].get("enforced", ra["extrema"]["count"])
            extra = (f" corr={ra['landform_correlation']:.4f}"
                     f" extrema_viol={ra['extrema']['violations']}/{n_enf}"
                     f" crease {ce['bilinear_base']:.2f}->{ce['smoothed']:.2f}"
                     f" drift mean={ra['drift_vs_bilinear_wu']['mean']:.2f}"
                     f"/p99={ra['drift_vs_bilinear_wu']['p99']:.1f}wu"
                     f" pyr {ra['pyramid_top_score_before']:.2f}->{ra['pyramid_top_score_after']:.2f}")
            if ra.get("cliff"):
                ci = ra["cliff"]
                dcl = ci.get("drift_on_cliff_wu") or {}
                extra += (f" cliff[{100*ci['coarse_cliff_frac']:.1f}%"
                          f" relaxed={ci['relaxed_extrema']}"
                          f" drift_p99={dcl.get('p99', 0.0):.1f}wu]")
            if ra.get("mountainify"):
                mi = ra["mountainify"]
                extra += (f" mtn[seed={mi['seed']} rock={100*mi['rock_area_frac']:.0f}%"
                          f" detail_rms={mi['detail_rms_on_rock_wu']:.1f}wu]")
        else:
            cap = (0.0 if not args.reshape else args.corner_clamp) + 1e-4
            ok = rep["max_corner_error_wu"] <= cap
            if rep.get("reshape"):
                ri = rep["reshape"]
                extra = (f" pyr_score {ri['pyramid_top_score_before']:.2f}->{ri['pyramid_top_score_after']:.2f}"
                         f" corner_move<={ri['max_corner_move_wu']:.1f}wu")
        # TERRAIN-LOD-GEOMORPH-1: mip max-lift per stride (all modes)
        if rep.get("max_mips"):
            lifts = ["%d:%.0f" % (lv["stride"], lv["max_lift_wu"])
                     for lv in rep["max_mips"]["levels"]]
            extra += " mips max_lift[" + " ".join(lifts) + "]wu"
        print(f"[visual-bake] {m}: coarse={rep['coarse_side']} -> visual={rep['visual_side']} "
              f"(x{rep['factor']}) corner_err={rep['max_corner_error_wu']:.3g} "
              f"{'PASS' if ok else 'FAIL'}  delta_max={rep['visual_delta_wu']['max_abs']:.2f}wu "
              f"delta_mean={rep['visual_delta_wu']['mean_abs']:.2f}wu{extra} "
              f"-> {out_root / (m + '.beauty')}")
        if not ok:
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
