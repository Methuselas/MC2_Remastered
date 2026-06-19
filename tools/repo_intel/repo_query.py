#!/usr/bin/env python3
"""
REPO-INTEL-1a/1b/1c/1d: repo_query.py
Read-only codebase intelligence CLI.

Usage:
    python tools/repo_intel/repo_query.py dirty
    python tools/repo_intel/repo_query.py harness build|deploy|smoke|tier1|all
    python tools/repo_intel/repo_query.py preflight
    python tools/repo_intel/repo_query.py env MC2_GPU_MECHS
    python tools/repo_intel/repo_query.py env --undocumented
    python tools/repo_intel/repo_query.py env --domain shadow
    python tools/repo_intel/repo_query.py env --all
    python tools/repo_intel/repo_query.py env          (summary only)
    python tools/repo_intel/repo_query.py binding 5
    python tools/repo_intel/repo_query.py binding --conflicts
    python tools/repo_intel/repo_query.py binding --namespace ssbo

Guards (apply to dirty + preflight):
    --expect-branch BRANCH   fail if not on BRANCH (branch contamination guard)
    --expect-root   PATH     fail if worktree root != PATH (shared-worktree guard)

Example full preflight:
    py -3 tools\\repo_intel\\repo_query.py preflight \\
        --expect-branch claude/nifty-mendeleev \\
        --expect-root A:\\Games\\mc2-opengl-src\\.claude\\worktrees\\nifty-mendeleev
"""

import json
import os
import re
import subprocess
import sys
from pathlib import Path

# Allow sibling modules (env_index, etc.) to be imported when running as script
sys.path.insert(0, str(Path(__file__).parent))


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def git(args, cwd=None, strip=True, timeout=10):
    try:
        result = subprocess.run(
            ["git"] + args,
            capture_output=True, text=True, cwd=cwd, timeout=timeout,
        )
        out = result.stdout.strip() if strip else result.stdout
        return out, result.returncode
    except subprocess.TimeoutExpired:
        return "", 1


def find_repo_root():
    out, rc = git(["rev-parse", "--show-toplevel"])
    if rc != 0:
        return None
    return Path(out)


def find_claude_md(repo_root):
    candidate = repo_root / "CLAUDE.md"
    if candidate.exists():
        return candidate
    # fall back to project-level
    parent = repo_root.parent
    candidate2 = parent / "CLAUDE.md"
    if candidate2.exists():
        return candidate2
    return None


def out_json(obj):
    print(json.dumps(obj, indent=2))


def _normalize_path(p: str) -> str:
    """Normalize a path for comparison: resolve, lower-case, forward slashes."""
    return str(Path(p).resolve()).lower().replace("\\", "/")


# ---------------------------------------------------------------------------
# File classification
# ---------------------------------------------------------------------------

# Order matters: first match wins.
_CLASSIFICATION_RULES = [
    # protected — never touch
    (re.compile(r"^(build[^/]*/|3rdparty/|\.git/|releases?/|dist/)", re.I),
     "protected", "build/3rdparty/git/release tree — never touch"),
    # generated cmake/build artefacts
    (re.compile(r"\.(vcxproj|cmake_install\.cmake|sln)$", re.I),
     "protected", "generated build artefact"),
    (re.compile(r"CMakeCache\.txt$|CMakeFiles/", re.I),
     "protected", "CMake cache/generated — never touch"),
    # deploy rail
    (re.compile(r"^scripts/deploy_payload\.py$", re.I),
     "deploy_rail", "deploy harness — edit with care"),
    (re.compile(r"^scripts/run_smoke\.py$", re.I),
     "deploy_rail", "smoke harness — edit with care"),
    # key source (render core, engine, game code, adapters)
    (re.compile(r"^(code|GameOS|mclib|RenderCore|GameAdapters)/", re.I),
     "key_source", "key engine source — requires smoke gate before merge"),
    # shaders
    (re.compile(r"^shaders/.*\.(vert|frag|comp|geom|hglsl|tesc|tese)$", re.I),
     "shader", "shader file — subject to shader discipline"),
    # docs / markdown
    (re.compile(r"^docs/|\.md$", re.I),
     "docs", "documentation"),
    # tools and scripts (general — not deploy rail)
    (re.compile(r"^(tools|scripts)/", re.I),
     "normal", "tool/script"),
    # data assets
    (re.compile(r"^data/", re.I),
     "normal", "data asset"),
    # .claude/ internal (config, memory, skills)
    (re.compile(r"^\.claude/", re.I),
     "normal", "claude config/memory"),
    # test artifacts (baselines, visual captures, smoke outputs)
    (re.compile(r"^tests/", re.I),
     "normal", "test artifact"),
    # graphify output (generated graph — not source)
    (re.compile(r"^graphify-out/", re.I),
     "normal", "graphify-generated output"),
    # runtime / temp artifacts at repo root
    (re.compile(
        r"^(run/|imgui\.ini$|sniff\.dat$|.*_stdout\.txt$|.*_stderr\.txt$|"
        r"fire_wire\.png$|.*bridge_manual.*\.txt$|central_merge.*/)"),
     "normal", "runtime/temp artifact"),
]

