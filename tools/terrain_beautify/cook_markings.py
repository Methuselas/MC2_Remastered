#!/usr/bin/env python3
"""ROAD-MARKINGS-VECTOR-1 + WHOLESALE-VECTORIZE-1: cook road/runway markings
AND parametric road-surface ribbons into the overlay-V2 sidecar
(`data/missions/<stem>.beauty/overlay_v2.png`).

Recon: `.claude/ROAD-MARKINGS-VECTOR-RECON-1.md`. Locked rulings:
  - Carrier = the EXISTING overlay-V2 sidecar (`overlay_extract.py` /
    `mclib/terrain.cpp` gate `MC2_TERRAIN_OVERLAY_V2`) -- markings are
    COMPOSED INTO `overlay_v2.png` at cook time, ZERO engine change.
  - Authoring = parametric glyph DSL (`marking_glyphs.py`), NOT raster
    upscale of the old 64x64 tile art and NOT SVG import (deferred).
  - Bridges/gates stay on the legacy overlay pass (runtime-mutable, recon
    landmine: never statically baked; the water-adjacency exclusion below).

WHOLESALE-VECTORIZE-1 extensions over the original slice:
  1. CEMENT-REGION classification (mc2_01 extraction-gap fix): the original
     extract scanned ONLY overlay-tagged cells, but missions like mc2_01
     build their runway/pads out of CEMENT terrain-type cells (the same
     `_CONCRETE` terrainType set `overlay_extract.py` rasterizes). Those
     regions are now classified too: PCA (second-moment) elongation of the
     distance-transform core catches DIAGONAL strips that a bbox aspect
     test cannot (mc2_01's runway is a ~-140 deg strip whose bbox aspect is
     1.06 but whose core PCA elongation is ~3.5) -> runway markings placed
     along the principal axis at arbitrary rotation. Compact cement blobs
     classify as PADS and optionally (--pad-edges) get perimeter striping.
  2. RUNWAY marking proportions are now AVIATION-STYLE FRACTIONS of the
     measured runway width (centerline/edge/threshold/numeral sizes scale
     with the strip; numerals derive from the strip's compass heading), so
     they stay visible at the full-map sidecar resolution instead of the
     old fixed-wu sizes that clamped sub-pixel.
  3. ROAD SURFACE ribbons: extract now emits `road_chains` (ordered world
     polylines traced from the overlay-tagged road cell mask -- the mask IS
     the skeleton at 1 cell wide; longest-path decomposition + Douglas-
     Peucker) and compose draws a parametric ribbon per chain: asphalt/
     gravel fill (world-XY tiled from a TGA albedo if provided, else flat
     parametric shading + deterministic noise), darker worn edge band, and
     centerline dashes. This visually replaces the legacy road tile art
     when MC2_TERRAIN_OVERLAY_V2=1 (the sidecar composites into the terrain
     chunk frag; bridges remain on the untouched legacy pass).

Subcommands:
  extract  --pak <mission.pak> --out <markings.json> [--pad-edges]
           [--road-width-wu N]
      Scans BOTH the mission's overlay-tagged cells (`textureData` hi-16)
      AND its cement/concrete terrain-type cells, and AUTO-GENERATES a
      markings.json placement + road-chain list. This is a FIDELITY-LIMITED
      auto-generate (documented, not a hidden approximation): it reproduces
      "there is a marked road/runway here, oriented along its run" but does
      NOT reproduce the exact legacy per-tile pixel art. The legacy
      per-family tile-orientation index is NOT content-derivable offline
      (needs runtime-assigned baseTXMIndex), so road surface family
      (dirt vs paved) is also not decodable -- all chains default to
      "asphalt" (override by hand-editing markings.json). Runway numerals
      are derived from the strip's compass heading (real designator
      convention), not authored text. Bridge/gate-likely cells are excluded
      via the water-adjacency heuristic (runtime-mutable, never baked).
  compose  --markings <json> --overlay <overlay_v2.png> [--bounds <file>]
           --out <png> [--px-per-wu N] [--asphalt-tga F] [--gravel-tga F]
           [--texture-repeat-wu N]
      Draws road ribbons first (fill + worn edge + centerline dashes), then
      rasterizes every glyph placement, alpha-composited OVER a copy of
      overlay_v2.png (resized to the marking canvas resolution if needed),
      and writes the combined sidecar PNG + companion bounds file.
  install  --png <file> --deploy <dir> --mission <stem>
      Copies the composed PNG (+ bounds file) to
      <deploy>/data/missions/<stem>.beauty/overlay_v2.png -- SAME contract
      as overlay_extract.py's install (this tool does not reinvent it).

PIL + numpy + scipy only, offline, read-only w.r.t. .pak files. No engine
launch. Deterministic (seeded RNG for wear noise; sorted traversals).
"""
from __future__ import annotations

import argparse
import json
import math
import shutil
import sys
from collections import deque
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mission_terrain_analyzer import (  # noqa: E402
    read_packets, locate_mapdata, extract_layers, read_water_elevation,
)
from overlay_extract import (  # noqa: E402  (same _CONCRETE set as the engine mirror)
    classify_concrete, read_bridge_gate_overlay_cells,
)
import marking_glyphs as mg  # noqa: E402

# --- bridge/gate exclusion ----------------------------------------------------
# Root fix landed in `overlay_extract.py::read_bridge_gate_overlay_cells`
# (WHOLESALE-VECTORIZE-1 flagged the bug there: its old body was IDENTICAL to
# "any overlay-tagged cell", which would have wrongly excluded 100% of
# road/runway cells if reused here -- verified empirically: mc2_24 has 790
# overlay-tagged cells and the old helper flagged all 790 as "bridge/gate").
# That helper now implements the same WATER-ADJACENCY heuristic this module
# used locally (bridges physically cross water; real bridges/gates aren't
# decodable from the overlay tile family offline -- the overlay_hi -> family
# decode needs TerrainTextures::baseTXMIndex, a runtime texture-manager-
# assigned index, not content-derivable from the pak alone), so the local
# copy here is retired in favour of the shared, now-correct implementation.
# Empirically small (0-67 cells across all 24 stock missions, checked
# against every mission in Carver5-feasibility).

WORLD_UNITS_PER_VERTEX = 128.0

# --- shape classifier thresholds (auto-extract) ------------------------------
# Overlay-tile runway (mc2_24 style: runway#### tile family painted as overlay)
RUNWAY_ASPECT_MIN = 2.5   # long/short bbox ratio
RUNWAY_DENSITY_MIN = 0.30  # filled_cells / bbox_area
RUNWAY_MIN_LONG_WU = 10 * WORLD_UNITS_PER_VERTEX  # ~10 cells minimum length
RUNWAY_MIN_COUNT = 15      # minimum cell count to call a component "runway"

