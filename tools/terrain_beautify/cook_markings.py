#!/usr/bin/env python3
"""ROAD-MARKINGS-VECTOR-1: cook road/runway markings into the overlay-V2
sidecar (`data/missions/<stem>.beauty/overlay_v2.png`).

Recon: `.claude/ROAD-MARKINGS-VECTOR-RECON-1.md`. Locked rulings:
  - Carrier = the EXISTING overlay-V2 sidecar (`overlay_extract.py` /
    `mclib/terrain.cpp` gate `MC2_TERRAIN_OVERLAY_V2`) -- markings are
    COMPOSED INTO `overlay_v2.png` at cook time, ZERO engine change.
  - Authoring = parametric glyph DSL (`marking_glyphs.py`), NOT raster
    upscale of the old 64x64 tile art and NOT SVG import (deferred).
  - Scope: markings-only (no surface re-texture; asphalt/gravel PBR from
    ROAD-PBR-1 stays as-is). Crossings (x_* families) ARE in scope.

Subcommands:
  extract  --pak <mission.pak> --out <markings.json>
      Scans the mission's overlay-tagged cells (`textureData` hi-16, same
      reader as `overlay_extract.py::read_bridge_gate_overlay_cells`) and
      AUTO-GENERATES a markings.json placement list approximating the
      legacy tile-grid marking layout, WITHOUT decoding the legacy
      per-family tile-orientation index (that decode needs
      `TerrainTextures::baseTXMIndex`, a runtime texture-manager-assigned
      index built from full texture-load order -- not content-derivable
      offline; recon explicitly scoped extraction to geometry/classification,
      not exact tile-id inversion). Instead this classifies purely from
      CONNECTED-COMPONENT SHAPE of the overlay-tagged cell mask:
        - components with high elongation (long/short >= RUNWAY_ASPECT_MIN)
          AND high fill density (>= RUNWAY_DENSITY_MIN) AND minimum length
          are classified RUNWAY -> centerline dashes + edge stripes + a
          numeral pair at each end + threshold bars near each end.
        - all other overlay-tagged cells are classified ROAD -> a centerline
          dash run following the cell run's local direction.
      This is a FIDELITY-LIMITED auto-generate (documented, not a hidden
      approximation): it reproduces "there is a marked road/runway here,
      oriented along its run" but does NOT reproduce the exact legacy
      per-tile pixel art or exact numeral text (numerals are a placeholder
      "09"/"27" pair -- real designators need author/editor input this tool
      does not have offline). Bridge/gate cells are excluded (same landmine
      as overlay_extract.py: runtime-mutable, must never be statically baked).
  compose  --markings <json> --overlay <overlay_v2.png> [--bounds <file>]
           --out <png> [--px-per-wu N]
      Rasterizes every glyph placement in markings.json at `px_per_wu`
      (default 8) into an RGBA buffer sized to the overlay_v2.png's own
      world bounds, alpha-composites glyphs OVER a copy of overlay_v2.png
      (resized to the marking canvas resolution if needed), and writes the
      combined sidecar PNG + a companion contact-sheet preview.
  install  --png <file> --deploy <dir> --mission <stem>
      Copies the composed PNG (+ bounds file) to
      <deploy>/data/missions/<stem>.beauty/overlay_v2.png -- SAME contract
      as overlay_extract.py's install (this tool does not reinvent it).

PIL + numpy only, offline, read-only w.r.t. .pak files. No engine launch.
"""
from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mission_terrain_analyzer import (  # noqa: E402
    read_packets, locate_mapdata, extract_layers, read_water_elevation,
)
import marking_glyphs as mg  # noqa: E402

