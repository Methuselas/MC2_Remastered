#!/usr/bin/env python3
"""residency_slim_report.py — full-deploy residency audit: classify every .tga,
determine slim safety, emit CSV, hard-fail on unsafe drop conditions.

Complements tools/residency_slim.py (which applies changes to data/tgl/128 only).
This script is read-only; it never modifies the deploy.

Classification uses filename heuristics supplemented by optional MC2_TEXMGR_LOAD_TRACE
output (captured by running MC2 with MC2_TEXMGR_LOAD_TRACE=1).

Residency classes (from docs/texture-residency-registry-recon.md):
  CPU_RGBA_REQUIRED        — mech/vehicle paint, UI, control GUI, effect LUT; must keep .tga
  GPU_SAMPLE_ONLY          — terrain detail/water/overlay/mine; safe to drop if .ktx2 exists
  LOAD_TGA_THEN_GPU_KTX   — static-prop batcher; safe to drop .tga if route-2 + .ktx2 exist
  GPU_ARRAY_PLUS_CPU_ALPHA — terrain PBR splat; direct TGA decode, bypasses loader; keep
  GPU_TILES_PLUS_CPU_FULL  — burnin colormap jpg/tga; cpuColorMap retained; keep
  LEGACY_TGA_ONLY          — no .ktx2 ever; keep
  unknown                  — cannot classify; keep (hard-fail if any drops would be inferred)

Hard-fail exit codes:
  0  — no violations; CSV emitted
  1  — classification error or unsafe drop condition detected
  2  — deploy root not found or other I/O error

CSV columns:
  texture,path,class,action,reason,bytes,has_ktx2,kept_cpu_source

Usage:
  py -3 scripts/residency_slim_report.py
  py -3 scripts/residency_slim_report.py --deploy A:/Games/mc2-opengl/mc2-win64-v0.4
  py -3 scripts/residency_slim_report.py --trace tests/smoke/artifacts/*/mc2_01.log
  py -3 scripts/residency_slim_report.py --out slim_report.csv
  py -3 scripts/residency_slim_report.py --summary-only   # no CSV, just counts + hard-fail
"""
from __future__ import annotations

import argparse
import csv
import os
import re
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------

DEFAULT_DEPLOY = Path("A:/Games/mc2-opengl/mc2-win64-v0.4")
DEFAULT_OUT = Path("slim_report.csv")

# ---------------------------------------------------------------------------
# Residency classes
# ---------------------------------------------------------------------------

RC_CPU_RGBA       = "CPU_RGBA_REQUIRED"
RC_GPU_SAMPLE     = "GPU_SAMPLE_ONLY"
RC_LOAD_TGA_KTX   = "LOAD_TGA_THEN_GPU_KTX"
RC_GPU_ARRAY_CPU  = "GPU_ARRAY_PLUS_CPU_ALPHA"
RC_GPU_TILES_CPU  = "GPU_TILES_PLUS_CPU_FULL"
RC_LEGACY         = "LEGACY_TGA_ONLY"
RC_UNKNOWN        = "unknown"

# Classes that must NEVER be dropped — hard-fail if action=DROP assigned.
NEVER_DROP = {RC_CPU_RGBA, RC_GPU_ARRAY_CPU, RC_GPU_TILES_CPU, RC_LEGACY, RC_UNKNOWN}

# ---------------------------------------------------------------------------
# Trace parsing
# ---------------------------------------------------------------------------

_TEXLOAD_RE = re.compile(r"\[TEXLOAD\]\s+uniq=(\d+)\s+name=(.+?)\s*$")


def load_traces(trace_paths: list[Path]) -> tuple[set[str], set[str]]:
    """Parse [TEXLOAD] lines.  Returns (paint_set, seen_set) of lowercased basenames.
    A texture is paint (CPU_RGBA_REQUIRED) iff ever loaded with uniqueInstance != 0."""
    paint: set[str] = set()
    seen: set[str] = set()
    for tp in trace_paths:
        try:
            with open(tp, encoding="utf-8", errors="ignore") as f:
                for line in f:
                    m = _TEXLOAD_RE.search(line)
                    if not m:
                        continue
                    uniq = int(m.group(1))
                    name = m.group(2).replace("\\", "/").lower()
                    base = os.path.basename(name)
                    if base.endswith(".tga"):
                        seen.add(base)
                        if uniq != 0:
                            paint.add(base)
        except OSError as e:
            print(f"[slim-report] WARNING: cannot read trace {tp}: {e}", file=sys.stderr)
    return paint, seen


