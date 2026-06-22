# SMOKE-EVIDENCE-CLASSIFIER-1

**Type:** Python, pure-function + CLI. Zero engine / runner / smoke-timing change.
**Module:** `scripts/smoke_lib/evidence_classifier.py` · **Tests:** `tests/smoke/test_evidence_classifier.py`

## What it adds

`crash_evidence.py` (SMOKE-CRASH-SILENT-EVIDENCE-1) *captures* evidence for a
flaky mission but stops at raw fields — a human still has to decode
`exit_code 3221225781` into "STATUS_DLL_NOT_FOUND → environment, not engine".
This is the missing **interpretation** layer: `classify(evidence) -> {...}` maps
one `crash_evidence/1` dict to a single primary label + confidence + the signals
it fired on + a one-line recommendation.

Motivated directly by the GAMEOS-TIMING-MATH gate: the bare smoke target failed
with `exit_code 3221225781` and a particle-init crash, and the diagnosis ("missing
DLLs / no game data — environment, not the timing change") was done by hand. This
classifier produces that verdict automatically.

## Labels (priority order, first match wins)

| Label | Trigger | Confidence |
|---|---|---|
| `HANG` | `killed_by_timeout` | high |
| `DEVICE_LOSS_GPU_TDR` | event log: 4101 / nvlddmkm / amdkmdag / Display | high |
| `ENVIRONMENT_MISSING_DLL` | exit `0xC0000135` STATUS_DLL_NOT_FOUND | high |
| `ENVIRONMENT_BAD_IMAGE` | exit `0xC000007B` STATUS_INVALID_IMAGE_FORMAT | high |
| `ENVIRONMENT_MISSING_EXPORT` | exit `0xC0000139` STATUS_ENTRYPOINT_NOT_FOUND | high |
| `APP_CRASH` | crash_handler_hit / minidump / WER(1000/1001) / access-violation code | high (medium if only an app-fault exit code, no dump) |
| `CONTENTION_SUSPECTED` | another mc2.exe live at failure time | medium |
| `UNKNOWN_RARE` | no decisive signal | low |

Priority matters: a timeout outranks a stale minidump; a GPU TDR outranks an
ambiguous exit code. Signed exit codes are normalized to unsigned 32-bit.

## Why pure + decoupled

It reads an already-written `crash_evidence/1` dict (or `.json` file / artifact
dir) and never decides a smoke verdict — it explains a failure *after* the
verdict is set. Safe to run over historical artifacts; no fake-green risk because
it touches no verdict path. The classifier is a pure function; tests drive every
label on synthetic dicts (incl. the real `3221225781` case) plus the CLI.

## Run

```
py -3 scripts/smoke_lib/evidence_classifier.py tests/smoke/artifacts/<ts>/
py -3 scripts/smoke_lib/evidence_classifier.py <stem>.crash_evidence.json --json
py -3 -m pytest tests/smoke/test_evidence_classifier.py -q
```

## Not in scope

Wiring the classifier into `run_smoke.py` to print labels inline at run time is a
separate, optional follow-up (would be a runner touch). This slice is the
analyzer + contract only.
