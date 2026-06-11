#!/bin/sh
# check-flag-registry-drift.sh — regenerate flags.auto.yaml to a temp file and
# diff against the committed docs/flags.auto.yaml.
# Advisory by default (always exit 0). --strict exits 1 on drift.
set -u

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
COMMITTED="$REPO_ROOT/docs/flags.auto.yaml"
TMP="$(mktemp)"
trap 'rm -f "$TMP"' EXIT

STRICT=0
[ "${1:-}" = "--strict" ] && STRICT=1

py -3 "$REPO_ROOT/scripts/flag-registry-auto-gen.py" --out "$TMP" >/dev/null || {
    echo "DRIFT-CHECK ERROR: generator failed" >&2
    [ "$STRICT" = 1 ] && exit 1
    exit 0
}

if [ ! -f "$COMMITTED" ]; then
    echo "DRIFT: docs/flags.auto.yaml missing (never generated/committed)"
    [ "$STRICT" = 1 ] && exit 1
    exit 0
fi

if diff -u "$COMMITTED" "$TMP"; then
    echo "flags.auto.yaml: up to date"
    exit 0
fi

echo "DRIFT: docs/flags.auto.yaml is stale -- regenerate: py -3 scripts/flag-registry-auto-gen.py"
[ "$STRICT" = 1 ] && exit 1
exit 0