# Cement-region runway (mc2_01 style: concrete terrain-type strip). A bbox
# aspect test FAILS on diagonal strips (mc2_01: bbox aspect 1.06), so the
# classifier uses PCA elongation of the distance-transform CORE (cells >=
# CEMENT_CORE_DIST from the region edge): the core strips off pads/aprons
# hugging the strip and the second-moment axis is rotation-invariant.
CEMENT_MIN_COUNT = 16          # ignore tiny concrete blobs
CEMENT_CORE_DIST = 2           # chessboard distance defining the core
CEMENT_RUNWAY_ELONG_MIN = 2.0  # sqrt(major/minor eigenvalue) on the core
ROAD_CHAIN_MIN_CELLS = 3       # shortest overlay cell run worth a ribbon
ROAD_RIBBON_WIDTH_WU = 90.0    # default ribbon width (legacy art ~fills a 128wu cell)
ROAD_RIBBON_EDGE_WU = 10.0     # worn edge band width per side
_DP_EPSILON_WU = 0.6 * WORLD_UNITS_PER_VERTEX  # Douglas-Peucker tolerance


# --- geometry helpers ---------------------------------------------------------
def _tile_center_world(row: float, col: float, side: int) -> tuple[float, float]:
    """Cell (row,col) -> world XY of its centre, inverting Terrain::worldToTile
    (terrain.h:454-458): tileC=(x-topLeftX)/128, tileR=(topLeftY-y)/128;
    topLeftX=-halfMap, topLeftY=+halfMap (same convention as
    overlay_extract.py / control_map_tool.py, row0=top=north, no flip)."""
    half_map = (side * WORLD_UNITS_PER_VERTEX) * 0.5
    x = (col + 0.5) * WORLD_UNITS_PER_VERTEX - half_map
    y = half_map - (row + 0.5) * WORLD_UNITS_PER_VERTEX
    return x, y


def _component_shape(rows: np.ndarray, cols: np.ndarray) -> dict:
    r0, r1 = int(rows.min()), int(rows.max())
    c0, c1 = int(cols.min()), int(cols.max())
    h = r1 - r0 + 1
    w = c1 - c0 + 1
    cnt = len(rows)
    long_dim, short_dim = max(h, w), max(1, min(h, w))
    return {
        "bbox": (r0, r1, c0, c1), "h": h, "w": w, "count": cnt,
        "aspect": long_dim / short_dim,
        "density": cnt / (h * w),
        "vertical": h >= w,  # long axis is rows (north-south) vs cols (east-west)
    }


def _pca_strip(rows: np.ndarray, cols: np.ndarray) -> dict:
    """Second-moment (PCA) fit of a cell mask in the world-XY convention
    (x=col, y=-row so +Y is north). Returns the principal-axis angle (deg CCW
    from +X/east), elongation sqrt(major/minor eigenvalue), grid centroid,
    and ROBUST extents (percentile-trimmed so pads jutting off a runway
    strip don't inflate the measured length/width)."""
    pts = np.stack([cols.astype(np.float64), -rows.astype(np.float64)], axis=1)
    mean = pts.mean(axis=0)
    cen = pts - mean
    cov = cen.T @ cen / max(1, len(pts))
    evals, evecs = np.linalg.eigh(cov)  # ascending; [:,1] = major axis
    major = evecs[:, 1]
    minor = evecs[:, 0]
    elong = math.sqrt(max(evals[1], 1e-9) / max(evals[0], 1e-9))
    angle = math.degrees(math.atan2(major[1], major[0]))
    if angle <= -90.0:
        angle += 180.0
    elif angle > 90.0:
        angle -= 180.0
    proj_maj = cen @ major
    proj_min = cen @ minor
    return {
        "angle_deg": angle,
        "elong": elong,
        "centroid_rc": (-mean[1], mean[0]),  # (row, col), fractional
        "len_cells": float(np.percentile(proj_maj, 98) - np.percentile(proj_maj, 2) + 1.0),
        "width_cells": float(np.percentile(proj_min, 90) - np.percentile(proj_min, 10) + 1.0),
    }


def _measure_strip(rows: np.ndarray, cols: np.ndarray, side: int,
                   core_rows: np.ndarray | None = None,
                   core_cols: np.ndarray | None = None) -> dict:
    """Full strip measurement for marking placement: orientation from the
    CORE cells (if given -- cleaner axis), extents from ALL cells projected
    onto that axis (robust percentiles)."""
    axis_src = _pca_strip(core_rows, core_cols) if core_rows is not None and len(core_rows) else _pca_strip(rows, cols)
    ang = math.radians(axis_src["angle_deg"])
    u = np.array([math.cos(ang), math.sin(ang)])
    v = np.array([-math.sin(ang), math.cos(ang)])
    pts = np.stack([cols.astype(np.float64), -rows.astype(np.float64)], axis=1)
    mean = pts.mean(axis=0)
    cen = pts - mean
    proj_u = cen @ u
    proj_v = cen @ v
    cr, cc = -mean[1], mean[0]
    wx, wy = _tile_center_world(cr, cc, side)
    return {
        "angle_deg": axis_src["angle_deg"],
        "elong": axis_src["elong"],
        "world_cx": wx, "world_cy": wy,
        "length_wu": float((np.percentile(proj_u, 98) - np.percentile(proj_u, 2) + 1.0) * WORLD_UNITS_PER_VERTEX),
        "width_wu": float((np.percentile(proj_v, 90) - np.percentile(proj_v, 10) + 1.0) * WORLD_UNITS_PER_VERTEX),
    }


