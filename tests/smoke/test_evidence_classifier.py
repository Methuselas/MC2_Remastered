# tests/smoke/test_evidence_classifier.py
# SMOKE-EVIDENCE-CLASSIFIER-1 — classify crash_evidence/1 into actionable labels.
#
# Pure-function tests on synthetic evidence dicts (one per classification path),
# including the real exit_code seen during GAMEOS-TIMING-MATH gate work
# (3221225781 = STATUS_DLL_NOT_FOUND) that motivated this slice.
import json

from scripts.smoke_lib.evidence_classifier import classify, main


def _ev(**over):
    base = {
        "schema": "crash_evidence/1",
        "mission": "mc2_01",
        "buckets": ["crash_silent"],
        "exit_code": 1,
        "killed_by_timeout": False,
        "crash_handler_hit": False,
        "minidumps_near_exe": [],
        "concurrent_mc2": [],
        "windows_event_log": {"queried": True, "events": []},
    }
    base.update(over)
    return base


def test_hang_from_timeout():
    r = classify(_ev(killed_by_timeout=True, buckets=["timeout"]))
    assert r["classification"] == "HANG"
    assert r["confidence"] == "high"


def test_missing_dll_real_session_code():
    # 3221225781 == 0xC0000135 == STATUS_DLL_NOT_FOUND (the exact code seen when
    # the bare smoke target lacked SDL2/glew DLLs).
    r = classify(_ev(exit_code=3221225781))
    assert r["classification"] == "ENVIRONMENT_MISSING_DLL"
    assert r["confidence"] == "high"
    assert r["exit_code_hex"] == "0xC0000135"
    assert r["exit_status"] == "STATUS_DLL_NOT_FOUND"


def test_bad_image_format():
    r = classify(_ev(exit_code=0xC000007B))
    assert r["classification"] == "ENVIRONMENT_BAD_IMAGE"


def test_missing_export():
    r = classify(_ev(exit_code=0xC0000139))
    assert r["classification"] == "ENVIRONMENT_MISSING_EXPORT"


def test_gpu_tdr_from_event_log_beats_exit_code():
    # A display-driver TDR outranks an otherwise-ambiguous exit code.
    ev = _ev(exit_code=1, windows_event_log={"queried": True, "events": [
        {"Id": 4101, "ProviderName": "nvlddmkm", "Message": "stopped responding"}]})
    r = classify(ev)
    assert r["classification"] == "DEVICE_LOSS_GPU_TDR"
    assert r["confidence"] == "high"


def test_app_crash_access_violation():
    r = classify(_ev(exit_code=0xC0000005))
    assert r["classification"] == "APP_CRASH"
    assert r["exit_status"] == "STATUS_ACCESS_VIOLATION"


def test_app_crash_from_minidump():
    r = classify(_ev(exit_code=1, minidumps_near_exe=["crash_2026.dmp"]))
    assert r["classification"] == "APP_CRASH"
    assert r["confidence"] == "high"


def test_app_crash_from_handler_flag():
    r = classify(_ev(exit_code=1, crash_handler_hit=True))
    assert r["classification"] == "APP_CRASH"


def test_app_crash_from_application_error_event():
    ev = _ev(exit_code=1, windows_event_log={"queried": True, "events": [
        {"Id": 1000, "ProviderName": "Application Error"}]})
    assert classify(ev)["classification"] == "APP_CRASH"


def test_contention_when_concurrent_and_no_fault_signal():
    r = classify(_ev(exit_code=1, concurrent_mc2=[{"pid": 42, "path": "x"}]))
    assert r["classification"] == "CONTENTION_SUSPECTED"
    assert r["confidence"] == "medium"


def test_unknown_rare_no_signal():
    r = classify(_ev(exit_code=1))
    assert r["classification"] == "UNKNOWN_RARE"
    assert r["confidence"] == "low"


def test_priority_timeout_outranks_app_crash():
    # killed_by_timeout dominates even if a stale minidump is present.
    r = classify(_ev(killed_by_timeout=True, minidumps_near_exe=["old.dmp"]))
    assert r["classification"] == "HANG"


def test_signed_exit_code_normalized():
    # subprocess may surface the signed form of 0xC0000135.
    signed = 3221225781 - (1 << 32)
    r = classify(_ev(exit_code=signed))
    assert r["classification"] == "ENVIRONMENT_MISSING_DLL"


def test_every_label_has_recommendation():
    for ev in (_ev(killed_by_timeout=True), _ev(exit_code=0xC0000135),
               _ev(exit_code=0xC0000005), _ev(exit_code=1)):
        r = classify(ev)
        assert r["recommendation"] and isinstance(r["recommendation"], str)


def test_cli_over_dir(tmp_path, capsys):
    (tmp_path / "mc2_01.crash_evidence.json").write_text(
        json.dumps(_ev(exit_code=3221225781)), encoding="utf-8")
    (tmp_path / "mc2_03.crash_evidence.json").write_text(
        json.dumps(_ev(exit_code=0xC0000005, mission="mc2_03")), encoding="utf-8")
    rc = main([str(tmp_path)])
    assert rc == 0
    out = capsys.readouterr().out
    assert "ENVIRONMENT_MISSING_DLL" in out
    assert "APP_CRASH" in out