_SAFE_CLASSES = {"docs", "normal"}


def classify_file(rel_path: str):
    for pattern, cls, reason in _CLASSIFICATION_RULES:
        if pattern.search(rel_path):
            return cls, reason
    return "unknown", "unclassified — treat as sensitive"


def get_dirty_state(repo_root: Path, expect_branch: str = None,
                    expect_root: str = None):
    branch, _ = git(["rev-parse", "--abbrev-ref", "HEAD"], cwd=repo_root)
    head, _    = git(["rev-parse", "--short", "HEAD"],      cwd=repo_root)
    status_out, _ = git(["status", "--porcelain=v1"],        cwd=repo_root, strip=False)

    files = []
    for line in status_out.splitlines():
        if not line.strip():
            continue
        xy   = line[:2]
        path = line[3:].strip()
        # handle renames: "old -> new"
        if " -> " in path:
            path = path.split(" -> ")[-1]
        status = xy.strip() or "?"
        cls, reason = classify_file(path)
        files.append({
            "path":   path,
            "status": status,
            "class":  cls,
            "reason": reason,
        })

    dirty        = bool(files)
    unsafe_files = [f for f in files if f["class"] not in _SAFE_CLASSES]

    # Branch guard
    branch_ok      = True
    branch_warning = None
    if expect_branch:
        if branch != expect_branch:
            branch_ok      = False
            branch_warning = (
                f"current branch '{branch}' does not match expected "
                f"'{expect_branch}' — work may have landed on the wrong branch"
            )
    else:
        branch_warning = "no expected branch supplied (pass --expect-branch to enable guard)"

    # Root guard — catches shared-worktree contamination
    root_ok      = True
    root_warning = None
    actual_root  = str(repo_root)
    if expect_root:
        if _normalize_path(actual_root) != _normalize_path(expect_root):
            root_ok      = False
            root_warning = (
                f"current worktree root '{actual_root}' does not match expected "
                f"'{expect_root}' — agent is running in the wrong worktree"
            )
    else:
        root_warning = "no expected root supplied (pass --expect-root to enable guard)"

    safe_to_touch = not bool(unsafe_files) and branch_ok and root_ok
    requires_ack  = not safe_to_touch

    result = {
        "repo_root":         actual_root,
        "branch":            branch,
        "head":              head,
        "dirty":             dirty,
        "safe_to_touch":     safe_to_touch,
        "requires_user_ack": requires_ack,
        "files":             files,
        "summary": (
            f"{len(files)} dirty file(s); "
            f"{len(unsafe_files)} require ack"
            if dirty else "clean"
        ),
    }
    if expect_branch:
        result["expected_branch"] = expect_branch
        result["branch_ok"]       = branch_ok
    if branch_warning:
        result["branch_warning"] = branch_warning
    if expect_root:
        result["expected_root"] = expect_root
        result["root_ok"]       = root_ok
    if root_warning:
        result["root_warning"] = root_warning
    return result


# ---------------------------------------------------------------------------
# Harness parsing
# ---------------------------------------------------------------------------