# --- overlay-tile component classification (roads + tile-family runways) ------
def _classify_components(tagged: np.ndarray) -> list[dict]:
    """Runway pads are WIDE (multiple cells across) and DENSELY filled;
    dirt/paved roads are almost always a single-cell-wide chain. A plain
    8-connected labeling of the raw tagged mask merges roads that happen to
    touch/cross a runway into one giant low-aspect blob (verified on
    mc2_24: 789 of 790 tagged cells form ONE component via 8-connectivity,
    aspect ~1.0 -- useless for shape classification). Instead:
      1. Morphological OPENING with a 3x3 structuring element erodes away
         anything only 1 cell wide (ordinary roads) while preserving wide
         filled regions (runway pads) -- a standard width filter.
      2. Label the OPENED mask -> these components are runway candidates;
         re-validate each against the aspect/density/length thresholds
         (opening can occasionally leave a wide road junction, so the
         thresholds still apply, just against a much cleaner input).
      3. Subtract the runway components' cells from `tagged`, then label
         WHAT'S LEFT (still road cells, now safely separable from the
         runway) as separate road components.
    """
    struct3 = np.ones((3, 3), dtype=bool)
    opened = ndimage.binary_opening(tagged, structure=struct3)
    comps: list[dict] = []
    runway_mask = np.zeros_like(tagged)

    if opened.any():
        lbl_open, ncomp_open = ndimage.label(opened, structure=np.ones((3, 3)))
        for i in range(1, ncomp_open + 1):
            comp_mask = lbl_open == i
            rows, cols = np.where(comp_mask)
            shape = _component_shape(rows, cols)
            long_wu = max(shape["h"], shape["w"]) * WORLD_UNITS_PER_VERTEX
            is_runway = (shape["aspect"] >= RUNWAY_ASPECT_MIN and
                         shape["density"] >= RUNWAY_DENSITY_MIN and
                         long_wu >= RUNWAY_MIN_LONG_WU and
                         shape["count"] >= RUNWAY_MIN_COUNT)
            if is_runway:
                shape["kind"] = "runway"
                shape["rows"] = rows
                shape["cols"] = cols
                comps.append(shape)
                runway_mask |= comp_mask

    remaining = tagged & ~runway_mask
    if remaining.any():
        lbl_road, ncomp_road = ndimage.label(remaining, structure=np.ones((3, 3)))
        for i in range(1, ncomp_road + 1):
            rows, cols = np.where(lbl_road == i)
            shape = _component_shape(rows, cols)
            shape["kind"] = "road"
            shape["rows"] = rows
            shape["cols"] = cols
            comps.append(shape)

    return comps


# --- cement-region classification (WHOLESALE-VECTORIZE-1, mc2_01 gap fix) -----
def _cement_components(concrete: np.ndarray, side: int) -> list[dict]:
    """Classify concrete TERRAIN-TYPE regions (a different system from the
    overlay tile grid -- mc2_01's runway/pads live here, invisible to the
    overlay scan). Per 8-connected component:
      - distance-transform core (chessboard >= CEMENT_CORE_DIST) strips the
        1-2 cell fringe + attached aprons; PCA on the core gives a
        rotation-invariant axis + elongation (bbox aspect fails on diagonal
        strips: mc2_01 bbox aspect 1.06 vs core PCA elongation ~3.5).
      - elongated long core -> cement_runway (full marking set along axis);
        everything else -> cement_pad (optional perimeter striping).
    """
    out: list[dict] = []
    if not concrete.any():
        return out
    lbl, ncomp = ndimage.label(concrete, structure=np.ones((3, 3)))
    for i in range(1, ncomp + 1):
        comp_mask = lbl == i
        rows, cols = np.where(comp_mask)
        if len(rows) < CEMENT_MIN_COUNT:
            continue
        dist = ndimage.distance_transform_cdt(comp_mask, metric="chessboard")
        core = dist >= CEMENT_CORE_DIST
        crows, ccols = np.where(core)
        if len(crows) >= 4:
            # Orientation + CENTRE + WIDTH from the CORE only: aprons/pads
            # hugging the strip (mc2_01's diamond bulge) inflate a full-
            # component width badly (1067wu vs the ~750wu actual strip) and
            # drag the centroid off-strip. Width adds back the core margin;
            # LENGTH comes from the full component (the strip's ends thin
            # below the core threshold), capped at core length + margin.
            core_meas = _measure_strip(crows, ccols, side, crows, ccols)
            full_meas = _measure_strip(rows, cols, side, crows, ccols)
            margin = CEMENT_CORE_DIST * WORLD_UNITS_PER_VERTEX
            meas = dict(core_meas)
            meas["width_wu"] = core_meas["width_wu"] + margin
            meas["length_wu"] = min(full_meas["length_wu"],
                                    core_meas["length_wu"] + 2.0 * margin)
            # taper refinement: keep markings on the run where the strip
            # holds >= half its median cross-width (ends thin to slivers)
            ref_len, ref_cx, ref_cy = _axis_length_refine(
                rows, cols, core_meas["angle_deg"],
                core_meas["world_cx"], core_meas["world_cy"], side)
            if ref_len > 0:
                meas["length_wu"] = min(meas["length_wu"], ref_len)
                meas["world_cx"], meas["world_cy"] = ref_cx, ref_cy
            core_elong = core_meas["elong"]
        else:
            meas = _measure_strip(rows, cols, side)
            core_elong = meas["elong"]
        is_runway = (core_elong >= CEMENT_RUNWAY_ELONG_MIN and
                     meas["length_wu"] >= RUNWAY_MIN_LONG_WU)
        meas["kind"] = "cement_runway" if is_runway else "cement_pad"
        meas["count"] = len(rows)
        meas["core_elong"] = core_elong
        out.append(meas)
    return out


def _axis_length_refine(rows: np.ndarray, cols: np.ndarray, angle_deg: float,
                        world_cx: float, world_cy: float, side: int) -> tuple[float, float, float]:
    """Refine a strip's usable marking LENGTH to the contiguous run where the
    strip keeps >= half its median cross-width. Cement strips taper to 1-2
    cells at the ends (mc2_01's diamond) -- placing threshold bars/numerals
    at the raw percentile extent puts them on slivers. Returns
    (length_wu, refined_world_cx, refined_world_cy)."""
    ang = math.radians(angle_deg)
    u = np.array([math.cos(ang), math.sin(ang)])
    pts = np.stack([cols.astype(np.float64), -rows.astype(np.float64)], axis=1)
    # project about the given centre expressed back in grid space
    half_map = (side * WORLD_UNITS_PER_VERTEX) * 0.5
    cen_col = (world_cx + half_map) / WORLD_UNITS_PER_VERTEX - 0.5
    cen_row = (half_map - world_cy) / WORLD_UNITS_PER_VERTEX - 0.5
    cen = pts - np.array([cen_col, -cen_row])
    proj_u = cen @ u
    bins = np.round(proj_u).astype(np.int64)
    uniq, counts = np.unique(bins, return_counts=True)
    med = float(np.median(counts))
    good = counts >= max(2.0, 0.5 * med)
    # longest contiguous run of good bins (uniq is sorted; holes break runs)
    best_lo = best_hi = None
    run_lo = None
    prev_b = None
    for b, g in zip(uniq.tolist(), good.tolist()):
        if g:
            if run_lo is None or (prev_b is not None and b != prev_b + 1):
                run_lo = b
            if best_lo is None or (b - run_lo) > (best_hi - best_lo):
                best_lo, best_hi = run_lo, b
        else:
            run_lo = None
        prev_b = b
    if best_lo is None:
        return 0.0, world_cx, world_cy
    length_wu = (best_hi - best_lo + 1) * WORLD_UNITS_PER_VERTEX
    mid_u = (best_hi + best_lo) / 2.0 * WORLD_UNITS_PER_VERTEX
    return (length_wu,
            world_cx + u[0] * mid_u,
            world_cy + u[1] * mid_u)


