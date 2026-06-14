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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
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


def extract_fst(fst_path, out_root, refs=None):
    """Extract entries of an FST into out_root, preserving its data\\... path.
    If refs is given, skip entries already provided by a ref data dir (delta only)."""
    n = 0
    seen = set()
    for off, comp, real, name, is_lz, data in fst.fst_entries(fst_path):
        if not name:
            continue
        rn = rel_norm(name)
        if rn in EXCLUDE_REL or rn in seen:
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


def copy_loose_delta(src_data, refs, out_data):
    """Copy files under src_data whose rel path is absent in every ref dir."""
    import shutil
    n = 0
    for root, _, files in os.walk(src_data):
        for fn in files:
            sp = os.path.join(root, fn)
            rel = os.path.relpath(sp, src_data).replace("\\", "/")
            if ("data/" + rel).lower() in EXCLUDE_REL:
                continue
            if any(os.path.isfile(os.path.join(ref, rel.replace("/", os.sep))) for ref in refs):
                continue
            out = os.path.join(out_data, rel.replace("/", os.sep))
            os.makedirs(os.path.dirname(out), exist_ok=True)
            shutil.copy2(sp, out)
            n += 1
    return n


def main():
    ap = argparse.ArgumentParser(description="Transfer an MC2X campaign install into a campaign mod.")
    ap.add_argument("--source", required=True, help="MC2X install root (has *.fst + data/)")
    ap.add_argument("--refs", nargs="+", required=True,
                    help="ref data dirs already provided elsewhere (mc2x-compat/data, base/data)")
    ap.add_argument("--out", required=True, help="campaign mod dir to write (…/mods/<name>)")
    ap.add_argument("--fsts", nargs="*", default=["mission.fst"],
                    help="FSTs extracted IN FULL (campaign-unique). default: mission.fst")
    ap.add_argument("--delta-fsts", nargs="*", default=["tgl.fst", "art.fst"],
                    help="FSTs extracted as DELTA vs --refs (campaign-specific appearances/shapes/"
                         "textures only). default: tgl.fst art.fst")
    args = ap.parse_args()

    src = args.source
    if not os.path.isdir(src):
        sys.exit("source not found: %s" % src)

    total_fst = 0
    for fname in args.fsts:
        fpath = os.path.join(src, fname)
        if not os.path.isfile(fpath):
            print("  (no %s — skipping)" % fname)
            continue
        n = extract_fst(fpath, args.out)
        print("  extracted %s (full): %d files" % (fname, n))
        total_fst += n
    for fname in args.delta_fsts:
        fpath = os.path.join(src, fname)
        if not os.path.isfile(fpath):
            print("  (no %s — skipping)" % fname)
            continue
        n = extract_fst(fpath, args.out, refs=args.refs)
        print("  extracted %s (delta): %d files" % (fname, n))
        total_fst += n

    src_data = os.path.join(src, "data")
    n_loose = 0
    if os.path.isdir(src_data):
        n_loose = copy_loose_delta(src_data, args.refs, os.path.join(args.out, "data"))
    print("  loose campaign delta: %d files" % n_loose)
    print("done: %s  (fst=%d loose=%d)" % (args.out, total_fst, n_loose))


if __name__ == "__main__":
    main()
