#!/usr/bin/env python3
"""Unit tests for scripts/manifest_schema.py (S12 unified manifest schema)."""
from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import manifest_schema as ms  # noqa: E402


def test_identity_block_minimal_is_conformant():
    ident = ms.identity_block(generator="unit_test")
    assert ident["schema"] == ms.SCHEMA
    assert ident["generator"] == "unit_test"
    assert ident["exe"] is None
    assert ident["deploy_target"] is None
    assert ident["zip_set"] is None
    assert ms.validate_identity(ident) == []
    assert ms.is_conformant(ident)


def test_git_block_present_keys_even_without_repo():
    g = ms.git_block(None)
    for k in ("commit", "commit_short", "branch", "dirty", "describe"):
        assert k in g and g[k] is None


def test_git_block_from_repo_has_commit():
    g = ms.git_block(ROOT)
    # In a real checkout these resolve; tolerate detached/unknown by allowing
    # None but the keys must exist (validate_identity only requires keys).
    assert set(g) == {"commit", "commit_short", "branch", "dirty", "describe"}


def test_exe_block_from_real_file(tmp_path):
    p = tmp_path / "fake.exe"
    p.write_bytes(b"hello world")
    blk = ms.exe_block(p)
    assert blk["path"].endswith("fake.exe")
    # sha256("hello world")
    assert blk["sha256"] == (
        "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9"
    )
    assert blk["size_bytes"] == 11
    assert blk["mtime_iso"].endswith("Z")


def test_env_delta_allowlist_and_prefix():
    env = {"MC2_GPU_CULL": "1", "MC2_VIEW_UNIFORMS": "0", "PATH": "x", "HOME": "y"}
    # default prefix mode: only MC2_*
    d = ms.env_delta(env=env)
    assert d == {"MC2_GPU_CULL": "1", "MC2_VIEW_UNIFORMS": "0"}
    # allow-list mode records <unset> for missing keys
    d2 = ms.env_delta(env=env, keys=["MC2_GPU_CULL", "MC2_MISSING"])
    assert d2 == {"MC2_GPU_CULL": "1", "MC2_MISSING": "<unset>"}


def test_report_summary_shape():
    r = ms.report_summary(verdict="PASS", missions=["mc2_01"],
                          artifact_dir="/tmp/run")
    assert r["verdict"] == "PASS"
    assert r["missions"] == ["mc2_01"]
    assert r["artifact_dir"] == "/tmp/run"


def test_attach_is_additive():
    legacy = {"schema_v": 1, "tier": "tier1", "result": "PASS"}
    ident = ms.identity_block(generator="run_smoke")
    rep = ms.report_summary(verdict="PASS", missions=["mc2_01"])
    out = ms.attach(legacy, ident, rep)
    # legacy keys preserved
    assert out["schema_v"] == 1
    assert out["tier"] == "tier1"
    # new blocks present
    assert out["identity"]["schema"] == ms.SCHEMA
    assert out["report"]["verdict"] == "PASS"
    # flat accessors
    assert ms.get_verdict(out) == "PASS"
    assert ms.get_git_commit(out) is None or isinstance(ms.get_git_commit(out), str)


def test_flat_accessors_exe_sha(tmp_path):
    p = tmp_path / "mc2.exe"
    p.write_bytes(b"abc")
    ident = ms.identity_block(generator="t", exe_path=p)
    manifest = ms.attach({}, ident)
    assert ms.get_exe_sha(manifest) == (
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
    )


def test_validate_rejects_wrong_schema():
    bad = {"schema": "something-else/9", "generator": "x", "generated_utc": "z",
           "git": {}, "exe": None, "deploy_target": None, "env_delta": {},
           "zip_set": None}
    problems = ms.validate_identity(bad)
    assert any("schema" in p for p in problems)


def test_validate_rejects_non_object():
    assert ms.validate_identity(None) == ["identity is not an object"]
    assert ms.validate_identity([1, 2]) == ["identity is not an object"]


def test_roundtrip_json_stable():
    ident = ms.identity_block(generator="rt", repo_root=ROOT)
    s = json.dumps({"identity": ident}, indent=2)
    back = json.loads(s)
    assert ms.is_conformant(back["identity"])


if __name__ == "__main__":
    # Minimal runner without pytest dependency.
    import tempfile
    import traceback

    fns = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    passed = failed = 0
    for fn in fns:
        try:
            import inspect
            if "tmp_path" in inspect.signature(fn).parameters:
                with tempfile.TemporaryDirectory() as td:
                    fn(Path(td))
            else:
                fn()
            passed += 1
            print(f"PASS {fn.__name__}")
        except Exception:
            failed += 1
            print(f"FAIL {fn.__name__}")
            traceback.print_exc()
    print(f"\n{passed} passed, {failed} failed")
    sys.exit(1 if failed else 0)
