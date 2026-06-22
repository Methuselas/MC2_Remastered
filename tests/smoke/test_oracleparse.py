# SMOKE-ORACLEPARSE-COVERAGE-1: coverage for the oracle verdict layer.
# scripts/smoke_lib/oracleparse.py had no tests; judge_oracles is a verdict-layer
# classifier (like gates.evaluate) — an untested regression silently mis-judges
# every oracle run. Pure functions, synthetic inputs, no game.
from scripts.smoke_lib.oracleparse import (
    OracleRow, parse_oracles, judge_oracles, OracleVerdict)


def _clean_row():
    # Minimal row that judge_oracles must PASS: gpu-armed, fully armed, a parity
    # MATCH observed, everything else at its clean default.
    return OracleRow(terrain_arm_path="gpu", terrain_fully_armed=True,
                     parity_match=1)


# --- judge_oracles (verdict layer) ---

def test_clean_row_passes():
    v = judge_oracles(_clean_row())
    assert isinstance(v, OracleVerdict)
    assert v.passed and v.fails == []


def test_fastpath_drop_fails():
    r = _clean_row(); r.fastpath_drops = 1
    v = judge_oracles(r)
    assert not v.passed and any("FASTPATH_DROP" in f for f in v.fails)


def test_terrain_arm_path_not_gpu_fails():
    r = _clean_row(); r.terrain_arm_path = "legacy"
    v = judge_oracles(r)
    assert not v.passed and any("terrain_arm_path" in f for f in v.fails)


def test_parity_mismatch_fails():
    r = _clean_row(); r.parity_mismatch = 2
    v = judge_oracles(r)
    assert not v.passed and any("parity MISMATCH" in f for f in v.fails)


def test_no_parity_match_warns_not_fails():
    r = _clean_row(); r.parity_match = 0
    v = judge_oracles(r)
    assert v.passed and any("parity MATCH" in w for w in v.warns)


def test_fully_armed_false_fails():
    r = _clean_row(); r.terrain_fully_armed = False
    v = judge_oracles(r)
    assert not v.passed and any("fullyArmed" in f for f in v.fails)


def test_tex_mismatch_and_oob_fail():
    r = _clean_row(); r.tex_mismatches = 3; r.tex_oob = 1
    v = judge_oracles(r)
    assert not v.passed
    assert any("TEX_RESOLVE mismatches" in f for f in v.fails)
    assert any("TEX_RESOLVE oob" in f for f in v.fails)


# --- the allow_late_register knob (the documented benign-vs-real distinction) ---

def test_objbatcher_late_register_benign_when_allowed():
    # cpu_fallback == late_register_skips => classified benign => WARN, still passes.
    r = _clean_row(); r.objb_cpu_fallback = 4; r.objb_late_register_skips = 4
    v = judge_oracles(r, allow_late_register=True)
    assert v.passed
    assert any("classified benign" in w for w in v.warns)
    assert not v.fails


def test_objbatcher_late_register_fails_when_disallowed():
    # Same row, allow_late_register=False => the benign case becomes a hard fail.
    r = _clean_row(); r.objb_cpu_fallback = 4; r.objb_late_register_skips = 4
    v = judge_oracles(r, allow_late_register=False)
    assert not v.passed
    assert any("classified benign" in f for f in v.fails)


def test_objbatcher_real_cpu_fallback_always_fails():
    # cpu_fallback EXCEEDS late_register => real geometry fell back => hard fail
    # regardless of the knob.
    r = _clean_row(); r.objb_cpu_fallback = 10; r.objb_late_register_skips = 4
    for allow in (True, False):
        v = judge_oracles(r, allow_late_register=allow)
        assert not v.passed
        assert any("real cpu_fallback" in f for f in v.fails)


def test_objbatcher_submit_legacy_fails():
    r = _clean_row(); r.objb_submit_legacy = 1
    v = judge_oracles(r)
    assert not v.passed and any("submit_legacy" in f for f in v.fails)


# --- parse_oracles (light, format-agnostic) ---

def test_parse_empty_is_default_row():
    r = parse_oracles("")
    assert r.fastpath_drops == 0 and r.terrain_arm_path is None
    # A default (never-armed) row must NOT pass judgement.
    assert not judge_oracles(r).passed


def test_parse_fastpath_drop_line():
    r = parse_oracles("noise\n[FASTPATH_DROP] terrain fell back\nmore noise\n")
    assert r.fastpath_drops >= 1


if __name__ == "__main__":
    import sys
    fns = [(n, f) for n, f in sorted(globals().items())
           if n.startswith("test_") and callable(f)]
    failed = 0
    for name, fn in fns:
        try:
            fn(); print(f"  [PASS] {name}")
        except AssertionError as e:
            failed += 1; print(f"  [FAIL] {name}: {e}")
        except Exception as e:  # noqa: BLE001
            failed += 1; print(f"  [ERROR] {name}: {e!r}")
    print(f"test_oracleparse: {'PASS' if not failed else 'FAIL'} "
          f"({len(fns)} tests, {failed} failed)")
    sys.exit(1 if failed else 0)
