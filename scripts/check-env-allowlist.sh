#!/bin/sh
# scripts/check-env-allowlist.sh
#
# Advisory drift check for the run_smoke.py env passthrough allowlist
# (modernization-acceleration-enablers.md slice 2, accel-1 sec 5).
#
# Problem class ("Mistake E"): a new MC2_* gate is added to the engine
# and/or to run_smoke.py harness logic, but is never added to the
# env_extra passthrough allowlist in scripts/run_smoke.py. The gate-ON
# smoke run then silently exercises the gate-OFF path and a feature
# merges with zero regression coverage.
#
# NOTE (verified 2026-06-11): scripts/smoke_lib/runner.py currently does
# env = os.environ.copy() + env.update(cfg.env_extra), so the parent env
# IS inherited and nothing is hard-dropped at the Popen layer today.
# The allowlist is still the documented contract ("Popen replaces env"
# comments in run_smoke.py) and the convention every lane relies on, so
# drift against it is still worth surfacing. This check is ADVISORY.
#
# Sections reported:
#   1. HARNESS MISSES — MC2_* vars referenced by run_smoke.py code
#      OUTSIDE the allowlist block (vars the harness itself sets/uses,
#      e.g. via --flags or os.environ) that are NOT in the allowlist.
#      Historical exemplar: MC2_GL_DEBUG_FATAL was set by
#      --gl-debug-fatal but missing from the allowlist until c8b7ac03;
#      this script exists so that class of miss is caught mechanically.
#   2. ENGINE getenv() SWEEP (informational) — getenv("MC2_...") names
#      in mclib/ GameOS/ code/ RenderCore/ not in the allowlist.
#
# Modeled on scripts/check-no-raw-gl-from-game.sh (grep + allowlist +
# exit code). Pure POSIX sh + grep/sed/sort/comm — no python.
#
# Exit codes:
#   default        : ALWAYS exit 0 (advisory)
#   --strict       : exit 1 if section-1 harness misses > 0 (future CI)
#
# Negative-test (verify the script catches misses): temporarily delete
# the "MC2_GL_DEBUG_FATAL" line from the allowlist block in
# scripts/run_smoke.py, run this script, expect MC2_GL_DEBUG_FATAL
# reported as a HARNESS MISS (and --strict exit 1); then revert.

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

STRICT=0
[ "$1" = "--strict" ] && STRICT=1

SMOKE="scripts/run_smoke.py"
if [ ! -f "$SMOKE" ]; then
    echo "check-env-allowlist.sh: $SMOKE not found" >&2
    exit 1
fi

TMPDIR_="$(mktemp -d 2>/dev/null || echo "/tmp/env-allowlist.$$")"
mkdir -p "$TMPDIR_"
trap 'rm -rf "$TMPDIR_"' EXIT

ALLOW="$TMPDIR_/allow.txt"
HARNESS="$TMPDIR_/harness.txt"
ENGINE="$TMPDIR_/engine.txt"

# --- 1. Extract the passthrough allowlist var set -------------------------
# The allowlist is the quoted "MC2_*" strings inside the `if k in (` ... `)}`
# tuple in the env_extra block of run_smoke.py. Locate the block by its
# unambiguous open/close anchors and harvest quoted names within.
sed -n '/if k in (/,/)},/p' "$SMOKE" \
    | grep -oE '"MC2_[A-Z0-9_]+"' \
    | tr -d '"' | sort -u > "$ALLOW"

if [ ! -s "$ALLOW" ]; then
    echo "check-env-allowlist.sh: failed to extract allowlist block from $SMOKE" >&2
    echo "(anchors 'if k in (' ... ')},' not found — script needs updating)" >&2
    exit 1
fi

# --- 2. Extract harness-referenced MC2_* vars OUTSIDE the allowlist block --
# Blank out the allowlist block, strip comment lines, then harvest every
# MC2_* token the remaining harness code mentions.
sed '/if k in (/,/)},/d' "$SMOKE" \
    | sed 's/[[:space:]]*#.*$//' \
    | grep -oE 'MC2_[A-Z0-9_]+' \
    | sort -u > "$HARNESS"

# Vars the runner sets directly in the child env (not gates that need
# passthrough) — informational noise, not misses.
RUNNER_DIRECT="MC2_SMOKE_MODE MC2_HEARTBEAT MC2_SMOKE_SEED MC2_TGL_POOL_TRACE MC2_ASSET_SCALE_TRACE MC2_MENU_CANARY_SKIP_INTRO MC2_GL_ERROR_DRAIN_SILENT"

is_runner_direct() {
    for d in $RUNNER_DIRECT; do
        [ "$1" = "$d" ] && return 0
    done
    return 1
}

MISSES=0
echo "== check-env-allowlist.sh =="
echo "allowlist vars: $(wc -l < "$ALLOW" | tr -d ' ')"
echo ""
echo "-- Section 1: HARNESS MISSES (run_smoke.py refs outside allowlist) --"
while IFS= read -r v; do
    [ -z "$v" ] && continue
    if grep -qx "$v" "$ALLOW"; then continue; fi
    if is_runner_direct "$v"; then
        echo "  info: $v (runner sets directly in child env; not a passthrough gate)"
        continue
    fi
    echo "  MISS: $v (harness sets/uses it but it is NOT in the passthrough allowlist)"
    MISSES=$((MISSES+1))
done < "$HARNESS"
[ "$MISSES" -eq 0 ] && echo "  none"

# --- 3. Engine getenv() sweep (informational) ------------------------------
echo ""
echo "-- Section 2 (informational): engine getenv(\"MC2_*\") not in allowlist --"
SCOPE_DIRS="mclib GameOS code RenderCore"
: > "$ENGINE"
for dir in $SCOPE_DIRS; do
    [ -d "$dir" ] || continue
    git grep -hoE 'getenv\(\s*"MC2_[A-Z0-9_]+"' -- "$dir" 2>/dev/null \
        | grep -oE 'MC2_[A-Z0-9_]+' >> "$ENGINE" || true
done
sort -u "$ENGINE" > "$ENGINE.sorted"

ENGINE_MISSES=0
while IFS= read -r v; do
    [ -z "$v" ] && continue
    if grep -qx "$v" "$ALLOW"; then continue; fi
    if is_runner_direct "$v"; then continue; fi
    echo "  not-in-allowlist: $v"
    ENGINE_MISSES=$((ENGINE_MISSES+1))
done < "$ENGINE.sorted"
echo "  ($ENGINE_MISSES engine getenv vars not in allowlist, of $(wc -l < "$ENGINE.sorted" | tr -d ' ') total)"

echo ""
if [ "$MISSES" -gt 0 ]; then
    echo "check-env-allowlist.sh: $MISSES harness miss(es)." >&2
    echo "Add the var(s) to the env_extra allowlist in $SMOKE so gate-ON" >&2
    echo "smoke runs actually exercise the gate (Mistake E, accel-1 sec 5)." >&2
    if [ "$STRICT" -eq 1 ]; then
        exit 1
    fi
    echo "(advisory mode: exit 0; use --strict for CI)"
else
    echo "check-env-allowlist.sh: clean (no harness misses; engine sweep informational)"
fi
exit 0
