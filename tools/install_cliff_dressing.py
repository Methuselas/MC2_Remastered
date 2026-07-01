#!/usr/bin/env python3
"""install_cliff_dressing.py -- installer for TERRAIN-CLIFF-MESH-DRESSING-P0.

Installs the MarbleCliff building-class dressing prop into a deployed MC2
install: copies data/tgl/{marblecliff.ini,MarbleCliffGLB.glb}, appends one
line to data/art/Buildings.csv (+ gui copy), and appends one FIT packet to
data/objects/object2.pak via tools/pak_append.py.

Always backs up object2.pak and both Buildings.csv files first (suffix
".bak_cliffp0"; skipped if a backup already exists, so re-running is safe).

Usage:
  python install_cliff_dressing.py --deploy-root "A:/Games/mc2-opengl/releases/0.5 testing/mc2-win64-v0.5.0"

To revert: restore the three ".bak_cliffp0" files over their originals and
delete data/tgl/marblecliff.ini + MarbleCliffGLB.glb.
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
CSV_LINE = "MarbleCliff,11,30434,BUILDING,{fit_id},,0,0,0,0,,1\r\n"


def backup(path: Path) -> None:
    bak = path.with_name(path.name + ".bak_cliffp0")
    if bak.exists():
        print(f"  backup already exists, skipping: {bak}")
        return
    shutil.copyfile(path, bak)
    print(f"  backed up {path} -> {bak}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--deploy-root", required=True,
                     help='e.g. "A:/Games/mc2-opengl/releases/0.5 testing/mc2-win64-v0.5.0"')
    ap.add_argument("--fit-text", default=str(REPO_ROOT / "tools" / "marblecliff_fit_template.txt"),
                     help="FIT packet text to append to object2.pak")
    args = ap.parse_args()

    deploy_root = Path(args.deploy_root)
    obj_pak = deploy_root / "data" / "objects" / "object2.pak"
    csv_main = deploy_root / "data" / "art" / "Buildings.csv"
    csv_gui = deploy_root / "data" / "art" / "gui" / "Buildings.csv"
    tgl_dir = deploy_root / "data" / "tgl"

    for p in (obj_pak, csv_main, csv_gui, tgl_dir):
        if not p.exists():
            print(f"error: expected path missing: {p}", file=sys.stderr)
            return 1

    print("Backing up (skipped if already present)...")
    backup(obj_pak)
    backup(csv_main)
    backup(csv_gui)

    print("Copying data/tgl assets...")
    shutil.copyfile(REPO_ROOT / "data" / "tgl" / "marblecliff.ini", tgl_dir / "marblecliff.ini")
    shutil.copyfile(REPO_ROOT / "data" / "tgl" / "MarbleCliffGLB.glb", tgl_dir / "MarbleCliffGLB.glb")

    print("Appending FIT packet to object2.pak...")
    pak_append = REPO_ROOT / "tools" / "pak_append.py"
    result = subprocess.run(
        [sys.executable, str(pak_append), "append", "--pak", str(obj_pak),
         "--text-file", args.fit_text],
        capture_output=True, text=True)
    print(result.stdout)
    if result.returncode != 0:
        print(result.stderr, file=sys.stderr)
        return 1
    fit_id = int(result.stdout.strip().splitlines()[-1])

    print(f"Appending Buildings.csv line (fitID={fit_id})...")
    line = CSV_LINE.format(fit_id=fit_id)
    with open(csv_main, "ab") as f:
        f.write(line.encode("ascii"))
    with open(csv_gui, "ab") as f:
        f.write(line.encode("ascii"))

    print(f"\nInstall complete. FitID={fit_id}. Backups: *.bak_cliffp0 next to each modified file.")
    print("Revert: copy each *.bak_cliffp0 back over the original, then delete "
          f"{tgl_dir / 'marblecliff.ini'} and {tgl_dir / 'MarbleCliffGLB.glb'}.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
