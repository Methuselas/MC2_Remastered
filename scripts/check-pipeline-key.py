#!/usr/bin/env python3
"""check-pipeline-key.py — PIPELINE-KEY-SCHEMA-1 consistency gate

Cross-validates the repo-owned logical PipelineKey schema against the two
contracts it is built from, so the schema cannot silently drift from reality:

  - docs/render-backend-seams/pipeline-key-schema.json     (this schema)
  - docs/render-backend-seams/shader-permutation-inventory.json  (variant macros)
  - RenderCore/PipelineRegistry.h                          (registered pipelines)

Asserts (FAIL):
  1. Every shaderVariantId macro named by any pipeline exists in the shader
     inventory (governance link — no phantom variant macros in a key).
  2. Every specializationParam (field-level + the params list) is typed.
  3. Every PipelineKey field declares a status in the allowed enum (no silent
     "is this authoritative?" — missing PSO fields must be explicit).
  4. The schema's registered_pipelines set matches the non-Invalid PipelineId
     enum exactly (no stale/missing pipeline row).

Read-only. No Vulkan, no PSO impl. Companion: pipeline-key-schema.md.

Usage:
  py -3 scripts/check-pipeline-key.py [--root <repo>] [--json <out>] [--quiet]
"""
import argparse
import json
import os
import re
import sys

SCHEMA = "docs/render-backend-seams/pipeline-key-schema.json"
INVENTORY = "docs/render-backend-seams/shader-permutation-inventory.json"
REGISTRY_H = "RenderCore/PipelineRegistry.h"

ALLOWED_STATUS = {"AUTHORITATIVE", "PARTIAL", "DESCRIPTIVE", "MISSING"}


