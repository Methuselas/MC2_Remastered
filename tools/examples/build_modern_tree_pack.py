#!/usr/bin/env python3
"""
build_modern_tree_pack.py  —  cook + assemble modern-tree-pack-v1 example mod.

Usage:
  python tools/examples/build_modern_tree_pack.py \
    --asset-root "C:/Users/Joe/Downloads/GameAsset" \
    --out "mods/modern-tree-pack-v1" \
    [--deploy-root "A:/Games/mc2-opengl/mc2-win64-v0.4"]
    [--family maple]
    [--clean]

Generated artifacts (GLBs, KTX2s, staging) are gitignored.
Commit only: this script, tests, docs, mod.json, models.json, cook_report.json.
"""
import argparse
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).parent.parent.parent
COOK_SCRIPT = REPO_ROOT / "tools" / "asset_cook" / "trackg_cook.py"
VALIDATE_SCRIPT = REPO_ROOT / "tools" / "asset_cook" / "validate_asset_manifest.py"

# lods[0] = LOD0 (highest detail). distance_m = threshold to switch to next LOD.
# NOTE: numbered source files are same-resolution variants used as a LOD chain
# to demonstrate the config structure — they are NOT decimated polygon LODs.
ASSET_FAMILIES = [
    {
        "name": "maple",
        "class": "tree",
        "replaces": "tree:maple1",
        "subdir": "uploads_files_4996646_Broadleaf_GLTF",
        "lods": [
            {"filename": "maple1.glb", "distance_m": 0},
            {"filename": "maple2.glb", "distance_m": 600},
            {"filename": "maple3.glb", "distance_m": 1200},
        ],
    },
    {
        "name": "poplar",
        "class": "tree",
        "replaces": "tree:pine1",
        "subdir": "uploads_files_4996646_Broadleaf_GLTF",
        "lods": [
            {"filename": "poplar1.glb", "distance_m": 0},
            {"filename": "poplar2.glb", "distance_m": 600},
            {"filename": "poplar3.glb", "distance_m": 1200},
            {"filename": "poplar4.glb", "distance_m": 2000},
        ],
    },
    {
        "name": "white_poplar",
        "class": "tree",
        "replaces": "tree:maple2",
        "subdir": "uploads_files_4996646_Broadleaf_GLTF",
        "lods": [
            {"filename": "whitePoplar1.glb", "distance_m": 0},
            {"filename": "whitePoplar2.glb", "distance_m": 600},
            {"filename": "whitePoplar3.glb", "distance_m": 1200},
            {"filename": "whitePoplar4.glb", "distance_m": 2000},
        ],
    },
    {
        "name": "crane_big",
        "class": "staticprop",
        "replaces": "staticprop:crane",
        "subdir": "uploads_files_5382247_GlTF_models/GlTF_models/Crane_Big_Small/Crane_big",
        "lods": [
            {"filename": "crane_big.glb", "distance_m": 0},
        ],
    },
]


def build_mapping_table(families):
    """Return one row per LOD across all families with status='pending'."""
    rows = []
    for fam in families:
        appearance = fam["replaces"].split(":")[1]
        for i, lod in enumerate(fam["lods"]):
            rows.append({
                "source_glb": lod["filename"],
                "target_stock_appearance": appearance,
                "class": fam["class"],
                "replaces": fam["replaces"],
                "lod_index": i,
                "distance_m": lod["distance_m"],
                "status": "pending",
                "manifest_path": None,
            })
    return rows


def build_mod_json(mod_id, name, version):
    return {
        "schema": "mc2-mod/1",
        "id": mod_id,
        "name": name,
        "version": version,
        "dependencies": [],
    }


def build_models_json(families, cooked_glb_paths):
    """Build model override entries for models.json.
    Only includes entries for the families passed in.
    """
    entries = []
    for fam in families:
        appearance = fam["replaces"].split(":")[1]
        lod_entries = []
        for lod in fam["lods"]:
            cooked = cooked_glb_paths.get(lod["filename"], lod["filename"])
            lod_entries.append({
                "distance": lod["distance_m"],
                "source": cooked,
            })
        entries.append({
            "class": fam["class"],
            "appearanceName": appearance,
            "replaces": fam["replaces"],
            "renderOnly": True,
            "fallback": "stock",
            "lods": lod_entries,
        })
    return entries


