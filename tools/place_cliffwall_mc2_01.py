#!/usr/bin/env python3
"""place_cliffwall_mc2_01.py -- deploy-side placement of the CLIFF_WALL mesh
decal (terrain mesh-decal system, Slice 0A visual proof).

Reuses the proven MarbleCliff decoration mechanism (object2.pak packet 1188 ->
marblecliff.ini -> a GLB in data/tgl) but repoints it at the GENERATED cliff-wall
mesh (tools/gen_cliff_wall_glb.py output) instead of the photogrammetry rock
blob. The engine still registers the appearance under the name "MarbleCliff";
the gated C++ CLIFF_WALL face-frame (bdactor.cpp registerStatic, gate
MC2_TERRAIN_DECAL) stands it up as a wall.

Deploy-side edits (no C++, all idempotent with .bak backups):
  1. Copy the generated CliffWallGLB.glb into <deploy>/data/tgl/.
  2. Repoint <deploy>/data/tgl/marblecliff.ini  [Import] Source -> "CliffWallGLB".
  3. Inject one terrain-object record (objType 1188 = MarbleCliff) into
     mc2_01.pak packet 1 at the cliff_side_on site (via place_marblecliff's
     inject_object; NO GLB bake -- our GLB is pre-scaled + _PAB-named).

Run AFTER deploy_payload.py so the deployed marblecliff.ini/pak exist.

Usage:
  python place_cliffwall_mc2_01.py --deploy-root "<...>/mc2-win64-v0.5.0" \
      --glb C:/Users/Joe/AppData/Local/Temp/CliffWallGLB.glb
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path


def repoint_ini(ini_path: Path, new_stem: str) -> None:
    bak = ini_path.with_suffix(ini_path.suffix + ".bak_cliffwall")
    if not bak.exists():
        bak.write_bytes(ini_path.read_bytes())
        print(f"[ini] backup -> {bak.name}")
    text = bak.read_text()
    out = []
    for line in text.splitlines():
        s = line.strip()
        if s.lower().startswith("st source"):
            out.append(f'st Source = "{new_stem}"')
        else:
            out.append(line)
    ini_path.write_text("\n".join(out) + "\n")
    print(f"[ini] Source -> \"{new_stem}\"  ({ini_path})")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--deploy-root", required=True)
    ap.add_argument("--glb", required=True, help="generated CliffWallGLB.glb")
    args = ap.parse_args(argv)

    root = Path(args.deploy_root)
    tgl = root / "data" / "tgl"
    ini = tgl / "marblecliff.ini"
    pak = root / "data" / "missions" / "mc2_01.pak"
    src_glb = Path(args.glb)
    for p in (tgl, ini, pak):
        if not p.exists():
            raise SystemExit(f"missing prerequisite: {p} (run deploy_payload first)")
    if not src_glb.exists():
        raise SystemExit(f"missing generated GLB: {src_glb}")

    dst_glb = tgl / "CliffWallGLB.glb"
    dst_glb.write_bytes(src_glb.read_bytes())
    print(f"[glb] deployed -> {dst_glb} ({dst_glb.stat().st_size} bytes)")

    repoint_ini(ini, "CliffWallGLB")

    # Reuse place_marblecliff's proven object injection (objType 1188 at the
    # cliff_side_on site). No GLB bake -- our GLB is already world-scaled + _PAB.
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from place_marblecliff_mc2_01 import inject_object
    inject_object(pak)

    print("done. capture with scripts/run_visual_capture.py --mission mc2_01 "
          "(gate MC2_TERRAIN_DECAL=1)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
