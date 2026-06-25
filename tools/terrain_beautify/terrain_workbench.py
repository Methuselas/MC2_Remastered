#!/usr/bin/env python3
"""TERRAIN-BEAUTIFY-WORKBENCH-1 / TERRAIN-WORKBENCH-REPORT-1.

Offline terrain workbench: load a stock mission + its beautify products and emit a
static report (contact-sheet PNG + cross-section profiles + HTML + a JSON of
PASS/WARN/FAIL checks) so the bake/reshape can be tuned WITHOUT launching MC2.

Loads (whatever exists):
  stock .pak heightfield + .fit water elevation
  analyzer-style masks (slope/curvature/water/shoreline/cliff/pyramid/protected)
  <mission>.beauty/height_delta.r32        (gameplay smoothing delta, optional)
  <mission>.beauty/protected.r8            (protection mask, optional)
  <mission>.beauty/visual_height_<F>x.r32  (visual heightfield, optional)

Emits to <out>/<mission>/ :
  contact_sheet.png   shaded panels (hillshade orig/visual, delta, slope, cliff,
                      pyramid, protected, water/shoreline)
  profiles.png        cross-section slices (worst pyramid H/V, largest cliff) orig vs visual
  workbench.html      contact sheet + profiles + checks table
  workbench_report.json   numeric checks (corner-pin, protected delta, waterline,
                      footprint delta, max visual delta, blockiness, pyramid score)

Run:  python terrain_workbench.py mc2_01 [--missions-dir DIR] [--beauty DIR] [--out DIR]

PIL + numpy only (no matplotlib). Exit 0 if no FAIL check, else 1.
"""
from __future__ import annotations

import argparse
import base64
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mission_terrain_analyzer import (  # noqa: E402
    read_packets, locate_mapdata, extract_layers, derive_masks,
    detect_pyramid_islands, read_object_footprints, read_water_elevation,
    _dilate, WORLD_UNITS_PER_VERTEX, CLIFF_SLOPE_DEG,
)

PANEL = 220   # panel pixel size


# --- rendering helpers -------------------------------------------------------
def _resize(rgb: np.ndarray, n: int = PANEL) -> Image.Image:
    return Image.fromarray(rgb, "RGB").resize((n, n), Image.NEAREST)


def hillshade(elev: np.ndarray) -> np.ndarray:
    gy, gx = np.gradient(elev)
    nx, ny, nz = -gx, -gy, np.full_like(elev, WORLD_UNITS_PER_VERTEX)
    L = np.sqrt(nx * nx + ny * ny + nz * nz) + 1e-9
    light = np.array([-0.5, -0.5, 0.7]); light /= np.linalg.norm(light)
    shade = np.clip((nx / L) * light[0] + (ny / L) * light[1] + (nz / L) * light[2], 0.05, 1.0)
    e = elev - elev.min(); e = e / (e.max() + 1e-9)
    base = 0.3 + 0.7 * e
    v = np.clip(base * shade * 1.5, 0, 1)
    g = (v * 255).astype(np.uint8)
    return np.stack([g, g, g], axis=-1)


def colorize(mask: np.ndarray, color, over=None) -> np.ndarray:
    h, w = mask.shape
    rgb = np.zeros((h, w, 3), np.uint8) if over is None else over.copy()
    rgb[mask] = color
    return rgb


def signed_delta_rgb(d: np.ndarray) -> np.ndarray:
    m = np.abs(d).max() + 1e-9
    t = np.clip(d / m, -1, 1)
    r = np.where(t > 0, (t * 255), 0).astype(np.uint8)
    b = np.where(t < 0, (-t * 255), 0).astype(np.uint8)
    g = np.zeros_like(r)
    return np.stack([r, g, b], axis=-1)


def label_panel(img: Image.Image, text: str) -> Image.Image:
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, img.width, 12], fill=(0, 0, 0))
    d.text((2, 1), text, fill=(255, 255, 0))
    return img


