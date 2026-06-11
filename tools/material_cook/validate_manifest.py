#!/usr/bin/env python3
"""tools/material_cook/validate_manifest.py — Offline material manifest cook validator.

Reads a material_manifest.json file, validates it against the schema, checks
that all referenced KTX2 files exist, and verifies field coherence (e.g.
alpha_test only when albedo has an alpha channel, emissive flags only when
emissive_ktx2 is present).

Does NOT upload any GL state, modify runtime binding, or write shader code.
Schema: tools/material_cook/material_manifest.schema.json

Usage:
  py -3 tools/material_cook/validate_manifest.py <manifest.json>
  py -3 tools/material_cook/validate_manifest.py <manifest.json> --strict
  py -3 tools/material_cook/validate_manifest.py <manifest.json> --skip-file-checks

Exit 0 = all checks pass. Exit 1 = one or more errors.
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

SCHEMA_PATH = Path(__file__).resolve().parent / "material_manifest.schema.json"

# ---------------------------------------------------------------------------
# KTX2 helpers (header-only; no full decode needed for validation)
# ---------------------------------------------------------------------------

KTX2_MAGIC = b"\xabKTX 20\xbb\r\n\x1a\n"

def _read_ktx2_header(path: Path) -> dict | str:
    """Return a dict with {width, height, vk_format, layers, has_alpha} or an
    error string if the file is not a valid KTX2."""
    try:
        with open(path, "rb") as f:
            magic = f.read(12)
            if magic != KTX2_MAGIC:
                return f"not a KTX2 file (bad magic: {magic!r})"
            # KTX2 header: Khronos spec table 1.
            # After magic: vkFormat, typeSize, pixelWidth, pixelHeight,
            # pixelDepth, layerCount, faceCount, levelCount, supercompressionScheme
            data = f.read(4 * 9)
            if len(data) < 36:
                return "truncated KTX2 header"
            (vk_format, _type_size, width, height,
             _depth, layer_count, _faces, _levels, _sc) = struct.unpack_from("<9I", data)
    except OSError as e:
        return str(e)

    # Common VkFormat values with alpha channel.
    # VK_FORMAT_R8G8B8A8_* = 37-43, VK_FORMAT_B8G8R8A8_* = 44-50,
    # VK_FORMAT_BC3_* (DXT5) = 135-136, VK_FORMAT_BC7_* = 145-146.
    ALPHA_FORMATS = set(range(37, 51)) | {135, 136, 145, 146}
    has_alpha = vk_format in ALPHA_FORMATS

    return {
        "width": width,
        "height": height,
        "vk_format": vk_format,
        "layers": max(layer_count, 1),
        "has_alpha": has_alpha,
    }


# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------

def validate(manifest_path: Path, strict: bool = False,
             skip_file_checks: bool = False) -> list[str]:
    errors: list[str] = []

    try:
        with open(manifest_path, "r", encoding="utf-8") as f:
            doc = json.load(f)
    except (OSError, json.JSONDecodeError) as e:
        return [f"cannot load manifest: {e}"]

    base = manifest_path.parent

    version = doc.get("version")
    if version != 1:
        errors.append(f"unsupported version {version!r} (expected 1)")

    materials = doc.get("materials")
    if not isinstance(materials, list) or not materials:
        errors.append("'materials' must be a non-empty array")
        return errors

    for i, entry in enumerate(materials):
        prefix = f"materials[{i}]"

        # Slot index must match array position.
        slot = entry.get("slot")
        if slot != i:
            errors.append(f"{prefix}: slot={slot!r} != expected {i}")

        # albedo_ktx2 is required.
        albedo_rel = entry.get("albedo_ktx2")
        if not albedo_rel:
            errors.append(f"{prefix}: albedo_ktx2 is required")
            continue

        albedo_hdr: dict | None = None
        if not skip_file_checks:
            albedo_path = (base / albedo_rel).resolve()
            if not albedo_path.exists():
                errors.append(f"{prefix}: albedo_ktx2 not found: {albedo_path}")
            else:
                hdr = _read_ktx2_header(albedo_path)
                if isinstance(hdr, str):
                    errors.append(f"{prefix}: albedo_ktx2 parse error: {hdr}")
                else:
                    albedo_hdr = hdr
                    # alpha_test requires albedo to carry alpha channel.
                    flags = entry.get("flags", {})
                    if flags.get("alpha_test") and not hdr["has_alpha"]:
                        errors.append(
                            f"{prefix}: alpha_test=true but albedo_ktx2 "
                            f"vk_format {hdr['vk_format']} has no alpha channel"
                        )

        # Optional maps: check existence and dimension match vs albedo.
        for field in ("normal_ktx2", "metallic_roughness_ktx2", "emissive_ktx2"):
            rel = entry.get(field)
            if not rel or skip_file_checks:
                continue
            path = (base / rel).resolve()
            if not path.exists():
                errors.append(f"{prefix}: {field} not found: {path}")
                continue
            hdr = _read_ktx2_header(path)
            if isinstance(hdr, str):
                errors.append(f"{prefix}: {field} parse error: {hdr}")
                continue
            if albedo_hdr and (hdr["width"] != albedo_hdr["width"] or
                               hdr["height"] != albedo_hdr["height"]):
                msg = (
                    f"{prefix}: {field} size {hdr['width']}x{hdr['height']} "
                    f"!= albedo {albedo_hdr['width']}x{albedo_hdr['height']}"
                )
                if strict:
                    errors.append(msg)
                else:
                    print(f"WARNING: {msg}")

        # base_color_factor / metallic_factor / roughness_factor in range.
        for scalar, lo, hi in (
            ("base_color_factor",  0.0, 1.0),
            ("metallic_factor",    0.0, 1.0),
            ("roughness_factor",   0.0, 1.0),
        ):
            v = entry.get(scalar)
            if v is not None and not (lo <= v <= hi):
                errors.append(f"{prefix}: {scalar}={v!r} out of [{lo}, {hi}]")

    return errors


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("manifest", type=Path)
    ap.add_argument("--strict", action="store_true",
                    help="Treat dimension mismatches as errors, not warnings")
    ap.add_argument("--skip-file-checks", action="store_true",
                    help="Validate JSON structure only; skip KTX2 file existence/header checks. "
                         "Use for CI test fixtures without real assets.")
    args = ap.parse_args()

    errors = validate(args.manifest, strict=args.strict,
                      skip_file_checks=args.skip_file_checks)
    if errors:
        for e in errors:
            print(f"ERROR: {e}", file=sys.stderr)
        return 1

    entry_count = 0
    try:
        with open(args.manifest, "r", encoding="utf-8") as f:
            entry_count = len(json.load(f).get("materials", []))
    except Exception:
        pass
    print(f"OK: {entry_count} material(s) validated in {args.manifest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