def _run(cmd, label):
    print(f"  [{label}] {' '.join(str(c) for c in cmd)}")
    result = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    if result.returncode != 0:
        print(f"  FAILED:\n{result.stderr}", file=sys.stderr)
        raise RuntimeError(f"{label} failed (exit {result.returncode})")
    return result


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def extract_glb_textures(src_glb: Path, tex_dir: Path) -> Path:
    """Extract embedded textures from a GLB to tex_dir using trimesh."""
    tex_dir.mkdir(parents=True, exist_ok=True)
    extract_script = f"""
import trimesh, pathlib, sys
scene = trimesh.load(r"{src_glb}", force="scene", process=False)
out = pathlib.Path(r"{tex_dir}")
for geom_name, geom in scene.geometry.items():
    vis = geom.visual
    if hasattr(vis, "material") and hasattr(vis.material, "image") and vis.material.image:
        img = vis.material.image
        stem = getattr(vis.material, "name", geom_name) or geom_name
        dest = out / (stem + ".png")
        if not dest.exists():
            img.save(str(dest))
            print("extracted:", dest.name)
"""
    result = subprocess.run(
        [sys.executable, "-c", extract_script],
        capture_output=True, text=True
    )
    if result.returncode != 0:
        print(f"  WARNING: texture extraction issues:\n{result.stderr}", file=sys.stderr)
    return tex_dir


def cook_one_lod(src_glb: Path, out_dir: Path, mod_out: Path,
                 asset_class: str, lod_tag: str, appearance: str) -> Path:
    """Run stage -> textures -> assemble for one GLB. Returns path to manifest.json.

    Verified trackg_cook.py signatures:
      stage: <source_glb> <out_dir> --id <id> --class <class> --appearance <appearance>
      textures: --staged --texture-dir --out-root <mod_out> --out-json
      assemble: --staged --materials --out-dir
      (replaces derived from staged.json, no --replaces flag)
    """
    out_dir.mkdir(parents=True, exist_ok=True)
    py = sys.executable

    _run([py, COOK_SCRIPT, "stage",
          str(src_glb), str(out_dir),
          "--id", lod_tag,
          "--class", asset_class,
          "--appearance", appearance], "stage")

    staged = out_dir / "staged.json"

    tex_dir = out_dir / "_textures"
    extract_glb_textures(src_glb, tex_dir)

    materials_json = out_dir / "materials.json"
    _run([py, COOK_SCRIPT, "textures",
          "--staged", staged,
          "--texture-dir", tex_dir,
          "--out-root", mod_out,
          "--out-json", materials_json], "textures")

    _run([py, COOK_SCRIPT, "assemble",
          "--staged", staged,
          "--materials", materials_json,
          "--out-dir", out_dir], "assemble")

    manifest = out_dir / "manifest.json"
    _run([py, VALIDATE_SCRIPT, manifest], "validate")
    return manifest


def fingerprint_deploy(deploy_root: Path) -> dict:
    mc2_exe = deploy_root / "mc2.exe"
    shaders = list(deploy_root.rglob("*.glsl")) + list(deploy_root.rglob("*.vert")) + list(deploy_root.rglob("*.frag"))
    return {
        "mc2_exe_sha256": _sha256(mc2_exe) if mc2_exe.exists() else None,
        "shader_count": len(shaders),
        "deploy_root": str(deploy_root),
    }


def validate_stock_targets(deploy_root: Path, families):
    missing = []
    tgl_dir = deploy_root / "data" / "tgl"
    if not tgl_dir.exists():
        print(f"  WARNING: tgl dir not found at {tgl_dir}, skipping target validation")
        return missing
    for fam in families:
        appearance = fam["replaces"].split(":")[1]
        ini = tgl_dir / f"{appearance}.ini"
        if not ini.exists():
            missing.append(f"{fam['replaces']} -> {ini} not found")
    return missing