# --- runway marking placement (shared: overlay-tile + cement runways) ---------
def _runway_headings(angle_deg: float) -> tuple[str, str]:
    """Compass-heading runway designators for a strip whose principal axis
    points `angle_deg` CCW from east (+Y north). Travelling along +axis has
    compass heading (90 - angle); the numeral painted at the -axis end is
    that heading /10 (aircraft landing FROM the -axis end travel +axis)."""
    h1 = (90.0 - angle_deg) % 360.0
    h2 = (h1 + 180.0) % 360.0

    def num(h: float) -> str:
        n = int(round(h / 10.0)) % 36
        if n == 0:
            n = 36
        return f"{n:02d}"
    return num(h1), num(h2)


def _runway_marking_placements(world_cx: float, world_cy: float, angle_deg: float,
                               length_wu: float, width_wu: float) -> list[dict]:
    """Full runway marking set along an ARBITRARY axis. All feature sizes are
    aviation-style FRACTIONS of the measured runway width (a real 45m runway
    paints ~0.9m lines, 9m numerals, 30m threshold bars => ~0.02W / 0.2W /
    0.67W), floored so they survive the full-map sidecar resolution."""
    ang = math.radians(angle_deg)
    u = (math.cos(ang), math.sin(ang))          # along-runway unit vector
    v = (-math.sin(ang), math.cos(ang))         # across-runway unit vector
    W, L = width_wu, length_wu

    def at(du: float, dv: float) -> tuple[float, float]:
        return (world_cx + u[0] * du + v[0] * dv,
                world_cy + u[1] * du + v[1] * dv)

    line_w = max(0.025 * W, 4.0)
    placements: list[dict] = []
    placements.append({
        "glyph": "runway_centerline_dashes", "x": world_cx, "y": world_cy,
        "rotation_deg": angle_deg, "scale": 1.0, "color": "white",
        "params": {"length_wu": L * 0.78, "dash_wu": W * 0.9, "gap_wu": W * 0.55,
                   "width_wu": line_w},
    })
    for side_sign in (-1.0, 1.0):
        ex, ey = at(0.0, side_sign * (W / 2.0 - W * 0.08))
        placements.append({
            "glyph": "runway_edge_stripes", "x": ex, "y": ey,
            "rotation_deg": angle_deg, "scale": 1.0, "color": "white",
            "params": {"length_wu": L * 0.94, "width_wu": max(0.02 * W, 4.0)},
        })
    # threshold bars + heading-derived numeral at each end; aiming-point pair
    # flanking the touchdown zones.
    numerals = _runway_headings(angle_deg)
    for end_sign, numeral in zip((-1.0, 1.0), numerals):
        tx, ty = at(end_sign * (L / 2.0 - L * 0.05), 0.0)
        placements.append({
            "glyph": "runway_threshold_bars", "x": tx, "y": ty,
            "rotation_deg": angle_deg, "scale": 1.0, "color": "white",
            "params": {"runway_width_wu": W * 0.8, "num_bars": 8,
                       "bar_length_wu": W * 0.5, "bar_width_wu": W * 0.045,
                       "gap_wu": W * 0.055},
        })
        nx, ny = at(end_sign * (L / 2.0 - L * 0.14), 0.0)
        # numeral reads for the aircraft travelling TOWARD the centre from
        # this end: text-up = -end_sign * u  =>  rotate (angle -/+ 90).
        placements.append({
            "glyph": "runway_numeral", "x": nx, "y": ny,
            "rotation_deg": angle_deg - end_sign * 90.0, "scale": 1.0, "color": "white",
            "params": {"text": numeral, "height_wu": W * 0.28},
        })
        ax, ay = at(end_sign * (L / 2.0 - L * 0.26), 0.0)
        placements.append({
            "glyph": "runway_aiming_point", "x": ax, "y": ay,
            "rotation_deg": angle_deg, "scale": 1.0, "color": "white",
            "params": {"block_length_wu": W * 0.8, "block_width_wu": W * 0.12,
                       "gap_wu": W * 0.4},
        })
    return placements


def _pad_edge_placements(meas: dict) -> list[dict]:
    """Optional (--pad-edges) perimeter striping for a compact cement pad:
    four edge stripes along the PCA-aligned robust-extent rectangle."""
    ang = math.radians(meas["angle_deg"])
    u = (math.cos(ang), math.sin(ang))
    v = (-math.sin(ang), math.cos(ang))
    L, W = meas["length_wu"], meas["width_wu"]
    cx, cy = meas["world_cx"], meas["world_cy"]
    stripe_w = max(0.02 * min(L, W), 4.0)
    inset = 0.06
    placements = []
    for sgn in (-1.0, 1.0):
        # along-length stripes (long edges)
        ex = cx + v[0] * sgn * (W / 2.0 - W * inset)
        ey = cy + v[1] * sgn * (W / 2.0 - W * inset)
        placements.append({
            "glyph": "lane_edge_line", "x": ex, "y": ey,
            "rotation_deg": meas["angle_deg"], "scale": 1.0, "color": "yellow",
            "params": {"length_wu": L * 0.88, "width_wu": stripe_w},
        })
        # across stripes (short edges)
        fx = cx + u[0] * sgn * (L / 2.0 - L * inset)
        fy = cy + u[1] * sgn * (L / 2.0 - L * inset)
        placements.append({
            "glyph": "lane_edge_line", "x": fx, "y": fy,
            "rotation_deg": meas["angle_deg"] + 90.0, "scale": 1.0, "color": "yellow",
            "params": {"length_wu": W * 0.88, "width_wu": stripe_w},
        })
    return placements


