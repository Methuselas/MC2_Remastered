#!/usr/bin/env python3
"""
transfer_mc2x_campaign.py -- turn a full MC2X campaign install into a drop-in
campaign mod that runs on top of the mc2x-compat compatibility layer.

An MC2X install (e.g. A:/MC2X-DEE) keeps its actual missions inside FST archives
at the install root (mission.fst, art.fst, tgl.fst, ...) plus a small loose data/
tree of campaign-specific overrides (warrior brains, purchase files, the ops/
campaign script, pilots, campaign music). Stock + MC2X shared content already
lives in the deploy base + mc2x-compat, so the campaign mod only needs:

  1. the campaign missions -> extracted from mission.fst (data\\missions\\*.fit/.pak)
  2. the loose campaign delta -> files under the install's data/ whose path is NOT
     already provided by mc2x-compat or the base deploy (warriors, purchase, ops
     script, pilots, campaign-only art/sound/csv).

object2.pak and data/art/buildings.csv are EXCLUDED (mc2x-compat owns the object
table; overlaying a campaign's buildings.csv re-routes logistics and crashes —
the same conflict seen with the MCO "everything diff" experiment).

Bytes come from the user's own install; nothing is redistributed. The player
selects this campaign + the mc2x-compat layer at launch (MC2_ACTIVE_MOD +
MC2_MOD_DEPS) — no mod.json needed.

Usage:
  py -3 transfer_mc2x_campaign.py --source A:/MC2X-DEE \
      --refs <deploy>/mods/mc2x-compat/data <deploy>/data \
      --out  <deploy>/mods/MC2X-DEE
"""
import argparse
import os
import sys

# FROZEN-AWARE: bundled modules live under sys._MEIPASS when PyInstaller-frozen.
sys.path.insert(0, getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__))))
import fst  # noqa: E402

EXCLUDE_REL = {
    "data/objects/object2.pak",
    "data/art/buildings.csv",
}


def rel_norm(p):
    return p.replace("\\", "/").lower()


def in_refs(name, refs):
    rel = name.replace("\\", "/")
    # strip leading "data/" so we can test against ref dirs that ARE the data dir
    sub = rel[5:] if rel.lower().startswith("data/") else rel
    for ref in refs:
        if os.path.isfile(os.path.join(ref, sub.replace("/", os.sep))):
            return True
        if os.path.isfile(os.path.join(ref, rel.replace("/", os.sep))):
            return True
    return False


def extract_fst(fst_path, out_root, refs=None, drop_upscale=False):
    """Extract entries of an FST into out_root, preserving its data\\... path.
    If refs is given, skip entries already provided by a ref data dir (delta only).
    drop_upscale -> skip entries under data/{textures,art,tgl} (upscale dirs)."""
    n = 0
    seen = set()
    for off, comp, real, name, is_lz, data in fst.fst_entries(fst_path):
        if not name:
            continue
        rn = rel_norm(name)
        if rn in EXCLUDE_REL or rn in seen:
            continue
        if drop_upscale:
            # strip leading "data/" then check against upscale dirs
            sub = rn[5:] if rn.startswith("data/") else rn
            if sub.startswith(UPSCALE_DIRS):
                continue
        if refs and in_refs(name, refs):
            continue
        seen.add(rn)
        out_path = os.path.join(out_root, name.replace("\\", os.sep))
        try:
            content = fst.decompress_entry(off, comp, real, is_lz, data)
        except Exception as e:
            print("  WARN: %s: %s" % (name, e))
            continue
        os.makedirs(os.path.dirname(out_path), exist_ok=True)
        with open(out_path, "wb") as f:
            f.write(content)
        n += 1
    return n


# Loose appearance/texture trees that the base + compat layers already own.
# Excluded under --drop-upscale to avoid bloating the campaign mod with an upscale dump.
UPSCALE_DIRS = ("textures/", "art/", "tgl/")


def copy_loose_delta(src_data, refs, out_data, drop_upscale=False):
    """Copy files under src_data whose rel path is absent in every ref dir.
    drop_upscale -> also exclude the loose data/{textures,art,tgl} trees (base+compat own them)."""
    import shutil
    n = 0
    for root, _, files in os.walk(src_data):
        for fn in files:
            sp = os.path.join(root, fn)
            rel = os.path.relpath(sp, src_data).replace("\\", "/")
            if ("data/" + rel).lower() in EXCLUDE_REL:
                continue
            if drop_upscale and rel.lower().startswith(UPSCALE_DIRS):
                continue
            if any(os.path.isfile(os.path.join(ref, rel.replace("/", os.sep))) for ref in refs):
                continue
            out = os.path.join(out_data, rel.replace("/", os.sep))
            os.makedirs(os.path.dirname(out), exist_ok=True)
            shutil.copy2(sp, out)
            n += 1
    return n


