#!/bin/sh
# scripts/check-no-raw-gl-from-game.sh
#
# RenderWorld Slice M6: function-level firewall enforcing the rule that
# game-side code (code/, mclib/) MUST NOT call raw OpenGL functions
# (gl*()). Rendering routes through engine abstractions
# (gos_*, MeshRenderer, MaterialSystem, RenderWorld, GameAdapters).
#
# Spec: docs/superpowers/specs/2026-05-24-renderworld-slice-m6-firewall-audit-spec.md
# Recon: docs/superpowers/explorations/2026-05-23-renderworld-slice-m6-firewall-audit-recon.md
#
# Orthogonal to scripts/check-include-firewall.sh:
#   - that script answers "is RenderWorld pure?" (include-level)
#   - this script answers  "is game-side abstracted?" (function-call-level)
#
# Pattern: \bgl[A-Z][a-zA-Z]+\s*\(  -- the function-call anchor (\s*\()
# eliminates known false positives like glX/glY field names,
# glTextureId (mc2video struct field), and glTexId (local variable).
#
# Allowlist: scripts/check-no-raw-gl-from-game.allowlist
#
# Negative-test (to verify the script catches violations): manually inject
# `glClear(GL_COLOR_BUFFER_BIT);` into any code/ file, run script, expect
# non-zero exit + the file/line reported; then revert.
#
# Exit 0  = clean
# Exit 1  = at least one non-allowlisted violation

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

SCOPE_DIRS="code mclib"
ALLOWLIST="scripts/check-no-raw-gl-from-game.allowlist"
PATTERN='\bgl[A-Z][a-zA-Z]+[[:space:]]*\('

VIOLATIONS=0

allowlisted() {
    # $1 = path
    [ -f "$ALLOWLIST" ] || return 1
    while IFS= read -r _aline; do
        case "$_aline" in
            ""|"#"*) continue ;;
        esac
        if [ "$1" = "$_aline" ]; then
            return 0
        fi
    done < "$ALLOWLIST"
    return 1
}

# Strip the path:lineno: prefix and check if the remaining content is
# a C/C++ comment-only line. Mirrors check-include-firewall.sh.
is_comment_line() {
    # $1 = full grep "path:lineno:content" line
    body="$(printf '%s' "$1" | sed 's/^[^:]*:[0-9][0-9]*://' | sed 's/^[[:space:]]*//')"
    case "$body" in
        "//"*) return 0 ;;
        "/*"*) return 0 ;;
        "*"*)  return 0 ;;
    esac
    return 1
}

TMPHITS="$(mktemp 2>/dev/null || echo "/tmp/no-raw-gl.$$")"
TMPFILES="$(mktemp 2>/dev/null || echo "/tmp/no-raw-gl-files.$$")"
trap 'rm -f "$TMPHITS" "$TMPFILES"' EXIT

# Enumerate game-side files via git ls-files to avoid build artifacts.
: > "$TMPFILES"
for dir in $SCOPE_DIRS; do
    if [ ! -d "$dir" ]; then continue; fi
    git ls-files "$dir" >> "$TMPFILES" 2>/dev/null || true
done

if [ ! -s "$TMPFILES" ]; then
    echo "scripts/check-no-raw-gl-from-game.sh: no files enumerated (git ls-files empty?)" >&2
    exit 1
fi

# Filter to C/C++ sources + headers.
SOURCES="$(grep -E '\.(c|cc|cpp|cxx|h|hh|hpp|hxx|inl)$' "$TMPFILES" || true)"

if [ -z "$SOURCES" ]; then
    echo "scripts/check-no-raw-gl-from-game.sh: clean (scope: ${SCOPE_DIRS}; 0 source files)"
    exit 0
fi

: > "$TMPHITS"
# xargs would be cleaner but we keep this POSIX-portable + per-file loop
# so the comment-strip + allowlist logic stays readable.
echo "$SOURCES" | while IFS= read -r src; do
    [ -z "$src" ] && continue
    [ -f "$src" ] || continue
    # Strip trailing `// ...` comments BEFORE applying the violation grep
    # so that documentation/comment text naming a GL function (e.g.
    # `// expects glDrawBuffers(...)`) is not flagged. Sed blanks the
    # comment portion in-place; lines remain in the stream so grep -n
    # reports the original line numbers. Full-line `//` comments survive
    # the legacy is_comment_line() filter below as a defense-in-depth net.
    sed 's|//.*$||' "$src" | grep -nE "$PATTERN" 2>/dev/null | while IFS= read -r match; do
        [ -z "$match" ] && continue
        # Prefix with filename so downstream parsing stays uniform.
        printf '%s:%s\n' "$src" "$match"
    done
done > "$TMPHITS"

if [ -s "$TMPHITS" ]; then
    while IFS= read -r line; do
        [ -z "$line" ] && continue
        file="$(printf '%s' "$line" | cut -d: -f1)"
        if allowlisted "$file"; then
            continue
        fi
        # is_comment_line expects "path:lineno:content"; our $line is
        # exactly that (we prepended "path:" to grep -n's "lineno:content").
        if is_comment_line "$line"; then
            continue
        fi
        # Extract the matched function token for a tidy message.
        matched="$(printf '%s' "$line" | grep -oE "$PATTERN" | head -1)"
        echo "VIOLATION: raw GL call ${matched} in ${line}" >&2
        VIOLATIONS=$((VIOLATIONS+1))
    done < "$TMPHITS"
fi

if [ "$VIOLATIONS" -gt 0 ]; then
    echo "" >&2
    echo "scripts/check-no-raw-gl-from-game.sh: ${VIOLATIONS} violation(s)" >&2
    echo "Spec: docs/superpowers/specs/2026-05-24-renderworld-slice-m6-firewall-audit-spec.md" >&2
    echo "Route rendering through engine API (gos_*, MeshRenderer, RenderWorld)." >&2
    echo "If the call is a diagnostic-only state query, add the TU to" >&2
    echo "scripts/check-no-raw-gl-from-game.allowlist with a justification." >&2
    exit 1
fi
echo "scripts/check-no-raw-gl-from-game.sh: clean (scope: ${SCOPE_DIRS})"
exit 0
