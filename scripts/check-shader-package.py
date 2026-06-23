#!/usr/bin/env python3
"""check-shader-package.py — SHADER-ARTIFACT-PACKAGE-METADATA-1

CI-cheap, tool-free validation of the deployable shader-artifact package
metadata (shaders/spv/spirv_package.json) against the actual artifacts. The
package is the single declared manifest of the SPIR-V pilot set; this gate fails
if it drifts from reality (stale hashes, missing/extra pilot family, undeclared
artifact). Tool-version *values* are validated by `build_variants.py --check`
(which re-runs the tools); this cheap gate validates content hashes + the variant
matrix + family declarations, all portable across worktrees/machines.

Reuses build_variants.package_hashes() (imported) so hashing stays in lockstep
with the generator. Does NOT run glslang/spirv-cross.

Asserts (FAIL):
  1. package present + schema_version matches generator;
  2. pilot_families == pilots.json programs (missing/extra family);
  3. variant_matrix == pilots.json variants (name/defines/artifacts) and every
     declared .spv + sidecar exists on disk, with reflection captured;
  4. content hashes (index / sidecars / source-set) match recomputed values
     (stale index / reflection / source);
  5. no undeclared .spv in shaders/spv/ (every artifact is in the matrix);
  6. tool-version + policy fields present (non-empty).

Usage:
  py -3 scripts/check-shader-package.py [--root R] [--json OUT] [--quiet]
"""
from __future__ import annotations
import argparse
import json
import sys
from pathlib import Path

ROOT_DEFAULT = Path(__file__).resolve().parents[1]
PILOTS = "tools/shader_offline_build/pilots.json"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None)
    ap.add_argument("--json", default=None)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()
    root = Path(args.root) if args.root else ROOT_DEFAULT

    # import the generator for lockstep hashing (no tools run on import/use)
    sys.path.insert(0, str(root / "tools" / "shader_offline_build"))
    import build_variants as bv  # noqa: E402

    cfg = json.load(open(root / PILOTS, encoding="utf-8"))
    spv_dir = root / cfg.get("spv_dir", "shaders/spv")
    pkg_path = spv_dir / "spirv_package.json"
    idx_path = spv_dir / "spirv_index.json"

    fails, warns = [], []

    if not pkg_path.exists():
        print("[check-shader-package] FAIL: spirv_package.json missing — run build_variants.py")
        return 1
    pkg = json.load(open(pkg_path, encoding="utf-8"))

    # 1. schema
    if pkg.get("schema_version") != bv.PACKAGE_SCHEMA_VERSION:
        fails.append(f"schema_version {pkg.get('schema_version')} != "
                     f"generator {bv.PACKAGE_SCHEMA_VERSION}")

    # 2. pilot families
    cfg_fams = [p["program"] for p in cfg["pilots"]]
    if pkg.get("pilot_families") != cfg_fams:
        fails.append(f"pilot_families {pkg.get('pilot_families')} != pilots.json {cfg_fams}")

    # 6. required descriptive fields present
    for path in (("tools", "glslangValidator"), ("tools", "spirv_cross"),
                 ("runtime", "required_extension"), ("runtime", "fallback_policy"),
                 ("runtime", "fatal_gate")):
        node = pkg
        for k in path:
            node = node.get(k, {}) if isinstance(node, dict) else {}
        if not node:
            fails.append(f"package missing/empty field {'.'.join(path)}")
    if not pkg.get("excluded_families"):
        fails.append("package missing excluded_families")

    # 3. variant matrix == generator's view of pilots.json + artifacts on disk
    expected_matrix = bv.package_variant_matrix(cfg)
    if pkg.get("variant_matrix") != expected_matrix:
        fails.append("variant_matrix does not match pilots.json (regenerate package)")
    declared_spv = set()
    for fam in expected_matrix:
        for v in fam["variants"]:
            for stage, art in v["stages"].items():
                declared_spv.add(art)
                if not (spv_dir / art).exists():
                    fails.append(f"{fam['base']}.{stage}.{v['name']}: declared .spv missing ({art})")
                sc = spv_dir / f"{fam['base']}.{stage}.{v['name']}.json"
                if not sc.exists():
                    fails.append(f"{fam['base']}.{stage}.{v['name']}: sidecar missing")
                else:
                    m = json.load(open(sc, encoding="utf-8"))
                    if "bindings" not in m or "interface" not in m:
                        fails.append(f"{sc.name}: reflection not captured (no bindings/interface)")

    # 4. content hashes (stale index / reflection / source)
    if idx_path.exists():
        idx_records = json.load(open(idx_path, encoding="utf-8")).get("records", [])
        recomputed = bv.package_hashes(spv_dir, idx_records)
        for k, v in recomputed.items():
            if pkg.get("hashes", {}).get(k) != v:
                fails.append(f"stale hash {k}: package {pkg.get('hashes', {}).get(k)} "
                             f"!= recomputed {v}")
    else:
        fails.append("spirv_index.json missing")

    # 5. no undeclared artifact
    for p in sorted(spv_dir.glob("*.spv")):
        if p.name not in declared_spv:
            fails.append(f"undeclared artifact in shaders/spv not in package: {p.name}")

    report = {"summary": {"fails": len(fails), "warns": len(warns),
                          "pilot_families": pkg.get("pilot_families")},
              "fails": fails, "warns": warns}
    if args.json:
        json.dump(report, open(args.json, "w", encoding="utf-8"), indent=2)
    if not args.quiet:
        print("[check-shader-package] SHADER-ARTIFACT-PACKAGE-METADATA-1")
        print(f"  pilot families : {pkg.get('pilot_families')}")
        print(f"  schema         : v{pkg.get('schema_version')}")
        for w in warns:
            print(f"  WARN: {w}")
        for f in fails:
            print(f"  FAIL: {f}")
        print(f"  result: {'FAIL' if fails else 'PASS'} ({len(fails)} fail, {len(warns)} warn)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
