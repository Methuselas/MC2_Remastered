"""SMOKE-EVIDENCE-CLASSIFIER-1 — classify crash_evidence/1 into actionable labels.

crash_evidence.py (SMOKE-CRASH-SILENT-EVIDENCE-1) CAPTURES evidence for a flaky
mission but stops at raw fields — a human still has to decode `exit_code
3221225781` into "STATUS_DLL_NOT_FOUND, environment not engine". This module is
the missing INTERPRETATION layer: a pure function over the evidence dict that
emits a single primary classification + confidence + the signals it fired on +
a one-line recommendation.

Pure + decoupled by design: it reads an already-written crash_evidence dict (or
.json file) and makes NO engine, runner, or smoke-timing change. It never
decides a smoke verdict — it only explains a failure after the fact, so it is
safe to run over historical artifacts.

Labels (primary, mutually exclusive, priority order):
  HANG                      — runner walltime cap hit (killed_by_timeout)
  DEVICE_LOSS_GPU_TDR       — display-driver reset/TDR in the event log
  ENVIRONMENT_MISSING_DLL   — exit 0xC0000135 STATUS_DLL_NOT_FOUND
  ENVIRONMENT_BAD_IMAGE     — exit 0xC000007B STATUS_INVALID_IMAGE_FORMAT
  ENVIRONMENT_MISSING_EXPORT— exit 0xC0000139 STATUS_ENTRYPOINT_NOT_FOUND
  APP_CRASH                 — in-engine fault (handler/minidump/WER/access-viol)
  CONTENTION_SUSPECTED      — another mc2.exe live at failure time
  UNKNOWN_RARE              — no decisive signal; needs a repro
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any, Dict, List

# Windows NTSTATUS exit codes (unsigned 32-bit) that are unambiguous.
_ENVIRONMENT_CODES = {
    0xC0000135: ("ENVIRONMENT_MISSING_DLL", "STATUS_DLL_NOT_FOUND"),
    0xC000007B: ("ENVIRONMENT_BAD_IMAGE", "STATUS_INVALID_IMAGE_FORMAT"),
    0xC0000139: ("ENVIRONMENT_MISSING_EXPORT", "STATUS_ENTRYPOINT_NOT_FOUND"),
}
# Codes that denote an in-process fault (engine crash), not the environment.
_APP_FAULT_CODES = {
    0xC0000005: "STATUS_ACCESS_VIOLATION",
    0xC00000FD: "STATUS_STACK_OVERFLOW",
    0xC0000409: "STATUS_STACK_BUFFER_OVERRUN",
    0xC0000374: "STATUS_HEAP_CORRUPTION",
    0xC0000417: "STATUS_INVALID_CRT_PARAMETER",
}

_RECOMMEND = {
    "HANG": "Engine hung past the walltime cap — inspect heartbeat_tail for the freeze point.",
    "DEVICE_LOSS_GPU_TDR": "GPU driver reset (TDR) — environment/driver, not the build. Retry; check driver.",
    "ENVIRONMENT_MISSING_DLL": "A required DLL was absent at launch — fix the deploy target, not the code.",
    "ENVIRONMENT_BAD_IMAGE": "Exe/DLL arch or corruption mismatch — re-deploy a matching x64 payload.",
    "ENVIRONMENT_MISSING_EXPORT": "DLL present but an export is missing — version-skewed DLL; re-deploy.",
    "APP_CRASH": "In-engine fault — symbolicate the minidump / stack; this is a real code bug.",
    "CONTENTION_SUSPECTED": "Another mc2.exe was live — likely contention/process-kill, not a true crash. Re-run isolated.",
    "UNKNOWN_RARE": "No decisive signal — re-run to reproduce; capture a minidump if it recurs.",
}


def _u32(code: Any) -> int:
    """Normalize an exit code to unsigned 32-bit (subprocess may report signed)."""
    try:
        return int(code) & 0xFFFFFFFF
    except (TypeError, ValueError):
        return -1


def _event_signals(evidence: Dict[str, Any]) -> Dict[str, bool]:
    """Scan the windows_event_log block for TDR vs app-error signatures."""
    tdr = app_err = False
    elog = evidence.get("windows_event_log") or {}
    for ev in (elog.get("events") or []):
        if not isinstance(ev, dict):
            continue
        eid = ev.get("Id")
        provider = str(ev.get("ProviderName") or "")
        if eid == 4101 or any(p in provider for p in ("nvlddmkm", "amdkmdag", "igfx", "Display")):
            tdr = True
        if eid in (1000, 1001) or "Application Error" in provider or "Windows Error Reporting" in provider:
            app_err = True
    return {"tdr": tdr, "app_err": app_err}


def classify(evidence: Dict[str, Any]) -> Dict[str, Any]:
    """Classify one crash_evidence/1 dict. Pure — no I/O. Returns a dict with
    keys: classification, confidence, signals, exit_code_hex, exit_status,
    recommendation, mission."""
    signals: List[str] = []
    code = _u32(evidence.get("exit_code"))
    code_hex = f"0x{code:08X}" if code >= 0 else None
    events = _event_signals(evidence)
    minidumps = evidence.get("minidumps_near_exe") or []
    concurrent = evidence.get("concurrent_mc2")
    concurrent_n = len(concurrent) if isinstance(concurrent, list) else 0
    crash_handler = evidence.get("crash_handler_hit")
    exit_status = None

    # Priority-ordered decision. First match wins (most specific / decisive).
    label = None
    conf = "low"

    if evidence.get("killed_by_timeout"):
        label, conf = "HANG", "high"
        signals.append("killed_by_timeout=true")
    elif events["tdr"]:
        label, conf = "DEVICE_LOSS_GPU_TDR", "high"
        signals.append("event_log: display-driver TDR/reset")
    elif code in _ENVIRONMENT_CODES:
        label, exit_status = _ENVIRONMENT_CODES[code]
        conf = "high"
        signals.append(f"exit_code {code_hex} = {exit_status}")
    elif crash_handler is True or minidumps or events["app_err"] or code in _APP_FAULT_CODES:
        label, conf = "APP_CRASH", "high"
        if crash_handler is True:
            signals.append("crash_handler_hit=true")
        if minidumps:
            signals.append(f"minidump(s): {', '.join(map(str, minidumps))}")
        if events["app_err"]:
            signals.append("event_log: Application Error / WER")
        if code in _APP_FAULT_CODES:
            exit_status = _APP_FAULT_CODES[code]
            signals.append(f"exit_code {code_hex} = {exit_status}")
        # No minidump/handler but an app-fault exit code = medium (could be masked).
        if not (crash_handler is True or minidumps or events["app_err"]):
            conf = "medium"
    elif concurrent_n > 0:
        label, conf = "CONTENTION_SUSPECTED", "medium"
        signals.append(f"concurrent_mc2={concurrent_n} live at failure")
    else:
        label, conf = "UNKNOWN_RARE", "low"
        if code_hex:
            signals.append(f"exit_code {code_hex} (undecoded)")
        if not signals:
            signals.append("no decisive signal")

    return {
        "mission": evidence.get("mission"),
        "classification": label,
        "confidence": conf,
        "exit_code_hex": code_hex,
        "exit_status": exit_status,
        "signals": signals,
        "recommendation": _RECOMMEND[label],
        "buckets": evidence.get("buckets"),
    }


# ---- CLI: classify .json files or an artifact dir --------------------------

def _iter_evidence_paths(paths: List[str]):
    for p in paths:
        pp = Path(p)
        if pp.is_dir():
            yield from sorted(pp.glob("*.crash_evidence.json"))
        elif pp.is_file():
            yield pp


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="Classify smoke crash_evidence/1 JSON into actionable labels.")
    ap.add_argument("paths", nargs="+",
                    help="crash_evidence.json file(s) or an artifact dir")
    ap.add_argument("--json", action="store_true", help="emit JSON array")
    args = ap.parse_args(argv)

    results = []
    for path in _iter_evidence_paths(args.paths):
        try:
            ev = json.loads(Path(path).read_text(encoding="utf-8"))
        except Exception as exc:  # noqa: BLE001
            results.append({"file": str(path), "error": str(exc)})
            continue
        r = classify(ev)
        r["file"] = str(path)
        results.append(r)

    if args.json:
        print(json.dumps(results, indent=2))
    else:
        for r in results:
            if "error" in r:
                print(f"  [SKIP] {r['file']}: {r['error']}")
                continue
            print(f"  {r.get('mission') or r['file']}: "
                  f"{r['classification']} ({r['confidence']})")
            for s in r["signals"]:
                print(f"      - {s}")
            print(f"      -> {r['recommendation']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
