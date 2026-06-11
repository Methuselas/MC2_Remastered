#!/usr/bin/env python3
"""tools/asset_cook/cook_all_stock.py -- Track G batch: cook ALL stock props.

For every <id>.meshdump.json (from the workbench --export-tgl-meshdump-all):
  build axis-inverted textured glb -> stage (geometry) -> resolve existing
  data/tgl/<tier>/<name>.ktx2 tiles by name -> assemble full manifest +
  models.generated.json. Untextured props (no tile for any material) get a
  geometry-only bundle (staged.json, no manifest -- schema requires >=1 albedo).

No KTX2 re-cook (references existing deployed tiles). No central models.json write.

  py -3 tools/asset_cook/cook_all_stock.py --meshdumps <dir> --deploy-root <dir> --out <dir>
"""
from __future__ import annotations

import argparse
import csv
import json
import re
import sys
import types
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import trackg_cook as tc            # noqa: E402
import tglmeshdump_to_glb as t2g    # noqa: E402
import asset_cook_classify as acc   # noqa: E402
from asset_cook_classify import CookClass, classify_appearance, can_emit_override  # noqa: E402

TIERS = (128, 256, 512, 1024)


def sanitize_texname(tn: str) -> str:
    """Mirror DeriveMC2TextureName: strip dir+ext, lowercase, [a-z0-9_-] -> _."""
    stem = Path(tn).stem.lower()
    return re.sub(r"[^a-z0-9_-]", "_", stem)