def profile_plot(width, height, title, x, series) -> Image.Image:
    """series: list of (label, yvals, color)."""
    img = Image.new("RGB", (width, height), (16, 16, 20))
    d = ImageDraw.Draw(img)
    allv = np.concatenate([s[1] for s in series])
    lo, hi = float(allv.min()), float(allv.max())
    rng = (hi - lo) or 1.0
    pad = 18
    def px(i): return pad + (i / max(1, len(x) - 1)) * (width - 2 * pad)
    def py(v): return height - pad - ((v - lo) / rng) * (height - 2 * pad)
    d.rectangle([pad, pad, width - pad, height - pad], outline=(80, 80, 80))
    for (lab, yv, col) in series:
        pts = [(px(i), py(yv[i])) for i in range(len(yv))]
        d.line(pts, fill=col, width=1)
    d.text((pad, 2), title, fill=(220, 220, 220))
    # legend
    for k, (lab, _, col) in enumerate(series):
        d.text((pad + 4, pad + 2 + 11 * k), lab, fill=col)
    return img


def compose_grid(panels, cols, gap=4, bg=(28, 28, 32)) -> Image.Image:
    if not panels:
        return Image.new("RGB", (PANEL, PANEL), bg)
    rows = (len(panels) + cols - 1) // cols
    W = cols * PANEL + (cols + 1) * gap
    H = rows * PANEL + (rows + 1) * gap
    sheet = Image.new("RGB", (W, H), bg)
    for i, p in enumerate(panels):
        r, c = divmod(i, cols)
        sheet.paste(p, (gap + c * (PANEL + gap), gap + r * (PANEL + gap)))
    return sheet


# --- main analysis -----------------------------------------------------------
def coarse_from_visual(visual: np.ndarray, side: int, factor: int) -> np.ndarray:
    """Sample the visual grid at coarse-vertex positions -> (side,side)."""
    return visual[::factor, ::factor][:side, :side]


