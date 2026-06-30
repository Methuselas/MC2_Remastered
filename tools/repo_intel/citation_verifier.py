"""
MCP-TOOLING-1: citation_verifier.py
Heuristic verifier for file:line and symbol citations in recon/handoff docs.

Parses a markdown doc for:
  - file:line patterns  (e.g. mclib/txmmgr.cpp:3017)
  - symbol references near those citations (backtick-quoted identifiers)

For each citation:
  - Checks the file exists under repo root
  - Checks the file has >= that many lines
  - OPTIONAL: if a symbol is adjacent, greps that line +/-3 for it

For symbol-only citations (backtick-quoted identifiers):
  - Checks symbol still exists anywhere in source
  - Reports caller_count so the reader can verify "dead / 0 callers" claims

Returns JSON with keys:
  doc, checked, ok, drifted, missing, confidence, note
"""

import json
import os
import re
import subprocess
from pathlib import Path
from typing import Optional

# ---------------------------------------------------------------------------
# Patterns
# ---------------------------------------------------------------------------

# Matches path-like:linenum  e.g. mclib/txmmgr.cpp:3017
_CITE_RE = re.compile(
    r'([\w./-]+\.(?:cpp|h|hglsl|py|vert|frag|glsl|comp)):(\d+)',
)

# Backtick-quoted symbol (C++ identifier style)
_SYM_RE = re.compile(r'`([A-Za-z_][A-Za-z0-9_:]{2,})`')

# Max lines to read when checking line count (we use line count from wc-style)
_MAX_LINE_SCAN = 50_000

# ---------------------------------------------------------------------------
# Exclude patterns (same as grep_tool defaults)
# ---------------------------------------------------------------------------
_EXCLUDE_DIRS = {
    ".git", "build64", "releases", "3rdparty", ".claude",
    "dist", ".vscode",
}


def _find_file(root: Path, rel_path: str) -> Optional[Path]:
    """
    Try to resolve rel_path against root.  Accepts both forward and back slashes.
    Returns the resolved path if the file exists, else None.
    """
    p = root / rel_path.replace("\\", "/")
    if p.exists():
        return p
    # Also try lowercased (Windows is case-insensitive but we want the canonical path)
    return None


def _line_count(path: Path, max_lines: int = _MAX_LINE_SCAN) -> int:
    """Return line count (capped at max_lines to stay fast on big files)."""
    count = 0
    try:
        with open(path, "r", encoding="utf-8", errors="ignore") as f:
            for _ in f:
                count += 1
                if count >= max_lines:
                    return max_lines
    except OSError:
        pass
    return count


def _read_lines(path: Path, around: int, n: int = 3) -> list[str]:
    """Return lines [around-n .. around+n] (1-based, clamped). Returns [] on error."""
    lo = max(0, around - 1 - n)
    hi = around - 1 + n + 1
    try:
        with open(path, "r", encoding="utf-8", errors="ignore") as f:
            all_lines = f.readlines()
        return [l.rstrip("\n") for l in all_lines[lo:hi]]
    except OSError:
        return []


def _symbol_exists(root: Path, symbol: str) -> dict:
    """
    Check whether symbol exists anywhere in source (using ripgrep or subprocess grep).
    Returns {present: bool, caller_count: int, files: [...]}.
    """
    pattern = r'\b' + re.escape(symbol) + r'\b'
    exclude_args = []
    for d in _EXCLUDE_DIRS:
        exclude_args += ["--glob", f"!{d}/**"]
    exclude_args += [
        "--glob", "!*.log", "--glob", "!*.jsonl",
        "--glob", "!*.pdb", "--glob", "!*.obj", "--glob", "!*.lib",
        "--glob", "!*.dll", "--glob", "!*.exe",
    ]
    try:
        result = subprocess.run(
            ["rg", "--no-heading", "--line-number", "-e", pattern,
             "--type-add", "src:*.{cpp,h,hglsl,py,vert,frag,glsl,comp}",
             "--type", "src",
             ] + exclude_args + [str(root)],
            capture_output=True, text=True, timeout=10,
        )
        lines = [l.strip() for l in result.stdout.splitlines() if l.strip()]
        files = list({l.split(":")[0] for l in lines if ":" in l})
        return {"present": len(lines) > 0, "caller_count": len(lines), "files": files[:8]}
    except (FileNotFoundError, subprocess.TimeoutExpired):
        pass

    # Fallback: pure Python walk (slow on large trees but correct)
    count = 0
    found_files: list[str] = []
    rx = re.compile(pattern)
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in _EXCLUDE_DIRS]
        for fn in filenames:
            if not fn.endswith((".cpp", ".h", ".hglsl", ".py", ".vert", ".frag", ".glsl", ".comp")):
                continue
            fpath = Path(dirpath) / fn
            try:
                text = fpath.read_text(encoding="utf-8", errors="ignore")
            except OSError:
                continue
            matches = rx.findall(text)
            if matches:
                count += len(matches)
                found_files.append(str(fpath.relative_to(root)))
    return {"present": count > 0, "caller_count": count, "files": found_files[:8]}


# ---------------------------------------------------------------------------
# Main entry point
# ---------------------------------------------------------------------------

