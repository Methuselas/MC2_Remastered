#!/usr/bin/env python3
"""V-BASELINE-0 visual baseline capture harness.

Captures PNG + JSON sidecar pairs for named camera presets defined in
`tests/visual/baselines/presets.json`. The captured frames serve as a
controlled reference set for objective before/after comparison of
StaticPropOpaque-lane visual changes.

Design (recon-driven, no engine edits):
  - Reuses the existing OS-level screenshot path from
    `scripts/smoke_with_screenshots.py` (pyautogui).
  - Mission camera is deterministic under MC2_SMOKE_MODE=1 +
    MC2_SMOKE_SEED=0xC0FFEE (RNG seeding in gos_smoke.cpp). A "camera preset"
    is therefore the tuple (mission, warmup_s) at fixed seed; no new in-engine
    camera-preset hook is introduced by this slice.
  - Sidecar JSON schema matches the V-BASELINE-0 brief:
      commit, mission, cameraPreset, resolution, flags{...}
  - Output: `tests/visual/baselines/<preset>_<commit-short>.png` + `.json`.

This script does not modify any rendering behavior. mc2.exe is launched with
the same env it normally sees during a smoke run. Capture mode is opt-in by
running this script; default mc2.exe behavior is unchanged.

Usage:
  py -3 scripts/capture_baseline.py                       # all presets
  py -3 scripts/capture_baseline.py --preset staticprop_baseline_01
  py -3 scripts/capture_baseline.py --verify              # 2x re-run, byte-diff PNG
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_EXE = Path(r"A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe")
BASELINE_DIR = ROOT / "tests" / "visual" / "baselines"
PRESETS_PATH = BASELINE_DIR / "presets.json"

# Render-affecting env vars whose state we record in the sidecar. If unset,
# the engine default applies (recorded as "default").
TRACKED_FLAGS = (
    "MC2_VIEW_UNIFORMS",
    "MC2_SNAPSHOT_STATIC_PROP_BUILD",
    "MC2_MATERIAL_GPU",
    "MC2_MATERIAL_GPU_SAMPLE",
    "MC2_STATIC_PROP_LEGACY_DISPATCH",
    "MC2_GPU_CULL",
    "MC2_GPU_CULL_SUBSTRATE",
    "MC2_STATIC_PROP_AMBIENT_V1",
    "MC2_STATIC_PROP_IBL_SH",
    "MC2_STATIC_PROP_IBL_SH_STRENGTH",
    "MC2_STATIC_PROP_IBL_SH_SET",
    "MC2_STATIC_PROP_PBR_V1",
    "MC2_STATIC_PROP_PBR_V1_STRENGTH",
    "MC2_STATIC_PROP_PBR_V1_DIAG_SUNFOUND",
    # TERRAIN-BASELINE-0: terrain-pass tunables + debug-mode selector. Without
    # these in the sidecar a captured terrain frame is not reproducible if a
    # tuning slider or debug-mode value changes between runs.
    "MC2_TERRAIN_DEBUG_MODE",
    # TERRAIN-NORMALS-FROM-HEIGHT-1: macroscopic-normal-from-height gate.
    "MC2_TERRAIN_NORMALS_FROM_HEIGHT",
)


def kill_existing_mc2() -> None:
    subprocess.run(["taskkill", "/F", "/IM", "mc2.exe"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)


def park_mouse_center() -> None:
    try:
        import pyautogui
        pyautogui.FAILSAFE = False
        w, h = pyautogui.size()
        pyautogui.moveTo(w // 2, h // 2)
    except Exception as e:
        print(f"[baseline] WARN park cursor: {e}", file=sys.stderr)


def bring_mc2_foreground() -> bool:
    try:
        import ctypes
        user32 = ctypes.windll.user32
        for title in ("mc2", "MechCommander 2", "mc2 - Remastered"):
            hwnd = user32.FindWindowW(None, title)
            if hwnd:
                user32.SetForegroundWindow(hwnd)
                time.sleep(0.3)
                return True
    except Exception as e:
        print(f"[baseline] WARN foreground: {e}", file=sys.stderr)
    return False


def screenshot_to(path: Path) -> tuple[bool, tuple[int, int] | None]:
    try:
        import pyautogui
        pyautogui.FAILSAFE = False
        img = pyautogui.screenshot()
        img.save(str(path))
        return True, img.size
    except Exception as e:
        print(f"[baseline] ERROR screenshot: {e}", file=sys.stderr)
        return False, None


def git_short_sha() -> str:
    try:
        out = subprocess.check_output(
            ["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"],
            stderr=subprocess.DEVNULL,
        ).decode("ascii").strip()
        return out
    except Exception:
        return "unknown"


def captured_flags(env: dict) -> dict:
    """Snapshot the render-affecting env vars actually seen by the subprocess.

    Reads from the supplied `env` dict (the dict passed to Popen(env=...)),
    NOT from os.environ — the parent's environment is irrelevant to what the
    captured mc2.exe process saw.
    """
    out: dict[str, str] = {}
    for k in TRACKED_FLAGS:
        v = env.get(k)
        out[k] = v if v is not None else "default"
    return out


def file_sha256(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def _strength_tag(strength: str | None) -> str:
    """Filename suffix for strength sweep. None -> 'off' (env unset)."""
    if strength is None:
        return "ibl_sh_off"
    # 0.25 -> s0p25, 1.0 -> s1p00, 1.5 -> s1p50
    try:
        f = float(strength)
    except ValueError:
        return f"ibl_sh_s{strength}"
    return f"ibl_sh_s{f:.2f}".replace(".", "p")


def run_capture(exe: Path, preset_name: str, preset: dict,
                commit_sha: str, out_png: Path,
                strength: str | None = None,
                terrain_debug_mode: str | None = None) -> dict:
    mission = preset["mission"]
    warmup = int(preset["warmup_s"])
    duration = int(preset.get("duration_s", warmup + 2))

    kill_existing_mc2()
    park_mouse_center()

    log_path = out_png.with_suffix(".log")
    log_fp = open(log_path, "w", encoding="utf-8", errors="replace")

    env = os.environ.copy()
    env["MC2_SMOKE_MODE"] = "1"
    env["MC2_SMOKE_SEED"] = "0xC0FFEE"
    env["MC2_HEARTBEAT"] = "1"
    # V-IBL-STATIC-1-SOAK: per-capture strength override drives the IBL-SH
    # gate + default slider value. strength=None means "off" baseline:
    # explicitly unset both env vars so this subprocess sees the legacy
    # gate-OFF path (byte-identical to pre-slice).
    if strength is None:
        env.pop("MC2_STATIC_PROP_IBL_SH", None)
        env.pop("MC2_STATIC_PROP_IBL_SH_STRENGTH", None)
    else:
        env["MC2_STATIC_PROP_IBL_SH"] = "1"
        env["MC2_STATIC_PROP_IBL_SH_STRENGTH"] = str(strength)

    # TERRAIN-BASELINE-0: terrain fragment debug-mode override (env var read
    # at all three GL upload sites in gameos_graphics.cpp). None = leave the
    # var unset (default mode 0 / OFF, byte-identical to legacy). "0" is also
    # valid (explicit OFF). Strings 1..9 / -1 select modes per kTerrainModes.
    if terrain_debug_mode is None:
        env.pop("MC2_TERRAIN_DEBUG_MODE", None)
    else:
        env["MC2_TERRAIN_DEBUG_MODE"] = str(terrain_debug_mode)

    proc_args = [str(exe), "--profile", "stock", "--mission", mission,
                 "--duration", str(duration)]
    # Launch minimized so the game does not steal foreground from the user's
    # desktop while the capture warm-up runs. bring_mc2_foreground() below
    # un-minimizes briefly just before the screenshot. Mirrors the pattern
    # in scripts/smoke_lib/runner.py.
    startupinfo = None
    if os.name == "nt":
        startupinfo = subprocess.STARTUPINFO()
        startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startupinfo.wShowWindow = 7  # SW_SHOWMINNOACTIVE
    print(f"[baseline] preset={preset_name} mission={mission} warmup={warmup}s")
    t0 = time.time()
    proc = subprocess.Popen(proc_args, stdout=log_fp, stderr=subprocess.STDOUT,
                            cwd=str(exe.parent), env=env,
                            startupinfo=startupinfo)

    time.sleep(max(0, warmup - 5))
    park_mouse_center()
    time.sleep(min(5, warmup))

    bring_mc2_foreground()
    park_mouse_center()
    time.sleep(0.3)
    shot_ok, size = screenshot_to(out_png)

    remaining = max(1, (duration + 10) - (time.time() - t0))
    try:
        proc.wait(timeout=remaining)
    except subprocess.TimeoutExpired:
        proc.kill()
        try:
            proc.wait(timeout=10)
        except Exception:
            pass
    log_fp.close()

    if not shot_ok:
        return {"ok": False, "reason": "screenshot_failed"}

    resolution = f"{size[0]}x{size[1]}" if size else "unknown"
    sidecar = {
        "commit": commit_sha,
        "mission": mission,
        "cameraPreset": preset_name,
        "resolution": resolution,
        "flags": captured_flags(env),
        "capture": {
            "warmup_s": warmup,
            "duration_s": duration,
            "smoke_seed": env["MC2_SMOKE_SEED"],
            "captured_at": dt.datetime.now().isoformat(timespec="seconds"),
            "png_sha256": file_sha256(out_png),
            "png_bytes": out_png.stat().st_size,
        },
        "preset_description": preset.get("description", ""),
    }
    sidecar_path = out_png.with_suffix(".json")
    sidecar_path.write_text(json.dumps(sidecar, indent=2) + "\n",
                            encoding="utf-8")
    print(f"[baseline]   PNG    {out_png}")
    print(f"[baseline]   JSON   {sidecar_path}")
    print(f"[baseline]   sha256 {sidecar['capture']['png_sha256'][:16]}...")
    return {"ok": True, "png": str(out_png), "json": str(sidecar_path),
            "sha256": sidecar["capture"]["png_sha256"]}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--preset", action="append", default=[],
                    help="Preset name to capture. Repeat for multiple. "
                         "Omit for all presets.")
    ap.add_argument("--exe", default=str(DEFAULT_EXE))
    ap.add_argument("--verify", action="store_true",
                    help="Capture each preset TWICE and compare PNG sha256.")
    ap.add_argument("--out-dir", default=str(BASELINE_DIR))
    ap.add_argument("--strength", default=None,
                    help="V-IBL-STATIC-1-SOAK: single strength override "
                         "(sets MC2_STATIC_PROP_IBL_SH=1 + STRENGTH=<f>). "
                         "Omit for legacy gate-OFF baseline.")
    ap.add_argument("--strength-sweep", default=None,
                    help="V-IBL-STATIC-1-SOAK: comma-separated strengths to "
                         "sweep per preset, e.g. 'off,0.25,0.5,1.0,1.5'. "
                         "'off' = env unset (legacy gate-OFF baseline).")
    ap.add_argument("--terrain-debug-mode", default=None,
                    help="TERRAIN-BASELINE-0: terrain fragment debug-mode "
                         "override (MC2_TERRAIN_DEBUG_MODE). Modes: -1 tess-"
                         "alive probe, 0 OFF (default), 1 depth-cmp, 2 raw "
                         "colormap, 3 blurred colormap, 4 material weights, "
                         "5 normal lighting, 6 shadow, 7 cloud shadow, 8 "
                         "cement diag, 9 thin-record. Omit for default (mode "
                         "0). Filename suffix: _tdmN.")
    args = ap.parse_args()

    exe = Path(args.exe)
    if not exe.exists():
        print(f"[baseline] ERROR exe missing: {exe}", file=sys.stderr)
        return 4

    if not PRESETS_PATH.exists():
        print(f"[baseline] ERROR presets.json missing: {PRESETS_PATH}",
              file=sys.stderr)
        return 4

    presets_doc = json.loads(PRESETS_PATH.read_text(encoding="utf-8"))
    all_presets = presets_doc["presets"]
    selected = args.preset or list(all_presets.keys())
    bad = [p for p in selected if p not in all_presets]
    if bad:
        print(f"[baseline] ERROR unknown preset(s): {bad}", file=sys.stderr)
        print(f"[baseline] known: {list(all_presets.keys())}", file=sys.stderr)
        return 4

    commit_sha = git_short_sha()
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    print(f"[baseline] commit={commit_sha} out={out_dir}")
    print(f"[baseline] presets: {selected}")

    # V-IBL-STATIC-1-SOAK: build list of strength values for the sweep.
    # 'off' / None means env unset (legacy gate-OFF baseline). A numeric
    # value sets MC2_STATIC_PROP_IBL_SH=1 + STRENGTH=<v>.
    if args.strength_sweep:
        sweep: list[str | None] = []
        for tok in args.strength_sweep.split(","):
            tok = tok.strip()
            if not tok:
                continue
            sweep.append(None if tok.lower() == "off" else tok)
    elif args.strength is not None:
        sweep = [args.strength]
    else:
        sweep = [None]

    overall_ok = True
    for name in selected:
        preset = all_presets[name]
        for strength in sweep:
            tag = _strength_tag(strength)
            # Sweep mode encodes strength in filename; legacy single-shot
            # mode (sweep == [None] and no --strength and no --terrain-debug-mode)
            # preserves the original `<name>_<sha>.png` filename for back-compat.
            # terrain-debug-mode appends a _tdmN suffix so multiple debug-mode
            # captures of the same preset coexist on disk.
            tdm = args.terrain_debug_mode
            tdm_suffix = f"_tdm{tdm}" if tdm is not None else ""
            if args.strength_sweep is None and args.strength is None and tdm is None:
                out_png = out_dir / f"{name}_{commit_sha}.png"
            else:
                out_png = out_dir / f"{name}_{commit_sha}_{tag}{tdm_suffix}.png"
            r = run_capture(exe, name, preset, commit_sha, out_png,
                            strength=strength,
                            terrain_debug_mode=tdm)
            if not r.get("ok"):
                overall_ok = False
                print(f"[baseline] FAIL preset={name} strength={tag}: {r}",
                      file=sys.stderr)
                continue
            time.sleep(2)
        # --verify only meaningful for the original single-shot path
        # (one capture per preset). Sweep mode produces N captures and is
        # not bit-comparison anyway.
        if (args.verify and args.strength_sweep is None
                and args.strength is None and args.terrain_debug_mode is None):
            single_png = out_dir / f"{name}_{commit_sha}.png"
            sha_first = file_sha256(single_png) if single_png.exists() else ""
            verify_png = out_dir / f"{name}_{commit_sha}.verify.png"
            time.sleep(2)
            r2 = run_capture(exe, name, preset, commit_sha, verify_png)
            if not r2.get("ok"):
                overall_ok = False
                continue
            same = (sha_first == r2["sha256"])
            print(f"[baseline] VERIFY preset={name} "
                  f"identical={'yes' if same else 'NO'} "
                  f"({sha_first[:16]} vs {r2['sha256'][:16]})")
            if not same:
                print("[baseline]   (OS-screenshot path; non-byte-identical "
                      "across runs is expected — see howto doc.)")

    return 0 if overall_ok else 1


if __name__ == "__main__":
    sys.exit(main())