def read(p):
    with open(p, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def parse_enum_ids(text):
    """PipelineId enum names in order, excluding Count_ and Invalid."""
    m = re.search(r"enum\s+class\s+PipelineId\s*:[^{]*\{(.*?)\}", text, re.S)
    if not m:
        return []
    ids = []
    for line in m.group(1).splitlines():
        em = re.match(r"\s*([A-Za-z_]\w*)\s*=\s*(\d+)", line)
        if em and em.group(1) not in ("Count_", "Invalid"):
            ids.append((em.group(1), int(em.group(2))))
    ids.sort(key=lambda x: x[1])
    return [n for n, _ in ids]


def parse_vertex_layouts(text):
    """VERTEXLAYOUT-AUTHORITY-1: VertexLayoutId enum value -> canonical layout
    name from its '// layout: <token>' comment. Excludes Count_/Invalid. A value
    with no layout comment maps to None (the checker flags it)."""
    m = re.search(r"enum\s+class\s+VertexLayoutId\s*:[^{]*\{(.*?)\}", text, re.S)
    if not m:
        return {}
    out = {}
    for line in m.group(1).splitlines():
        em = re.match(
            r"\s*([A-Za-z_]\w*)\s*=\s*\d+\s*,?\s*(?://\s*layout:\s*(\S+))?", line)
        if em and em.group(1) not in ("Count_", "Invalid"):
            out[em.group(1)] = em.group(2)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None)
    ap.add_argument("--json", default=None)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()
    root = args.root or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    schema = json.load(open(os.path.join(root, SCHEMA), encoding="utf-8"))
    inv = json.load(open(os.path.join(root, INVENTORY), encoding="utf-8"))
    inv_macros = {m["name"] for m in inv["macros"]}
    registry_raw = read(os.path.join(root, REGISTRY_H))
    enum_ids = parse_enum_ids(re.sub(r"/\*.*?\*/", "", registry_raw, flags=re.S))
    # VERTEXLAYOUT-AUTHORITY-1: raw text (// layout: comments preserved).
    vlayouts = parse_vertex_layouts(registry_raw)

    fails, warns = [], []

    # 3. every field has an allowed status -----------------------------------
    field_status = {}
    for fld in schema.get("fields", []):
        name = fld.get("field", "<unnamed>")
        st = fld.get("status")
        field_status[name] = st
        if st not in ALLOWED_STATUS:
            fails.append(f"field '{name}' has status '{st}' not in {sorted(ALLOWED_STATUS)}")
        if st in ("MISSING", "PARTIAL") and not (fld.get("gaps") or fld.get("descriptive_subaxes")):
            warns.append(f"field '{name}' is {st} but lists no gaps — gaps should be explicit")

    # 2. specialization params typed -----------------------------------------
    spec_field = next((f for f in schema["fields"] if f["field"] == "specializationParams"), None)
    if spec_field:
        for p in spec_field.get("params", []):
            if not p.get("type"):
                fails.append(f"specializationParam '{p.get('name')}' has no type")

    # 1 + 4. per-pipeline variant macros in inventory; pipeline set matches ---
    schema_ids = [p["id"] for p in schema.get("registered_pipelines", [])]
    missing_from_schema = [i for i in enum_ids if i not in schema_ids]
    stale_in_schema = [i for i in schema_ids if i not in enum_ids]
    if missing_from_schema:
        fails.append(f"registered PipelineId(s) missing from schema: {missing_from_schema}")
    if stale_in_schema:
        fails.append(f"schema lists pipeline(s) not in PipelineId enum: {stale_in_schema}")

    for p in schema.get("registered_pipelines", []):
        for mac in p.get("shaderVariantId", {}).get("macros", []):
            if mac not in inv_macros:
                fails.append(
                    f"pipeline '{p['id']}' names variant macro '{mac}' absent "
                    f"from {INVENTORY} (phantom / ungoverned variant)")
        # per-pipeline specializationParams must also be typed if present
        for sp in p.get("specializationParams", []):
            if isinstance(sp, dict) and not sp.get("type"):
                fails.append(f"pipeline '{p['id']}' spec param '{sp.get('name')}' untyped")

    # VERTEXLAYOUT-AUTHORITY-1: vertexLayout is now AUTHORITATIVE — recorded in
    # RenderCore::VertexLayoutId and checked here. (1) The field must declare
    # AUTHORITATIVE so the promotion cannot silently regress. (2) Every enum
    # value must carry a canonical "// layout: <name>" comment. (3) Every
    # registered pipeline must declare a vertexLayoutId that EXISTS in the enum
    # (missing -> FAIL; stale/renamed enum value -> FAIL) whose canonical name
    # EQUALS the pipeline's vertexLayout string (rename drift -> FAIL).
    if field_status.get("vertexLayout") != "AUTHORITATIVE":
        fails.append("field 'vertexLayout' must be AUTHORITATIVE "
                     "(VERTEXLAYOUT-AUTHORITY-1 promotion); "
                     f"found '{field_status.get('vertexLayout')}'")
    if not vlayouts:
        fails.append("VertexLayoutId enum not found / empty in "
                     f"{REGISTRY_H} (vertex layout authority missing)")
    for name, lname in vlayouts.items():
        if not lname:
            fails.append(f"VertexLayoutId '{name}' has no '// layout: <name>' "
                         "canonical-name comment")
    for p in schema.get("registered_pipelines", []):
        vid = p.get("vertexLayoutId")
        if not vid:
            fails.append(f"pipeline '{p['id']}' has no vertexLayoutId "
                         "(vertexLayout is authoritative — id is required)")
            continue
        if vid not in vlayouts:
            fails.append(f"pipeline '{p['id']}' vertexLayoutId '{vid}' absent "
                         f"from VertexLayoutId enum (stale/renamed)")
            continue
        canon = vlayouts[vid]
        if canon and p.get("vertexLayout") != canon:
            fails.append(
                f"pipeline '{p['id']}' vertexLayout '{p.get('vertexLayout')}' "
                f"!= enum canonical '{canon}' for {vid} (rename drift)")

    # SPIRV-MECHOPAQUE-PIPELINEKEY-INTEGRATION-1: cross-link the pipeline-key
    # contract to the baked SPIR-V artifact contract. For any registered pipeline
    # whose shaderVariantId.base is also a baked SPIR-V family, the schema's
    # variant macros MUST equal the macro-name set the package actually bakes
    # (define names, stripped of "=VALUE"). This proves the runtime
    # recordPipelineVariantKey identity, the pipeline-key schema, and the baked
    # artifacts all describe the SAME MechOpaque variant space — SPIR-V selection
    # cannot fork pipeline-key accounting.
    pkg_path = os.path.join(root, "shaders/spv/spirv_package.json")
    if os.path.exists(pkg_path):
        pkg = json.load(open(pkg_path, encoding="utf-8"))
        pkg_macros = {}
        for fam in pkg.get("variant_matrix", []):
            macs = set()
            for v in fam.get("variants", []):
                for d in v.get("defines", []):
                    macs.add(d.split("=", 1)[0])
            pkg_macros[fam["base"]] = macs
        for p in schema.get("registered_pipelines", []):
            base = p.get("shaderVariantId", {}).get("base")
            if base in pkg_macros:
                schema_macs = set(p["shaderVariantId"].get("macros", []))
                if schema_macs != pkg_macros[base]:
                    fails.append(
                        f"pipeline '{p['id']}' shaderVariantId macros {sorted(schema_macs)} "
                        f"!= baked '{base}' package macros {sorted(pkg_macros[base])} "
                        f"(pipeline-key contract drifted from SPIR-V artifacts)")

    report = {
        "summary": {
            "schema_verdict": schema["summary"]["verdict_schema"],
            "impl_verdict": schema["summary"]["verdict_implementation"],
            "fields": len(schema.get("fields", [])),
            "registered_pipelines": len(schema_ids),
            "enum_ids": enum_ids,
            "fails": len(fails),
            "warns": len(warns),
        },
        "field_status": field_status,
        "fails": fails,
        "warns": warns,
    }
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=2)

    if not args.quiet:
        print("[check-pipeline-key] PIPELINE-KEY-SCHEMA-1")
        print(f"  schema verdict   : {schema['summary']['verdict_schema']} "
              f"(impl {schema['summary']['verdict_implementation']})")
        print(f"  key fields       : {len(schema.get('fields', []))}")
        print(f"  registered pipes : {len(schema_ids)} ({', '.join(schema_ids)})")
        for n, st in field_status.items():
            print(f"    {n:<26} {st}")
        for w in warns:
            print(f"  WARN: {w}")
        for fl in fails:
            print(f"  FAIL: {fl}")
        print(f"  result: {'FAIL' if fails else 'PASS'} "
              f"({len(fails)} fail, {len(warns)} warn)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
