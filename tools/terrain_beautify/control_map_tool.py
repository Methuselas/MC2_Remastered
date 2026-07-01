#!/usr/bin/env python3
"""TERRAIN-CONTROLMAP-SAMPLE-1: offline authoring/validation harness for the
authored terrain control-map sidecar (`data/missions/<stem>.beauty/control_map.png`).

Engine contract (mclib/terrain.cpp, gated MC2_TERRAIN_CONTROLMAP, search
"TERRAIN_CONTROLMAP v1" / gos_TerrainLodChunk_UploadControlMap):
  - RGBA8 PNG, size side x side where side == Terrain::realVerticesMapSide
    (one texel per terrain VERTEX; side in {60,80,100,120}).
  - R=rock, G=grass, B=dirt, A=concrete weight, normalized in-shader (GL_LINEAR
    sampling gives bilinear cross-cell blend, matching how the colormap atlas
    is sampled).
  - Loader decode: mclib/control_map_png_decode.cpp ControlMapPng_DecodeRGBA()
    -> stb_image `stbi_load_from_memory(..., 4)`. stb_image returns rows in
    on-disk PNG top-to-bottom order (row 0 of the decoded buffer = the TOP row
    of the authored image, standard image convention, NOT flipped).
  - terrain.cpp:897 requires `pw == mapSide && ph == mapSide` (exact match,
    no rescale) then passes the flat decoded buffer straight to
    gos_TerrainLodChunk_UploadControlMap() -> glTexImage2D(..., rgba) with NO
    row reversal anywhere in that call chain (gos_terrain_lod_chunk.cpp:1466).
  - The vertex-side arrays this must line up with (elev[], ttype[] uploaded a
    few lines above at terrain.cpp:817/821, and mission_terrain_analyzer.py's
    `extract_layers()` reshape) are flat row-major `blks[i]`, i = row*side+col,
    with NO flip applied either.
  - CONCLUSION (row-order finding): the control-map PNG's pixel row 0 (top of
    the image as opened in any editor) corresponds DIRECTLY to vertex row 0
    (i = 0..side-1) with no vertical flip anywhere in the pipeline. Authors
    can treat the PNG like a plain top-down 2D array matching `blks[]`/
    `extract_layers()` output index-for-index. This tool follows that
    convention throughout (numpy array row 0 == PNG row 0 == vertex row 0).

Subcommands:
  generate  --pak <mission.pak> --out <png>
      v2-authoring-seed classification: reuses mission_terrain_analyzer's
      PostcompVertex.terrtype extraction (NOT a copy) and maps the raw
      terrainType index -> material bucket with the SAME mapping the engine
      uses (mclib/terrain.cpp terrainTypeToMaterial lambda), then writes
      RGBA = one-hot(material) * 255. Classifier PARITY with the live
      colormap-colour classifier (chunkColorWeights) is explicitly NOT
      required here -- this is an authoring seed (v1(a)-adjacent), not the
      runtime classifier twin.
  pattern   --side N --out <png> --kind quadrant|gradient|checker
      Synthetic deterministic test maps for in-game visual verification.
  validate  --png <file> --pak <mission.pak>
      Size match (against the mission's realVerticesMapSide), RGBA8 format,
      weight-sum sanity. Prints PASS/FAIL, exit 0/1.
  install   --png <file> --deploy <dir> --mission <stem>
      Copies to <deploy>/data/missions/<stem>.beauty/control_map.png.
  sheet     --png <file> --out <file>
      Per-channel grayscale + composite RGB contact sheet (reuses
      terrain_workbench's panel/resize convention).

PIL + numpy only, offline, read-only w.r.t. .pak files (generate/validate never
write pak/fit). No engine launch, no gameos import.
"""
from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mission_terrain_analyzer import (  # noqa: E402
    read_packets, locate_mapdata, extract_layers, read_water_elevation,
    VALID_GRID_SIDES,
)

# Mirrors mclib/terrain.cpp terrainTypeToMaterial lambda EXACTLY (search
# "MUST mirror gos_terrain_indirect.cpp terrainTypeToMaterialLocal" in
# terrain.cpp ~line 806). Material index: 0=Rock 1=Grass 2=Dirt 3=Concrete.
_GRASS = {3, 8, 9, 12}
_DIRT = {2, 4}
_CONCRETE = {10, 13, 14, 15, 16, 17, 18, 19, 20}


