"""TEST helper: strip a campaign mod's data/art files that duplicate canonical
GUI (base art.fst + base loose data/art + compat layers). Campaign-unique files
(ops/tacmap/campaign-map/intro) are kept. Reversible (.import-removed rename).

Usage: py -3 _strip_gui_overrides.py <rc1_root> <mod_folder> [--apply]
Without --apply it only reports.
"""
import importlib.util, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('fst', os.path.join(HERE, 'fst.py'))
fst = importlib.util.module_from_spec(spec); spec.loader.exec_module(fst)

rc = sys.argv[1]
mod = sys.argv[2]
apply = '--apply' in sys.argv

# CONTENT atlases / layout fits: these share a canonical filename with base/compat
# but carry CAMPAIGN-SPECIFIC CONTENT (custom mech/pilot icons + the cell geometry the
# engine reads from the layout fit). Stripping them drops the campaign's content and
# falls back to the base atlas -> custom mechs render as magenta/wrong-offset icons.
# NEVER strip these, even when a canonical-named copy exists. (Bug: MCO icons, 2026-06-20.)
CONTENT_KEEP = {
    'mcui_gn_mechicons.tga',          # mech-bay roster atlas (per-campaign mech icons)
    'mc2x_mechicons.tga',             # MC2X variant of the roster atlas
    'mcl_pr_pilotskillicons.tga',     # pilot skill-icon atlas
    'mcl_pr_pilotskillicons2.tga',    # MCO spelling variant
    'mcl_gn_deploymentteams.fit',     # MechEntryIcon/PilotIcon cell geometry (Width/Height)
}

canon = set()  # lowercased basenames under data/art/

# base art.fst entries
art_fst = os.path.join(rc, 'art.fst')
if os.path.isfile(art_fst):
    for e in fst.fst_entries(art_fst):
        name = e[3]  # 'data/art/....'
        if name and name.lower().startswith('data/art/'):
            canon.add(os.path.basename(name).lower())

# loose canonical dirs: base + ALL compat/dependency layers (cveg, *-compat).
# A GUI file present in any of these is canonical -> the campaign's copy is an override.
loose_dirs = [os.path.join(rc, 'data', 'art')]
mods_root = os.path.join(rc, 'mods')
if os.path.isdir(mods_root):
    for m in os.listdir(mods_root):
        if m == mod:
            continue  # never treat the target campaign as a canonical source
        if m.lower() == 'cveg' or 'compat' in m.lower():
            loose_dirs.append(os.path.join(mods_root, m, 'data', 'art'))
for d in loose_dirs:
    if os.path.isdir(d):
        for fn in os.listdir(d):
            if os.path.isfile(os.path.join(d, fn)):
                canon.add(fn.lower().replace('.import-removed', ''))

print('canonical art names: %d (art.fst + base loose + mc2x-compat + cveg)' % len(canon))

dee_art = os.path.join(rc, 'mods', mod, 'data', 'art')
strip, keep = [], []
for fn in os.listdir(dee_art):
    full = os.path.join(dee_art, fn)
    if not os.path.isfile(full) or fn.endswith('.import-removed'):
        continue
    if fn.lower() in CONTENT_KEEP:
        keep.append(fn)          # campaign content atlas/fit -> never strip
    elif fn.lower() in canon:
        strip.append(fn)
    else:
        keep.append(fn)

print('mod=%s  live art=%d  STRIP(override of canonical)=%d  KEEP(unique)=%d'
      % (mod, len(strip) + len(keep), len(strip), len(keep)))
print('--- sample STRIP ---')
for fn in sorted(strip)[:40]:
    print('  ', fn)
print('--- sample KEEP ---')
for fn in sorted(keep)[:40]:
    print('  ', fn)

if apply:
    n = 0
    for fn in strip:
        os.rename(os.path.join(dee_art, fn), os.path.join(dee_art, fn + '.import-removed'))
        n += 1
    print('APPLIED: renamed %d files to .import-removed' % n)
else:
    print('(dry run; pass --apply to strip)')
