"""resolve_campaign_config.py — CAMPAIGN-CONFIG-RESOLVER-1

Emits the EXACT per-campaign launch config the launcher (mc2_launcher.cpp) would use,
so a sweep runner can set env vars correctly without guessing.

Replicates launcher logic exactly (tools/mc2_launcher/mc2_launcher.cpp):
  - Dep ordering: [add-ons (none by default)] then [base compat] (highest-priority-first in file.cpp)
  - Compat detection: scan data/missions/ for .fit ObjectNumber > 1188 + .abl magicAttack
  - mod.json "dependencies" list overrides content-scan when present
  - MC2_BOOT_TO_BAY: the campaign .fit basename (no ext, no path) in data/campaign/
  - MC2_ACTIVE_MOD: the mod folder name (OS name)
  - MC2_MOD_DEPS: pipe-joined string (comma in launcher, but HANDOFF docs use pipe for soak)
    -> This script emits BOTH: deps_order list AND mc2_mod_deps_string (comma-joined, as launcher sets it)

Usage:
  py -3 scripts/resolve_campaign_config.py [<deploy_dir>] [<campaign_folder_or_all>]

  deploy_dir defaults to A:/Games/mc2-opengl/releases/mc2-win64-v0.4d-rc1
  campaign   defaults to "all"

Examples:
  py -3 scripts/resolve_campaign_config.py
  py -3 scripts/resolve_campaign_config.py all
  py -3 scripts/resolve_campaign_config.py PicturesOfARebeliion
  py -3 scripts/resolve_campaign_config.py "A:/Games/mc2-opengl/mc2-win64-v0.4" MechCommanderOmnitech

Output: JSON array (or single object) to stdout.
"""

import json
import os
import re
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# Constants mirrored from mc2_launcher.cpp
# ---------------------------------------------------------------------------
HIGH_OBJ_THRESHOLD = 1188  # ObjectNumber > this => MC2X/MCO range
ABL_LIBS = {"corebrain.abx", "orders.abx", "miscfunc.abx"}  # never count as missions

DEFAULT_DEPLOY = "A:/Games/mc2-opengl/releases/mc2-win64-v0.4d-rc1"


# ---------------------------------------------------------------------------
# Compat detection (mirrors ScanDirSignals + bucketing logic in launcher)
# ---------------------------------------------------------------------------

def _scan_one_dir(scan_dir: Path, has_high_obj: bool, has_magic: bool):
    """Scan a single directory (non-recursive) for .fit/.abl signals.
    Mirrors ScanDirSignals() in mc2_launcher.cpp — skips subdirs.
    Returns (has_high_obj, has_magic).
    """
    if not scan_dir.is_dir():
        return has_high_obj, has_magic

    for f in scan_dir.iterdir():
        if f.is_dir():
            continue  # ScanDirSignals skips directories
        ext = f.suffix.lower()
        is_fit = (ext == ".fit")
        is_abl = (ext in (".abl", ".abi"))
        if not is_fit and not is_abl:
            continue
        if is_fit and has_high_obj:
            continue
        if is_abl and has_magic:
            continue
        if has_high_obj and has_magic:
            break

        try:
            text = f.read_text(encoding="latin-1", errors="replace")
        except OSError:
            continue

        if is_abl:
            if "magicattack" in text.lower():
                has_magic = True
            continue

        # .fit: scan all ObjectNumber= values
        for m in re.finditer(r"ObjectNumber\s*=\s*(\d+)", text, re.IGNORECASE):
            if int(m.group(1)) > HIGH_OBJ_THRESHOLD:
                has_high_obj = True
                break

    return has_high_obj, has_magic


def scan_missions_dir(missions_dir: Path):
    """Return (has_high_obj, has_magic) by scanning data/missions/ and data/missions/warriors/.

    Mirrors DetectCompat() in mc2_launcher.cpp (lines 1192-1206):
      1. Scan data/missions/
      2. If not both signals found, also scan data/missions/warriors/
      3. hasMagic alone => MCO (even without high ObjectNumber — e.g. MC2-Exodus objNum 878)
      4. hasHighObj (no magic) => MC2X
    """
    has_high_obj, has_magic = _scan_one_dir(missions_dir, False, False)
    if not (has_high_obj and has_magic):
        warriors_dir = missions_dir / "warriors"
        has_high_obj, has_magic = _scan_one_dir(warriors_dir, has_high_obj, has_magic)
    return has_high_obj, has_magic