def nearest_up(elev, factor):
    N = elev.shape[0]; V = (N - 1) * factor + 1
    idx = np.clip((np.arange(V) + factor // 2) // factor, 0, N - 1)
    return elev[np.ix_(idx, idx)]


def blockiness(elev):
    sr = np.zeros_like(elev); sc = np.zeros_like(elev)
    sr[1:-1, :] = elev[2:, :] - 2 * elev[1:-1, :] + elev[:-2, :]
    sc[:, 1:-1] = elev[:, 2:] - 2 * elev[:, 1:-1] + elev[:, :-2]
    return float((sr * sr + sc * sc).mean())


def run(mission: str, missions_dir: Path, beauty_root: Path, out_root: Path) -> dict:
    pak = missions_dir / f"{mission}.pak"
    if not pak.is_file():
        return {"mission": mission, "error": f"not found: {pak}"}
    _, side, blocks = locate_mapdata(read_packets(pak))
    water_elev = read_water_elevation(missions_dir / f"{mission}.fit")
    layers = extract_layers(side, blocks, water_elev)
    elev = layers["elev"]
    masks = derive_masks(elev, layers["water"], layers["overlay"])
    pyramids, pyr_mask = detect_pyramid_islands(elev, masks["land"], layers["water"])
    foot, _objs, _ = read_object_footprints(read_packets(pak), side)
    protect_struct = _dilate(layers["overlay"] | foot)

    bdir = beauty_root / f"{mission}.beauty"
    checks = []

    def chk(name, status, detail):
        checks.append({"check": name, "status": status, "detail": detail})

    # --- gameplay smoothing delta (height_delta.r32), if present ---
    gdelta = None
    f = bdir / "height_delta.r32"
    if f.is_file():
        gdelta = np.fromfile(f, "<f4").reshape(side, side).astype(np.float64)
        pmax = float(np.abs(gdelta[protect_struct]).max()) if protect_struct.any() else 0.0
        chk("protected_struct_delta", "PASS" if pmax == 0 else "FAIL",
            f"max|Δ| on roads/buildings = {pmax:.3f}wu (want 0)")
        wmax = float(np.abs(gdelta[layers["water"]]).max()) if layers["water"].any() else 0.0
        chk("waterline_delta", "PASS" if wmax <= 0.0 else "WARN",
            f"max|Δ| on water = {wmax:.3f}wu")
        fmax = float(np.abs(gdelta[foot]).max()) if foot.any() else 0.0
        chk("building_footprint_delta", "PASS" if fmax == 0 else "FAIL",
            f"max|Δ| on footprints = {fmax:.3f}wu (want 0)")

    # --- visual heightfield (visual_height_<F>x.r32), if present ---
    visual = None; factor = None
    rep = bdir / "visual_height_report.json"
    if rep.is_file():
        vmeta = json.loads(rep.read_text())
        factor = vmeta.get("factor", 4)
        vf = bdir / vmeta.get("visual_height_file", f"visual_height_{factor}x.r32")
        if vf.is_file():
            V = vmeta["visual_side"]
            visual = np.fromfile(vf, "<f4").reshape(V, V).astype(np.float64)
            cs = coarse_from_visual(visual, side, factor)
            cerr = float(np.abs(cs - elev).max())
            # Reshaped bakes intentionally move corners up to corner_clamp; bilinear
            # bakes must be exact. Read the bound from the report.
            clamp = (vmeta.get("reshape") or {}).get("corner_clamp_wu", 0.0)
            chk("corner_pin", "PASS" if cerr <= clamp + 1e-4 else "FAIL",
                f"max corner move = {cerr:.3g}wu (clamp {clamp:.1f})")
            # blockiness: compare visual (sampled at coarse) vs original — should be == (bilinear
            # preserves coarse), and visual-fine should be smoother than nearest-up.
            b_near = blockiness(nearest_up(elev, factor))
            b_vis = blockiness(visual)
            chk("blockiness_reduced", "PASS" if b_vis < b_near else "WARN",
                f"visual 2nd-diff var {b_vis:.2f} vs blocky {b_near:.2f} "
                f"({100*(1-b_vis/(b_near+1e-9)):.0f}% lower)")

    # --- contact sheet panels ---
    panels = []
    panels.append(label_panel(_resize(hillshade(elev)), "orig height (hillshade)"))
    if visual is not None:
        panels.append(label_panel(_resize(hillshade(visual)), f"visual height x{factor}"))
        d = visual - nearest_up(elev, factor)
        panels.append(label_panel(_resize(signed_delta_rgb(d)), "visual delta (r=up,b=down)"))
    if gdelta is not None:
        panels.append(label_panel(_resize(signed_delta_rgb(gdelta)), "gameplay delta"))
    panels.append(label_panel(_resize(colorize(masks["slope_deg"] > CLIFF_SLOPE_DEG, (230, 120, 40),
                                               hillshade(elev))), "cliff candidates"))
    panels.append(label_panel(_resize(colorize(pyr_mask, (255, 60, 255), hillshade(elev))), "pyramid islands"))
    panels.append(label_panel(_resize(colorize(protect_struct, (230, 40, 40),
                                               colorize(layers["water"], (40, 80, 220), hillshade(elev)))),
                              "protected (red struct / blue water)"))
    panels.append(label_panel(_resize(colorize(masks["shoreline"], (60, 220, 220),
                                               colorize(layers["water"], (30, 60, 160), hillshade(elev)))),
                              "water + shoreline"))
    sheet = compose_grid(panels, cols=4)

    # --- cross-section profiles: worst pyramid H/V + largest cliff ---
    prof_imgs = []
    PW, PH = 360, 200
    if pyramids:
        py = pyramids[0]["peak_rc"]
        r0, c0 = py
        xs = np.arange(side)
        ser = [("orig", elev[r0, :], (120, 200, 255))]
        if visual is not None:
            vrow = visual[r0 * factor, :][::1]   # full-res row at the pinned coarse row
            ser.append(("visual(fine)", np.interp(np.linspace(0, len(vrow) - 1, side), np.arange(len(vrow)), vrow), (255, 150, 80)))
        prof_imgs.append(profile_plot(PW, PH, f"pyramid#1 H-slice row={r0}", xs, ser))
        ser = [("orig", elev[:, c0], (120, 200, 255))]
        if visual is not None:
            vcol = visual[:, c0 * factor]
            ser.append(("visual(fine)", np.interp(np.linspace(0, len(vcol) - 1, side), np.arange(len(vcol)), vcol), (255, 150, 80)))
        prof_imgs.append(profile_plot(PW, PH, f"pyramid#1 V-slice col={c0}", np.arange(side), ser))
    # largest cliff candidate row
    cliffrows = np.where((masks["slope_deg"] > CLIFF_SLOPE_DEG).any(axis=1))[0]
    if len(cliffrows):
        rc = int(cliffrows[len(cliffrows) // 2])
        ser = [("orig", elev[rc, :], (120, 200, 255))]
        if visual is not None:
            vrow = visual[rc * factor, :]
            ser.append(("visual(fine)", np.interp(np.linspace(0, len(vrow) - 1, side), np.arange(len(vrow)), vrow), (255, 150, 80)))
        prof_imgs.append(profile_plot(PW, PH, f"cliff slice row={rc}", np.arange(side), ser))
    profiles = compose_grid([p.resize((PANEL * 2, PANEL)) for p in prof_imgs], cols=2) if prof_imgs else None

    out = out_root / mission
    out.mkdir(parents=True, exist_ok=True)
    sheet.save(out / "contact_sheet.png")
    if profiles is not None:
        profiles.save(out / "profiles.png")

    # summary status
    has_fail = any(c["status"] == "FAIL" for c in checks)
    has_warn = any(c["status"] == "WARN" for c in checks)
    summary = "FAIL" if has_fail else ("WARN" if has_warn else "PASS")

    report = {
        "mission": mission, "grid_side": int(side), "factor": factor,
        "summary": summary, "checks": checks,
        "pyramid_count": len(pyramids),
        "blockiness_orig": blockiness(elev),
    }
    (out / "workbench_report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")

    # HTML
    def b64(p):
        return base64.b64encode((out / p).read_bytes()).decode() if (out / p).is_file() else ""
    rows = "".join(
        f"<tr><td>{c['check']}</td><td style='color:{'#5f5' if c['status']=='PASS' else '#fd5' if c['status']=='WARN' else '#f55'}'>"
        f"{c['status']}</td><td>{c['detail']}</td></tr>" for c in checks)
    html = f"""<!doctype html><meta charset=utf-8><title>workbench {mission}</title>
<body style='background:#1a1a1f;color:#ddd;font-family:monospace'>
<h2>terrain workbench — {mission} ({side}^2, x{factor}) — <b style='color:{'#5f5' if summary=='PASS' else '#fd5' if summary=='WARN' else '#f55'}'>{summary}</b></h2>
<table border=1 cellpadding=4 style='border-collapse:collapse'>{rows}</table>
<h3>contact sheet</h3><img src='data:image/png;base64,{b64('contact_sheet.png')}'>
<h3>profiles</h3><img src='data:image/png;base64,{b64('profiles.png')}'>
</body>"""
    (out / "workbench.html").write_text(html, encoding="utf-8")
    return report


def main() -> int:
    ap = argparse.ArgumentParser(description="TERRAIN-WORKBENCH-REPORT-1 (offline)")
    ap.add_argument("missions", nargs="*", default=["mc2_01", "mc2_24"])
    ap.add_argument("--missions-dir", default="A:/Games/Carver5-feasibility/data/missions")
    ap.add_argument("--beauty", default="tests/terrain/beautify", help="dir holding <mission>.beauty/")
    ap.add_argument("--out", default="tests/terrain/workbench")
    args = ap.parse_args()
    rc = 0
    for m in args.missions:
        r = run(m, Path(args.missions_dir), Path(args.beauty), Path(args.out))
        if r.get("error"):
            print(f"[workbench] {m}: ERROR {r['error']}", file=sys.stderr); rc = 1; continue
        print(f"[workbench] {m}: {r['summary']}  " +
              "  ".join(f"{c['check']}={c['status']}" for c in r["checks"]) +
              f"  -> {Path(args.out) / m}/workbench.html")
        if r["summary"] == "FAIL":
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
