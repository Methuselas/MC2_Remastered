#!/usr/bin/env python3
"""S12 -- manifest schema conformance / drift checker (ADVISORY).

Scans a directory tree for artifact-manifest files and reports which ones
carry a conformant unified `identity` block (schema "mc2-manifest/1") and
which still use a legacy-only shape. This is the drift gate for the
unified-manifest rollout: as the five species adopt `manifest_schema.attach`,
this checker tracks coverage.

ADVISORY by default (exit 0 even with non-conformant legacy files) so it can
run in CI as a coverage report without blocking the pre-adoption species.
Pass --strict to exit nonzero when any KNOWN-adopted file is non-conformant
(use once a species has been migrated, to prevent regressions).

Recognized manifest filenames (the five species + new tools):
    manifest.json            run manifest (cockpit) / release-zip report
    *.install-receipt.json   install receipt (mc2mod)
    package.json             mod package (mc2mod)        [legacy-allowed]
    cook.json                cook sidecar (asset_cook)   [legacy-allowed]
    *_capture_manifest.json  visual-capture sidecar (S9 runner)

Usage:
    py -3 scripts/check-manifest-schema.py [ROOT ...]
    py -3 scripts/check-manifest-schema.py --strict tests/visual
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import manifest_schema  # noqa: E402

# Filenames that SHOULD carry a unified identity block once adopted. Species
# marked legacy_ok=True are not yet migrated and never fail --strict.
KNOWN = {
    "manifest.json": dict(legacy_ok=False, species="run/release manifest"),
    "package.json": dict(legacy_ok=True, species="mod package"),
    "cook.json": dict(legacy_ok=True, species="cook sidecar"),
    "set.json": dict(legacy_ok=False, species="golden set record"),
    "golden-sets.json": dict(legacy_ok=True, species="golden-sets registry"),
    "release_install_report.json": dict(legacy_ok=False, species="release install report"),
}
SUFFIX_KNOWN = {
    ".install-receipt.json": dict(legacy_ok=True, species="install receipt"),
    "_capture_manifest.json": dict(legacy_ok=False, species="visual-capture sidecar"),
}


def classify(path: Path) -> dict | None:
    name = path.name
    if name in KNOWN:
        return KNOWN[name]
    for suf, meta in SUFFIX_KNOWN.items():
        if name.endswith(suf):
            return meta
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("roots", nargs="*", default=None,
                    help="dirs to scan (default: tests/, run/, releases under repo)")
    ap.add_argument("--strict", action="store_true",
                    help="exit nonzero if any non-legacy species is non-conformant")
    args = ap.parse_args()

    roots = [Path(r) for r in (args.roots or [ROOT / "tests", ROOT / "run"])]

    scanned = conformant = legacy = broken = 0
    failures: list[str] = []
    for root in roots:
        if not root.exists():
            continue
        for path in sorted(root.rglob("*.json")):
            meta = classify(path)
            if meta is None:
                continue
            scanned += 1
            try:
                doc = json.loads(path.read_text(encoding="utf-8"))
            except Exception as e:
                broken += 1
                failures.append(f"UNREADABLE {path}: {e}")
                continue
            ident = doc.get("identity") if isinstance(doc, dict) else None
            if ident is None:
                legacy += 1
                tag = "legacy-ok" if meta["legacy_ok"] else "LEGACY (no identity)"
                print(f"[manifest-check] {tag:22} {meta['species']:24} {path}")
                if not meta["legacy_ok"] and args.strict:
                    failures.append(f"NON-CONFORMANT (no identity block): {path}")
                continue
            problems = manifest_schema.validate_identity(ident)
            if problems:
                broken += 1
                print(f"[manifest-check] {'BROKEN':22} {meta['species']:24} {path}")
                for p in problems:
                    print(f"                   - {p}")
                failures.append(f"BROKEN identity in {path}: {problems}")
            else:
                conformant += 1
                print(f"[manifest-check] {'OK (mc2-manifest/1)':22} "
                      f"{meta['species']:24} {path}")

    print(f"\n[manifest-check] scanned={scanned} conformant={conformant} "
          f"legacy={legacy} broken={broken}")
    if args.strict and failures:
        print(f"[manifest-check] STRICT FAIL ({len(failures)} problem(s)):",
              file=sys.stderr)
        for f in failures:
            print(f"  - {f}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