# --- road chain tracing (WHOLESALE-VECTORIZE-1: ribbon skeleton) ---------------
def _trace_chains(rows: np.ndarray, cols: np.ndarray) -> list[list[tuple[int, int]]]:
    """Order a road component's cells into polyline chains. Stock MC2 roads
    are ~1 cell wide, so the cell mask IS the skeleton; branching (T/X
    junctions) is handled by LONGEST-PATH DECOMPOSITION: repeatedly extract
    the longest shortest-path through the remaining cells (double-BFS
    diameter), remove it, and re-trace what's left as branch chains. A
    branch whose endpoint touches an already-extracted trunk cell gets that
    trunk cell prepended/appended so ribbons visually connect at junctions.
    Fully deterministic: sorted traversal order everywhere."""
    all_cells = set(zip(rows.tolist(), cols.tolist()))
    remaining = set(all_cells)
    chains: list[list[tuple[int, int]]] = []

    def neighbors(cell: tuple[int, int], pool: set) -> list[tuple[int, int]]:
        r, c = cell
        out = []
        for dr in (-1, 0, 1):
            for dc in (-1, 0, 1):
                if dr == 0 and dc == 0:
                    continue
                n = (r + dr, c + dc)
                if n in pool:
                    out.append(n)
        return sorted(out)

    def flood(start: tuple[int, int], pool: set) -> set:
        seen = {start}
        q = deque([start])
        while q:
            cur = q.popleft()
            for n in neighbors(cur, pool):
                if n not in seen:
                    seen.add(n)
                    q.append(n)
        return seen

    def bfs_far(start: tuple[int, int], pool: set):
        prev = {start: None}
        dist = {start: 0}
        q = deque([start])
        far, far_d = start, 0
        while q:
            cur = q.popleft()
            for n in neighbors(cur, pool):
                if n not in prev:
                    prev[n] = cur
                    dist[n] = dist[cur] + 1
                    q.append(n)
                    if dist[n] > far_d or (dist[n] == far_d and n < far):
                        far, far_d = n, dist[n]
        return far, prev

    while True:
        candidates = sorted(remaining)
        if not candidates:
            break
        comp = flood(candidates[0], remaining)
        if len(comp) < ROAD_CHAIN_MIN_CELLS:
            remaining -= comp
            continue
        a, _ = bfs_far(min(comp), comp)
        b, prev = bfs_far(a, comp)
        path = []
        cur: tuple[int, int] | None = b
        while cur is not None:
            path.append(cur)
            cur = prev[cur]
        # connect branch endpoints to an already-extracted trunk cell
        used = all_cells - remaining
        for endidx in (0, len(path) - 1):
            adj = neighbors(path[endidx], used)
            if adj:
                if endidx == 0:
                    path.insert(0, adj[0])
                else:
                    path.append(adj[0])
        chains.append(path)
        remaining -= set(path)
    return chains


def _dp_simplify(points: list[tuple[float, float]], eps: float) -> list[tuple[float, float]]:
    """Douglas-Peucker polyline simplification (iterative, deterministic)."""
    if len(points) < 3:
        return list(points)
    keep = [False] * len(points)
    keep[0] = keep[-1] = True
    stack = [(0, len(points) - 1)]
    while stack:
        i0, i1 = stack.pop()
        if i1 <= i0 + 1:
            continue
        x0, y0 = points[i0]
        x1, y1 = points[i1]
        dx, dy = x1 - x0, y1 - y0
        seg_len = math.hypot(dx, dy)
        best_d, best_i = -1.0, -1
        for i in range(i0 + 1, i1):
            px, py = points[i]
            if seg_len < 1e-9:
                d = math.hypot(px - x0, py - y0)
            else:
                d = abs(dx * (y0 - py) - dy * (x0 - px)) / seg_len
            if d > best_d:
                best_d, best_i = d, i
        if best_d > eps:
            keep[best_i] = True
            stack.append((i0, best_i))
            stack.append((best_i, i1))
    return [p for p, k in zip(points, keep) if k]


def _road_chain_records(shape: dict, side: int, width_wu: float) -> list[dict]:
    """Road component -> list of ribbon chain records (world polylines)."""
    records = []
    for cell_path in _trace_chains(shape["rows"], shape["cols"]):
        pts = [_tile_center_world(r, c, side) for (r, c) in cell_path]
        pts = _dp_simplify(pts, _DP_EPSILON_WU)
        if len(pts) < 2:
            continue
        records.append({
            "points": [[round(x, 2), round(y, 2)] for (x, y) in pts],
            "width_wu": width_wu,
            "surface": "asphalt",   # family (dirt vs paved) is NOT decodable
                                    # offline (needs runtime baseTXMIndex);
                                    # hand-edit to "gravel" per chain if desired
            "centerline": True,
        })
    return records


# --- extract ------------------------------------------------------------------
def cmd_extract(args) -> int:
    pak = Path(args.pak)
    if not pak.is_file():
        print(f"[extract] ERROR pak not found: {pak}", file=sys.stderr)
        return 4
    packets = read_packets(pak)
    md = locate_mapdata(packets)
    if md is None:
        print(f"[extract] ERROR no MapData packet matched signature in {pak}", file=sys.stderr)
        return 4
    pkt_idx, side, blocks = md

    n = side * side
    PCV_SIZE = 32
    OFF_TEXDATA = 16
    arr = np.frombuffer(blocks, dtype=np.uint8, count=n * PCV_SIZE).reshape(n, PCV_SIZE)
    texdata = arr[:, OFF_TEXDATA:OFF_TEXDATA + 4].copy().view(np.uint32).reshape(side, side)
    overlay_hi = (texdata >> 16) & 0xFFFF
    tagged = (overlay_hi != 0) & (overlay_hi != 0xFFFF)

    water_elev = read_water_elevation(pak.with_suffix(".fit"))
    bridge_gate = read_bridge_gate_overlay_cells(side, blocks, water_elev)
    excluded = int((tagged & bridge_gate).sum())
    tagged = tagged & ~bridge_gate  # never bake bridge/gate overlay cells (runtime-mutable)

    # cement/concrete terrain-type regions (mc2_01 gap: runway/pads that the
    # overlay scan cannot see -- different system, same classifier set as
    # overlay_extract.py / the engine's terrainTypeToMaterial mirror).
    layers = extract_layers(side, blocks, water_elev)
    concrete = classify_concrete(layers["terrtype"])
    cement_comps = _cement_components(concrete, side)

    comps = _classify_components(tagged)
    placements: list[dict] = []
    road_chains: list[dict] = []
    runway_rects: list[dict] = []

    def _emit_runway(meas: dict, emit_rect: bool) -> None:
        placements.extend(_runway_marking_placements(
            meas["world_cx"], meas["world_cy"], meas["angle_deg"],
            meas["length_wu"], meas["width_wu"]))
        # pavement footprint rect: compose clips marking paint to the union
        # of the base sidecar alpha + road ribbons + these rects. Rects are
        # emitted ONLY for overlay-TILE runways (mc2_24 style) whose pavement
        # is NOT in the base cement raster; cement runways clip to the true
        # cement alpha instead (a rect would spill onto bare terrain where
        # the strip tapers).
        if emit_rect:
            runway_rects.append({
                "cx": round(float(meas["world_cx"]), 2),
                "cy": round(float(meas["world_cy"]), 2),
                "angle_deg": round(float(meas["angle_deg"]), 2),
                "length_wu": round(float(meas["length_wu"]), 2),
                "width_wu": round(float(meas["width_wu"]), 2),
            })

    n_runway = n_road = 0
    road_width = float(args.road_width_wu)
    for shape in comps:
        if shape["count"] < 2:
            continue  # single stray tagged cell, not a marked run
        if shape["kind"] == "runway":
            _emit_runway(_measure_strip(shape["rows"], shape["cols"], side), emit_rect=True)
            n_runway += 1
        else:
            road_chains.extend(_road_chain_records(shape, side, road_width))
            n_road += 1

    n_cem_runway = n_cem_pad = 0
    for meas in cement_comps:
        if meas["kind"] == "cement_runway":
            _emit_runway(meas, emit_rect=False)
            n_cem_runway += 1
        else:
            if args.pad_edges:
                placements.extend(_pad_edge_placements(meas))
            n_cem_pad += 1

    doc = {"mission": pak.stem, "side": side, "glyphs": placements,
           "road_chains": road_chains, "runway_rects": runway_rects}
    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(doc, indent=2, sort_keys=False))

    print(f"[extract] {pak.stem}: side={side} overlay_tagged_cells={int(tagged.sum())} "
          f"bridge_gate_excluded={excluded} components={len(comps)} "
          f"runway_components={n_runway} road_components={n_road} "
          f"cement_runway_components={n_cem_runway} cement_pad_components={n_cem_pad} "
          f"road_chains={len(road_chains)} glyph_placements={len(placements)} -> {out_path}")
    return 0


