#!/usr/bin/env python3
"""
REPO-INTEL-MCP-1: repo_intel_server.py
Read-only MCP wrapper around tools/repo_intel/repo_query.py.

Exposes tools:
  repo.preflight      — branch + root guard + harness summary
  repo.slice_preflight— anti-rediscovery / stale-base gate (PASS/WARN/STOP)
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
import dead_gate_scan as dgs

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
def slice_preflight(slice: str = "", symbols: str = "", paths: str = "",
                    base: str = "", nifty: str = "claude/nifty-mendeleev") -> str:
    """
    Anti-rediscovery / stale-base gate to run BEFORE writing a recon-derived fix
    slice. Answers "should I proceed, or did a parallel lane already fix this /
    has my branch base gone stale?" — read-only, no mutations.

    Args (all optional but base+symbols give the strongest signal):
      slice    — the slice name (e.g. "WATCHID-LOAD-GUARD-1"); checked against
                 git log --grep (already landed?).
      symbols  — comma/space-separated identifiers (e.g. "watchSave,getByWatchID");
                 checked via git log -G across base..nifty (did someone change
                 them since you forked?).
      paths    — comma/space-separated repo-relative paths your slice will touch.
      base     — the commit/branch your branch was cut from (enables drift checks).
      nifty    — mainline ref (default claude/nifty-mendeleev).

    Returns JSON with verdict: PASS / WARN / STOP.
      STOP  = a target symbol changed on the mainline since base -> RE-RECON
              against current HEAD before coding (it may already be fixed).
      WARN  = slice name already in log, base is stale, paths changed, or dirty
              files overlap your targets -> review before proceeding.
      PASS  = clear to proceed.
    """
    root = _repo()
    syms = [s for s in symbols.replace(",", " ").split() if s]
    pth  = [p for p in paths.replace(",", " ").split() if p]
    result = rq.slice_preflight(
        root,
        slice_name = slice or None,
        symbols    = syms,
        paths      = pth,
        base       = base or None,
        nifty      = nifty or "claude/nifty-mendeleev",
    )
    return _j(result)


@mcp.tool()
def dirty(expect_branch: str = "", expect_root: str = "", full: bool = False) -> str:
    """
    Return dirty-file classification for the current worktree.

    Each file gets a class: protected / deploy_rail / key_source / shader / docs / normal / unknown.
    safe_to_touch=false means at least one file needs user acknowledgement before edits.

    Pass expect_branch and expect_root to enable branch + root guards.

    DIGEST DEFAULT (token cut): by default this lists ONLY elevated (non-safe)
    files and returns class_counts + omitted_safe_count for the rest. Pass
    full=true to restore the complete file list (every dirty path).
    """
    root   = _repo()
    result = rq.get_dirty_state(
        root,
        expect_branch = expect_branch or None,
        expect_root   = expect_root   or None,
        digest        = (not full),
    )
    return _j(result)


@mcp.tool()
def commit_plan(slice: str = "", paths: Optional[list] = None,
                base: str = "HEAD") -> str:
    """
    Lane attribution for a multi-lane dirty tree — flags dirty files/hunks whose
    added tokens belong to a DIFFERENT slice's .claude doc, so you can stage only
    your slice. Heuristic/advisory.

    Args:
      slice — your slice name (e.g. "EDITOR-STATIC-TEXTURE-PREWARM-1"); used to
              tell this-slice .claude docs from foreign ones.
      paths — repo-relative paths to attribute; default = all elevated dirty files.
      base  — diff base (default HEAD) for extracting added tokens.

    Returns JSON: {slice, base, confidence, note, files:[{path, recommendation,
      foreign_signals, this_slice_signals, hunk_count}]}.
    recommendation: stage / skip_or_selective / review_mixed.
    """
    root   = _repo()
    result = rq.commit_plan(
        root,
        slice_name = slice,
        paths      = paths,
        base       = base,
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
def dead_gate_scan(tier: str = "all", name: str = "") -> str:
    """
    ADVISORY, READ-ONLY classifier of MC2_* env gates by DELETABILITY EVIDENCE.
    NEVER deletes anything. Deletion is ALWAYS human-gated — verify each entry
    by hand before removing a gate.

    ⚠ DEFAULT-OFF IS *NOT* DEAD. Default-OFF is the project's deliberate
    feature-gate system. Default-OFF gates include LIVE FIXES
    (e.g. MC2_ANIM_CADENCE_FIX) and SHIPPED FEATURES
    (e.g. MC2_ASSIMP_MECH_IMPORT). A gate's default STATE is IRRELEVANT to
    deletability. This tool classifies by CODE-PATH EVIDENCE ONLY.

    Tiers:
      TIER_A_dead       — orphaned/dead code path (in_if0, unused getenv result,
                          ledger-removed-but-reader-lingers, or doc_only string
                          litter). REVIEW THEN DELETE — still human-gated.
      TIER_B_diag_strip — single-reader *_TRACE/_DIAG/_DEBUG/_PROBE diagnostic
                          not in registry/tier1-doc. Strippable clutter (gated
                          off either way, low risk).
      TIER_C_keep       — feature/fix/guard gates. DO NOT DELETE (count only).

    Args:
      tier — "A" | "B" | "all" (default). Filters which tier lists are emitted;
             totals always reflect the full classification.
      name — optional substring filter on gate name (e.g. "HDR", "BLOOM").

    Returns JSON: {confidence, note, totals, tier_a:[{name,reason,readers,
    ledger_marker}], tier_b:[{name,reader_file}], tier_c_count}. tier_a is the
    actionable list (capped ~150, tier_a_truncated flag if more).
    """
    root   = _repo()
    result = dgs.dead_gate_scan(root, tier=tier, name=name)
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
    mode: str = "content",
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
      mode           — "content" (default, full snippets) | "files" (unique file
                       list only) | "count" (counts only). "files"/"count" are
                       cheaper — no snippets — for exploratory sweeps.

    Returns (mode="content"): {matches:[{file, line, snippet}], match_count, truncated, confidence:"lexical"}
    """
    root = _repo()
    result = gt.grep_source(
        root,
        pattern       = pattern,
        include_globs = include_globs,
        exclude_globs = exclude_globs,
        case_sensitive = case_sensitive,
        max_results   = max_results,
        mode          = mode,
    )
    return _j(result)


@mcp.tool()
def repo_symbol(
    symbol: str,
    max_results: int = 300,
    in_ref: str = "",
    def_context: int = 0,
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

    Optional:
      in_ref="HEAD"   — reports presence-in-ref (is the symbol already in HEAD,
                        i.e. existing vs new) via fixed-string git grep.
      def_context=N   — attaches N lines of body context to the FIRST definition.
      caller_files    — always returned: unique list of files that reference it.

    Returns:
      {symbol, definitions:[{file,line,snippet}], references:[{file,line,snippet}],
       caller_files:[...], match_count, truncated, confidence:"lexical",
       note:"Lexical only — not clangd, not AST, not graphify.",
       in_ref:{ref,present,match_count} (when in_ref given)}
    """
    root = _repo()
    result = gt.symbol_lookup(
        root, symbol,
        max_results = max_results,
        in_ref      = in_ref,
        def_context = def_context,
    )
    return _j(result)


# ---------------------------------------------------------------------------
# Entrypoint
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    mcp.run(transport="stdio")
