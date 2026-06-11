#!/usr/bin/env python3
"""tools/asset_cook/promote_cooked.py — Track G: wire cooked bundles into a deploy.

The deliberately-deferred reviewed promotion step. Merges each cooked bundle's
models.generated.json entry into a target deploy's CENTRAL models.json and copies
the cooked glb into <deploy>/data/model_overrides/cooked/<id>/. Textures are NOT
copied — cooked manifests reference existing data/tgl/<tier> tiles already deployed.

SAFETY:
  - backs up the central models.json (timestamped) before writing,
  - validates every entry against the registry accept rules (registry_resolves)
    and DROPS any that wouldn't resolve (reported), never writing a bad manifest,
  - dedups by 'replaces' key (existing entries win; cooked staticprops are new),
  - writes the central manifest exactly once, atomically.

  py -3 tools/asset_cook/promote_cooked.py --bundles <lib> --deploy <root> [--limit N] [--dry-run]
"""
from __future__ import annotations

import argparse
import json
import shutil
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import trackg_cook as tc  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bundles", required=True, type=Path, help="cooked library dir (<id>/models.generated.json)")
    ap.add_argument("--deploy", required=True, type=Path)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    central = args.deploy / "data" / "model_overrides" / "models.json"
    if not central.exists():
        print(f"FAIL: central manifest not found: {central}")
        return 1
    existing = json.loads(central.read_text(encoding="utf-8"))
    existing_keys = {o.get("replaces") for o in existing.get("overrides", [])}

    gens = sorted(args.bundles.glob("*/models.generated.json"))
    if args.limit:
        gens = gens[:args.limit]

    new_entries, glb_copies = [], []
    dropped_invalid = dropped_dup = dropped_noglb = 0
    for g in gens:
        bundle = g.parent
        asset_id = bundle.name
        entry = json.loads(g.read_text(encoding="utf-8"))["overrides"][0]
        ok, why = tc.registry_resolves(entry)
        if not ok:
            dropped_invalid += 1
            continue
        if entry["replaces"] in existing_keys:
            dropped_dup += 1
            continue
        glb = bundle / f"{asset_id}.glb"
        if not glb.exists():
            dropped_noglb += 1
            continue
        existing_keys.add(entry["replaces"])
        new_entries.append(entry)
        glb_copies.append((glb, args.deploy / "data" / "model_overrides" / entry["source"]))

    print(f"bundles={len(gens)}  promote={len(new_entries)}  "
          f"dropped[dup={dropped_dup} invalid={dropped_invalid} noglb={dropped_noglb}]")
    if args.dry_run:
        print("DRY-RUN: no writes. sample:", [e["replaces"] for e in new_entries[:5]])
        return 0
    if not new_entries:
        print("nothing to promote.")
        return 0

    # backup + write central
    bak = central.with_suffix(f".json.bak_promote_{int(time.time())}")
    shutil.copyfile(central, bak)
    merged = {"overrides": existing.get("overrides", []) + new_entries}
    central.write_text(json.dumps(merged, indent=1), encoding="utf-8")

    # copy glbs
    for src, dst in glb_copies:
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, dst)

    print(f"WIRED: central models.json {len(existing.get('overrides', []))} -> {len(merged['overrides'])} "
          f"entries (+{len(new_entries)}); {len(glb_copies)} glbs deployed. backup: {bak.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