# --- bridge/gate exclusion ----------------------------------------------------
# `overlay_extract.py::read_bridge_gate_overlay_cells` is a concrete-scope
# placeholder (its body is IDENTICAL to "any overlay-tagged cell", which is a
# no-op for concrete's terrainType classifier but would wrongly exclude 100%
# of road/runway cells if reused here -- verified empirically: mc2_24 has
# 790 overlay-tagged cells and that helper flags all 790 as "bridge/gate").
# Real bridges/gates aren't decodable from the overlay tile family offline
# (recon: the overlay_hi -> family decode needs TerrainTextures::baseTXMIndex,
# a runtime texture-manager-assigned index, not content-derivable from the
# pak alone). Bridges physically cross water, so instead this uses a
# WATER-ADJACENCY heuristic: an overlay-tagged cell within `_BRIDGE_DILATE`
# cells of a water cell is treated as bridge-likely and excluded. This is
# conservative (errs toward excluding, never toward baking a runtime-mutable
# cell) and empirically small (0-67 cells across all 24 stock missions,
# checked against every mission in Carver5-feasibility; 0 for mc2_24, the
# mission this slice ships).
_BRIDGE_DILATE = 2


def read_bridge_gate_overlay_cells(side: int, blocks: bytes, water_elev: float) -> np.ndarray:
    layers = extract_layers(side, blocks, water_elev)
    water = layers["water"]
    dilated = water.copy()
    for _ in range(_BRIDGE_DILATE):
        nxt = dilated.copy()
        nxt[1:, :] |= dilated[:-1, :]
        nxt[:-1, :] |= dilated[1:, :]
        nxt[:, 1:] |= dilated[:, :-1]
        nxt[:, :-1] |= dilated[:, 1:]
        dilated = nxt
    return dilated

WORLD_UNITS_PER_VERTEX = 128.0

# --- shape classifier thresholds (auto-extract) ------------------------------
RUNWAY_ASPECT_MIN = 2.5   # long/short bbox ratio
RUNWAY_DENSITY_MIN = 0.30  # filled_cells / bbox_area
RUNWAY_MIN_LONG_WU = 10 * WORLD_UNITS_PER_VERTEX  # ~10 cells minimum length
RUNWAY_MIN_COUNT = 15      # minimum cell count to call a component "runway"


# --- extract ------------------------------------------------------------------
def _tile_center_world(row: int, col: int, side: int) -> tuple[float, float]:
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


def _runway_placements(shape: dict, side: int) -> list[dict]:
    r0, r1, c0, c1 = shape["bbox"]
    vertical = shape["vertical"]
    length_wu = max(shape["h"], shape["w"]) * WORLD_UNITS_PER_VERTEX
    width_wu = min(shape["h"], shape["w"]) * WORLD_UNITS_PER_VERTEX
    cx = (c0 + c1) / 2.0
    cy = (r0 + r1) / 2.0
    world_cx, world_cy = _tile_center_world(cy, cx, side)
    rotation = 90.0 if vertical else 0.0  # glyphs author along local +X; rotate for N-S runs

    placements = []
    placements.append({
        "glyph": "runway_centerline_dashes", "x": world_cx, "y": world_cy,
        "rotation_deg": rotation, "scale": 1.0, "color": "white",
        "params": {"length_wu": length_wu * 0.92, "dash_wu": 6.0, "gap_wu": 4.0,
                    "width_wu": max(0.6, width_wu * 0.06)},
    })
    for side_sign in (-1.0, 1.0):
        offset = side_sign * (width_wu / 2.0 - width_wu * 0.08)
        ex = world_cx + (offset if vertical else 0.0)
        ey = world_cy + (0.0 if vertical else offset)
        placements.append({
            "glyph": "runway_edge_stripes", "x": ex, "y": ey,
            "rotation_deg": rotation, "scale": 1.0, "color": "white",
            "params": {"length_wu": length_wu * 0.96, "width_wu": max(0.4, width_wu * 0.05)},
        })
    # threshold bars + numeral pair near each end (auto-extract placeholder
    # designator "09"/"27" -- see module docstring: real designators need
    # authored input this offline tool doesn't have).
    end_frac = 0.42
    numerals = ("09", "27")
    for end_sign, numeral in zip((-1.0, 1.0), numerals):
        d = end_sign * (length_wu / 2.0 - length_wu * (0.5 - end_frac))
        px = world_cx + (0.0 if vertical else d)
        py = world_cy + (d if vertical else 0.0)
        placements.append({
            "glyph": "runway_threshold_bars", "x": px, "y": py,
            "rotation_deg": rotation, "scale": 1.0, "color": "white",
            "params": {"runway_width_wu": width_wu * 0.9, "num_bars": 8,
                        "bar_length_wu": 6.0, "bar_width_wu": 0.9, "gap_wu": 0.6},
        })
        nd = end_sign * (length_wu / 2.0 - length_wu * (0.5 - end_frac) - 10.0)
        nx = world_cx + (0.0 if vertical else nd)
        ny = world_cy + (nd if vertical else 0.0)
        placements.append({
            "glyph": "runway_numeral", "x": nx, "y": ny,
            "rotation_deg": rotation, "scale": 1.0, "color": "white",
            "params": {"text": numeral, "height_wu": min(8.0, width_wu * 0.4)},
        })
    return placements


