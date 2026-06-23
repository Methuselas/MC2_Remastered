#!/usr/bin/env bash
# scripts/check-contracts.sh
# CONTRACTS-AGGREGATOR-1: run all cheap static-contract checks and report a
# concise PASS/FAIL table.  Exits 0 only if every check passes.
#
# Usage:
#   bash scripts/check-contracts.sh           # run from worktree root
#   bash scripts/check-contracts.sh --quiet   # suppress individual script output
#
# What counts as "cheap":
#   * Pure grep/awk over source files (no build required)
#   * Run time under ~5 seconds on a cold file cache
#   * Idempotent and read-only
#
# Excluded (too heavy for this aggregator):
#   check-shader-schema.sh   -- requires glslangValidator + compiled goldens
#   check-debug-state-json.py -- requires a live engine JSON snapshot

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WORKTREE_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
QUIET=0
for arg in "$@"; do
    case "$arg" in
        --quiet|-q) QUIET=1 ;;
        --help|-h)
            echo "Usage: bash $0 [--quiet]"
            echo "  Runs cheap source-level contract checks and prints a PASS/FAIL table."
            echo "  Exits 0 if all pass, nonzero if any fail."
            exit 0
            ;;
        *) echo "Unknown argument: $arg" >&2; exit 1 ;;
    esac
done

# ---------------------------------------------------------------------------
# Check runner
# ---------------------------------------------------------------------------
PASS_COUNT=0
FAIL_COUNT=0

# Arrays to collect results for the summary table.
NAMES=()
RESULTS=()

run_check() {
    local label="$1"
    local cmd="$2"

    if [ "$QUIET" -eq 1 ]; then
        if (cd "$WORKTREE_ROOT" && eval "$cmd" > /dev/null 2>&1); then
            PASS_COUNT=$((PASS_COUNT + 1))
            NAMES+=("$label")
            RESULTS+=("PASS")
        else
            FAIL_COUNT=$((FAIL_COUNT + 1))
            NAMES+=("$label")
            RESULTS+=("FAIL")
        fi
    else
        echo "--- $label ---"
        if (cd "$WORKTREE_ROOT" && eval "$cmd"); then
            PASS_COUNT=$((PASS_COUNT + 1))
            NAMES+=("$label")
            RESULTS+=("PASS")
        else
            FAIL_COUNT=$((FAIL_COUNT + 1))
            NAMES+=("$label")
            RESULTS+=("FAIL")
        fi
        echo ""
    fi
}

# ---------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------
run_check "env_registry"           "sh scripts/check-env-registry.sh"
run_check "material_gpu_mirror"    "sh scripts/check-material-gpu-mirror.sh"
run_check "visibility_log_schema"  "sh scripts/check-visibility-log-schema.sh"
run_check "include_firewall"       "sh scripts/check-include-firewall.sh"
run_check "no_raw_gl_from_game"    "sh scripts/check-no-raw-gl-from-game.sh"
run_check "vfx_no_objectid"        "sh scripts/check-vfx-no-objectid.sh"
run_check "destroy_invariant"      "sh scripts/check-destroy-invariant.sh"
run_check "render_contract_gbuf1"  "sh scripts/check-render-contract-gbuffer1.sh"
run_check "binding_slots"          "py -3 scripts/check-binding-slots.py --quiet"
run_check "sampler_bindings"       "py -3 scripts/check-sampler-bindings.py --quiet"
run_check "pipeline_desc"          "py -3 scripts/check-pipeline-desc.py --quiet"
run_check "shader_injectors"       "py -3 scripts/check-shader-injectors.py --quiet"
run_check "pipeline_key"           "py -3 scripts/check-pipeline-key.py --quiet"
run_check "spirv_artifacts"        "py -3 scripts/check-spirv-artifacts.py --quiet"
run_check "spirv_reflection"       "py -3 scripts/check-spirv-reflection-contract.py --quiet"
run_check "shader_package"         "py -3 scripts/check-shader-package.py --quiet"
run_check "visual_compare"         "py -3 scripts/visual_compare.py --self-test"
run_check "brain_fit_schema"       "py -3 scripts/check-brain-fit-schema.py --quiet"

# ---------------------------------------------------------------------------
# Summary table
# ---------------------------------------------------------------------------
echo "====================================================================="
echo "  CONTRACT CHECK RESULTS"
echo "====================================================================="
for i in "${!NAMES[@]}"; do
    result="${RESULTS[$i]}"
    label="${NAMES[$i]}"
    if [ "$result" = "PASS" ]; then
        printf "  %-40s  PASS\n" "$label"
    else
        printf "  %-40s  FAIL  <---\n" "$label"
    fi
done
echo "---------------------------------------------------------------------"
printf "  %d passed, %d failed\n" "$PASS_COUNT" "$FAIL_COUNT"
echo "====================================================================="

if [ "$FAIL_COUNT" -gt 0 ]; then
    exit 1
fi
exit 0
