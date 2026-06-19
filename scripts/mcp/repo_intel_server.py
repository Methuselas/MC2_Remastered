#!/usr/bin/env python3
"""
REPO-INTEL-MCP-1: repo_intel_server.py
Read-only MCP wrapper around tools/repo_intel/repo_query.py.

Exposes tools:
  repo.preflight      — branch + root guard + harness summary
  repo.dirty          — dirty-file classification with optional guards
  repo.env_var        — env var index query (MC2_* variables)
  repo.shader_binding — GL binding point index query
  repo.harness        — canonical build/deploy/smoke command lookup
  repo.grep           — lexical pattern search with sane default excludes
  repo.symbol         — best-effort definition/reference split (lexical, not AST)

No new indexing logic. No writes. No build/deploy execution.
Read-only.

Configuration:
  REPO_INTEL_ROOT  — override worktree root (default: two parents above this file)
"""

import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Optional

# ---------------------------------------------------------------------------
# Path setup — allow importing sibling repo_intel library
# ---------------------------------------------------------------------------

_MCP_DIR      = Path(__file__).resolve().parent          # scripts/mcp/
_WORKTREE_DIR = _MCP_DIR.parents[1]                      # worktree root
_REPO_INTEL   = _WORKTREE_DIR / "tools" / "repo_intel"

sys.path.insert(0, str(_REPO_INTEL))

# Override worktree root via env var for testing in non-standard layouts
_ROOT_OVERRIDE = os.environ.get("REPO_INTEL_ROOT")
_REPO_ROOT     = Path(_ROOT_OVERRIDE).resolve() if _ROOT_OVERRIDE else _WORKTREE_DIR

from mcp.server.fastmcp import FastMCP

import repo_query  as rq
import env_index   as ei
import binding_index as bi
import grep_tool   as gt

# ---------------------------------------------------------------------------
# Server
# ---------------------------------------------------------------------------

mcp = FastMCP(
    "mc2-repo-intel",
    instructions=(
        "Read-only codebase intelligence for the MC2 OpenGL source repo. "
        "Always call preflight() first — it checks branch, worktree root, dirty state, "
        "and canonical harness commands in one shot. "
        "If preflight returns branch_ok=false or root_ok=false, STOP and report to user. "
        "If safe_to_touch=false, report elevated dirty files and wait for direction. "
        "All tools return JSON. No mutations, no build/deploy execution."
    ),
)


def _repo() -> Path:
    """Return the live repo root (from git rev-parse or env override)."""
    if _ROOT_OVERRIDE:
        return _REPO_ROOT
    detected = rq.find_repo_root()
    return detected if detected else _REPO_ROOT


def _claude_md() -> Optional[Path]:
    root = _repo()
    return rq.find_claude_md(root)


def _j(obj) -> str:
    return json.dumps(obj, indent=2)


def _git_fast(args: list, cwd: Path, timeout: int = 5) -> tuple:
    """Run a single git command with a hard timeout. Returns (stdout, ok).
    stdin=DEVNULL: prevents git from reading the MCP stdio pipe."""
    try:
        r = subprocess.run(
            ["git"] + args,
            capture_output=True, text=True, cwd=str(cwd), timeout=timeout,
            stdin=subprocess.DEVNULL,
        )
        return r.stdout.strip(), r.returncode == 0
    except subprocess.TimeoutExpired:
        return "", False
    except Exception:
        return "", False


# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------

@mcp.tool()
def preflight_fast(expect_branch: str = "", expect_root: str = "") -> str:
    """
    Fast branch + root guard only. Two git rev-parse calls, <200ms.
    No git status, no harness parse, no source walk.

    Use this as the mandatory session-start seatbelt.
    Use preflight() only when you also need dirty-file detail or harness commands.

    HARD STOP RULES:
      branch_ok=false → stop immediately, do not edit
      root_ok=false   → stop immediately, do not edit
    """
    root = _repo()

    branch, branch_ok_call = _git_fast(["rev-parse", "--abbrev-ref", "HEAD"], root)
    head,   _              = _git_fast(["rev-parse", "--short",       "HEAD"], root)
    actual_root, root_ok_call = _git_fast(["rev-parse", "--show-toplevel"],   root)

    if not branch_ok_call:
        return _j({"error": "git rev-parse failed — is this a git repo?", "repo_root": str(root)})

    branch_ok = True
    root_ok   = True
    warnings  = []

    if expect_branch and branch != expect_branch:
        branch_ok = False
        warnings.append(f"branch '{branch}' != expected '{expect_branch}'")

    if expect_root and root_ok_call:
        if rq._normalize_path(actual_root) != rq._normalize_path(expect_root):
            root_ok = False
            warnings.append(f"root '{actual_root}' != expected '{expect_root}'")

    return _j({
        "branch_ok":       branch_ok,
        "root_ok":         root_ok,
        "safe_to_proceed": branch_ok and root_ok,
        "branch":          branch,
        "head":            head,
        "repo_root":       actual_root or str(root),
        "warnings":        warnings,
    })


@mcp.tool()
def preflight(expect_branch: str = "", expect_root: str = "") -> str:
    """
    Run full preflight: branch guard, worktree-root guard, dirty-file classification,
    and harness command lookup.

    Pass expect_branch (e.g. "claude/nifty-mendeleev") and expect_root
    (absolute path to worktree) to enable guards. Omit to skip them.

    Returns JSON with keys: ok, summary (PRECHECK line), dirty, harness.
    summary contains branch_ok and root_ok when guards are enabled.

    HARD STOP RULES (enforced by caller, not this tool):
      branch_ok=false → stop, do not switch branches or stash
      root_ok=false   → stop, do not repair
      safe_to_touch=false → report elevated dirty files, wait for user direction
    """
    root  = _repo()
    cml   = _claude_md()
    if cml is None:
        return _j({"error": "CLAUDE.md not found", "repo_root": str(root)})
    result = rq.preflight(
        root, cml,
        expect_branch = expect_branch or None,
        expect_root   = expect_root   or None,
    )
    return _j(result)


