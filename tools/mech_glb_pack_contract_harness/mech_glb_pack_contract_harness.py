#!/usr/bin/env python3
"""SUBSYSTEM-HARNESS-ARC / MECH-IMPORT-GLB-PACK-MANIFEST-1

Validates the imported-GLB asset contract from the manifest
(docs/testing/mech_glb_external_pack.json) against the repo — no game launch, no
tg_import_dump by default, no large-asset commit. Same pattern as the IBL
external-HDRI manifest: large/deploy-only GLBs (e.g. Flea.glb) are declared
external rather than committed; a tracked GLB that drifts out of the manifest, or
a missing tracked GLB, or a path escape, FAILS.

The manifest is committed (always present), so the default suite is green on a
clean checkout with zero configuration. External GLBs absent locally are reported,
not failed; MC2_MECH_GLB_PACK_STRICT=1 (or --test strict_external_glbs_present)
additionally requires them installed.

Run:
  py -3 .../mech_glb_pack_contract_harness.py --list
  py -3 .../mech_glb_pack_contract_harness.py --json
  MC2_MECH_GLB_PACK_STRICT=1 py -3 .../mech_glb_pack_contract_harness.py
"""

import json
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT = os.path.dirname(os.path.dirname(_HERE))
sys.path.insert(0, os.path.join(_REPO_ROOT, "tools", "contract_harness_common"))

from contract_harness import Harness, Ctx           # noqa: E402

MANIFEST_REL = "docs/testing/mech_glb_external_pack.json"
VALID_SOURCE_TYPES = {"tracked", "external", "generated"}
GLB_SCAN_DIR = "data/tgl"   # where imported GLBs live; scanned for undeclared


def _strict_env():
    return os.environ.get("MC2_MECH_GLB_PACK_STRICT", "") == "1"


def _manifest_path():
    return os.path.join(_REPO_ROOT, MANIFEST_REL)


def _load_manifest():
    """Returns (data, error_or_None)."""
    try:
        with open(_manifest_path(), "r", encoding="utf-8") as f:
            return json.load(f), None
    except FileNotFoundError:
        return None, f"manifest not found: {MANIFEST_REL}"
    except json.JSONDecodeError as e:
        return None, f"manifest is not valid JSON: {e}"


def _entries():
    data, err = _load_manifest()
    if err:
        return [], err
    if not isinstance(data, dict) or not isinstance(data.get("entries"), list):
        return [], "manifest missing top-level 'entries' list"
    return data["entries"], None


def _norm(p):
    return p.replace("\\", "/")


# ---- tests -----------------------------------------------------------------

def test_manifest_json_valid(t: Ctx) -> bool:
    data, err = _load_manifest()
    if err:
        t.fail(err); return False
    if not isinstance(data, dict):
        t.fail("manifest root is not an object")
    elif data.get("schema") != "mech-glb-pack/v1":
        t.fail(f"unexpected schema: {data.get('schema')!r}")
    elif not isinstance(data.get("entries"), list):
        t.fail("'entries' is not a list")
    return t.failures == 0


def test_entries_have_unique_ids(t: Ctx) -> bool:
    entries, err = _entries()
    if err:
        t.fail(err); return False
    seen_id, seen_name = set(), set()
    for i, e in enumerate(entries):
        mid = e.get("mech_id")
        nm = e.get("name")
        if not mid:
            t.fail(f"entry {i} missing mech_id")
        elif mid in seen_id:
            t.fail(f"duplicate mech_id: {mid}")
        else:
            seen_id.add(mid)
        if nm and nm in seen_name:
            t.fail(f"duplicate name: {nm}")
        elif nm:
            seen_name.add(nm)
    return t.failures == 0


def test_entries_declare_valid_source_type(t: Ctx) -> bool:
    entries, err = _entries()
    if err:
        t.fail(err); return False
    for e in entries:
        st = e.get("source_type")
        if st not in VALID_SOURCE_TYPES:
            t.fail(f"entry {e.get('mech_id')!r} has invalid source_type {st!r} "
                   f"(must be one of {sorted(VALID_SOURCE_TYPES)})")
        if not e.get("glb"):
            t.fail(f"entry {e.get('mech_id')!r} missing 'glb' path")
    return t.failures == 0