_HARNESSES = {
    "build": {
        "description": "Build mc2 exe (RelWithDebInfo)",
        # Extracted from CLAUDE.md 'Codex build/deploy rails' + cmake path
        "search_patterns": [
            # CMake path line
            re.compile(r"CMake:\s+(.+cmake\.exe)", re.I),
            # build flags
            re.compile(r"--build\s+build64\s+--config\s+RelWithDebInfo\s+--target\s+mc2"),
        ],
        "note": (
            "Invoke via /mc2-build skill. "
            "Do not substitute MSBuild, random cmake, or manual copy commands."
        ),
    },
    "deploy": {
        "description": "Deploy payload to release directory",
        "search_patterns": [
            re.compile(r"scripts/deploy_payload\.py[^\n]*", re.I),
        ],
        "note": (
            "Invoke scripts/deploy_payload.py with --source-root, --build-dir, --exe-name. "
            "For rc1 pass target dir positionally. Never copy files manually."
        ),
    },
    "smoke": {
        "description": "Smoke test gate (tier1, 30s, all 5 missions)",
        "search_patterns": [
            # Capture the canonical powershell block
            re.compile(
                r"Canonical invocation.*?```(?:powershell)?\n(.+?)\n```",
                re.S | re.I
            ),
        ],
        "note": "ALWAYS --keep-logs. NEVER --kill-existing. NEVER --duration >30.",
    },
    "tier1": {
        "description": "Smoke test gate — tier1 5-mission full run",
        "search_patterns": [
            re.compile(
                r"Canonical invocation.*?```(?:powershell)?\n(.+?)\n```",
                re.S | re.I
            ),
        ],
        "note": "Same command as smoke. --tier tier1 --duration 30 --keep-logs.",
    },
}


def parse_harness(name: str, claude_md_path: Path):
    if name not in _HARNESSES:
        return {
            "name": name,
            "error": f"Unknown harness '{name}'. Valid: build, deploy, smoke, tier1",
        }

    h      = _HARNESSES[name]
    text   = claude_md_path.read_text(encoding="utf-8", errors="replace")
    source = str(claude_md_path)

    command   = None
    evidence  = []
    ambiguous = False

    if name in ("smoke", "tier1"):
        # Extract the canonical powershell block verbatim
        m = re.search(
            r"Canonical invocation[^\n]*\n+```(?:powershell)?\n(.+?)\n```",
            text, re.S | re.I
        )
        if m:
            command  = m.group(1).strip()
            evidence = [{"match": command[:120] + ("..." if len(command) > 120 else "")}]
        else:
            ambiguous = True

    elif name == "build":
        cmake_path = None
        # CMake line may have backtick-quoted path with spaces
        m = re.search(r"CMake:\s+`?(.+?cmake\.exe)`?", text, re.I)
        if m:
            cmake_path = m.group(1).strip()
        if cmake_path:
            command = f'"{cmake_path}" --build build64 --config RelWithDebInfo --target mc2'
            evidence = [{"cmake_exe": cmake_path}]
        else:
            ambiguous = True

    elif name == "deploy":
        m = re.search(r"Deploy only with[^\n]*(`scripts/deploy_payload\.py`)[^\n]*", text, re.I)
        if m:
            # Return the canonical script name + documented arg pattern
            command  = "scripts/deploy_payload.py <rc1-target-dir> --source-root <path> --build-dir build64 --exe-name mc2.exe"
            evidence = [{"source_line": m.group(0)[:120]}]
        else:
            ambiguous = True

    result = {
        "name":        name,
        "source":      source,
        "description": h["description"],
        "confidence":  "ambiguous" if ambiguous else "documented",
        "note":        h["note"],
    }
    if command:
        result["command"] = command
    if evidence:
        result["evidence"] = evidence
    if ambiguous:
        result["warning"] = (
            f"Could not extract '{name}' command from {source}. "
            "Do not invent a command — read CLAUDE.md manually."
        )
    return result


# ---------------------------------------------------------------------------
# Preflight
# ---------------------------------------------------------------------------

def all_harnesses(claude_md_path: Path):
    names = ["build", "deploy", "smoke", "tier1"]
    return {name: parse_harness(name, claude_md_path) for name in names}


def preflight(repo_root: Path, claude_md_path: Path, expect_branch: str = None,
              expect_root: str = None):
    dirty   = get_dirty_state(repo_root, expect_branch=expect_branch,
                               expect_root=expect_root)
    harness = all_harnesses(claude_md_path)

    all_documented = all(
        h.get("confidence") == "documented" for h in harness.values()
    )
    ok = not dirty["requires_user_ack"] and all_documented

    harness_status = " ".join(
        f"{name}={h.get('confidence','unknown')}"
        for name, h in harness.items()
    )
    summary_parts = [
        f"dirty={str(dirty['dirty']).lower()}",
        f"safe_to_touch={str(dirty['safe_to_touch']).lower()}",
        f"branch={dirty['branch']}",
        f"head={dirty['head']}",
        f"harness=[{harness_status}]",
    ]
    if expect_branch:
        summary_parts.append(f"branch_ok={str(dirty.get('branch_ok', True)).lower()}")
    if expect_root:
        summary_parts.append(f"root_ok={str(dirty.get('root_ok', True)).lower()}")
    summary = "PRECHECK: " + " ".join(summary_parts)

    return {
        "ok":      ok,
        "summary": summary,
        "dirty":   dirty,
        "harness": harness,
    }


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def _parse_flags(args):
    """Extract --expect-branch and --expect-root from args.
    Returns (remaining_args, expect_branch, expect_root)."""
    expect_branch = None
    expect_root   = None
    remaining     = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--expect-branch":
            if i + 1 < len(args):
                i += 1
                expect_branch = args[i]
        elif a.startswith("--expect-branch="):
            expect_branch = a.split("=", 1)[1]
        elif a == "--expect-root":
            if i + 1 < len(args):
                i += 1
                expect_root = args[i]
        elif a.startswith("--expect-root="):
            expect_root = a.split("=", 1)[1]
        else:
            remaining.append(a)
        i += 1
    return remaining, expect_branch, expect_root