def _road_placements(shape: dict, side: int) -> list[dict]:
    r0, r1, c0, c1 = shape["bbox"]
    vertical = shape["vertical"]
    length_wu = max(shape["h"], shape["w"]) * WORLD_UNITS_PER_VERTEX
    cx = (c0 + c1) / 2.0
    cy = (r0 + r1) / 2.0
    world_cx, world_cy = _tile_center_world(cy, cx, side)
    rotation = 90.0 if vertical else 0.0
    return [{
        "glyph": "road_centerline_dash", "x": world_cx, "y": world_cy,
        "rotation_deg": rotation, "scale": 1.0, "color": "yellow",
        "params": {"length_wu": min(length_wu * 0.9, 200.0), "width_wu": 0.3},
    }]


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

    comps = _classify_components(tagged)
    placements: list[dict] = []
    n_runway = n_road = 0
    for shape in comps:
        if shape["count"] < 2:
            continue  # single stray tagged cell, not a marked run
        if shape["kind"] == "runway":
            placements.extend(_runway_placements(shape, side))
            n_runway += 1
        else:
            placements.extend(_road_placements(shape, side))
            n_road += 1

    doc = {"mission": pak.stem, "side": side, "glyphs": placements}
    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(doc, indent=2, sort_keys=False))

    print(f"[extract] {pak.stem}: side={side} overlay_tagged_cells={int(tagged.sum())} "
          f"bridge_gate_excluded={excluded} components={len(comps)} "
          f"runway_components={n_runway} road_components={n_road} "
          f"glyph_placements={len(placements)} -> {out_path}")
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
        canvas.alpha_composite(glyph_img, (paste_x, paste_y))
        placed += 1

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(out_path)

    out_bounds = out_path.with_suffix("").with_name(out_path.with_suffix("").name + ".bounds.txt")
    out_bounds.write_text(f"{top_left_x:.3f} {top_left_y:.3f} {size_x:.3f} {size_y:.3f}\n")

    print(f"[compose] {markings_path} + {overlay_path} -> {out_path} "
          f"size={canvas_w}x{canvas_h} px_per_wu={px_per_wu} glyphs_placed={placed} "
          f"bounds={out_bounds}")
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
        description="cook_markings: parametric road/runway marking glyphs composed into the overlay-V2 sidecar")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("extract", help="scan mission overlay tile indices -> auto-generate markings.json")
    p.add_argument("--pak", required=True)
    p.add_argument("--out", required=True)
    p.set_defaults(func=cmd_extract)

    p = sub.add_parser("compose", help="rasterize markings.json over an existing overlay_v2.png")
    p.add_argument("--markings", required=True)
    p.add_argument("--overlay", required=True)
    p.add_argument("--bounds", help="explicit bounds file (default: <overlay-stem>.bounds.txt)")
    p.add_argument("--out", required=True)
    p.add_argument("--px-per-wu", type=float, default=8.0)
    p.add_argument("--max-canvas", type=int, default=4096,
                    help="safety cap on canvas width/height in pixels (default 4096)")
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