def detect_compat(mod_dir: Path, mod_json_deps: list[str] | None) -> str | None:
    """Return the compat base folder name ('mco-compat', 'mc2x-compat', or None).

    Priority:
    1. mod.json "dependencies" list: if it contains a known compat base, use that.
       (Matches launcher: explicit mod.json type always wins over content-scan.)
    2. Content scan: MCO (magicAttack + high ObjectNumber) -> mco-compat
                     MC2X (high ObjectNumber, no magicAttack)  -> mc2x-compat
                     otherwise                                 -> None
    """
    COMPAT_BASES = {"mco-compat", "mc2x-compat"}

    if mod_json_deps is not None:
        for dep in mod_json_deps:
            if dep in COMPAT_BASES:
                return dep

    missions_dir = mod_dir / "data" / "missions"
    has_high_obj, has_magic = scan_missions_dir(missions_dir)

    # Mirrors DetectCompat() lines 1203-1205:
    # hasMagic alone => MCO (definitive; even stock-range objNum like MC2-Exodus)
    # hasHighObj (no magic) => MC2X
    if has_magic:
        return "mco-compat"
    if has_high_obj:
        return "mc2x-compat"
    return None


# ---------------------------------------------------------------------------
# Add-on detection from mod.json dependencies (non-compat items)
# ---------------------------------------------------------------------------

def extract_addon_deps(mod_json_deps: list[str] | None) -> list[str]:
    """Return dep entries that are NOT compat bases (i.e. asset add-ons like cveg)."""
    if not mod_json_deps:
        return []
    COMPAT_BASES = {"mco-compat", "mc2x-compat"}
    return [d for d in mod_json_deps if d not in COMPAT_BASES]


# ---------------------------------------------------------------------------
# Campaign .fit discovery
# Mirrors: HasPlayableMissions() in launcher — campaign folder has data/campaign/*.fit
# The .fit basename (no ext) is what MC2_BOOT_TO_BAY takes.
# ---------------------------------------------------------------------------

def find_campaign_fits(mod_dir: Path) -> list[str]:
    """Return list of campaign .fit basenames from data/campaign/."""
    campaign_dir = mod_dir / "data" / "campaign"
    if not campaign_dir.is_dir():
        return []
    fits = [f.stem for f in campaign_dir.iterdir()
            if f.suffix.lower() == ".fit" and not f.is_dir()]
    return sorted(fits)


def pick_primary_fit(fits: list[str]) -> str | None:
    """Pick the primary campaign fit.

    If exactly one: return it.
    If multiple: prefer 'campaign' (stock MCO pattern), else return the first alphabetically.
    The sweep runner SHOULD verify this against actual campaign content for multi-fit mods.
    """
    if not fits:
        return None
    if len(fits) == 1:
        return fits[0]
    if "campaign" in fits:
        return "campaign"
    return fits[0]


# ---------------------------------------------------------------------------
# Mission count (optional — reads campaign .fit)
# ---------------------------------------------------------------------------

def count_missions_from_fit(mod_dir: Path, fit_basename: str) -> int | None:
    """Count missions by summing MissionCount keys in the campaign .fit.

    Returns None if not derivable (multi-section ambiguity or parse failure).
    This is a best-effort count; the engine is authoritative.
    """
    fit_path = mod_dir / "data" / "campaign" / f"{fit_basename}.fit"
    if not fit_path.exists():
        return None
    try:
        text = fit_path.read_text(encoding="latin-1", errors="replace")
    except OSError:
        return None

    total = 0
    for m in re.finditer(r"^\s*l\s+MissionCount\s*=\s*(\d+)", text, re.MULTILINE | re.IGNORECASE):
        total += int(m.group(1))
    return total if total > 0 else None


# ---------------------------------------------------------------------------
# Bucket a mod into campaign / dependency / addon / unknown
# (mirrors ScanMods bucketing in mc2_launcher.cpp)
# ---------------------------------------------------------------------------

def is_campaign_folder(mod_dir: Path, mod_json: dict | None) -> bool:
    """True if this mod is a playable campaign."""
    if mod_json:
        t = mod_json.get("type", "")
        if t == "campaign":
            return True
        if t in ("dependency", "assets"):
            return False

    # "-compat" in name -> dependency bucket (never campaign)
    if "-compat" in mod_dir.name.lower():
        return False

    # Content-detect: has .fit in data/missions (excluding ABL libs)
    missions_dir = mod_dir / "data" / "missions"
    if missions_dir.is_dir():
        for f in missions_dir.iterdir():
            if f.name.lower() in ABL_LIBS:
                continue
            if f.suffix.lower() == ".fit":
                return True

    # Alternate: campaign .fit in data/campaign (mod packs the campaign there)
    return bool(find_campaign_fits(mod_dir))


