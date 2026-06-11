#!/usr/bin/env python3
# scripts/check-deploy-coherence.py
"""Advisory deploy-coherence check for a deploy target directory.

Guards against the recurring deploy-target trap: a fix is built but the
deployed exe at v0.4 (game) or 0.4c (editor) is stale, producing false
bug reports.

Reads the newest deploy manifest -- .deployed_manifest.csv (written by
scripts/deploy_payload.py, preferred when newer) or .deploy-manifest.json
(written by scripts/write-deploy-manifest.py) -- and reports drift:
  - deployed files mutated/missing since the manifest was written
    (size/sha256 mismatch; mtime-only drift is informational)
  - manifest HEAD older than current branch commits touching
    code/ mclib/ GameOS/ shaders/

ALWAYS exits 0 — advisory only, never a hard gate. All warnings are
prefixed [DEPLOY_COHERENCE].

Example:
  py -3 scripts/check-deploy-coherence.py A:/Games/mc2-opengl/mc2-win64-v0.4
"""
from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

MANIFEST_NAME = ".deploy-manifest.json"
CSV_MANIFEST_NAME = ".deployed_manifest.csv"  # written by scripts/deploy_payload.py
DEFAULT_DEPLOY_DIR = "A:/Games/mc2-opengl/mc2-win64-v0.4"
CODE_PATHS = ["code", "mclib", "GameOS", "shaders"]
PREFIX = "[DEPLOY_COHERENCE]"


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _git(worktree: Path, *args: str) -> str:
    try:
        return subprocess.check_output(
            ["git", "-C", str(worktree), *args],
            text=True, stderr=subprocess.DEVNULL).strip()
    except Exception:
        return ""


def _newest_manifest(deploy_dir: Path) -> Path | None:
    """Pick the most recently written manifest; deploy_payload.py csv and
    write-deploy-manifest.py json both count."""
    candidates = [p for p in (deploy_dir / CSV_MANIFEST_NAME,
                              deploy_dir / MANIFEST_NAME) if p.is_file()]
    if not candidates:
        return None
    return max(candidates, key=lambda p: p.stat().st_mtime)


def _load_csv_manifest(path: Path) -> dict:
    """Normalize deploy_payload.py's .deployed_manifest.csv (manifest v1:
    relpath,sha256,bytes,src_commit,timestamp) into the json manifest shape."""
    import csv as _csv
    files = []
    head = ""
    written_at = "?"
    with open(path, newline="", encoding="utf-8") as f:
        for row in _csv.reader(f):
            if not row or row[0] in ("manifest_version", "relpath"):
                continue
            rel, sha, size, src_commit, ts = (row + ["", "", "", "", ""])[:5]
            files.append({"path": rel, "sha256": sha,
                          "size": int(size) if size.isdigit() else None,
                          "mtime": None})
            head = head or src_commit
            written_at = ts or written_at
    return {"git_head": head, "git_branch": "?", "git_dirty": False,
            "written_at": written_at,
            "source_worktree": f"({CSV_MANIFEST_NAME} via deploy_payload.py)",
            "files": files}


def check(deploy_dir: Path, worktree: Path, out=sys.stdout) -> int:
    """Run the coherence check; returns number of WARN lines (informational)."""
    warns = 0

    def warn(msg: str) -> None:
        nonlocal warns
        warns += 1
        print(f"{PREFIX} WARN: {msg}", file=out)

    def info(msg: str) -> None:
        print(f"{PREFIX} {msg}", file=out)

    manifest_path = _newest_manifest(deploy_dir)
    if manifest_path is None:
        info(f"no {MANIFEST_NAME} or {CSV_MANIFEST_NAME} in {deploy_dir} -- "
             "coherence unknown (deploy via scripts/deploy_payload.py or write "
             "one via scripts/write-deploy-manifest.py)")
        return 0
    try:
        if manifest_path.name == CSV_MANIFEST_NAME:
            manifest = _load_csv_manifest(manifest_path)
        else:
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except Exception as e:
        warn(f"unreadable manifest {manifest_path}: {e}")
        return warns

    head = manifest.get("git_head", "")
    info(f"manifest: written {manifest.get('written_at', '?')} from "
         f"{manifest.get('git_branch', '?')}@{head[:8]}"
         f"{' (dirty)' if manifest.get('git_dirty') else ''} "
         f"({manifest.get('source_worktree', '?')})")

    # Per-file drift vs manifest.
    for entry in manifest.get("files", []):
        rel = entry.get("path", "")
        p = deploy_dir / rel
        if not p.is_file():
            warn(f"deployed file missing: {rel}")
            continue
        st = p.stat()
        if entry.get("size") is not None and st.st_size != entry.get("size"):
            warn(f"size drift: {rel} (manifest {entry.get('size')} vs "
                 f"actual {st.st_size}) -- file changed since deploy")
            continue
        if _sha256(p) != entry.get("sha256"):
            warn(f"hash drift: {rel} -- contents changed since deploy "
                 "(stale or out-of-band overwrite)")
            continue
        m_mtime = entry.get("mtime")
        if m_mtime is not None and abs(st.st_mtime - m_mtime) > 2.0:
            info(f"note: mtime drift on {rel} but contents match (harmless)")

    # Staleness vs current branch: commits after manifest HEAD touching code dirs.
    if head:
        in_repo = _git(worktree, "cat-file", "-t", head)
        if in_repo != "commit":
            warn(f"manifest HEAD {head[:8]} not found in {worktree} -- "
                 "deployed from a different/forgotten branch?")
        else:
            newer = _git(worktree, "rev-list", "--count",
                         f"{head}..HEAD", "--", *CODE_PATHS)
            if newer and newer != "0":
                cur = _git(worktree, "rev-parse", "HEAD")
                warn(f"deployed exe is STALE: {newer} commit(s) touching "
                     f"{'/'.join(CODE_PATHS)} since manifest HEAD {head[:8]} "
                     f"(current HEAD {cur[:8]}) -- rebuild + redeploy "
                     "(remember: v0.4 = game, 0.4c = editor; deploy BOTH "
                     "targets you intend to run)")

    if warns == 0:
        info(f"OK: {deploy_dir} matches manifest, no newer code commits")
    return warns


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("deploy_dir", nargs="?", default=DEFAULT_DEPLOY_DIR)
    ap.add_argument("--worktree", default=str(Path(__file__).resolve().parents[1]),
                    help="worktree for git staleness comparison (default: this repo)")
    args = ap.parse_args()

    deploy_dir = Path(args.deploy_dir)
    if not deploy_dir.is_dir():
        print(f"{PREFIX} deploy dir not found: {deploy_dir} (skipping check)")
        return 0
    check(deploy_dir, Path(args.worktree))
    return 0  # advisory only -- never hard-fail


if __name__ == "__main__":
    sys.exit(main())
