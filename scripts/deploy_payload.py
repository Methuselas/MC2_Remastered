#!/usr/bin/env python3
"""deploy_payload.py — deploy payload completeness gate (slice S8 seed, manifest v1).

Wraps/validates a deploy of the MC2 runtime payload (exe + pdb + ffmpeg DLLs +
shaders) from a source worktree build to a runtime target dir (e.g.
A:/Games/mc2-opengl/mc2-win64-v0.4 or .../mc2-win64-0.4c).

Hardens against the four documented stale-deploy failure classes
(docs/superpowers/recon/scratch-accel-1-deploy.md):
  A. stale exe via process lock          -> Step 0 lock check, HARD FAIL, never taskkill
  B. stale shaders via cp -r no-overwrite -> per-file copy + post-copy byte diff, HARD FAIL
  C. wrong deploy target                  -> .deployed_manifest.csv identity record
  D. stale PDB                            -> PDB present + hash == source, HARD FAIL
                                             (override: --allow-stale-pdb)

After a successful deploy, writes <target>/.deployed_manifest.csv (manifest v1):
  rows = relpath,sha256,bytes,src_commit,timestamp

--verify-only re-checks an existing target against its manifest (post-hoc
staleness detector). Advisory: exit 0 with a report, unless --strict.

Python (not .sh) to match sibling conventions: scripts/ is python-dominated
(run_smoke.py, check-asset-manifests.py, ...) and we need sha256 + csv + robust
Windows file-lock probing, all stdlib here.

NO engine code. NO run_smoke verdict-path involvement. tier1 not required
(scripts-only, zero runtime change).
"""

import argparse
import csv
import ctypes
import datetime
import hashlib
import os
import shutil
import subprocess
import sys

MANIFEST_NAME = ".deployed_manifest.csv"
MANIFEST_VERSION = "v1"
SHADER_EXTS = (".vert", ".frag", ".tesc", ".tese")
FFMPEG_DLLS = [
    "avcodec-61.dll",
    "avformat-61.dll",
    "avutil-59.dll",
    "swscale-8.dll",
    "swresample-5.dll",
]


def log(msg):
    print(f"[deploy_payload] {msg}")


def fail(msg, code=1):
    print(f"[deploy_payload] FAIL: {msg}", file=sys.stderr)
    sys.exit(code)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def repo_root():
    """Source worktree root = parent of the scripts/ dir this file lives in."""
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def git_head(root):
    try:
        out = subprocess.run(
            ["git", "-C", root, "rev-parse", "HEAD"],
            capture_output=True, text=True, timeout=30,
        )
        if out.returncode == 0:
            return out.stdout.strip()
    except Exception:
        pass
    return "unknown"


# ---------------------------------------------------------------------------
# Step 0: exe lock check (Mistake A). Windows: a running process holding the
# target exe makes overwrite silently fail / be refused. Probe by attempting
# an exclusive-open of the existing target binary. NEVER taskkill.
# ---------------------------------------------------------------------------

def is_file_locked_windows(path):
    if not os.path.exists(path):
        return False
    GENERIC_WRITE = 0x40000000
    OPEN_EXISTING = 3
    FILE_SHARE_NONE = 0
    INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
    k32 = ctypes.windll.kernel32
    k32.CreateFileW.restype = ctypes.c_void_p  # default c_int truncates -1
    handle = k32.CreateFileW(path, GENERIC_WRITE, FILE_SHARE_NONE,
                             None, OPEN_EXISTING, 0, None)
    if handle is None or handle == INVALID_HANDLE_VALUE:
        return True  # ERROR_SHARING_VIOLATION et al -> locked
    k32.CloseHandle(ctypes.c_void_p(handle))
    return False


def running_processes_named(names):
    """Report (advisory context) running processes matching names via tasklist."""
    found = []
    try:
        out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"],
                             capture_output=True, text=True, timeout=30)
        for line in out.stdout.splitlines():
            for n in names:
                if line.lower().startswith(f'"{n.lower()}'):
                    found.append(line.split(",")[0].strip('"'))
    except Exception:
        pass
    return found