def terrain_type_to_material(raw: np.ndarray) -> np.ndarray:
    """raw: uint32 array of PostcompVertex.terrainType indices (any shape).
    Returns uint8 array of material index 0..3, same shape."""
    out = np.zeros(raw.shape, dtype=np.uint8)  # default 0 = Rock
    for v in _GRASS:
        out[raw == v] = 1
    for v in _DIRT:
        out[raw == v] = 2
    for v in _CONCRETE:
        out[raw == v] = 3
    return out


def material_to_rgba(material: np.ndarray) -> np.ndarray:
    """One-hot material index (0..3) -> RGBA8 weight image.
    R=rock(0) G=grass(1) B=dirt(2) A=concrete(3), 0 or 255 per channel."""
    h, w = material.shape
    rgba = np.zeros((h, w, 4), dtype=np.uint8)
    for idx in range(4):
        rgba[..., idx] = np.where(material == idx, 255, 0).astype(np.uint8)
    return rgba


# --- generate -----------------------------------------------------------------
def cmd_generate(args) -> int:
    pak = Path(args.pak)
    if not pak.is_file():
        print(f"[generate] ERROR pak not found: {pak}", file=sys.stderr)
        return 4
    packets = read_packets(pak)
    md = locate_mapdata(packets)
    if md is None:
        print(f"[generate] ERROR no MapData packet matched signature in {pak}", file=sys.stderr)
        return 4
    pkt_idx, side, blocks = md

    mission = pak.stem
    fit_path = pak.with_suffix(".fit")
    water_elev = read_water_elevation(fit_path)
    layers = extract_layers(side, blocks, water_elev)  # terrtype shape (side, side), row-major, no flip
    material = terrain_type_to_material(layers["terrtype"])
    rgba = material_to_rgba(material)

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(rgba, mode="RGBA").save(out_path)

    counts = {name: int((material == idx).sum())
              for idx, name in enumerate(("rock", "grass", "dirt", "concrete"))}
    print(f"[generate] {mission}: side={side} -> {out_path} counts={counts}")
    return 0


