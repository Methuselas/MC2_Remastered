#!/usr/bin/env python3
# scripts/tests/test_fingerprint.py
"""Tests for scripts/smoke_lib/fingerprint.py (deploy-fingerprint parse/compare).

Pure functions only -- no game launch, no git, no filesystem.

Run:
  py -3 -m pytest scripts/tests/test_fingerprint.py -v
"""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from scripts.smoke_lib.fingerprint import parse_fingerprint, check_fingerprint

BANNER = ("[BUILD_FINGERPRINT v1] sha=0123456789ab dirty=0 "
          "branch=claude/sp-deploy-fingerprint-1 built=2026-06-11T12:00:00 "
          "src=A:/Games/mc2-opengl-src/.claude/worktrees/sp-deploy-fingerprint-1")
LOG = "noise line\n" + BANNER + "\n[exit] gos_TerminateApplication called\n"


def test_parse_ok():
    fp = parse_fingerprint(LOG)
    assert fp is not None
    assert fp["sha"] == "0123456789ab"
    assert fp["dirty"] == 0
    assert fp["branch"] == "claude/sp-deploy-fingerprint-1"
    assert fp["built"] == "2026-06-11T12:00:00"
    assert fp["src"].endswith("sp-deploy-fingerprint-1")


def test_parse_dirty_flag():
    fp = parse_fingerprint(LOG.replace("dirty=0", "dirty=1"))
    assert fp["dirty"] == 1


def test_parse_absent():
    assert parse_fingerprint("no banner here\n") is None
    assert parse_fingerprint("") is None


def test_check_match():
    fp = parse_fingerprint(LOG)
    lines, hard_fail = check_fingerprint(fp, "0123456789abcdef0123456789abcdef01234567")
    assert not hard_fail
    assert any("OK" in l for l in lines)


def test_check_mismatch_advisory_text():
    fp = parse_fingerprint(LOG)
    lines, hard_fail = check_fingerprint(fp, "deadbeefdeadbeefdeadbeefdeadbeefdeadbeef")
    assert hard_fail  # caller enforces only under MC2_SMOKE_REQUIRE_FINGERPRINT=1
    assert any(l.startswith("[DEPLOY_FINGERPRINT] MISMATCH: exe sha=0123456789ab "
                            "expected=deadbeefdead") for l in lines)


def test_check_absent_is_advisory_hard_fail():
    lines, hard_fail = check_fingerprint(None, "0123456789ab")
    assert hard_fail
    assert any("fingerprint absent (pre-fingerprint exe)" in l for l in lines)


def test_check_dirty_warning_passthrough():
    fp = parse_fingerprint(LOG.replace("dirty=0", "dirty=1"))
    lines, hard_fail = check_fingerprint(fp, "0123456789ab")
    assert not hard_fail  # dirty alone is not a mismatch
    assert any("BUILT-FROM-DIRTY-TREE" in l for l in lines)


def test_check_no_expected_sha():
    fp = parse_fingerprint(LOG)
    lines, hard_fail = check_fingerprint(fp, None)
    assert not hard_fail
    assert any("expected sha unavailable" in l for l in lines)


def test_check_case_insensitive_sha():
    fp = parse_fingerprint(LOG.replace("0123456789ab", "0123456789AB"))
    lines, hard_fail = check_fingerprint(fp, "0123456789ABCDEF")
    assert not hard_fail