# --- compose ------------------------------------------------------------------
def _load_bounds(bounds_path: Path | None, overlay_path: Path) -> tuple[float, float, float, float]:
    if bounds_path is None:
        bounds_path = overlay_path.with_suffix("").with_name(
            overlay_path.with_suffix("").name + ".bounds.txt")
    if bounds_path.is_file():
        parts = bounds_path.read_text().split()
        if len(parts) == 4:
            return tuple(float(p) for p in parts)  # type: ignore[return-value]
    # engine default: full map extent is unknown offline without a mission
    # reference, so compose requires an explicit/companion bounds file for a
    # correct world->pixel mapping; caller must supply one (validate below).
    raise ValueError(f"compose: no usable bounds file found (looked for {bounds_path}); "
                     f"pass --bounds explicitly or ensure it sits alongside --overlay")


# flat parametric surface colours (used when no albedo TGA is supplied)
_SURFACE_FLAT_RGB = {
    "asphalt": (52, 52, 56),
    "gravel": (108, 98, 86),
}
_CENTERLINE_RGBA = (255, 214, 0, 255)  # yellow (marking_glyphs.YELLOW)


def _load_albedo(path: str | None) -> np.ndarray | None:
    if not path:
        return None
    p = Path(path)
    if not p.is_file():
        print(f"[compose] WARN albedo tga not found, falling back to flat shading: {p}")
        return None
    return np.array(Image.open(p).convert("RGB"), dtype=np.uint8)


def _dash_runs(pts: list[tuple[float, float]], dash_wu: float, gap_wu: float):
    """Walk a world polyline by arc length, yielding (p0, p1) world sub-
    segments for centerline dashes."""
    period = dash_wu + gap_wu
    runs = []
    dist_into = 0.0  # arc-length position within the current period
    for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
        seg_len = math.hypot(x1 - x0, y1 - y0)
        if seg_len < 1e-9:
            continue
        ux, uy = (x1 - x0) / seg_len, (y1 - y0) / seg_len
        t = 0.0
        while t < seg_len:
            pos = dist_into % period
            if pos < dash_wu:  # inside a dash
                run_len = min(dash_wu - pos, seg_len - t)
                runs.append(((x0 + ux * t, y0 + uy * t),
                             (x0 + ux * (t + run_len), y0 + uy * (t + run_len))))
            else:              # inside a gap
                run_len = min(period - pos, seg_len - t)
            if run_len < 1e-9:  # fp stall guard at exact dash/gap boundaries
                run_len = 1e-9
            t += run_len
            dist_into += run_len
    return runs


def _draw_ribbons(canvas: Image.Image, chains: list[dict],
                  world_to_px, px_per_wu: float,
                  bounds: tuple[float, float, float, float],
                  asphalt: np.ndarray | None, gravel: np.ndarray | None,
                  texture_repeat_wu: float) -> tuple[int, Image.Image | None]:
    """Draw parametric road ribbons (surface fill + worn edge band +
    centerline dashes) for every chain, compositing into `canvas` in place.
    Deterministic: seeded RNG for edge-wear noise. Returns (n_chains,
    fill-mask image) so the caller can include ribbons in the marking clip
    footprint."""
    if not chains:
        return 0, None
    top_left_x, top_left_y, size_x, size_y = bounds
    cw, ch = canvas.size
    rng = np.random.default_rng(0xC00C)

    fill_mask = Image.new("L", (cw, ch), 0)
    outer_mask = Image.new("L", (cw, ch), 0)
    fill_draw = ImageDraw.Draw(fill_mask)
    outer_draw = ImageDraw.Draw(outer_mask)

    for chain in chains:
        pts_px = [world_to_px(float(x), float(y)) for x, y in chain["points"]]
        if len(pts_px) < 2:
            continue
        w_px = max(2, round(float(chain.get("width_wu", ROAD_RIBBON_WIDTH_WU)) * px_per_wu))
        e_px = max(1, round(ROAD_RIBBON_EDGE_WU * px_per_wu))
        for draw, width in ((outer_draw, w_px + 2 * e_px), (fill_draw, w_px)):
            draw.line(pts_px, fill=255, width=width, joint="curve")
            r = width / 2.0
            for (px, py) in (pts_px[0], pts_px[-1]):  # round caps
                draw.ellipse([px - r, py - r, px + r, py + r], fill=255)

    fill_arr = np.array(fill_mask, dtype=np.uint8)
    outer_arr = np.array(outer_mask, dtype=np.uint8)
    ys, xs = np.nonzero(outer_arr)
    if len(ys) == 0:
        return 0, None
    y0, y1 = int(ys.min()), int(ys.max()) + 1
    x0, x1 = int(xs.min()), int(xs.max()) + 1

    # world coords per canvas pixel (bbox-restricted)
    wx = top_left_x + (np.arange(x0, x1, dtype=np.float64) + 0.5) / cw * size_x
    wy = top_left_y - (np.arange(y0, y1, dtype=np.float64) + 0.5) / ch * size_y

    def surface_rgb(tex: np.ndarray | None, name: str) -> np.ndarray:
        h, w = y1 - y0, x1 - x0
        if tex is not None:
            th, tw = tex.shape[:2]
            u = ((wx / texture_repeat_wu) % 1.0 * tw).astype(np.int64) % tw
            v = ((wy / texture_repeat_wu) % 1.0 * th).astype(np.int64) % th
            return tex[v[:, None], u[None, :]]
        base = np.array(_SURFACE_FLAT_RGB[name], dtype=np.float64)
        noise = (rng.random((h, w, 1)) - 0.5) * 16.0
        return np.clip(base[None, None, :] + noise, 0, 255).astype(np.uint8)

    # v1: family not decodable offline -> single surface pass per compose
    # (all-"asphalt" default from extract; hand-edited "gravel" chains would
    # need per-chain masks -- acceptable v1 simplification: pick the surface
    # of the FIRST chain for the shared fill).
    surface = chains[0].get("surface", "asphalt")
    tex = asphalt if surface == "asphalt" else gravel
    rgb = surface_rgb(tex, surface)

    fill_bb = fill_arr[y0:y1, x0:x1]
    outer_bb = outer_arr[y0:y1, x0:x1]
    edge_bb = (outer_bb > 0) & (fill_bb == 0)

    layer_rgb = rgb.copy()
    layer_rgb[edge_bb] = (rgb[edge_bb].astype(np.float64) * 0.68).astype(np.uint8)
    alpha = np.zeros(fill_bb.shape, dtype=np.uint8)
    alpha[fill_bb > 0] = 255
    # worn/broken edge: noise-modulated alpha so the band reads as wear, not
    # a hard vector outline
    edge_noise = (150.0 + 105.0 * rng.random(fill_bb.shape)).astype(np.uint8)
    alpha[edge_bb] = edge_noise[edge_bb]

    layer_np = np.zeros((ch, cw, 4), dtype=np.uint8)
    layer_np[y0:y1, x0:x1, :3] = layer_rgb
    layer_np[y0:y1, x0:x1, 3] = alpha
    layer = Image.fromarray(layer_np, "RGBA")
    canvas.alpha_composite(layer)

    # centerline dashes on top of the fill
    line_layer = Image.new("RGBA", (cw, ch), (0, 0, 0, 0))
    line_draw = ImageDraw.Draw(line_layer)
    n_dashes = 0
    for chain in chains:
        if not chain.get("centerline", True):
            continue
        w_wu = float(chain.get("width_wu", ROAD_RIBBON_WIDTH_WU))
        cl_w_px = max(1, round(max(4.0, 0.05 * w_wu) * px_per_wu))
        pts = [(float(x), float(y)) for x, y in chain["points"]]
        for (a, b) in _dash_runs(pts, dash_wu=w_wu * 0.35, gap_wu=w_wu * 0.35):
            line_draw.line([world_to_px(*a), world_to_px(*b)],
                           fill=_CENTERLINE_RGBA, width=cl_w_px)
            n_dashes += 1
    canvas.alpha_composite(line_layer)
    return len(chains), fill_mask