def main():
    raw_args = sys.argv[1:]
    if not raw_args:
        print(
            "Usage: repo_query.py <dirty|harness <name>|preflight|\n"
            "                      env [MC2_NAME|--undocumented|--domain D|--all]|\n"
            "                      binding [N|--conflicts|--namespace NS|--all]>\n"
            "       [--expect-branch BRANCH] [--expect-root PATH]\n"
            "\n"
            "Example full preflight:\n"
            r"  py -3 tools\repo_intel\repo_query.py preflight"
            " --expect-branch claude/nifty-mendeleev"
            r" --expect-root A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev",
            file=sys.stderr,
        )
        sys.exit(1)

    args, expect_branch, expect_root = _parse_flags(raw_args)

    repo_root = find_repo_root()
    if repo_root is None:
        out_json({"error": "Not inside a git repository"})
        sys.exit(1)

    claude_md = find_claude_md(repo_root)

    cmd = args[0].lower() if args else ""

    if cmd == "dirty":
        out_json(get_dirty_state(repo_root, expect_branch=expect_branch,
                                 expect_root=expect_root))

    elif cmd == "harness":
        if len(args) < 2:
            out_json({"error": "harness requires a name: build | deploy | smoke | tier1 | all"})
            sys.exit(1)
        if claude_md is None:
            out_json({"error": "CLAUDE.md not found — cannot parse harnesses"})
            sys.exit(1)
        name = args[1].lower()
        if name == "all":
            out_json(all_harnesses(claude_md))
        else:
            out_json(parse_harness(name, claude_md))

    elif cmd == "preflight":
        if claude_md is None:
            out_json({"error": "CLAUDE.md not found — cannot parse harnesses"})
            sys.exit(1)
        result = preflight(repo_root, claude_md, expect_branch=expect_branch,
                           expect_root=expect_root)
        print(result["summary"])
        out_json(result)

    elif cmd == "binding":
        from binding_index import query_binding
        rest      = args[1:]
        binding_n  = None
        namespace  = None
        conflicts  = False
        show_all   = False
        i = 0
        while i < len(rest):
            a = rest[i]
            if a == '--conflicts':
                conflicts = True
            elif a == '--all':
                show_all = True
            elif a.startswith('--namespace'):
                if '=' in a:
                    namespace = a.split('=', 1)[1]
                elif i + 1 < len(rest):
                    i += 1
                    namespace = rest[i]
            elif not a.startswith('--'):
                try:
                    binding_n = int(a)
                except ValueError:
                    out_json({'error': f"Binding number must be an integer, got '{a}'"})
                    sys.exit(1)
            i += 1
        out_json(query_binding(repo_root, binding=binding_n, namespace=namespace,
                               conflicts=conflicts, show_all=show_all))

    elif cmd == "env":
        from env_index import query_env
        rest = args[1:]
        name         = None
        domain       = None
        undocumented = False
        show_all     = False
        i = 0
        while i < len(rest):
            a = rest[i]
            if a == "--undocumented":
                undocumented = True
            elif a == "--all":
                show_all = True
            elif a.startswith("--domain"):
                if "=" in a:
                    domain = a.split("=", 1)[1]
                elif i + 1 < len(rest):
                    i += 1
                    domain = rest[i]
            elif not a.startswith("--"):
                name = a
            i += 1
        out_json(query_env(repo_root, name=name, domain=domain,
                           undocumented=undocumented, show_all=show_all))

    else:
        out_json({"error": f"Unknown command '{cmd}'. Valid: dirty, harness, preflight, env, binding"})
        sys.exit(1)


if __name__ == "__main__":
    main()