def resolve_tiers(texname: str, deploy: Path) -> dict:
    san = sanitize_texname(texname)
    out = {}
    for t in TIERS:
        p = deploy / "data" / "tgl" / str(t) / f"{san}.ktx2"
        if p.exists():
            out[str(t)] = f"data/tgl/{t}/{san}.ktx2"
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--meshdumps", required=True, type=Path)
    ap.add_argument("--deploy-root", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--limit", type=int, default=0)
    args = ap.parse_args()

    dumps = sorted(args.meshdumps.glob("*.meshdump.json"))
    if args.limit:
        dumps = dumps[:args.limit]

    tgl_dir = args.deploy_root / "data" / "tgl"

    n_total = len(dumps)
    n_glb = n_staged = n_manifest = n_geomonly = n_animated_skipped = n_err = 0
    n_unknown_unsafe_skipped = 0
    n_multisubmesh = 0
    errors = []

    class_counts: dict[str, int] = {c.value: 0 for c in CookClass}
    report_entries: list[dict] = []

    for i, dp in enumerate(dumps):
        asset_id = dp.name[:-len(".meshdump.json")]

        cook_class, reason = classify_appearance(asset_id, tgl_dir)
        class_counts[cook_class.value] += 1

        ini_path = acc._find_ini(asset_id, tgl_dir)
        ini_rel = (
            str(ini_path.relative_to(args.deploy_root)).replace("\\", "/")
            if ini_path else ""
        )

        emitted = False

        if not can_emit_override(cook_class):
            if cook_class is CookClass.NODE_ANIMATED_PROP:
                n_animated_skipped += 1
                print(f"  skipped animated appearance: {asset_id} ({reason})")
            else:
                n_unknown_unsafe_skipped += 1
                print(f"  skipped {cook_class.value}: {asset_id} ({reason})")
            report_entries.append({
                "asset_id": asset_id,
                "source_meshdump": dp.name,
                "appearance_ini": ini_rel,
                "cook_class": cook_class.value,
                "reason": reason,
                "emitted_override": False,
            })
            continue

        bundle = args.out / asset_id
        try:
            dump = json.loads(dp.read_text(encoding="utf-8"))
            if not any(s["verts"] for s in dump["submeshes"]):
                n_geomonly += 1
                report_entries.append({
                    "asset_id": asset_id,
                    "source_meshdump": dp.name,
                    "appearance_ini": ini_rel,
                    "cook_class": cook_class.value,
                    "reason": "geometry-only (no verts)",
                    "emitted_override": False,
                })
                continue
            bundle.mkdir(parents=True, exist_ok=True)
            glb = bundle / f"{asset_id}.glb"
            glb.write_bytes(t2g.build_glb(dump))
            n_glb += 1
            if len(dump["submeshes"]) > 1:
                n_multisubmesh += 1

            ns = types.SimpleNamespace(
                source=str(glb), out_dir=str(bundle), id=asset_id, _class="staticprop",
                appearance=asset_id, source_rel=f"data/model_overrides/source/props/{asset_id}.glb",
                ground=2, yoff=0.0)
            if tc.stage(ns) != 0:
                raise RuntimeError("stage failed")
            n_staged += 1
            staged = json.loads((bundle / "staged.json").read_text())

            mats = []
            for m in staged["materials_discovered"]:
                tn = m["textureName"]
                tiers = resolve_tiers(tn, args.deploy_root)
                if not tiers:
                    continue
                mats.append({
                    "slot": m["slot"], "textureName": sanitize_texname(tn),
                    "alphaClass": m.get("alphaClass", 0),
                    "albedo_ktx2": tiers, "normal_ktx2": None,
                    "metallic_roughness_ktx2": None, "emissive_ktx2": None,
                    "base_color_factor": 1.0, "metallic_factor": 0.0, "roughness_factor": 1.0,
                    "flags": {"alpha_test": m.get("alphaClass", 0) == 1,
                              "double_sided": False, "window": False},
                })
            for idx, mm in enumerate(mats):
                mm["slot"] = idx
            if not mats:
                n_geomonly += 1
                report_entries.append({
                    "asset_id": asset_id,
                    "source_meshdump": dp.name,
                    "appearance_ini": ini_rel,
                    "cook_class": cook_class.value,
                    "reason": "geometry-only (no texture tiles resolved)",
                    "emitted_override": False,
                })
                continue
            (bundle / "materials.json").write_text(json.dumps({"materials": mats, "warnings": []}))

            na = types.SimpleNamespace(
                staged=str(bundle / "staged.json"), materials=str(bundle / "materials.json"),
                out_dir=str(bundle), override_source=f"cooked/{asset_id}/{asset_id}.glb",
                casts_shadow=1, has_legacy=1, cooked_utc="")
            if tc.assemble(na) != 0:
                raise RuntimeError("assemble failed")
            n_manifest += 1
            emitted = True

        except Exception as e:  # noqa: BLE001
            n_err += 1
            errors.append(f"{asset_id}: {e}")
            report_entries.append({
                "asset_id": asset_id,
                "source_meshdump": dp.name,
                "appearance_ini": ini_rel,
                "cook_class": cook_class.value,
                "reason": f"error: {e}",
                "emitted_override": False,
            })
            continue

        report_entries.append({
            "asset_id": asset_id,
            "source_meshdump": dp.name,
            "appearance_ini": ini_rel,
            "cook_class": cook_class.value,
            "reason": reason,
            "emitted_override": emitted,
        })

        if (i + 1) % 250 == 0:
            print(f"  ... {i + 1}/{n_total}  (manifest={n_manifest} geom-only={n_geomonly} err={n_err})")

    report = {
        "total_meshdumps": n_total, "glb_built": n_glb, "staged": n_staged,
        "full_manifest": n_manifest, "geometry_only_untextured": n_geomonly,
        "animated_skipped": n_animated_skipped,
        "unknown_unsafe_skipped": n_unknown_unsafe_skipped,
        "errors": n_err, "multi_submesh": n_multisubmesh,
        "classification_counts": class_counts,
        "error_samples": errors[:20],
    }
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "_batch_report.json").write_text(json.dumps(report, indent=2))

    class_report = {
        "summary": {k: v for k, v in class_counts.items() if v > 0},
        "entries": report_entries,
    }
    (args.out / "cook_classification_report.json").write_text(
        json.dumps(class_report, indent=2))

    csv_fields = ["asset_id", "source_meshdump", "appearance_ini",
                  "cook_class", "reason", "emitted_override"]
    with open(args.out / "cook_classification_report.csv", "w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=csv_fields)
        writer.writeheader()
        writer.writerows(report_entries)

    print("\n=== BATCH COOK REPORT ===")
    for k, v in report.items():
        if k not in ("error_samples", "classification_counts"):
            print(f"  {k}: {v}")
    print("  classification_counts:")
    for cls, cnt in class_counts.items():
        if cnt:
            print(f"    {cls}: {cnt}")
    if errors:
        print(f"  first errors: {errors[:5]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())