def _write_marker(out_dir, source, fst_n, loose_n, drop_upscale, missing=None):
    """Provenance marker matching the mc2x_import contract (generated_by/source_fingerprint).
    FIX-4: files_missing is the count of requested FST inputs that were ABSENT from the
    install (the real loose/fst miss count), not a hardcoded 0 — so import_tool's
    validate-warn (A4) fires for transfers too. `missing` is the list of absent FST names."""
    import json
    missing = missing or []
    sys.path.insert(0, getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__))))
    try:
        import manifest
        fp = manifest.source_fingerprint(source)
    except Exception:
        fp = None
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "mc2x_import_report.json"), "w", encoding="utf-8") as f:
        json.dump({"generated_by": "transfer_mc2x_campaign", "tool_version": "1.0",
                   "source_path": os.path.abspath(source), "source_fingerprint": fp,
                   "drop_upscale": bool(drop_upscale),
                   "files_written": fst_n + loose_n, "fst_files": fst_n,
                   "loose_files": loose_n, "files_missing": len(missing),
                   "missing": missing}, f, indent=2)


def main():
    ap = argparse.ArgumentParser(description="Transfer an MC2X campaign install into a campaign mod.")
    ap.add_argument("--source", required=True, help="MC2X install root (has *.fst + data/)")
    ap.add_argument("--refs", nargs="+", required=True,
                    help="ref data dirs already provided elsewhere (mc2x-compat/data, base/data)")
    ap.add_argument("--out", required=True, help="campaign mod dir to write (…/mods/<name>)")
    ap.add_argument("--fsts", nargs="*", default=["mission.fst"],
                    help="FSTs extracted IN FULL (campaign-unique). default: mission.fst")
    ap.add_argument("--delta-fsts", nargs="*", default=["tgl.fst", "art.fst", "textures.fst"],
                    help="FSTs extracted as DELTA vs --refs (campaign-specific appearances/shapes/"
                         "textures only). default: tgl.fst art.fst textures.fst")
    ap.add_argument("--drop-upscale", action="store_true",
                    help="exclude the loose data/{textures,art,tgl} trees (base+compat own "
                         "appearances; avoids bloating the mod with an upscale dump)")
    ap.add_argument("--force", action="store_true",
                    help="overwrite an existing --out even if it was not generated by this tool")
    args = ap.parse_args()

    src = args.source
    if not os.path.isdir(src):
        sys.exit("source not found: %s" % src)

    # guard: refuse to clobber a foreign --out unless --force.
    if os.path.isdir(args.out) and not args.force:
        marker = os.path.join(args.out, "mc2x_import_report.json")
        gen = None
        if os.path.isfile(marker):
            try:
                import json
                with open(marker) as f:
                    gen = json.load(f).get("generated_by")
            except Exception:
                gen = None
        if gen not in ("transfer_mc2x_campaign", "import_tool"):
            sys.exit("ERROR: %s already exists and was not generated by this tool; "
                     "re-run with --force." % args.out)

    total_fst = 0
    missing_fsts = []  # FIX-4: requested FST inputs absent from the install
    for fname in args.fsts:
        fpath = os.path.join(src, fname)
        if not os.path.isfile(fpath):
            print("  (no %s — skipping)" % fname)
            missing_fsts.append(fname)
            continue
        n = extract_fst(fpath, args.out)
        print("  extracted %s (full): %d files" % (fname, n))
        total_fst += n
    for fname in args.delta_fsts:
        fpath = os.path.join(src, fname)
        if not os.path.isfile(fpath):
            # delta FSTs are OPTIONAL appearance overlays (absent on loose-data/MCO
            # installs) -> NOT counted as missing inputs (would false-warn every MCO import).
            print("  (no %s — skipping, optional delta)" % fname)
            continue
        n = extract_fst(fpath, args.out, refs=args.refs, drop_upscale=args.drop_upscale)
        print("  extracted %s (delta): %d files" % (fname, n))
        total_fst += n

    src_data = os.path.join(src, "data")
    n_loose = 0
    if os.path.isdir(src_data):
        n_loose = copy_loose_delta(src_data, args.refs, os.path.join(args.out, "data"),
                                   drop_upscale=args.drop_upscale)
    print("  loose campaign delta: %d files%s"
          % (n_loose, " (--drop-upscale)" if args.drop_upscale else ""))
    _write_marker(args.out, src, total_fst, n_loose, args.drop_upscale, missing=missing_fsts)
    if missing_fsts:
        print("  NOTE: %d requested FST input(s) absent: %s"
              % (len(missing_fsts), ", ".join(missing_fsts)))
    print("done: %s  (fst=%d loose=%d)" % (args.out, total_fst, n_loose))


if __name__ == "__main__":
    main()