# ---------------------------------------------------------------------------
# Per-path classification
# ---------------------------------------------------------------------------

def classify_tga(
    rel: str,
    basename_lower: str,
    stem_lower: str,
    has_ktx2: bool,
    paint_set: set[str],
    seen_set: set[str],
) -> tuple[str, str, str, bool]:
    """Return (residency_class, action, reason, kept_cpu_source)."""
    # --- Trace-based ground truth (highest priority) ---
    if basename_lower in paint_set:
        return RC_CPU_RGBA, "KEEP", "trace: loaded with uniqueInstance!=0 (paint)", True

    # --- burnin colormaps ---
    if ".burnin." in basename_lower:
        # *.burnin.jpg / *.burnin.tga retain cpuColorMap for HSV terrain typing.
        return RC_GPU_TILES_CPU, "KEEP", "burnin colormap: cpuColorMap retained for terrain typing", True

    # --- terrain PBR splat (mat*_normal.tga, mat*_displacement.tga, mat*_*.tga) ---
    rel_lower = rel.replace("\\", "/").lower()
    if _is_terrain_splat(rel_lower, stem_lower):
        return RC_GPU_ARRAY_CPU, "KEEP", "terrain splat: direct TGA decode bypasses loader (terrtxm2.cpp:2415)", True

    # --- UI / art (CPU reads for button atlas, StaticInfo dims, etc.) ---
    if _is_ui_art(rel_lower):
        return RC_CPU_RGBA, "KEEP", "UI/art: CPU pixel reads (controlgui.cpp, utilities.cpp)", True

    # --- mech / vehicle paint by name convention (*rgb.tga) ---
    if stem_lower.endswith("rgb"):
        return RC_CPU_RGBA, "KEEP", "mech/vehicle paint: *rgb convention (gos_LockTexture)", True

    # --- static-prop batcher textures (data/tgl/**/*.tga) ---
    if "/data/tgl/" in rel_lower or rel_lower.startswith("data/tgl/"):
        if not has_ktx2:
            return RC_LOAD_TGA_KTX, "KEEP", "static-prop: LOAD_TGA_THEN_GPU_KTX; no .ktx2 — cannot slim", False
        # Route-2 (MC2_TEXMGR_KTX_PRIMARY, default=1) provides .ktx2 fallback.
        # Trace observed → drop; unobserved → conservative keep.
        if seen_set and basename_lower not in seen_set:
            return RC_LOAD_TGA_KTX, "KEEP", "static-prop: unobserved in trace — conservative keep", False
        return RC_LOAD_TGA_KTX, "DROP", "static-prop: route-2 .ktx2 fallback active; .tga redundant", False

    # --- terrain detail, water, overlays, masks (GPU_SAMPLE_ONLY via loadTexture) ---
    if _is_gpu_sample_terrain(rel_lower, stem_lower):
        if not has_ktx2:
            return RC_GPU_SAMPLE, "KEEP", "GPU_SAMPLE_ONLY: no .ktx2 — cannot slim", False
        return RC_GPU_SAMPLE, "DROP", "GPU_SAMPLE_ONLY: .ktx2 exists; route-2 covers loader", False

    # --- insignia / mods ---
    if "/insignia/" in rel_lower or "/mods/" in rel_lower:
        return RC_LEGACY, "KEEP", "insignia/mod: not classified for slim", False

    # --- anything else ---
    return RC_UNKNOWN, "KEEP", "unknown: cannot classify; conservative keep", False


def _is_terrain_splat(rel_lower: str, stem_lower: str) -> bool:
    """mat[0-4]_*.tga in data/textures/ — direct TGA decoder."""
    import re as _re
    if "/data/textures/" in rel_lower or rel_lower.startswith("data/textures/"):
        if _re.match(r"mat\d+_", os.path.basename(stem_lower)):
            return True
    return False