def main():
    ap = argparse.ArgumentParser(description="Cook and assemble modern-tree-pack-v1 example mod")
    ap.add_argument("--asset-root", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--deploy-root", default=None,
                    help="Optional: validates stock targets + records deploy fingerprint. Does NOT install.")
    ap.add_argument("--family", default=None,
                    help="Cook only this family. WARNING: writes only this family's entries to models.json.")
    ap.add_argument("--clean", action="store_true",
                    help="Wipe --out before cooking. Required for full run after partial --family runs.")
    args = ap.parse_args()

    asset_root = Path(args.asset_root)
    out_dir = Path(args.out)
    MOD_ID = "modern-tree-pack-v1"

    if args.clean and out_dir.exists():
        print(f"--clean: removing {out_dir}")
        shutil.rmtree(out_dir)

    if not args.family and not args.clean and out_dir.exists():
        print("WARNING: full run without --clean on existing output dir. "
              "Pass --clean to ensure clean state.", file=sys.stderr)

    deploy_fingerprint = None
    target_warnings = []
    if args.deploy_root:
        deploy_root = Path(args.deploy_root)
        deploy_fingerprint = fingerprint_deploy(deploy_root)
        sha_prefix = (deploy_fingerprint["mc2_exe_sha256"] or "none")[:12]
        print(f"Deploy fingerprint: mc2.exe={sha_prefix}... shaders={deploy_fingerprint['shader_count']}")
        families_to_check = ASSET_FAMILIES if not args.family else \
            [f for f in ASSET_FAMILIES if f["name"] == args.family]
        target_warnings = validate_stock_targets(deploy_root, families_to_check)
        if target_warnings:
            print("WARNING - stock target appearances not found:")
            for w in target_warnings:
                print(f"  {w}")

    families_to_cook = ASSET_FAMILIES
    if args.family:
        families_to_cook = [f for f in ASSET_FAMILIES if f["name"] == args.family]
        if not families_to_cook:
            print(f"ERROR: no family '{args.family}'. Valid: {[f['name'] for f in ASSET_FAMILIES]}", file=sys.stderr)
            sys.exit(1)

    mapping = build_mapping_table(families_to_cook)
    cooked_glb_paths = {}
    cook_errors = []
    cooked_dir = out_dir / "data" / "model_overrides" / "cooked"

    for fam in families_to_cook:
        appearance = fam["replaces"].split(":")[1]
        print(f"\n=== Cooking family: {fam['name']} -> {fam['replaces']} ===")
        for i, lod in enumerate(fam["lods"]):
            src = asset_root / fam["subdir"] / lod["filename"]
            if not src.exists():
                msg = f"Source not found: {src}"
                print(f"  ERROR: {msg}", file=sys.stderr)
                cook_errors.append({"family": fam["name"], "lod": i, "error": msg})
                continue

            lod_tag = f"{appearance}_lod{i}"
            lod_out = out_dir / "_cook_staging" / lod_tag

            try:
                manifest_path = cook_one_lod(
                    src, lod_out, out_dir,
                    fam["class"], lod_tag, appearance)

                staged_data = json.loads((lod_out / "staged.json").read_text())
                cooked_glb_src = lod_out / staged_data["geometry"]["cooked"]
                dest_glb = cooked_dir / f"{lod_tag}.glb"
                dest_glb.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(cooked_glb_src, dest_glb)
                cooked_glb_paths[lod["filename"]] = str(dest_glb.relative_to(out_dir).as_posix())

                for row in mapping:
                    if row["source_glb"] == lod["filename"] and row["lod_index"] == i:
                        row["status"] = "cooked"
                        row["manifest_path"] = str(manifest_path)
                print(f"  OK: {lod['filename']} -> {dest_glb.name}")

            except RuntimeError as e:
                cook_errors.append({"family": fam["name"], "lod": i, "error": str(e)})

    models_dir = out_dir / "data" / "model_overrides"
    models_dir.mkdir(parents=True, exist_ok=True)
    models_entries = build_models_json(families_to_cook, cooked_glb_paths)
    (models_dir / "models.json").write_text(json.dumps(models_entries, indent=2))

    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "mod.json").write_text(json.dumps(
        build_mod_json(MOD_ID, "Modern Tree Pack v1", "1.0.0"), indent=2))

    report = {
        "mod_id": MOD_ID,
        "family_filter": args.family,
        "deploy_fingerprint": deploy_fingerprint,
        "target_warnings": target_warnings,
        "mapping": mapping,
        "errors": cook_errors,
        "total": len(mapping),
        "cooked": sum(1 for r in mapping if r["status"] == "cooked"),
        "failed": len(cook_errors),
    }
    (out_dir / "cook_report.json").write_text(json.dumps(report, indent=2))

    print(f"\n=== Cook complete: {report['cooked']}/{report['total']} LODs cooked, "
          f"{report['failed']} errors ===")
    print(f"Mod folder: {out_dir}")
    if cook_errors:
        print("ERRORS:", file=sys.stderr)
        for e in cook_errors:
            print(f"  {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