def cmd_compose(args) -> int:
    markings_path = Path(args.markings)
    overlay_path = Path(args.overlay)
    if not markings_path.is_file():
        print(f"[compose] ERROR markings json not found: {markings_path}", file=sys.stderr)
        return 4
    if not overlay_path.is_file():
        print(f"[compose] ERROR overlay png not found: {overlay_path}", file=sys.stderr)
        return 4

    doc = json.loads(markings_path.read_text())
    glyphs = doc.get("glyphs", [])
    chains = doc.get("road_chains", [])

    bounds_path = Path(args.bounds) if args.bounds else None
    try:
        top_left_x, top_left_y, size_x, size_y = _load_bounds(bounds_path, overlay_path)
    except ValueError as e:
        print(f"[compose] ERROR {e}", file=sys.stderr)
        return 4

    px_per_wu = float(args.px_per_wu)
    canvas_w = max(1, round(size_x * px_per_wu))
    canvas_h = max(1, round(size_y * px_per_wu))

    # `--px-per-wu` is a DENSITY, and overlay_v2's bounds cover the WHOLE
    # mission map (thousands of world units per side) -- naively multiplying
    # density * full-map-size can request an astronomical canvas (e.g. 8
    # px/wu over a 15360wu map side = 122880px side, ~60GB RGBA). Clamp to
    # `--max-canvas` (default 4096, a sane single-sidecar-texture ceiling)
    # and rescale px_per_wu down to fit, rather than silently trying to
    # allocate an impossible buffer. This keeps the CLI contract
    # ("target res, default 8 px/wu") honest for the common per-region
    # authoring case while staying safe for a full-map compose.
    max_canvas = int(args.max_canvas)
    if canvas_w > max_canvas or canvas_h > max_canvas:
        shrink = max_canvas / max(canvas_w, canvas_h)
        px_per_wu *= shrink
        canvas_w = max(1, round(size_x * px_per_wu))
        canvas_h = max(1, round(size_y * px_per_wu))
        print(f"[compose] WARN requested px-per-wu would exceed --max-canvas={max_canvas}; "
              f"clamped to px_per_wu={px_per_wu:.4f} -> {canvas_w}x{canvas_h}")

    base = Image.open(overlay_path).convert("RGBA")
    if base.size != (canvas_w, canvas_h):
        base = base.resize((canvas_w, canvas_h), Image.BICUBIC)
    canvas = base.copy()

    def world_to_px(x: float, y: float) -> tuple[float, float]:
        # SAME convention as overlay_extract.py / control_map_tool.py:
        # uv.x=(worldX-topLeftX)/sizeX, uv.y=(topLeftY-worldY)/sizeY, row0=top=north.
        px = (x - top_left_x) / size_x * canvas_w
        py = (top_left_y - y) / size_y * canvas_h
        return px, py

    # pavement clip footprint BEFORE ribbons/glyphs touch the canvas: marking
    # paint must never spill onto bare terrain. Union of (a) the base
    # sidecar's own opaque coverage (cement raster), (b) the road ribbon fill
    # masks, (c) the extracted runway footprint rects (covers overlay-TILE
    # runways like mc2_24 whose pavement is NOT in the base cement raster).
    base_alpha = np.array(canvas, dtype=np.uint8)[..., 3]
    clip = base_alpha >= 100

    # road ribbons UNDER the marking glyphs
    asphalt = _load_albedo(args.asphalt_tga)
    gravel = _load_albedo(args.gravel_tga)
    ribbons, ribbon_fill = _draw_ribbons(canvas, chains, world_to_px, px_per_wu,
                                         (top_left_x, top_left_y, size_x, size_y),
                                         asphalt, gravel, float(args.texture_repeat_wu))
    if ribbon_fill is not None:
        clip |= np.array(ribbon_fill, dtype=np.uint8) > 0

    rect_layer = Image.new("L", (canvas_w, canvas_h), 0)
    rect_draw = ImageDraw.Draw(rect_layer)
    for rect in doc.get("runway_rects", []):
        ang = math.radians(float(rect["angle_deg"]))
        u = (math.cos(ang), math.sin(ang))
        v = (-math.sin(ang), math.cos(ang))
        hl = float(rect["length_wu"]) / 2.0
        hw = float(rect["width_wu"]) / 2.0
        cx, cy = float(rect["cx"]), float(rect["cy"])
        corners = [world_to_px(cx + su * u[0] * hl + sv * v[0] * hw,
                               cy + su * u[1] * hl + sv * v[1] * hw)
                   for su, sv in ((-1, -1), (-1, 1), (1, 1), (1, -1))]
        rect_draw.polygon(corners, fill=255)
    clip |= np.array(rect_layer, dtype=np.uint8) > 0

    glyph_layer = Image.new("RGBA", (canvas_w, canvas_h), (0, 0, 0, 0))
    placed = 0
    for g in glyphs:
        name = g["glyph"]
        color_name = g.get("color", "white")
        params = g.get("params", {})
        rgba = mg.render_glyph(name, px_per_wu, color_name, **params)
        rgba = mg.rotate_glyph(rgba, float(g.get("rotation_deg", 0.0)))
        rgba = mg.scale_glyph(rgba, float(g.get("scale", 1.0)))
        glyph_img = Image.fromarray(rgba, mode="RGBA")
        gw, gh = glyph_img.size
        px, py = world_to_px(float(g["x"]), float(g["y"]))
        paste_x = round(px - gw / 2.0)
        paste_y = round(py - gh / 2.0)
        glyph_layer.alpha_composite(glyph_img, (paste_x, paste_y))
        placed += 1

    glyph_np = np.array(glyph_layer, dtype=np.uint8)
    if not args.no_clip:
        glyph_np[..., 3] = np.where(clip, glyph_np[..., 3], 0)
    canvas.alpha_composite(Image.fromarray(glyph_np, "RGBA"))

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(out_path)

    out_bounds = out_path.with_suffix("").with_name(out_path.with_suffix("").name + ".bounds.txt")
    out_bounds.write_text(f"{top_left_x:.3f} {top_left_y:.3f} {size_x:.3f} {size_y:.3f}\n")

    print(f"[compose] {markings_path} + {overlay_path} -> {out_path} "
          f"size={canvas_w}x{canvas_h} px_per_wu={px_per_wu} glyphs_placed={placed} "
          f"road_ribbons={ribbons} bounds={out_bounds}")
    return 0