def _is_ui_art(rel_lower: str) -> bool:
    return ("/data/art/" in rel_lower or rel_lower.startswith("data/art/") or
            "/data/terrain/" in rel_lower or rel_lower.startswith("data/terrain/"))


def _is_gpu_sample_terrain(rel_lower: str, stem_lower: str) -> bool:
    """Overlay, mask, detail, water, defaults in data/textures/ — via loadTexture."""
    if "/data/textures/" not in rel_lower and not rel_lower.startswith("data/textures/"):
        return False
    # Burnin and splat handled above — anything else in data/textures/ is GPU_SAMPLE.
    return True


# ---------------------------------------------------------------------------
# Scan deploy root
# ---------------------------------------------------------------------------

Row = tuple[str, str, str, str, str, int, bool, bool]  # noqa: E501


def scan_deploy(
    deploy: Path,
    paint_set: set[str],
    seen_set: set[str],
    skip_mods: bool = True,
) -> list[Row]:
    rows: list[Row] = []
    for root, dirs, files in os.walk(deploy):
        # Optionally skip mods/ to avoid noisy results from mod-specific textures.
        if skip_mods:
            dirs[:] = [d for d in dirs if d.lower() != "mods"]

        for fname in files:
            if not fname.lower().endswith(".tga"):
                continue
            full = os.path.join(root, fname)
            rel = os.path.relpath(full, deploy).replace("\\", "/")
            base_lower = fname.lower()
            stem_lower = os.path.splitext(base_lower)[0]

            # Check for same-stem .ktx2 sidecar (loose).
            ktx2_path = os.path.join(root, os.path.splitext(fname)[0] + ".ktx2")
            has_ktx2 = os.path.isfile(ktx2_path)

            try:
                size = os.path.getsize(full)
            except OSError:
                size = 0

            rc, action, reason, kept_cpu = classify_tga(
                rel, base_lower, stem_lower, has_ktx2, paint_set, seen_set
            )
            rows.append((fname, rel, rc, action, reason, size, has_ktx2, kept_cpu))
    return rows


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------

def emit_csv(rows: list[Row], out_path: Path) -> None:
    with open(out_path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["texture", "path", "class", "action", "reason",
                    "bytes", "has_ktx2", "kept_cpu_source"])
        for texture, path, rc, action, reason, size, has_ktx2, kept_cpu in rows:
            w.writerow([texture, path, rc, action, reason, size,
                        str(has_ktx2).lower(), str(kept_cpu).lower()])


def print_summary(rows: list[Row], violations: list[str]) -> None:
    by_class: dict[str, list[Row]] = {}
    for row in rows:
        by_class.setdefault(row[2], []).append(row)

    drop_rows = [r for r in rows if r[3] == "DROP"]
    keep_rows = [r for r in rows if r[3] == "KEEP"]
    drop_bytes = sum(r[5] for r in drop_rows)
    keep_bytes = sum(r[5] for r in keep_rows)

    print("[slim-report] SUMMARY")
    print(f"  total .tga files  : {len(rows)}")
    print(f"  action=DROP       : {len(drop_rows)}  ({drop_bytes / 1048576:.1f} MB recoverable)")
    print(f"  action=KEEP       : {len(keep_rows)}  ({keep_bytes / 1048576:.1f} MB)")
    print()
    for rc in [RC_CPU_RGBA, RC_GPU_SAMPLE, RC_LOAD_TGA_KTX,
               RC_GPU_ARRAY_CPU, RC_GPU_TILES_CPU, RC_LEGACY, RC_UNKNOWN]:
        rrows = by_class.get(rc, [])
        if not rrows:
            continue
        drop_c = sum(1 for r in rrows if r[3] == "DROP")
        keep_c = sum(1 for r in rrows if r[3] == "KEEP")
        print(f"  {rc:<32}  {len(rrows):5d}  ({drop_c} DROP, {keep_c} KEEP)")
    print()
    if violations:
        print(f"  HARD-FAIL — {len(violations)} violation(s):")
        for v in violations:
            print(f"    {v}")
    else:
        print("  no hard-fail violations")


# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------

def check_violations(rows: list[Row]) -> list[str]:
    """Return list of violation strings.  Empty = clean."""
    violations: list[str] = []
    for texture, path, rc, action, reason, size, has_ktx2, kept_cpu in rows:
        if action == "DROP" and rc in NEVER_DROP:
            violations.append(
                f"UNSAFE DROP: {path}  class={rc}  reason={reason}"
            )
    return violations


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    ap = argparse.ArgumentParser(
        description="Residency-aware slim audit: classify all .tga files in deploy, "
                    "emit CSV, hard-fail on unsafe drop conditions."
    )
    ap.add_argument(
        "--deploy", type=Path, default=DEFAULT_DEPLOY,
        help=f"Deploy root to scan (default: {DEFAULT_DEPLOY})",
    )
    ap.add_argument(
        "--trace", nargs="+", type=Path, default=[],
        metavar="LOG",
        help="MC2_TEXMGR_LOAD_TRACE=1 log file(s). Improves classification accuracy.",
    )
    ap.add_argument(
        "--out", type=Path, default=None,
        metavar="CSV",
        help=f"CSV output path (default: <deploy>/{DEFAULT_OUT.name}; use - for stdout)",
    )
    ap.add_argument(
        "--summary-only", action="store_true",
        help="Print summary to stdout; do not write CSV.",
    )
    ap.add_argument(
        "--include-mods", action="store_true",
        help="Scan mods/ subdirectory (excluded by default — mod textures vary).",
    )
    ap.add_argument(
        "--filter-action",
        choices=["DROP", "KEEP"],
        help="Restrict CSV output to this action only.",
    )
    args = ap.parse_args()

    deploy: Path = args.deploy.resolve()
    if not deploy.is_dir():
        print(f"[slim-report] FAIL: deploy root not found: {deploy}", file=sys.stderr)
        sys.exit(2)

    # Load traces if provided.
    paint_set: set[str] = set()
    seen_set: set[str] = set()
    if args.trace:
        paint_set, seen_set = load_traces(args.trace)
        print(f"[slim-report] trace: {len(seen_set)} observed, "
              f"{len(paint_set)} painted (CPU_RGBA_REQUIRED override)")

    if not args.trace:
        print(
            "[slim-report] WARNING: no --trace provided. CPU_RGBA_REQUIRED detection "
            "relies on the *rgb filename heuristic only (mech3d.cpp:1825 pattern). "
            "Vehicle paint and other non-*rgb CPU-locked textures may be misclassified "
            "as safe to DROP. Run with MC2_TEXMGR_LOAD_TRACE=1 for ground truth.",
            file=sys.stderr,
        )

    print(f"[slim-report] scanning {deploy} ...")
    rows = scan_deploy(deploy, paint_set, seen_set, skip_mods=not args.include_mods)
    print(f"[slim-report] {len(rows)} .tga files classified")

    violations = check_violations(rows)

    # Apply filter before CSV.
    out_rows = rows
    if args.filter_action:
        out_rows = [r for r in rows if r[3] == args.filter_action]

    # Output.
    if not args.summary_only:
        if args.out and str(args.out) == "-":
            # Stdout CSV.
            import io
            buf = io.StringIO()
            w = csv.writer(buf)
            w.writerow(["texture", "path", "class", "action", "reason",
                        "bytes", "has_ktx2", "kept_cpu_source"])
            for texture, path, rc, action, reason, size, has_ktx2, kept_cpu in out_rows:
                w.writerow([texture, path, rc, action, reason, size,
                            str(has_ktx2).lower(), str(kept_cpu).lower()])
            print(buf.getvalue(), end="")
        else:
            out_path = args.out if args.out else (deploy / DEFAULT_OUT.name)
            emit_csv(out_rows, out_path)
            print(f"[slim-report] CSV: {out_path}  ({len(out_rows)} rows)")

    print_summary(rows, violations)

    if violations:
        sys.exit(1)


if __name__ == "__main__":
    main()
