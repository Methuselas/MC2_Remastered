#!/usr/bin/env python3
"""validate_asset_manifest.py -- validate an asset manifest JSON.

VALIDATION-SCAFFOLD-PREFLIGHT-1 / ASSET-MANIFEST-SCHEMA-SCAFFOLD-1.

A tiny schema-validation FOUNDATION for the future asset-pipeline probe lane
(TRACKG-ASSET-PIPELINE-PROBE-OPUS-1). It checks the SHAPE of a manifest, not the
existence of real asset files (those are not committed; see docs gitignore).
No asset import, no KTX2 bake, no runtime — pure JSON schema check.

Schema (one manifest object):
  assetId        : non-empty string (stable identity)
  source         : non-empty string (source asset path, relative)
  kind | type    : non-empty string (one of them required; e.g. "mesh","prop","mech")
  materials      : array (may be empty)
  capabilities   : object of the six booleans below (all required, all bool)
  lods           : array (may be empty -> explicit "no LODs")
  textureRefs    : array (may be empty -> explicit "no textures")

Capability booleans (vocabulary reconciled with RenderCore/RenderObjectDesc.h
ArchetypeFlags so manifest terms match the renderer):
  hasNormals       (vertex attribute; validator-only, no engine flag)
  hasTangents      (vertex attribute; validator-only, no engine flag)
  hasLods       -> ArchetypeFlags.hasClusterLod
  hasImpostor   -> ArchetypeFlags.usesImpostor
  castsShadow   -> ArchetypeFlags.castsShadow
  supportsObjectId (relates to RenderObjectDesc.gameObjectId / object-ID buffer)

Usage:
  py -3 tools/validate_asset_manifest.py tests/fixtures/assets/minimal_asset_manifest.json
  py -3 tools/validate_asset_manifest.py <path> --quiet

Exit 0 = valid. Exit 1 = invalid or usage error.
"""

import argparse
import json
import sys
from pathlib import Path

PREFIX = "[ASSET_MANIFEST]"

REQUIRED_CAPABILITIES = (
    "hasNormals",
    "hasTangents",
    "hasLods",
    "hasImpostor",
    "castsShadow",
    "supportsObjectId",
)


def validate_manifest(m: dict) -> list:
    """Return a list of error strings (empty = valid)."""
    errs = []

    def req_str(key):
        v = m.get(key)
        if not isinstance(v, str) or not v:
            errs.append(f"missing/empty required string field '{key}'")

    req_str("assetId")
    req_str("source")

    # kind OR type (accept either spelling)
    kind = m.get("kind", m.get("type"))
    if not isinstance(kind, str) or not kind:
        errs.append("missing/empty required field 'kind' (or 'type')")

    for key in ("materials", "lods", "textureRefs"):
        if key not in m:
            errs.append(f"missing required field '{key}' (use an explicit empty array if none)")
        elif not isinstance(m[key], list):
            errs.append(f"'{key}' must be an array")

    caps = m.get("capabilities")
    if not isinstance(caps, dict):
        errs.append("missing/invalid required field 'capabilities' (object)")
    else:
        for cap in REQUIRED_CAPABILITIES:
            if cap not in caps:
                errs.append(f"capabilities.{cap} is required")
            elif not isinstance(caps[cap], bool):
                errs.append(f"capabilities.{cap} must be a boolean")
        extra = set(caps) - set(REQUIRED_CAPABILITIES)
        if extra:
            errs.append(f"capabilities has unknown keys: {sorted(extra)}")

    return errs


def main() -> int:
    ap = argparse.ArgumentParser(description="Validate an asset manifest JSON.")
    ap.add_argument("manifest", help="Path to the manifest JSON to validate.")
    ap.add_argument("--quiet", action="store_true", help="Suppress PASS output.")
    args = ap.parse_args()

    path = Path(args.manifest)
    if not path.exists():
        print(f"{PREFIX} ERROR manifest not found: {path}", flush=True)
        return 1
    try:
        m = json.loads(path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError) as e:
        print(f"{PREFIX} FAIL {path.name}: JSON parse failed: {e}", flush=True)
        return 1
    if not isinstance(m, dict):
        print(f"{PREFIX} FAIL {path.name}: top-level must be an object", flush=True)
        return 1

    errs = validate_manifest(m)
    if errs:
        print(f"{PREFIX} FAIL {path.name}: {len(errs)} error(s)", file=sys.stderr, flush=True)
        for e in errs:
            print(f"  - {e}", file=sys.stderr, flush=True)
        return 1

    if not args.quiet:
        print(f"{PREFIX} PASS {path.name} assetId={m.get('assetId')} "
              f"kind={m.get('kind', m.get('type'))} "
              f"materials={len(m.get('materials', []))} lods={len(m.get('lods', []))} "
              f"textureRefs={len(m.get('textureRefs', []))}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