# --- install (delegates to overlay_extract.py's contract) --------------------
def cmd_install(args) -> int:
    src = Path(args.png)
    if not src.is_file():
        print(f"[install] ERROR source png not found: {src}", file=sys.stderr)
        return 4
    dest_dir = Path(args.deploy) / "data" / "missions" / f"{args.mission}.beauty"
    dest_dir.mkdir(parents=True, exist_ok=True)
    dest = dest_dir / "overlay_v2.png"
    shutil.copyfile(src, dest)
    print(f"[install] {src} -> {dest}")

    src_bounds = src.with_suffix("").with_name(src.with_suffix("").name + ".bounds.txt")
    if src_bounds.is_file():
        dest_bounds = dest_dir / "overlay_v2.bounds.txt"
        shutil.copyfile(src_bounds, dest_bounds)
        print(f"[install] {src_bounds} -> {dest_bounds}")
    else:
        print(f"[install] WARN no bounds file found at {src_bounds} "
              "-- engine will use full-map-extent default")
    return 0


# --- sheet (contact sheet preview) --------------------------------------------
PANEL = 220


def _resize(img: Image.Image, n: int = PANEL) -> Image.Image:
    return img.resize((n, n), Image.NEAREST)


def cmd_sheet(args) -> int:
    png_path = Path(args.png)
    if not png_path.is_file():
        print(f"[sheet] ERROR png not found: {png_path}", file=sys.stderr)
        return 4
    img = Image.open(png_path).convert("RGBA")
    arr = np.array(img)

    panels = [_resize(Image.fromarray(arr[..., :3], "RGB"))]
    labels = ["composed RGB"]
    a = arr[..., 3]
    panels.append(_resize(Image.fromarray(np.stack([a, a, a], axis=-1), "RGB")))
    labels.append("alpha")

    cols = len(panels)
    sheet = Image.new("RGB", (PANEL * cols, PANEL + 24), (30, 30, 30))
    from PIL import ImageDraw as _ImageDraw
    draw = _ImageDraw.Draw(sheet)
    for i, (panel, label) in enumerate(zip(panels, labels)):
        sheet.paste(panel, (i * PANEL, 24))
        draw.text((i * PANEL + 4, 4), label, fill=(255, 255, 255))

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(out_path)
    print(f"[sheet] {png_path} -> {out_path}")
    return 0


# --- CLI ----------------------------------------------------------------------
def main() -> int:
    ap = argparse.ArgumentParser(
        description="cook_markings: parametric road/runway marking glyphs + road ribbons composed into the overlay-V2 sidecar")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("extract", help="scan mission overlay + cement cells -> auto-generate markings.json (glyphs + road_chains)")
    p.add_argument("--pak", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--pad-edges", action="store_true",
                   help="emit optional perimeter striping placements for compact cement pads")
    p.add_argument("--road-width-wu", type=float, default=ROAD_RIBBON_WIDTH_WU,
                   help=f"ribbon width for extracted road chains (default {ROAD_RIBBON_WIDTH_WU})")
    p.set_defaults(func=cmd_extract)

    p = sub.add_parser("compose", help="rasterize markings.json (ribbons + glyphs) over an existing overlay_v2.png")
    p.add_argument("--markings", required=True)
    p.add_argument("--overlay", required=True)
    p.add_argument("--bounds", help="explicit bounds file (default: <overlay-stem>.bounds.txt)")
    p.add_argument("--out", required=True)
    p.add_argument("--px-per-wu", type=float, default=8.0)
    p.add_argument("--max-canvas", type=int, default=4096,
                   help="safety cap on canvas width/height in pixels (default 4096)")
    p.add_argument("--asphalt-tga", help="asphalt albedo TGA for world-XY tiled ribbon fill (default: flat parametric shading)")
    p.add_argument("--gravel-tga", help="gravel albedo TGA for world-XY tiled ribbon fill (default: flat parametric shading)")
    p.add_argument("--texture-repeat-wu", type=float, default=256.0,
                   help="world units per albedo texture repeat (default 256)")
    p.add_argument("--no-clip", action="store_true",
                   help="do NOT clip marking glyphs to the pavement footprint "
                        "(base alpha | ribbons | runway rects)")
    p.set_defaults(func=cmd_compose)

    p = sub.add_parser("install", help="copy composed png(+bounds) into <deploy>/data/missions/<stem>.beauty/overlay_v2.png")
    p.add_argument("--png", required=True)
    p.add_argument("--deploy", required=True)
    p.add_argument("--mission", required=True)
    p.set_defaults(func=cmd_install)

    p = sub.add_parser("sheet", help="RGB + alpha contact sheet")
    p.add_argument("--png", required=True)
    p.add_argument("--out", required=True)
    p.set_defaults(func=cmd_sheet)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