def check_locks(target_dir, exe_name):
    target_exe = os.path.join(target_dir, exe_name)
    if sys.platform == "win32" and is_file_locked_windows(target_exe):
        procs = running_processes_named(["mc2.exe", "Mission Editor.exe",
                                         "mc2_asset_viewer.exe"])
        hint = f" Running candidates: {', '.join(procs)}." if procs else ""
        fail(
            f"target exe is LOCKED by a running process: {target_exe}.{hint}\n"
            "  Copy-to-locked-exe silently fails on Windows (stale-exe trap, "
            "Mistake A). Close the game/editor yourself and re-run. "
            "This script will NEVER taskkill automatically.",
            code=2,
        )
    log(f"lock check OK: {target_exe} not held by a running process")


# ---------------------------------------------------------------------------
# Payload enumeration
# ---------------------------------------------------------------------------

def enumerate_payload(src_root, build_dir, exe_name, pdb_name):
    """Returns list of (src_abspath, target_relpath, kind)."""
    items = []
    exe_src = os.path.join(build_dir, exe_name)
    if not os.path.isfile(exe_src):
        fail(f"source exe not found: {exe_src}")
    items.append((exe_src, exe_name, "exe"))

    pdb_src = os.path.join(build_dir, pdb_name)
    items.append((pdb_src, pdb_name, "pdb"))  # presence checked later

    for dll in FFMPEG_DLLS:
        p = os.path.join(build_dir, dll)
        if os.path.isfile(p):
            items.append((p, dll, "dll"))

    shader_dir = os.path.join(src_root, "shaders")
    if not os.path.isdir(shader_dir):
        fail(f"source shader dir not found: {shader_dir}")
    for fn in sorted(os.listdir(shader_dir)):
        if fn.lower().endswith(SHADER_EXTS):
            items.append((os.path.join(shader_dir, fn),
                          f"shaders/{fn}", "shader"))
    inc_dir = os.path.join(shader_dir, "include")
    if os.path.isdir(inc_dir):
        for fn in sorted(os.listdir(inc_dir)):
            p = os.path.join(inc_dir, fn)
            if os.path.isfile(p):
                items.append((p, f"shaders/include/{fn}", "shader"))
    return items


# ---------------------------------------------------------------------------
# Deploy + verify
# ---------------------------------------------------------------------------

def deploy(items, target_dir, allow_stale_pdb):
    src_hashes = {}
    shader_mismatches = []
    pdb_problem = None

    for src, rel, kind in items:
        dst = os.path.join(target_dir, rel)
        if kind == "pdb" and not os.path.isfile(src):
            pdb_problem = f"source PDB missing: {src}"
            continue
        os.makedirs(os.path.dirname(dst) or target_dir, exist_ok=True)
        try:
            shutil.copyfile(src, dst)  # per-file copy, overwrite (never cp -r)
        except PermissionError:
            fail(f"copy refused (file locked?): {dst}\n"
                 "  A process is holding this file. Close it and re-run; "
                 "this script never taskkills.", 2)
        s_hash = sha256_file(src)
        d_hash = sha256_file(dst)
        src_hashes[rel] = (d_hash, os.path.getsize(dst))
        if s_hash != d_hash:
            if kind == "exe":
                fail(f"post-copy exe hash MISMATCH: {rel}\n"
                     f"  source   {s_hash}\n  deployed {d_hash}\n"
                     "  Deployed exe is STALE (locked file / copy failure).", 3)
            elif kind == "shader":
                shader_mismatches.append(rel)
            elif kind == "pdb":
                pdb_problem = f"deployed PDB hash != source ({rel})"
            else:
                shader_mismatches.append(rel)  # DLLs hard-fail too
        log(f"deployed {rel} ({'OK' if s_hash == d_hash else 'MISMATCH'})")

    if shader_mismatches:
        fail("post-copy diff MISMATCH (stale files in target):\n  "
             + "\n  ".join(shader_mismatches), 4)

    if pdb_problem:
        if allow_stale_pdb:
            log(f"WARNING (--allow-stale-pdb): {pdb_problem}")
        else:
            fail(f"{pdb_problem}\n  Stale PDB = wrong Tracy symbols "
                 "(Mistake D). Use --allow-stale-pdb to override.", 5)
    else:
        log("PDB check OK: deployed PDB matches source hash")

    return src_hashes


