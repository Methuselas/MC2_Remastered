"""
mech_shot.py — MECH-SHOT-SELFVERIFY-1

Headless framebuffer screenshot of an imported mech, for AGENT self-verification
(orientation / scale / texture) without a human eyeballing. Pure Python — uses the
engine's existing MC2_SCREENSHOT_AT_FRAME hook (gameosmain.cpp:1664), which captures
the offscreen sceneFBO (works even when the window is minimized), so NO engine change.

  py -3 scripts/mech_shot.py [--mission mc2_24] [--frame 600] [--out shot.png]

Pairs with tg_import_dump: tg-dump confirms the import->TG data is sane; mech_shot
confirms the actual rendered pixels (textured, upright, right size) in-mission.

Default exe is the 0.5.0 release; the imported mech must be deployed there (skinned
GLB as data/tgl/MadCat.glb + base tga) and MC2_ASSIMP_MECH_IMPORT is set here.
"""
import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_EXE = Path(r"A:/Games/mc2-opengl/releases/mc2-win64-v0.5.0/mc2.exe")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=str(DEFAULT_EXE))
    ap.add_argument("--mission", default="mc2_24")
    ap.add_argument("--frame", type=int, default=600, help="capture at this frame counter")
    ap.add_argument("--duration", type=int, default=30, help="engine run seconds (>= time to reach frame)")
    ap.add_argument("--out", default="", help="output PNG (default tests/smoke/artifacts/mech-shots/<mission>_f<frame>.png)")
    ap.add_argument("--height", default="", help="MC2_MECH_SKEL_HEIGHT override (optional)")
    ap.add_argument("--axis", default="", help="MC2_GLTF_AXIS override (optional)")
    args = ap.parse_args()

    exe = Path(args.exe).resolve()
    if not exe.is_file():
        sys.exit(f"exe not found: {exe}")

    out_png = Path(args.out).resolve() if args.out else \
        (ROOT / "tests/smoke/artifacts/mech-shots" / f"{args.mission}_f{args.frame}.png")
    out_png.parent.mkdir(parents=True, exist_ok=True)
    # The engine writes a TGA; we convert to PNG for the agent to read.
    tga = out_png.with_suffix(".tga")
    if tga.exists():
        tga.unlink()

    env = os.environ.copy()
    env.update({
        "MC2_ASSIMP_MECH_IMPORT": "1",
        "MC2_SMOKE_MODE": "1",            # enables --mission + skips launcher
        "MC2_SMOKE_SEED": "0xC0FFEE",     # deterministic camera/AI -> reproducible frame
        "MC2_SCREENSHOT_AT_FRAME": str(args.frame),
        "MC2_SCREENSHOT_PATH": str(tga),
    })
    if args.height:
        env["MC2_MECH_SKEL_HEIGHT"] = args.height
    if args.axis:
        env["MC2_GLTF_AXIS"] = args.axis

    cmd = [str(exe), "--profile", "stock", "--mission", args.mission, "--duration", str(args.duration)]
    print(f"[mech_shot] launching {exe.name} mission={args.mission} frame={args.frame}")
    proc = subprocess.Popen(cmd, cwd=str(exe.parent), env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    # Wait for the engine to write the TGA at the target frame (poll, with grace).
    deadline = time.monotonic() + args.duration + 30
    while time.monotonic() < deadline:
        if tga.exists() and tga.stat().st_size > 0:
            time.sleep(0.5)  # let the write finish
            break
        if proc.poll() is not None and not tga.exists():
            break
        time.sleep(1.0)

    try:
        proc.wait(timeout=15)
    except subprocess.TimeoutExpired:
        proc.terminate()

    if not tga.exists() or tga.stat().st_size == 0:
        sys.exit(f"[mech_shot] FAIL: no screenshot written (frame {args.frame} not reached? "
                 f"increase --duration or lower --frame)")

    from PIL import Image
    img = Image.open(tga)
    # Engine writeTGA stores the GL framebuffer; flip if it comes out upside down.
    img.save(out_png)
    tga.unlink(missing_ok=True)
    print(f"[mech_shot] wrote {out_png} ({img.size[0]}x{img.size[1]})")
    print(str(out_png))


if __name__ == "__main__":
    main()