def test_tracked_entries_exist_in_repo(t: Ctx) -> bool:
    entries, err = _entries()
    if err:
        t.fail(err); return False
    for e in entries:
        if e.get("source_type") != "tracked":
            continue
        rel = _norm(e.get("glb", ""))
        if not os.path.isfile(os.path.join(_REPO_ROOT, rel)):
            t.fail(f"tracked GLB declared but absent in repo: {rel}")
    return t.failures == 0


def test_external_entries_reported(t: Ctx) -> bool:
    # Informational: external GLBs may be absent by default; report present/absent.
    entries, err = _entries()
    if err:
        t.fail(err); return False
    ext = [e for e in entries if e.get("source_type") == "external"]
    present = sum(1 for e in ext
                  if os.path.isfile(os.path.join(_REPO_ROOT, _norm(e.get("glb", "")))))
    print(f"    external GLBs: {len(ext)} declared, {present} present locally, "
          f"{len(ext) - present} absent (OK by default)", file=sys.stderr)
    for e in ext:
        rel = _norm(e.get("glb", ""))
        if not os.path.isfile(os.path.join(_REPO_ROOT, rel)):
            print(f"      absent (external): {rel}", file=sys.stderr)
    return True


def test_paths_repo_relative_no_escape(t: Ctx) -> bool:
    entries, err = _entries()
    if err:
        t.fail(err); return False
    root_real = os.path.realpath(_REPO_ROOT)
    for e in entries:
        rel = e.get("glb", "")
        if os.path.isabs(rel):
            t.fail(f"glb path is absolute (must be repo-relative): {rel}")
            continue
        resolved = os.path.realpath(os.path.join(_REPO_ROOT, rel))
        if resolved != root_real and not resolved.startswith(root_real + os.sep):
            t.fail(f"glb path escapes repo root: {rel}")
        if not _norm(rel).lower().endswith(".glb"):
            t.fail(f"glb path does not end in .glb: {rel}")
    return t.failures == 0


def test_no_undeclared_tracked_glb(t: Ctx) -> bool:
    # Every GLB physically present under data/tgl/ in the repo must be declared
    # (catches a new imported GLB committed/installed without a manifest entry).
    entries, err = _entries()
    if err:
        t.fail(err); return False
    declared = {_norm(e.get("glb", "")).lower() for e in entries}
    scan = os.path.join(_REPO_ROOT, GLB_SCAN_DIR)
    found = 0
    if os.path.isdir(scan):
        for f in os.listdir(scan):
            if f.lower().endswith(".glb"):
                found += 1
                rel = _norm(os.path.join(GLB_SCAN_DIR, f)).lower()
                if rel not in declared:
                    t.fail(f"GLB present under {GLB_SCAN_DIR}/ but not declared in "
                           f"manifest: {f}")
    print(f"    {GLB_SCAN_DIR}/ GLBs on disk: {found}", file=sys.stderr)
    return t.failures == 0


# Strict (inDefault gated by MC2_MECH_GLB_PACK_STRICT; or --test): every external
# GLB must be installed locally.
def test_strict_external_glbs_present(t: Ctx) -> bool:
    entries, err = _entries()
    if err:
        t.fail(err); return False
    for e in entries:
        if e.get("source_type") != "external":
            continue
        rel = _norm(e.get("glb", ""))
        if not os.path.isfile(os.path.join(_REPO_ROOT, rel)):
            t.fail(f"strict: external GLB not installed locally: {rel}")
    return t.failures == 0


def main():
    h = Harness("mech_glb_pack_contract_harness")
    h.add("manifest_json_valid",                test_manifest_json_valid)
    h.add("entries_have_unique_ids",            test_entries_have_unique_ids)
    h.add("entries_declare_valid_source_type",  test_entries_declare_valid_source_type)
    h.add("tracked_entries_exist_in_repo",      test_tracked_entries_exist_in_repo)
    h.add("external_entries_reported",          test_external_entries_reported)
    h.add("paths_repo_relative_no_escape",      test_paths_repo_relative_no_escape)
    h.add("no_undeclared_tracked_glb",          test_no_undeclared_tracked_glb)
    h.add("strict_external_glbs_present",        test_strict_external_glbs_present,
          in_default=_strict_env())
    return h.run()


if __name__ == "__main__":
    raise SystemExit(main())