def write_manifest(target_dir, hashes, src_commit):
    ts = datetime.datetime.now().isoformat(timespec="seconds")
    path = os.path.join(target_dir, MANIFEST_NAME)
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["manifest_version", MANIFEST_VERSION, "", "", ""])
        w.writerow(["relpath", "sha256", "bytes", "src_commit", "timestamp"])
        for rel in sorted(hashes):
            h, size = hashes[rel]
            w.writerow([rel, h, size, src_commit, ts])
    log(f"manifest written: {path} ({len(hashes)} rows, {MANIFEST_VERSION}, "
        f"src_commit {src_commit[:12]})")


def verify_only(target_dir, strict):
    path = os.path.join(target_dir, MANIFEST_NAME)
    if not os.path.isfile(path):
        msg = f"no manifest at {path} — target never deployed via this tool"
        if strict:
            fail(msg)
        log(f"ADVISORY: {msg}")
        return 0
    stale, missing, ok = [], [], 0
    with open(path, newline="") as f:
        rows = list(csv.reader(f))
    if not rows or rows[0][:2] != ["manifest_version", MANIFEST_VERSION]:
        fail(f"unrecognized manifest version in {path}")
    for row in rows[2:]:
        if len(row) < 3:
            continue
        rel, expect_hash, _ = row[0], row[1], row[2]
        p = os.path.join(target_dir, rel)
        if not os.path.isfile(p):
            missing.append(rel)
        elif sha256_file(p) != expect_hash:
            stale.append(rel)
        else:
            ok += 1
    log(f"verify: {ok} match, {len(stale)} stale, {len(missing)} missing")
    for r in stale:
        log(f"  STALE:   {r}")
    for r in missing:
        log(f"  MISSING: {r}")
    if (stale or missing):
        if strict:
            fail("target drifted from manifest (--strict)", 6)
        log("ADVISORY: target drifted from manifest (exit 0; use --strict to gate)")
    else:
        log("target matches manifest")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("target", help="deploy target dir (e.g. "
                    "A:/Games/mc2-opengl/mc2-win64-v0.4)")
    ap.add_argument("--source-root", default=repo_root(),
                    help="source worktree root (default: this script's repo)")
    ap.add_argument("--build-dir", default=None,
                    help="build output dir (default: <source-root>/build64/RelWithDebInfo)")
    ap.add_argument("--exe-name", default="mc2.exe",
                    help="exe filename (default mc2.exe; editor lane differs)")
    ap.add_argument("--allow-stale-pdb", action="store_true",
                    help="downgrade PDB staleness to a warning")
    ap.add_argument("--verify-only", action="store_true",
                    help="re-check existing target against its manifest; no copy")
    ap.add_argument("--strict", action="store_true",
                    help="with --verify-only: drift = nonzero exit")
    args = ap.parse_args()

    target = os.path.abspath(args.target)
    if args.verify_only:
        sys.exit(verify_only(target, args.strict))

    if not os.path.isdir(target):
        fail(f"target dir does not exist: {target} (refusing to create deploy "
             "targets — wrong-target trap, Mistake C)")

    src_root = os.path.abspath(args.source_root)
    build_dir = args.build_dir or os.path.join(src_root, "build64", "RelWithDebInfo")
    pdb_name = os.path.splitext(args.exe_name)[0] + ".pdb"

    log(f"source root: {src_root}")
    log(f"build dir:   {build_dir}")
    log(f"target:      {target}")

    check_locks(target, args.exe_name)
    items = enumerate_payload(src_root, build_dir, args.exe_name, pdb_name)
    log(f"payload: {len(items)} files "
        f"({sum(1 for *_x, k in items if k == 'shader')} shaders)")
    hashes = deploy(items, target, args.allow_stale_pdb)
    write_manifest(target, hashes, git_head(src_root))
    log("deploy COMPLETE — payload verified, manifest written")


if __name__ == "__main__":
    main()