@mcp.tool()
def dirty(expect_branch: str = "", expect_root: str = "") -> str:
    """
    Return dirty-file classification for the current worktree.

    Each file gets a class: protected / deploy_rail / key_source / shader / docs / normal / unknown.
    safe_to_touch=false means at least one file needs user acknowledgement before edits.

    Pass expect_branch and expect_root to enable branch + root guards.
    """
    root   = _repo()
    result = rq.get_dirty_state(
        root,
        expect_branch = expect_branch or None,
        expect_root   = expect_root   or None,
    )
    return _j(result)


@mcp.tool()
def env_var(
    name: str = "",
    domain: str = "",
    undocumented: bool = False,
    show_all: bool = False,
) -> str:
    """
    Query the MC2_* environment variable index.

    Modes (priority order):
      name="MC2_FOO"     — look up one var; fuzzy substring if no exact match
      undocumented=true  — vars present in code but not in registry or tier1 docs
      domain="shadow"    — filter by substring in name (e.g. "shadow", "terrain")
      show_all=true      — dump all vars (slow; ~500 entries)
      (default)          — summary counts only

    Returns JSON with sources: registry (RendererFeatureRegistry.h),
    tier1_doc (docs/tier1_env_vars.md), getenv_grep (source walk).
    """
    root   = _repo()
    result = ei.query_env(
        root,
        name         = name         or None,
        domain       = domain       or None,
        undocumented = undocumented,
        show_all     = show_all,
    )
    return _j(result)


@mcp.tool()
def shader_binding(
    binding: int = -1,
    namespace: str = "",
    conflicts: bool = False,
    show_all: bool = False,
) -> str:
    """
    Query the GL binding point index (UBO, SSBO, texture, image).

    Modes (priority order):
      binding=N [namespace="ssbo"]  — look up slot N (all namespaces if namespace omitted)
      conflicts=true                — slots with shader uses not documented in render-binding-registry.md
      namespace="ssbo"              — all slots in one namespace
      (default)                     — summary counts per namespace

    UBO and SSBO slots are independent namespaces; slot 5 in ssbo != slot 5 in ubo.
    Sources: docs/render-binding-registry.md + shaders/ live grep + C++ glBindBufferBase grep.
    """
    root = _repo()
    result = bi.query_binding(
        root,
        binding   = binding   if binding >= 0 else None,
        namespace = namespace  or None,
        conflicts = conflicts,
        show_all  = show_all,
    )
    return _j(result)


@mcp.tool()
def harness(name: str = "all") -> str:
    """
    Return the canonical command for a build/deploy/smoke harness.

    name: "build" | "deploy" | "smoke" | "tier1" | "all"

    Extracts the exact command from CLAUDE.md — do not invent commands.
    The returned command field is the ground truth to copy-paste verbatim.

    smoke/tier1: NEVER add --kill-existing. NEVER --duration > 30. ALWAYS --keep-logs.
    build: invoke via /mc2-build skill. Never substitute direct MSBuild.
    deploy: scripts/deploy_payload.py only. Never manual file copies.
    """
    root = _repo()
    cml  = _claude_md()
    if cml is None:
        return _j({"error": "CLAUDE.md not found", "repo_root": str(root)})

    if name == "all":
        result = rq.all_harnesses(cml)
    else:
        result = rq.parse_harness(name, cml)
    return _j(result)


@mcp.tool()
def repo_grep(
    pattern: str,
    include_globs: Optional[list] = None,
    exclude_globs: Optional[list] = None,
    case_sensitive: bool = True,
    max_results: int = 200,
) -> str:
    """
    Lexical grep over the MC2 source tree.

    Default excludes (always applied):
      .git/  build64/  releases/  3rdparty/  .claude/
      tests/smoke/artifacts/  *.log  *.jsonl  binaries

    Parameters:
      pattern        — Python regex (ripgrep if available, else re fallback)
      include_globs  — restrict to matching filenames (e.g. ["*.cpp", "*.h"])
      exclude_globs  — additional globs to exclude
      case_sensitive — default True
      max_results    — cap (default 200); set higher for broad sweeps

    Returns: {matches:[{file, line, snippet}], match_count, truncated, confidence:"lexical"}
    """
    root = _repo()
    result = gt.grep_source(
        root,
        pattern       = pattern,
        include_globs = include_globs,
        exclude_globs = exclude_globs,
        case_sensitive = case_sensitive,
        max_results   = max_results,
    )
    return _j(result)


@mcp.tool()
def repo_symbol(
    symbol: str,
    max_results: int = 300,
) -> str:
    """
    Best-effort lexical symbol lookup. NOT clangd, NOT AST, NOT graphify.

    Searches for \\bsymbol\\b across the source tree, then splits hits into
    definition-candidates and references using C++ heuristics (typed decl,
    class/struct/enum keyword, function signature, #define).

    Useful for:
      "where is MC2_BUILDING_PBR read?"
      "where is TG_SetRenderShapePbrOverride called?"
      "which files reference RenderObjectDesc::gameObjectId?"

    Returns:
      {symbol, definitions:[{file,line,snippet}], references:[{file,line,snippet}],
       match_count, truncated, confidence:"lexical",
       note:"Lexical only — not clangd, not AST, not graphify."}
    """
    root = _repo()
    result = gt.symbol_lookup(root, symbol, max_results=max_results)
    return _j(result)


# ---------------------------------------------------------------------------
# Entrypoint
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    mcp.run(transport="stdio")
