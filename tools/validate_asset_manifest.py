#!/usr/bin/env python3
"""validate_asset_manifest.py -- validate an asset manifest JSON.

VALIDATION-SCAFFOLD-PREFLIGHT-1 / ASSET-MANIFEST-SCHEMA-SCAFFOLD-1
  + ASSET-MANIFEST-0-EXTEND       (optional geometry/provenance/generatedOutputs
                                   + LOD-stat / textureRef extended-field shapes)
  + MATERIAL-AUTHORING-VALIDATION-1 (material authoring metadata + colorSpace
                                   conventions + normal->tangent cross-check)

A schema-validation FOUNDATION for the asset-pipeline probe lane
(TRACKG-ASSET-PIPELINE-PROBE-OPUS-1). It checks the SHAPE and authoring
INVARIANTS of a manifest, NOT the existence of real asset files (those are not
committed; see .gitignore). No asset import, no KTX2 bake, no runtime -- pure
JSON validation.

REQUIRED core (one manifest object) -- unchanged from the scaffold:
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

OPTIONAL sections (ASSET-MANIFEST-0-EXTEND) -- validated for shape only when
present; a manifest without them stays valid (backward compatible):
  geometry          : object -- the ASSIMP-IMPORTER-PHASE-0 output contract:
                        meshCount, vertexCount, indexCount, materialSlotCount (ints),
                        hasNormals, hasTangents (bools), bounds {min[3],max[3],radius}
  lods[] items      : may be objects {level:int, vertexCount:int, triangleCount:int,
                        error?:float} -- the MESHOPT LOD-stat contract
  textureRefs[] item: requires slot (string) + path (string); optional cooked-asset
                        fields colorSpace/format/vkFormat/mips/dims/cooked
  provenance        : object -- {tool, toolVersion, generatedAt, ...} string-ish
  generatedOutputs  : array of {path:string, kind:string}

OPTIONAL material authoring (MATERIAL-AUTHORING-VALIDATION-1) -- when present:
  materials[] item  : name (string) + shader (string) required; optional
                        alphaMode in {opaque,alphaTest,blend}, alphaTestThreshold,
                        doubleSided (bool), pbr {baseColorFactor, metallicFactor,
                        roughnessFactor in [0,1]}
  colorSpace        : per-slot convention (albedo/emissive=srgb; normal/orm/mask=linear)
  cross-check       : any textureRef slot "normal" => capabilities.hasTangents true

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

# Texture slot vocabulary, reconciled with tools/mc2texcook/mc2texcook.py presets
# and RenderCore/MaterialGpu.h. Each slot has a required cook color space.
SLOT_COLORSPACE = {
    "albedo": "srgb",
    "emissive": "srgb",
    "normal": "linear",
    "orm": "linear",  # R=AO G=roughness B=metalness (mc2texcook 'orm' preset)
    "mask": "linear",
}

ALPHA_MODES = ("opaque", "alphaTest", "blend")


def _is_number(v) -> bool:
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def _in_unit(v) -> bool:
    return _is_number(v) and 0.0 <= float(v) <= 1.0


def _validate_geometry(g, errs: list) -> None:
    """ASSIMP-IMPORTER-PHASE-0 output contract (shape only)."""
    if not isinstance(g, dict):
        errs.append("'geometry' must be an object")
        return
    for key in ("meshCount", "vertexCount", "indexCount", "materialSlotCount"):
        if key in g and not (isinstance(g[key], int) and not isinstance(g[key], bool)):
            errs.append(f"geometry.{key} must be an integer")
    for key in ("hasNormals", "hasTangents"):
        if key in g and not isinstance(g[key], bool):
            errs.append(f"geometry.{key} must be a boolean")
    if "bounds" in g:
        b = g["bounds"]
        if not isinstance(b, dict):
            errs.append("geometry.bounds must be an object")
        else:
            for axis in ("min", "max"):
                if axis in b:
                    v = b[axis]
                    if not (isinstance(v, list) and len(v) == 3 and all(_is_number(x) for x in v)):
                        errs.append(f"geometry.bounds.{axis} must be a 3-number array")
            if "radius" in b and not _is_number(b["radius"]):
                errs.append("geometry.bounds.radius must be a number")


def _validate_lods(lods, errs: list) -> None:
    """lods[] entries may be plain (legacy) or LOD-stat objects (MESHOPT contract)."""
    for i, lod in enumerate(lods):
        if not isinstance(lod, dict):
            continue  # legacy / opaque entries permitted
        for key in ("level", "vertexCount", "triangleCount"):
            if key in lod and not (isinstance(lod[key], int) and not isinstance(lod[key], bool)):
                errs.append(f"lods[{i}].{key} must be an integer")
        if "error" in lod and not _is_number(lod["error"]):
            errs.append(f"lods[{i}].error must be a number")


def _validate_texture_refs(refs, errs: list) -> set:
    """Require slot+path; validate cooked-asset shape + colorSpace convention.

    Returns the set of slots seen (used for the normal->tangent cross-check).
    """
    slots_seen = set()
    for i, ref in enumerate(refs):
        if not isinstance(ref, dict):
            errs.append(f"textureRefs[{i}] must be an object")
            continue
        slot = ref.get("slot")
        if not isinstance(slot, str) or not slot:
            errs.append(f"textureRefs[{i}] missing/empty required string 'slot'")
        else:
            slots_seen.add(slot)
            if slot not in SLOT_COLORSPACE:
                errs.append(
                    f"textureRefs[{i}].slot '{slot}' unknown "
                    f"(expected one of {sorted(SLOT_COLORSPACE)})"
                )
        if not isinstance(ref.get("path"), str) or not ref.get("path"):
            errs.append(f"textureRefs[{i}] missing/empty required string 'path'")

        # MATERIAL-AUTHORING-VALIDATION-1: colorSpace must match the slot convention.
        if "colorSpace" in ref:
            cs = ref["colorSpace"]
            if cs not in ("srgb", "linear"):
                errs.append(f"textureRefs[{i}].colorSpace must be 'srgb' or 'linear'")
            elif isinstance(slot, str) and slot in SLOT_COLORSPACE and cs != SLOT_COLORSPACE[slot]:
                errs.append(
                    f"textureRefs[{i}] slot '{slot}' requires colorSpace "
                    f"'{SLOT_COLORSPACE[slot]}', got '{cs}'"
                )

        # ASSET-MANIFEST-0-EXTEND: optional cooked-asset metadata shape.
        if "vkFormat" in ref and not (isinstance(ref["vkFormat"], int) and not isinstance(ref["vkFormat"], bool)):
            errs.append(f"textureRefs[{i}].vkFormat must be an integer")
        if "mips" in ref and not (isinstance(ref["mips"], int) and not isinstance(ref["mips"], bool)):
            errs.append(f"textureRefs[{i}].mips must be an integer")
        if "dims" in ref:
            d = ref["dims"]
            if not (isinstance(d, list) and len(d) == 2 and all(isinstance(x, int) and not isinstance(x, bool) for x in d)):
                errs.append(f"textureRefs[{i}].dims must be a 2-integer array")
        if "format" in ref and not isinstance(ref["format"], str):
            errs.append(f"textureRefs[{i}].format must be a string")
    return slots_seen


def _validate_materials(materials, errs: list) -> None:
    """MATERIAL-AUTHORING-VALIDATION-1: authoring metadata for each material."""
    for i, mat in enumerate(materials):
        if not isinstance(mat, dict):
            errs.append(f"materials[{i}] must be an object")
            continue
        if not isinstance(mat.get("name"), str) or not mat.get("name"):
            errs.append(f"materials[{i}] missing/empty required string 'name'")
        if not isinstance(mat.get("shader"), str) or not mat.get("shader"):
            errs.append(f"materials[{i}] missing/empty required string 'shader'")

        if "alphaMode" in mat:
            am = mat["alphaMode"]
            if am not in ALPHA_MODES:
                errs.append(f"materials[{i}].alphaMode must be one of {list(ALPHA_MODES)}")
            if "alphaTestThreshold" in mat and am != "alphaTest":
                errs.append(
                    f"materials[{i}].alphaTestThreshold is only valid when alphaMode='alphaTest'"
                )
        if "alphaTestThreshold" in mat and not _in_unit(mat["alphaTestThreshold"]):
            errs.append(f"materials[{i}].alphaTestThreshold must be a number in [0,1]")
        if "doubleSided" in mat and not isinstance(mat["doubleSided"], bool):
            errs.append(f"materials[{i}].doubleSided must be a boolean")

        if "pbr" in mat:
            pbr = mat["pbr"]
            if not isinstance(pbr, dict):
                errs.append(f"materials[{i}].pbr must be an object")
            else:
                bcf = pbr.get("baseColorFactor")
                if bcf is not None:
                    ok = _in_unit(bcf) or (
                        isinstance(bcf, list)
                        and len(bcf) in (3, 4)
                        and all(_in_unit(x) for x in bcf)
                    )
                    if not ok:
                        errs.append(
                            f"materials[{i}].pbr.baseColorFactor must be a [0,1] number "
                            f"or a 3-4 element [0,1] array"
                        )
                for key in ("metallicFactor", "roughnessFactor"):
                    if key in pbr and not _in_unit(pbr[key]):
                        errs.append(f"materials[{i}].pbr.{key} must be a number in [0,1]")


def _validate_provenance(p, errs: list) -> None:
    if not isinstance(p, dict):
        errs.append("'provenance' must be an object")
        return
    for key in ("tool", "toolVersion", "generatedAt", "sourceHash"):
        if key in p and not isinstance(p[key], str):
            errs.append(f"provenance.{key} must be a string")


def _validate_generated_outputs(outs, errs: list) -> None:
    if not isinstance(outs, list):
        errs.append("'generatedOutputs' must be an array")
        return
    for i, o in enumerate(outs):
        if not isinstance(o, dict):
            errs.append(f"generatedOutputs[{i}] must be an object")
            continue
        for key in ("path", "kind"):
            if not isinstance(o.get(key), str) or not o.get(key):
                errs.append(f"generatedOutputs[{i}] missing/empty required string '{key}'")


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

    # --- Optional sections (validated only when present) ---
    if isinstance(m.get("materials"), list):
        _validate_materials(m["materials"], errs)
    if isinstance(m.get("lods"), list):
        _validate_lods(m["lods"], errs)

    slots_seen = set()
    if isinstance(m.get("textureRefs"), list):
        slots_seen = _validate_texture_refs(m["textureRefs"], errs)

    if "geometry" in m:
        _validate_geometry(m["geometry"], errs)
    if "provenance" in m:
        _validate_provenance(m["provenance"], errs)
    if "generatedOutputs" in m:
        _validate_generated_outputs(m["generatedOutputs"], errs)

    # Cross-check: a normal map needs tangents.
    if "normal" in slots_seen and isinstance(caps, dict):
        if caps.get("hasTangents") is not True:
            errs.append(
                "textureRefs has a 'normal' slot but capabilities.hasTangents is not true "
                "(normal mapping requires tangents)"
            )

    return errs


def main() -> int:
    ap = argparse.ArgumentParser(description="Validate an asset manifest JSON.")
    ap.add_argument("manifest", help="Path to the manifest JSON to validate.")
    ap.add_argument("--quiet", action="store_true", help="Suppress PASS output.")
    ap.add_argument(
        "--expect-fail",
        action="store_true",
        help="Invert: exit 0 only if the manifest is INVALID (for negative fixtures).",
    )
    args = ap.parse_args()

    path = Path(args.manifest)
    if not path.exists():
        print(f"{PREFIX} ERROR manifest not found: {path}", flush=True)
        return 1
    try:
        m = json.loads(path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError) as e:
        print(f"{PREFIX} FAIL {path.name}: JSON parse failed: {e}", flush=True)
        return 0 if args.expect_fail else 1
    if not isinstance(m, dict):
        print(f"{PREFIX} FAIL {path.name}: top-level must be an object", flush=True)
        return 0 if args.expect_fail else 1

    errs = validate_manifest(m)

    if args.expect_fail:
        if errs:
            if not args.quiet:
                print(f"{PREFIX} XFAIL {path.name}: invalid as expected ({len(errs)} error(s))", flush=True)
            return 0
        print(f"{PREFIX} FAIL {path.name}: expected invalid but it validated", file=sys.stderr, flush=True)
        return 1

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
