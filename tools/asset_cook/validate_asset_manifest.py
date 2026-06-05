#!/usr/bin/env python3
"""tools/asset_cook/validate_asset_manifest.py — Offline cooked-asset manifest validator.

Validates an mc2-asset-manifest-v1 file against the schema, then enforces
cross-field coherence the schema cannot express (slot==index, replaces composite,
lowercase class, alphaClass<->alpha_test<->'a_' prefix agreement, capability
consistency, bounds ordering, LOD ordering). Optionally checks referenced files.

Does NOT cook, upload GL state, modify runtime binding, or write the central
models.json. Pure read+validate.

Schema: tools/asset_cook/asset_manifest.schema.json

Usage:
  py -3 tools/asset_cook/validate_asset_manifest.py <manifest.json>
  py -3 tools/asset_cook/validate_asset_manifest.py <manifest.json> --check-files
  py -3 tools/asset_cook/validate_asset_manifest.py <manifest.json> --deploy-root <dir>

Exit 0 = all checks pass. Exit 1 = one or more errors.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

try:
    import jsonschema
except ImportError:
    print("ERROR: jsonschema not installed (py -3 -m pip install jsonschema)", file=sys.stderr)
    sys.exit(2)

SCHEMA_PATH = Path(__file__).resolve().parent / "asset_manifest.schema.json"


def _is_lower(s: str) -> bool:
    return s == s.lower()


def coherence_errors(m: dict) -> list[str]:
    """Cross-field checks the JSON schema cannot express."""
    errs: list[str] = []

    asset = m.get("asset", {})
    cls = asset.get("class", "")
    appn = asset.get("appearanceName", "")
    replaces = asset.get("replaces", "")

    # 1. lowercase identity (Patch 5 lock: canonical lowercase 'staticprop'/'tree')
    for label, val in (("class", cls), ("appearanceName", appn), ("replaces", replaces)):
        if val and not _is_lower(val):
            errs.append(f"asset.{label} must be lowercase (got {val!r}); registry normalizeKey lowercases, manifest must emit lowercase")

    # 2. replaces composite == '<class>:<appearanceName>'
    expected = f"{cls}:{appn}"
    if replaces != expected:
        errs.append(f"asset.replaces {replaces!r} != '<class>:<appearanceName>' ({expected!r})")

    geom = m.get("geometry", {})

    # 3. bounds ordering min<=max per axis
    b = geom.get("bounds", {})
    bmin, bmax = b.get("min", []), b.get("max", [])
    if len(bmin) == 3 and len(bmax) == 3:
        for k, axis in enumerate("xyz"):
            if bmin[k] > bmax[k]:
                errs.append(f"geometry.bounds.min.{axis} ({bmin[k]}) > max.{axis} ({bmax[k]})")

    # 4. LOD chain strictly ascending, lod>=1
    lods = geom.get("lods", []) or []
    prev = 0
    for i, lod in enumerate(lods):
        n = lod.get("lod", 0)
        if n <= prev:
            errs.append(f"geometry.lods[{i}].lod ({n}) not strictly ascending (prev {prev}); LOD0==cooked")
        prev = n

    # 5. material slot==index + alphaClass<->alpha_test<->'a_' agreement
    mats = m.get("materials", [])
    any_alpha = False
    for i, mat in enumerate(mats):
        slot = mat.get("slot")
        if slot != i:
            errs.append(f"materials[{i}].slot ({slot}) != index {i} (no gaps/reorder)")
        ac = mat.get("alphaClass", 0)
        at = bool(mat.get("flags", {}).get("alpha_test", False))
        tn = mat.get("textureName", "")
        has_a = tn.startswith("a_")
        if ac == 1:
            any_alpha = True
        # all three signals must agree
        if (ac == 1) != at:
            errs.append(f"materials[{i}]: alphaClass={ac} but flags.alpha_test={at} (must agree)")
        if (ac == 1) != has_a:
            errs.append(f"materials[{i}]: alphaClass={ac} but textureName {tn!r} {'has' if has_a else 'lacks'} 'a_' prefix (resolver convention: a_ iff alpha)")

    # 6. capability consistency
    caps = m.get("capabilities", {})
    if "alphaTest" in caps and caps["alphaTest"] != any_alpha:
        errs.append(f"capabilities.alphaTest ({caps['alphaTest']}) != any material alphaClass==1 ({any_alpha})")
    if "hasLodChain" in caps and caps["hasLodChain"] != (len(lods) > 0):
        errs.append(f"capabilities.hasLodChain ({caps['hasLodChain']}) != (geometry.lods non-empty: {len(lods) > 0})")

    # 7. deps.stockFallback == appearanceName (if present)
    deps = m.get("deps", {})
    sf = deps.get("stockFallback")
    if sf is not None and sf != appn:
        errs.append(f"deps.stockFallback ({sf!r}) != asset.appearanceName ({appn!r})")

    return errs


def file_errors(m: dict, manifest_path: Path, deploy_root: Path | None) -> list[str]:
    """Optional: referenced files exist. cooked glb is relative to manifest dir;
    source + texture tier paths are relative to deploy_root (or manifest dir)."""
    errs: list[str] = []
    mdir = manifest_path.resolve().parent
    root = deploy_root.resolve() if deploy_root else mdir

    geom = m.get("geometry", {})
    cooked = geom.get("cooked")
    if cooked and not (mdir / cooked).exists():
        errs.append(f"geometry.cooked not found: {mdir / cooked}")
    src = geom.get("source")
    if src and not (root / src).exists():
        errs.append(f"geometry.source not found: {root / src}")

    for i, mat in enumerate(m.get("materials", [])):
        for tier, p in (mat.get("albedo_ktx2") or {}).items():
            if not (root / p).exists():
                errs.append(f"materials[{i}].albedo_ktx2.{tier} not found: {root / p}")
    return errs


def main() -> int:
    ap = argparse.ArgumentParser(description="Validate an mc2-asset-manifest-v1 file.")
    ap.add_argument("manifest", type=Path)
    ap.add_argument("--check-files", action="store_true", help="verify referenced glb/ktx2 exist")
    ap.add_argument("--deploy-root", type=Path, default=None, help="root for source/texture relative paths")
    args = ap.parse_args()

    try:
        schema = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))
    except OSError as e:
        print(f"ERROR: cannot read schema: {e}", file=sys.stderr)
        return 2
    try:
        m = json.loads(args.manifest.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as e:
        print(f"FAIL {args.manifest}: cannot read/parse: {e}")
        return 1

    errors: list[str] = []
    try:
        jsonschema.validate(m, schema)
    except jsonschema.ValidationError as e:
        loc = "/".join(str(x) for x in e.absolute_path) or "<root>"
        errors.append(f"schema: {loc}: {e.message}")

    # coherence checks only meaningful if the doc is structurally a dict
    if isinstance(m, dict):
        errors.extend(coherence_errors(m))
        if args.check_files:
            errors.extend(file_errors(m, args.manifest, args.deploy_root))

    if errors:
        print(f"FAIL {args.manifest} ({len(errors)} error(s)):")
        for e in errors:
            print(f"  - {e}")
        return 1
    print(f"PASS {args.manifest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
