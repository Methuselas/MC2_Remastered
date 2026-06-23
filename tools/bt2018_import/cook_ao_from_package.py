#!/usr/bin/env python3
"""BT2018-MECH-MATERIAL-AO-1: cook the AO maps declared by the material packages.

Package-driven (honors the contract): for each deployed <chassis>.package.json that
declares materials.ao, cook its dump source PNG -> BC7 .ktx2 under the package-declared
name (materials.ao.ktx2), deploy to data/tgl/ AND data/tgl/128/.

AO logical format is linear single-channel; the cooked container is BC7 (the txmmgr
KTX_PRIMARY decoder accepts only BC7 145/146 — see the package "container":"bc7-compat").
The PNG is fed as-is (grayscale-in-RGB); only the R channel is read by the shader.

Usage:
  py -3 cook_ao_from_package.py --deploy <release>/data/tgl \
      --dump A:/Games/mc2-opengl/BattleTech_2018_Dump [--ktx A:/Games/mc2-tools/ktx/ktx.exe]
"""
import argparse, glob, json, os, shutil, subprocess, sys

DEFAULT_KTX = r"A:/Games/mc2-tools/ktx/ktx.exe"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--deploy", required=True)
    ap.add_argument("--dump", default="A:/Games/mc2-opengl/BattleTech_2018_Dump")
    ap.add_argument("--ktx", default=DEFAULT_KTX)
    args = ap.parse_args()
    if not os.path.isfile(args.ktx):
        sys.exit(f"FATAL: ktx.exe not found: {args.ktx}")
    tmp = os.path.join(args.deploy, "_aoktxtmp"); os.makedirs(tmp, exist_ok=True)
    size_dir = os.path.join(args.deploy, "128"); os.makedirs(size_dir, exist_ok=True)

    cooked, skipped = 0, 0
    for p in sorted(glob.glob(os.path.join(args.deploy, "*.package.json"))):
        name = os.path.basename(p)[:-len(".package.json")]
        d = json.load(open(p, encoding="utf-8"))
        ao = d.get("materials", {}).get("ao")
        if not ao:
            continue
        src_ref = ao.get("source", "")
        if not src_ref.startswith("dump:"):
            print(f"[ao] {name}: AO source not a dump ref ({src_ref!r}) — SKIP"); skipped += 1; continue
        src = os.path.join(args.dump, src_ref[len("dump:"):])
        if not os.path.isfile(src):
            print(f"[ao] {name}: dump AO missing ({src}) — SKIP"); skipped += 1; continue
        out_name = ao["ktx2"]                      # chrtxrmech_<chassis>-base-amb.ktx2
        uastc = os.path.join(tmp, out_name + ".uastc.ktx2")
        bc7 = os.path.join(tmp, out_name)
        r1 = subprocess.run([args.ktx, "create", "--encode", "uastc",
                             "--format", "R8G8B8A8_UNORM", "--assign-tf", "linear",
                             "--levels", "1", src, uastc],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if r1.returncode != 0:
            print(f"[ao] {name}: ktx create FAIL {r1.stdout.strip()[:140]}"); skipped += 1; continue
        r2 = subprocess.run([args.ktx, "transcode", "--target", "bc7", uastc, bc7],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if r2.returncode != 0:
            print(f"[ao] {name}: ktx transcode FAIL {r2.stdout.strip()[:140]}"); skipped += 1; continue
        for dd in (args.deploy, size_dir):
            shutil.copy2(bc7, os.path.join(dd, out_name))
        cooked += 1
        print(f"[ao] {name}: {out_name} (BC7, from {os.path.basename(src)})")
    print(f"\n[ao] done: cooked={cooked} skipped={skipped}")


if __name__ == "__main__":
    main()