# --- pattern --------------------------------------------------------------
def cmd_pattern(args) -> int:
    side = args.side
    if side <= 1:
        print(f"[pattern] ERROR --side must be > 1, got {side}", file=sys.stderr)
        return 4
    rgba = np.zeros((side, side, 4), dtype=np.uint8)
    kind = args.kind

    if kind == "quadrant":
        # 4 pure corners: TL=rock(R) TR=grass(G) BL=dirt(B) BR=concrete(A).
        # Row 0 = top per the PNG/vertex-row-0 convention documented above.
        half = side // 2
        rgba[:half, :half, 0] = 255            # top-left    -> rock
        rgba[:half, half:, 1] = 255            # top-right   -> grass
        rgba[half:, :half, 2] = 255            # bottom-left -> dirt
        rgba[half:, half:, 3] = 255            # bottom-right-> concrete
    elif kind == "gradient":
        # Horizontal rock->grass gradient (R falls, G rises left->right);
        # dirt/concrete held at 0 so weights always sum to 255 (R+G=255).
        ramp = np.linspace(0, 255, side, dtype=np.uint8)
        rgba[:, :, 0] = 255 - ramp[np.newaxis, :]
        rgba[:, :, 1] = ramp[np.newaxis, :]
    elif kind == "checker":
        cell = max(1, side // 8)
        rr, cc = np.indices((side, side))
        is_a = ((rr // cell) + (cc // cell)) % 2 == 0
        rgba[is_a, 0] = 255     # rock squares
        rgba[~is_a, 1] = 255    # grass squares
    else:
        print(f"[pattern] ERROR unknown --kind {kind}", file=sys.stderr)
        return 4

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(rgba, mode="RGBA").save(out_path)
    print(f"[pattern] kind={kind} side={side} -> {out_path}")
    return 0


# --- validate -------------------------------------------------------------
def cmd_validate(args) -> int:
    png_path = Path(args.png)
    ok = True

    if not png_path.is_file():
        print(f"[validate] FAIL png not found: {png_path}")
        return 1

    img = Image.open(png_path)
    if img.mode != "RGBA":
        print(f"[validate] FAIL mode={img.mode}, want RGBA (8-bit/channel)")
        ok = False
    w, h = img.size
    if w != h:
        print(f"[validate] FAIL non-square image {w}x{h}")
        ok = False

    expected_side = None
    if args.pak:
        pak = Path(args.pak)
        if not pak.is_file():
            print(f"[validate] FAIL pak not found: {pak}")
            return 1
        packets = read_packets(pak)
        md = locate_mapdata(packets)
        if md is None:
            print(f"[validate] FAIL no MapData packet in {pak}")
            return 1
        _, expected_side, _ = md
    elif args.side:
        expected_side = args.side

    if expected_side is not None:
        if w != expected_side or h != expected_side:
            print(f"[validate] FAIL size {w}x{h} != expected {expected_side}x{expected_side} "
                  "(engine requires EXACT match, terrain.cpp:897)")
            ok = False
        else:
            print(f"[validate] size {w}x{h} == expected {expected_side}x{expected_side}: PASS")
    else:
        if w not in VALID_GRID_SIDES:
            print(f"[validate] WARN side={w} not in known VALID_GRID_SIDES {VALID_GRID_SIDES} "
                  "(no --pak/--side given to confirm against mission)")

    arr = np.array(img.convert("RGBA"), dtype=np.uint16)
    total = arr[..., 0].astype(np.uint16) + arr[..., 1] + arr[..., 2] + arr[..., 3]
    zero_weight = int((total == 0).sum())
    overflow = int((total > 255 * 4).sum())  # structurally impossible for uint8 channels, sanity only
    if zero_weight > 0:
        print(f"[validate] WARN {zero_weight} texel(s) have all-zero RGBA weight "
              "(engine normalizes in-shader -> undefined/zero material there)")
    if overflow > 0:
        print(f"[validate] FAIL {overflow} texel(s) with impossible channel overflow")
        ok = False
    print(f"[validate] weight-sum stats: min={int(total.min())} max={int(total.max())} "
          f"mean={float(total.mean()):.1f} (255 = single-material texel, engine normalizes any sum)")

    print("[validate] PASS" if ok else "[validate] FAIL")
    return 0 if ok else 1


# --- install ----------------------------------------------------------------
def cmd_install(args) -> int:
    src = Path(args.png)
    if not src.is_file():
        print(f"[install] ERROR source png not found: {src}", file=sys.stderr)
        return 4
    dest_dir = Path(args.deploy) / "data" / "missions" / f"{args.mission}.beauty"
    dest_dir.mkdir(parents=True, exist_ok=True)
    dest = dest_dir / "control_map.png"
    shutil.copyfile(src, dest)
    print(f"[install] {src} -> {dest}")
    return 0


# --- sheet ------------------------------------------------------------------
PANEL = 220  # matches terrain_workbench.py's panel size convention


def _resize(rgb: np.ndarray, n: int = PANEL) -> Image.Image:
    return Image.fromarray(rgb, "RGB").resize((n, n), Image.NEAREST)


def cmd_sheet(args) -> int:
    png_path = Path(args.png)
    if not png_path.is_file():
        print(f"[sheet] ERROR png not found: {png_path}", file=sys.stderr)
        return 4
    arr = np.array(Image.open(png_path).convert("RGBA"))
    names = ("rock(R)", "grass(G)", "dirt(B)", "concrete(A)")

    panels = []
    labels = []
    for idx, name in enumerate(names):
        chan = arr[..., idx]
        gray = np.stack([chan, chan, chan], axis=-1)
        panels.append(_resize(gray))
        labels.append(name)
    composite = arr[..., :3].copy()
    panels.append(_resize(composite))
    labels.append("composite RGB")

    cols = len(panels)
    sheet = Image.new("RGB", (PANEL * cols, PANEL + 24), (30, 30, 30))
    from PIL import ImageDraw
    draw = ImageDraw.Draw(sheet)
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
    ap = argparse.ArgumentParser(description="control_map_tool: offline authoring/validation for terrain control maps")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("generate", help="classify stock mission terrainType -> RGBA control map (authoring seed)")
    p.add_argument("--pak", required=True)
    p.add_argument("--out", required=True)
    p.set_defaults(func=cmd_generate)

    p = sub.add_parser("pattern", help="synthetic test control map")
    p.add_argument("--side", type=int, required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--kind", choices=("quadrant", "gradient", "checker"), required=True)
    p.set_defaults(func=cmd_pattern)

    p = sub.add_parser("validate", help="size/format/weight-sum checks")
    p.add_argument("--png", required=True)
    p.add_argument("--pak", help="mission pak to confirm expected side against")
    p.add_argument("--side", type=int, help="expected side if no --pak available")
    p.set_defaults(func=cmd_validate)

    p = sub.add_parser("install", help="copy png into <deploy>/data/missions/<stem>.beauty/control_map.png")
    p.add_argument("--png", required=True)
    p.add_argument("--deploy", required=True)
    p.add_argument("--mission", required=True, help="mission stem, e.g. mc2_01")
    p.set_defaults(func=cmd_install)

    p = sub.add_parser("sheet", help="per-channel + composite contact sheet")
    p.add_argument("--png", required=True)
    p.add_argument("--out", required=True)
    p.set_defaults(func=cmd_sheet)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
