#!/usr/bin/env python3
"""deploy_payload.py — deploy payload completeness gate (slice S8 seed, manifest v1).

Wraps/validates a deploy of the MC2 runtime payload from a source worktree
build to a runtime target dir (e.g. A:/Games/mc2-opengl/mc2-win64-v0.4 or
.../mc2-win64-0.4c). Payload, by target:
  both    : exe + pdb + ffmpeg DLLs + shaders/ + run-with-log.bat
  game    : + run-mc2.bat
  editor  : + run-editor.bat + tools/terrain_gen/** (authoring support — the
            editor's Generate-Mission / -gen-map path shells `py -3
            tools\\terrain_gen\\terrain_gen.py`; without it the editor builds
            and launches but every gen/mission-load smoke case fails)
Support files (launch .bat, terrain_gen tree) are source-tracked, not build
outputs, so build -> deploy stays self-complete with no manual copy step.

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
SHADER_EXTS = (".vert", ".frag", ".tesc", ".tese", ".comp")

# Editor target marker (exe_name match, case-insensitive).
EDITOR_EXE = "mission editor.exe"

# Launch scripts that belong in a user download, by target. Sourced from
# src_root (tracked at repo root). run-with-log.bat is the generic stderr-
# capturing launcher and ships with both.
SUPPORT_SCRIPTS = {
    "game":   ["run-mc2.bat", "run-with-log.bat"],
    "editor": ["run-editor.bat", "run-with-log.bat"],
}

# mc2-launcher.exe — the install front-door (campaign mod-picker GUI that
# launches mc2.exe with MC2_ACTIVE_MOD). A separate build target
# (tools/mc2_launcher -> build64/out/mc2_launcher/<cfg>/mc2-launcher.exe), so
# it lives outside build_dir; we locate it from the build64 root. Ships with
# both game and editor installs.
LAUNCHER_NAME = "mc2-launcher.exe"
LAUNCHER_FROM_BUILD64 = os.path.join("out", "mc2_launcher")  # + <cfg> + exe

# Editor authoring support trees (src_root-relative dirs copied recursively,
# structure preserved). The editor's -gen-map / Generate Mission path shells
# `py -3 tools\\terrain_gen\\terrain_gen.py`; without this tree the editor
# builds and launches but every gen/mission-load smoke case fails. Tying it to
# the deploy payload keeps "build -> deploy" self-complete (no manual copy).
EDITOR_SUPPORT_TREES = ["tools/terrain_gen"]

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

def enumerate_payload(src_root, build_dir, exe_name, pdb_name,
                      require_build=True):
    """Returns list of (src_abspath, target_relpath, kind).

    require_build=False (manifest-only): the build outputs may be gone; we
    only need the relpath list to know which target files to re-hash, so a
    missing source exe/dll is tolerated (the entry is still emitted so the
    deployed copy can be recorded).
    """
    items = []
    exe_src = os.path.join(build_dir, exe_name)
    if require_build and not os.path.isfile(exe_src):
        fail(f"source exe not found: {exe_src}")
    items.append((exe_src, exe_name, "exe"))

    pdb_src = os.path.join(build_dir, pdb_name)
    items.append((pdb_src, pdb_name, "pdb"))  # presence checked later

    for dll in FFMPEG_DLLS:
        p = os.path.join(build_dir, dll)
        if os.path.isfile(p):
            items.append((p, dll, "dll"))

    # mc2-launcher.exe: separate build target outside build_dir. Locate via the
    # build64 root (walk up from build_dir) using this deploy's config name.
    bd = os.path.abspath(build_dir).rstrip(os.sep)
    cfg = os.path.basename(bd)  # e.g. RelWithDebInfo
    b64 = bd
    while b64 and os.path.basename(b64) != "build64":
        parent = os.path.dirname(b64)
        if parent == b64:
            b64 = ""
            break
        b64 = parent
    launcher_src = (os.path.join(b64, LAUNCHER_FROM_BUILD64, cfg, LAUNCHER_NAME)
                    if b64 else "")
    if launcher_src and os.path.isfile(launcher_src):
        items.append((launcher_src, LAUNCHER_NAME, "exe"))
    elif require_build:
        fail(f"mc2-launcher.exe not found (looked: {launcher_src or '<no build64 root>'})\n"
             "  Build it as part of the release: cmake --build build64 "
             "--config RelWithDebInfo --target mc2_launcher")
    else:
        # manifest-only: source build output may be gone; still record the
        # relpath so an existing target copy gets hashed.
        items.append((launcher_src or LAUNCHER_NAME, LAUNCHER_NAME, "exe"))

    # Deploy the ENTIRE shaders/ tree recursively (every file, all extensions),
    # not an extension allowlist. Shaders #include each other across stages and
    # extensions (.comp pulls .glsl/.hglsl; .frag pulls include/*.hglsl); a
    # per-ext allowlist silently dropped .comp + root .glsl and the GPU-driven
    # terrain cull/lighting/water compute then failed to compile -> NaN frustum
    # planes -> terrain culled to black, while smoke still PASSED (compile-fail
    # is non-fatal sticky-disable, not a crash). Only skip test fixtures.
    shader_dir = os.path.join(src_root, "shaders")
    if not os.path.isdir(shader_dir):
        fail(f"source shader dir not found: {shader_dir}")
    for dirpath, dirs, files in os.walk(shader_dir):
        dirs[:] = [d for d in dirs if d.lower() != "fixtures"]
        for fn in sorted(files):
            p = os.path.join(dirpath, fn)
            rel = os.path.relpath(p, src_root).replace(os.sep, "/")
            items.append((p, rel, "shader"))

    # Support payload: launch scripts (per target) + editor authoring trees.
    # These are source-tracked, not build outputs, so they ship even though
    # they never appear in build_dir. require_build is irrelevant here: a
    # missing SOURCE support file is a real error (loud copy failure), while
    # manifest-only tolerates a missing TARGET copy downstream as usual.
    is_editor = exe_name.lower() == EDITOR_EXE
    for script in SUPPORT_SCRIPTS["editor" if is_editor else "game"]:
        sp = os.path.join(src_root, script)
        items.append((sp, script, "support"))
    if is_editor:
        for tree in EDITOR_SUPPORT_TREES:
            tree_abs = os.path.join(src_root, tree)
            if require_build and not os.path.isdir(tree_abs):
                fail(f"editor support tree not found in source: {tree_abs}\n"
                     "  The editor's Generate-Mission path needs this tree; "
                     "a deploy without it builds a non-functional editor.")
            for dirpath, _dirs, files in os.walk(tree_abs):
                for fn in sorted(files):
                    sp = os.path.join(dirpath, fn)
                    rel = os.path.relpath(sp, src_root).replace(os.sep, "/")
                    items.append((sp, rel, "support"))
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


def hash_present_payload(items, target_dir):
    """Re-hash the payload files that are ALREADY in target_dir (no copy).

    Used by --write-manifest-only: another deploy path (deploy-editor.sh,
    the manual recipe, etc.) already copied the binaries in; we just record
    them in the SAME .deployed_manifest.csv that --verify-only checks, so
    the CSV does not go stale behind the JSON manifest.
    Files missing from the target are skipped (not every payload item is
    present in every target — e.g. an editor-only deploy has no mc2.exe).
    """
    hashes = {}
    present, absent = 0, 0
    for src, rel, kind in items:
        dst = os.path.join(target_dir, rel)
        if os.path.isfile(dst):
            hashes[rel] = (sha256_file(dst), os.path.getsize(dst))
            present += 1
        else:
            absent += 1
    log(f"manifest-only: {present} payload files present in target, "
        f"{absent} absent (skipped)")
    return hashes


def write_manifest_only(src_root, build_dir, exe_name, pdb_name, target_dir):
    """Refresh <target>/.deployed_manifest.csv from files ALREADY in target.

    No copy, no lock check (we are not writing binaries — only the CSV).
    Merges with an existing CSV so a partial deploy (editor-only into a
    shared install) does not drop the game's rows, mirroring the JSON
    manifest's --merge behaviour.
    """
    if not os.path.isdir(target_dir):
        fail(f"target dir does not exist: {target_dir}")
    items = enumerate_payload(src_root, build_dir, exe_name, pdb_name,
                              require_build=False)
    hashes = hash_present_payload(items, target_dir)

    # Merge: keep prior rows for payload files we did not just re-hash but
    # that still exist on disk (e.g. game exe rows when refreshing after an
    # editor-only copy).
    existing = os.path.join(target_dir, MANIFEST_NAME)
    if os.path.isfile(existing):
        try:
            with open(existing, newline="") as f:
                rows = list(csv.reader(f))
            if rows and rows[0][:2] == ["manifest_version", MANIFEST_VERSION]:
                for row in rows[2:]:
                    if len(row) < 3:
                        continue
                    rel, h, size = row[0], row[1], row[2]
                    if rel in hashes:
                        continue  # freshly re-hashed wins
                    p = os.path.join(target_dir, rel)
                    if os.path.isfile(p):
                        hashes[rel] = (h, size)
        except Exception as e:
            log(f"WARNING: could not merge existing CSV manifest ({e}); replacing")

    if not hashes:
        fail("no payload files present in target — nothing to record")
    write_manifest(target_dir, hashes, git_head(src_root))
    log("manifest-only COMPLETE — .deployed_manifest.csv refreshed (no copy)")


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


# Canned target presets (--target). Presets only SET DEFAULTS; explicit
# positional target / --build-dir / --exe-name always override.
#   game   -> live game install   A:/Games/mc2-opengl/mc2-win64-v0.4, mc2.exe,
#             build64/RelWithDebInfo
#   editor -> live editor install A:/Games/mc2-opengl/mc2-win64-0.4c,
#             "Mission Editor.exe", build64/out/editor/RelWithDebInfo (EditRel)
TARGET_PRESETS = {
    "game": {
        "target_dir": "A:/Games/mc2-opengl/mc2-win64-v0.4",
        "exe_name": "mc2.exe",
        "build_subdir": os.path.join("build64", "RelWithDebInfo"),
    },
    "editor": {
        "target_dir": "A:/Games/mc2-opengl/mc2-win64-0.4c",
        "exe_name": "Mission Editor.exe",
        "build_subdir": os.path.join("build64", "out", "editor", "RelWithDebInfo"),
    },
}


def main():
    ap = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        epilog="Presets: --target game   = A:/Games/mc2-opengl/mc2-win64-v0.4 "
               "(mc2.exe, build64/RelWithDebInfo); "
               "--target editor = A:/Games/mc2-opengl/mc2-win64-0.4c "
               "(\"Mission Editor.exe\", build64/out/editor/RelWithDebInfo). "
               "Explicit target dir / --build-dir / --exe-name override preset "
               "defaults.")
    ap.add_argument("target_dir", nargs="?", default=None,
                    help="deploy target dir (e.g. "
                    "A:/Games/mc2-opengl/mc2-win64-v0.4); optional when "
                    "--target preset is given (and then overrides the preset dir)")
    ap.add_argument("--target", choices=sorted(TARGET_PRESETS),
                    help="canned target preset: 'game' (v0.4, mc2.exe) or "
                    "'editor' (0.4c, Mission Editor.exe, EditRel build dir). "
                    "Sets defaults only; explicit flags still override.")
    ap.add_argument("--source-root", default=repo_root(),
                    help="source worktree root (default: this script's repo)")
    ap.add_argument("--build-dir", default=None,
                    help="build output dir (default: <source-root>/build64/RelWithDebInfo, "
                    "or the preset's build dir with --target)")
    ap.add_argument("--exe-name", default=None,
                    help="exe filename (default mc2.exe, or the preset's exe "
                    "with --target)")
    ap.add_argument("--allow-stale-pdb", action="store_true",
                    help="downgrade PDB staleness to a warning")
    ap.add_argument("--verify-only", action="store_true",
                    help="re-check existing target against its manifest; no copy")
    ap.add_argument("--write-manifest-only", action="store_true",
                    help="re-hash payload files ALREADY in the target and "
                    "rewrite .deployed_manifest.csv; no copy, no lock check. "
                    "Use as the final step of any OTHER deploy path "
                    "(deploy-editor.sh, manual recipe) so the CSV manifest "
                    "that --verify-only checks does not go stale.")
    ap.add_argument("--strict", action="store_true",
                    help="with --verify-only: drift = nonzero exit")
    args = ap.parse_args()

    preset = TARGET_PRESETS.get(args.target) if args.target else None
    target_dir = args.target_dir or (preset and preset["target_dir"])
    if not target_dir:
        ap.error("a target dir is required: pass it positionally or via "
                 "--target game|editor")
    exe_name = args.exe_name or (preset["exe_name"] if preset else "mc2.exe")
    if preset:
        log(f"preset '{args.target}': target {preset['target_dir']}, "
            f"exe {preset['exe_name']}, build {preset['build_subdir']}")

    target = os.path.abspath(target_dir)
    if args.verify_only:
        sys.exit(verify_only(target, args.strict))

    src_root = os.path.abspath(args.source_root)
    default_subdir = (preset["build_subdir"] if preset
                      else os.path.join("build64", "RelWithDebInfo"))
    build_dir = args.build_dir or os.path.join(src_root, default_subdir)
    pdb_name = os.path.splitext(exe_name)[0] + ".pdb"

    if args.write_manifest_only:
        write_manifest_only(src_root, build_dir, exe_name, pdb_name, target)
        return

    if not os.path.isdir(target):
        fail(f"target dir does not exist: {target} (refusing to create deploy "
             "targets — wrong-target trap, Mistake C)")

    log(f"source root: {src_root}")
    log(f"build dir:   {build_dir}")
    log(f"target:      {target}")

    check_locks(target, exe_name)
    items = enumerate_payload(src_root, build_dir, exe_name, pdb_name)
    log(f"payload: {len(items)} files "
        f"({sum(1 for *_x, k in items if k == 'shader')} shaders)")
    hashes = deploy(items, target, args.allow_stale_pdb)
    write_manifest(target, hashes, git_head(src_root))
    log("deploy COMPLETE — payload verified, manifest written")


if __name__ == "__main__":
    main()
