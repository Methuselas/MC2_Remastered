# BRAIN-DISPATCH-HARNESS-V2: A/B effect-identity runner.
# Runs the brain_dispatch_harness in --apply-mode twice:
#   pass A: MC2_BRAIN_INTENT_QUEUE unset (gate OFF) -> direct setGeneralTacOrder
#   pass B: MC2_BRAIN_INTENT_QUEUE=1  (gate ON)  -> emit -> commitBrainIntents -> setGeneralTacOrder
# Asserts both passes exit 0 (all apply assertions green), proving gate-OFF == gate-ON A/B identity.
#
# Usage (from worktree root):
#   powershell -NoProfile -File scripts/run_brain_apply_ab.ps1 [-HarnessExe <path>] [-ManifestPath <path>]
#
# Defaults:
#   HarnessExe   = build64-brain-harness\RelWithDebInfo\brain_dispatch_harness.exe
#   ManifestPath = tests/fixtures/brain_runtime/manifest.json

param(
    [string]$HarnessExe   = "build64-brain-harness\RelWithDebInfo\brain_dispatch_harness.exe",
    [string]$ManifestPath = "tests\fixtures\brain_runtime\manifest.json",
    [string]$FixtureDir   = "tests\fixtures\brain_runtime"
)

$ErrorActionPreference = "Stop"
$worktreeRoot = Split-Path -Parent $PSScriptRoot

# Resolve harness exe relative to worktree root if not absolute
if (-not [System.IO.Path]::IsPathRooted($HarnessExe)) {
    $HarnessExe = Join-Path $worktreeRoot $HarnessExe
}
if (-not (Test-Path $HarnessExe)) {
    Write-Error "Harness exe not found: $HarnessExe"
    exit 1
}

function Run-HarnessApply {
    param([bool]$GateOn)
    $gateLabel = if ($GateOn) { "ON" } else { "OFF" }
    Write-Host ""
    Write-Host "=== brain_dispatch_harness --apply-mode gate=$gateLabel ==="
    $env:MC2_BRAIN_DISPATCH        = "1"
    $env:MC2_BRAIN_DISPATCH_APPLY  = "1"
    $env:MC2_BRAIN_DISPATCH_CALL   = "1"
    $env:MC2_BRAIN_DISPATCH_VAR    = "1"
    if ($GateOn) {
        $env:MC2_BRAIN_INTENT_QUEUE = "1"
    } else {
        # HARNESS-INTENT-GATE-SCOPE-1: set "0" explicitly (not unset) so the harness's
        # default-when-unset does NOT flip the OFF pass back to ON. This is the real gate-OFF control.
        $env:MC2_BRAIN_INTENT_QUEUE = "0"
    }
    & $HarnessExe --manifest $ManifestPath --fixture-dir $FixtureDir --apply-mode
    return $LASTEXITCODE
}

$exitA = Run-HarnessApply -GateOn $false
$exitB = Run-HarnessApply -GateOn $true

Write-Host ""
Write-Host "=== A/B Summary ==="
Write-Host "gate-OFF exit: $exitA"
Write-Host "gate-ON  exit: $exitB"

if ($exitA -eq 0 -and $exitB -eq 0) {
    Write-Host "PASS: A/B effect-identity proven offline (gate-OFF == gate-ON committed orders)"
    exit 0
} else {
    Write-Host "FAIL: one or both passes failed"
    exit 1
}