# ---------------------------------------------------------------------------
# Core resolver
# ---------------------------------------------------------------------------

def resolve_one(folder_name: str, mods_dir: Path, deploy_dir: Path) -> dict:
    mod_dir = mods_dir / folder_name

    # Load mod.json if present
    mod_json_path = mod_dir / "mod.json"
    mod_json = None
    mod_json_deps = None
    if mod_json_path.exists():
        try:
            with open(mod_json_path, encoding="utf-8") as fh:
                mod_json = json.load(fh)
            mod_json_deps = mod_json.get("dependencies", None)
        except (json.JSONDecodeError, OSError):
            pass

    # Compat base
    compat_base = detect_compat(mod_dir, mod_json_deps)

    # Add-on deps (non-compat items from mod.json, e.g. cveg)
    addon_deps = extract_addon_deps(mod_json_deps)

    # MC2_MOD_DEPS ordering (mirrors launcher lines 1526-1530):
    #   [add-ons (highest priority first)] then [base compat (last)]
    #   file.cpp treats MC2_MOD_DEPS as highest-priority-FIRST
    deps_order = addon_deps[:]
    if compat_base:
        deps_order.append(compat_base)

    # Campaign .fit
    campaign_fits = find_campaign_fits(mod_dir)
    primary_fit = pick_primary_fit(campaign_fits)

    # Mission count
    expected_missions = None
    mission_count_note = None
    if primary_fit:
        expected_missions = count_missions_from_fit(mod_dir, primary_fit)
        if expected_missions is None:
            mission_count_note = "not derivable from campaign .fit (parse failed or empty)"
    else:
        mission_count_note = "no campaign .fit found in data/campaign/"

    if len(campaign_fits) > 1:
        mission_count_note = (
            f"multiple campaign fits: {campaign_fits}; "
            f"primary='{primary_fit}' (verify manually)"
        )

    result = {
        "folder_name": folder_name,
        "active_mod": folder_name,
        "deps_order": deps_order,
        "mc2_mod_deps_string": ",".join(deps_order) if deps_order else "",
        "fit": primary_fit,
        "campaign_fits_all": campaign_fits,
        "expected_missions": expected_missions,
        "deploy_folder": str(deploy_dir),
        "compat_base": compat_base,
        "addon_deps": addon_deps,
    }
    if mission_count_note:
        result["mission_count_note"] = mission_count_note
    return result


def resolve_all(mods_dir: Path, deploy_dir: Path, filter_name: str | None = None) -> list[dict]:
    results = []
    if not mods_dir.is_dir():
        print(f"ERROR: mods dir not found: {mods_dir}", file=sys.stderr)
        sys.exit(1)

    for entry in sorted(mods_dir.iterdir()):
        if not entry.is_dir():
            continue
        if filter_name and filter_name.lower() != entry.name.lower():
            continue

        mod_json_path = entry / "mod.json"
        mod_json = None
        if mod_json_path.exists():
            try:
                with open(mod_json_path, encoding="utf-8") as fh:
                    mod_json = json.load(fh)
            except (json.JSONDecodeError, OSError):
                pass

        if not is_campaign_folder(entry, mod_json):
            continue

        results.append(resolve_one(entry.name, mods_dir, deploy_dir))

    return results


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main():
    args = sys.argv[1:]

    deploy_dir_str = DEFAULT_DEPLOY
    campaign_filter = "all"

    if len(args) == 0:
        pass
    elif len(args) == 1:
        # Either a deploy dir or a campaign name
        if os.path.isdir(args[0]):
            deploy_dir_str = args[0]
        else:
            campaign_filter = args[0]
    elif len(args) == 2:
        deploy_dir_str = args[0]
        campaign_filter = args[1]
    else:
        print("Usage: resolve_campaign_config.py [deploy_dir] [campaign_name|all]", file=sys.stderr)
        sys.exit(1)

    deploy_dir = Path(deploy_dir_str)
    mods_dir = deploy_dir / "mods"

    filter_name = None if campaign_filter.lower() == "all" else campaign_filter
    results = resolve_all(mods_dir, deploy_dir, filter_name)

    if not results:
        print(f"No campaigns found (filter={campaign_filter!r}, mods={mods_dir})", file=sys.stderr)
        sys.exit(1)

    if filter_name and len(results) == 1:
        print(json.dumps(results[0], indent=2))
    else:
        print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