def verify_citations(
    doc_path: str,
    repo_root: Path,
    max_checks: int = 200,
) -> dict:
    """
    Parse doc at doc_path, verify its file:line and symbol citations.

    Returns {doc, checked, ok, drifted, missing, confidence, note}.
    """
    doc = Path(doc_path)
    if not doc.exists():
        return {
            "error": f"doc not found: {doc_path}",
            "doc": doc_path,
        }

    text = doc.read_text(encoding="utf-8", errors="ignore")
    lines = text.splitlines()

    ok: list[dict] = []
    drifted: list[dict] = []
    missing: list[dict] = []

    checked = 0

    # ------------------------------------------------------------------
    # Pass 1 — file:line citations
    # ------------------------------------------------------------------
    for m in _CITE_RE.finditer(text):
        if checked >= max_checks:
            break
        rel = m.group(1)
        lineno = int(m.group(2))
        citation = f"{rel}:{lineno}"

        # Find context window in the *doc* around this citation (for symbol extraction)
        cite_pos = m.start()
        # Grab ~100 chars before and after in the doc text for symbol context
        ctx_start = max(0, cite_pos - 120)
        ctx_end = min(len(text), cite_pos + 120)
        ctx = text[ctx_start:ctx_end]
        # Filter out C++ keywords and short tokens that are not identifiers
        _KEYWORD_SKIP = frozenset({
            "else", "if", "for", "while", "return", "true", "false", "nullptr",
            "void", "int", "bool", "char", "float", "double", "const", "static",
            "inline", "auto", "this", "new", "delete", "class", "struct", "enum",
            "public", "private", "protected", "virtual", "override",
        })
        nearby_syms = [s for s in _SYM_RE.findall(ctx) if s not in _KEYWORD_SKIP]

        resolved = _find_file(repo_root, rel)
        if resolved is None:
            missing.append({
                "citation": citation,
                "reason": f"file not found under repo root: {rel}",
            })
            checked += 1
            continue

        lc = _line_count(resolved)
        if lc < lineno:
            drifted.append({
                "citation": citation,
                "reason": f"file has only {lc} lines (citation says :{lineno}) — line deleted/moved",
                "file_line_count": lc,
            })
            checked += 1
            continue

        # Stronger check: if nearby symbols, verify they appear at that line ±3
        if nearby_syms:
            window = _read_lines(resolved, lineno, n=3)
            window_text = " ".join(window)
            absent = [s for s in nearby_syms if s not in window_text]
            if absent and len(absent) == len(nearby_syms):
                # All nearby symbols missing — likely drifted
                drifted.append({
                    "citation": citation,
                    "reason": (
                        f"symbol(s) {absent!r} not found in ±3 lines around :{lineno} — "
                        f"line may have moved. Nearby text: {window_text[:120]!r}"
                    ),
                    "symbols_checked": nearby_syms,
                    "absent_symbols": absent,
                })
                checked += 1
                continue

        ok.append({"citation": citation, "symbols_checked": nearby_syms})
        checked += 1

    # ------------------------------------------------------------------
    # Pass 2 — symbol-only claims (backtick identifiers NOT already
    #          covered by a file:line match; look for "0 callers" / "dead" claims)
    # ------------------------------------------------------------------
    # Find lines that mention a symbol with a "dead / no caller / 0 caller" claim
    dead_claim_re = re.compile(
        r'`([A-Za-z_][A-Za-z0-9_:]{2,})`[^`]{0,80}(?:0 caller|no caller|dead|unreachable|delete)',
        re.IGNORECASE,
    )
    # Also: "delete X" / "remove X"
    delete_claim_re = re.compile(
        r'(?:delete|remove|retire)\s+`([A-Za-z_][A-Za-z0-9_:]{2,})`',
        re.IGNORECASE,
    )

    claimed_dead: set[str] = set()
    for rx in (dead_claim_re, delete_claim_re):
        for m in rx.finditer(text):
            claimed_dead.add(m.group(1))

    for sym in list(claimed_dead)[:max(0, max_checks - checked)]:
        if checked >= max_checks:
            break
        result = _symbol_exists(repo_root, sym)
        if result["present"]:
            # Doc claims it's dead/deleted but we found it — flag as drifted
            drifted.append({
                "citation": f"`{sym}` (symbol)",
                "reason": (
                    f"doc claims symbol is dead/deleted but found {result['caller_count']} "
                    f"reference(s) in source. Files: {result['files'][:4]}"
                ),
                "caller_count": result["caller_count"],
            })
        else:
            ok.append({
                "citation": f"`{sym}` (symbol)",
                "note": "symbol not found in source — consistent with dead/deleted claim",
            })
        checked += 1

    return {
        "doc": str(doc),
        "checked": checked,
        "ok_count": len(ok),
        "drifted_count": len(drifted),
        "missing_count": len(missing),
        "ok": ok,
        "drifted": drifted,
        "missing": missing,
        "confidence": "lexical",
        "note": (
            "Heuristic — surfaces LIKELY-stale citations. "
            "Verify flagged items by hand before acting. "
            "Symbols with caller_count > 0 that docs call 'dead' are the highest-signal findings."
        ),
    }


# ---------------------------------------------------------------------------
# __main__ test shim
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    import sys

    if len(sys.argv) < 3:
        print("Usage: citation_verifier.py <repo_root> <doc_path> [max_checks]")
        sys.exit(1)

    root = Path(sys.argv[1])
    doc_p = sys.argv[2]
    max_c = int(sys.argv[3]) if len(sys.argv) > 3 else 200

    result = verify_citations(doc_p, root, max_c)
    print(json.dumps(result, indent=2))
