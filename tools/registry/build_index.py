"""
MC2 Registry Index Builder (S6)
================================
Builds registry-index.json (schema "mc2-registry-index/1") by scanning a
game deploy root.  The index is a READ-ONLY, REGENERATABLE CACHE -- it is
not authoritative; the engine files it was built from are.  Delete it at any
time; the next tool query rebuilds it.

Spec refs:
  docs/superpowers/strategy/data-ownership-registry-strategy.md §4, §10.1-2
  docs/superpowers/strategy/superpowers-execution-roadmap.md §5 S6

Resolution: imports tools/mod_install/resolver.py (ruling C5 -- never
            reimplement the resolution ladder here).

Engine never reads this file.  No engine edits.  Python 3 stdlib only.

V2 deferred items (noted inline with V2-DEFER):
  - Per-entry object type enumeration (pak payload parsing): object2.pak is
    recorded as an opaque input; packet count comes from header walk; per-entry
    name/type requires decompressing pak payloads -- deferred to slice S6v2
    (data-ownership-registry-strategy.md §10.5).
  - compbas.csv row-level component enumeration: recorded as opaque input;
    row count only -- deferred to S6v2 (strategy doc §10.5).
  - mc2.fx effect-name table scan: recorded as opaque input (presence + sha256
    only) -- gosFX stream format is complex; deferred to S6v2.
  - Terrain type FIT full texture-path enumeration: TerrainType block count
    recorded; texture name list deferred to S6v2.
  - model_overrides/models.json record-level enumeration: aggregated as input;
    record count only -- deferred to S6v2.

Usage
-----
  # Build base-game index (no active mod):
  python tools/registry/build_index.py --game-dir A:/Games/mc2-opengl/mc2-win64-v0.4

  # Build with active mod:
  python tools/registry/build_index.py --game-dir ... --active-mod darkrain

  # Check staleness (exit 1 + loud report if any input changed):
  python tools/registry/build_index.py --game-dir ... --check

  # Force rebuild even if index exists:
  python tools/registry/build_index.py --game-dir ... --rebuild

  # Query a domain:
  python tools/registry/build_index.py --game-dir ... --query missions
  python tools/registry/build_index.py --game-dir ... --query appearances
  python tools/registry/build_index.py --game-dir ... --query mods

Index placement: <game-dir>/.registry/registry-index.json
  The .registry/ prefix is a dot-dir under the deploy root (NOT under mods/),
  so the S5 unified dot-dir skip rule (ruling C4) keeps it invisible to the
  engine's mod-index walk -- the engine only walks mods/<id>/data/, not the
  deploy root's dot-dirs.  Cite: superpowers-execution-roadmap.md ruling C4.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

# ---------------------------------------------------------------------------
# Locate resolver.py (ruling C5: single shared module)
# ---------------------------------------------------------------------------
_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_TOOLS_DIR = os.path.dirname(_THIS_DIR)  # tools/
_MOD_INSTALL = os.path.join(_TOOLS_DIR, "mod_install")
if _MOD_INSTALL not in sys.path:
    sys.path.insert(0, _MOD_INSTALL)

from resolver import ResolverConfig, load_config, resolve, normalize_key  # noqa: E402

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

SCHEMA_ID = "mc2-registry-index/1"
TOOL_VERSION = "S6-v1.0"
INDEX_SUBDIR = ".registry"
INDEX_FILENAME = "registry-index.json"

# PacketFile header layout (mclib/packet.cpp + mclib/packet.h):
#   u32  version_or_checksum   0xFEEDFACE if no checksum, else the checksum value
#   u32  firstPacketOffset     byte offset of first data packet
#   numPackets = (firstPacketOffset / 4) - 2
#   table[numPackets] u32: each encodes (type<<29) | byteOffset
#     type STORAGE_TYPE_NUL (7) = null/empty packet
#   TABLE_ENTRY(p) = (2+p)*4  (byte offset of packet p's table slot)
PACKET_FILE_VERSION = 0xFEEDFACE
PACKET_STORAGE_TYPE_NUL = 0x07
PACKET_TYPE_SHIFT = 29
PACKET_OFFSET_MASK = (1 << 29) - 1
PACKET_HEADER_SIZE = 8


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _mtime_size(path: str) -> Tuple[float, int]:
    """Return (mtime_unix_float, size_bytes) for a file."""
    st = os.stat(path)
    return st.st_mtime, st.st_size


def _sha256(path: str) -> str:
    """Return hex sha256 of a file (first 64K for speed on large files)."""
    h = hashlib.sha256()
    try:
        with open(path, "rb") as f:
            chunk = f.read(65536)
            h.update(chunk)
    except OSError:
        return ""
    return h.hexdigest()


def _input_record(path: str) -> Dict[str, Any]:
    """Build an inputs[] entry for a file path."""
    try:
        mtime, size = _mtime_size(path)
    except OSError:
        mtime, size = 0.0, 0
    return {"path": path, "mtime": mtime, "size": size}


def _rel(base: str, path: str) -> str:
    """Return forward-slash relative path from base, or absolute if no rel."""
    try:
        return os.path.relpath(path, base).replace("\\", "/")
    except ValueError:
        return path.replace("\\", "/")


# ---------------------------------------------------------------------------
# Minimal FIT reader  (FitIniFile format: section [X] + key=value lines)
# ---------------------------------------------------------------------------

def _parse_fit_blocks(path: str) -> Dict[str, Dict[str, str]]:
    """
    Minimal FIT (FitIniFile) block reader.  Returns dict of
    {section_name: {key: value}}.  Values are stripped of surrounding quotes
    and whitespace.  Case-insensitive keys stored lowercase.
    Handles the 'l ', 'st ', 'f ', 'b ' type-prefix notation.
    """
    blocks: Dict[str, Dict[str, str]] = {}
    current: Optional[Dict[str, str]] = None
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for raw_line in f:
                line = raw_line.strip()
                if not line or line.startswith("//"):
                    continue
                if line.startswith("[") and line.endswith("]"):
                    sec = line[1:-1].strip()
                    current = {}
                    blocks[sec] = current
                    continue
                if current is None:
                    continue
                # Strip C++ style inline comments
                if "//" in line:
                    line = line[:line.index("//")].strip()
                if "=" not in line:
                    continue
                lhs, _, rhs = line.partition("=")
                lhs = lhs.strip().lower()
                rhs = rhs.strip().strip('"')
                # Strip type prefixes: 'l ', 'st ', 'f ', 'ul ', 'b '
                for prefix in ("st ", "ul ", "l ", "f ", "b "):
                    if lhs.startswith(prefix):
                        lhs = lhs[len(prefix):]
                        break
                current[lhs] = rhs
    except OSError:
        pass
    return blocks


# ---------------------------------------------------------------------------
# PacketFile header-only walk (no payload decompression)
# ---------------------------------------------------------------------------

def _packet_header_walk(pak_path: str) -> Tuple[int, bool]:
    """
    Read PacketFile header and return (num_packets, packet4_nonzero).
    packet4_nonzero = True iff packet index 4 exists with a non-NUL type.
    Returns (0, False) on parse error (not a valid pak or too small).

    Header layout (mclib/packet.cpp afterOpen + packet.h TABLE_ENTRY macro):
      u32  word0              0xFEEDFACE (version marker) or checksum value
      u32  firstPacketOffset  byte offset of first data packet
      numPackets = (firstPacketOffset / 4) - 2
      u32[numPackets]  table: each entry encodes (type<<29) | byteOffset
        STORAGE_TYPE_NUL (7<<29) = null/empty packet
    """
    try:
        with open(pak_path, "rb") as f:
            hdr = f.read(PACKET_HEADER_SIZE)
            if len(hdr) < PACKET_HEADER_SIZE:
                return 0, False
            _word0, first_packet_offset = struct.unpack("<II", hdr)
            if first_packet_offset < PACKET_HEADER_SIZE or first_packet_offset % 4 != 0:
                return 0, False
            num_packets = (first_packet_offset // 4) - 2
            if num_packets <= 0:
                return 0, False
            # Read up to 5 table entries to check packet 4
            read_count = min(num_packets, 5)
            table_bytes = f.read(4 * read_count)
            packet4_nonzero = False
            if len(table_bytes) == 4 * read_count:
                for i in range(read_count):
                    entry, = struct.unpack_from("<I", table_bytes, i * 4)
                    pkt_type = entry >> PACKET_TYPE_SHIFT
                    if i == 4 and pkt_type != PACKET_STORAGE_TYPE_NUL:
                        packet4_nonzero = True
        return num_packets, packet4_nonzero
    except (OSError, struct.error):
        return 0, False


# ---------------------------------------------------------------------------
# Domain scanners
# ---------------------------------------------------------------------------

def _scan_missions(
    game_dir: str,
    config: ResolverConfig,
    inputs: List[Dict],
) -> List[Dict[str, Any]]:
    """
    Enumerate missions from data/missions/*.fit and mod-overlay missions.
    Each entry: {name, fit, pak, pakPackets, movePacketPresent, providedBy}.
    Solo .fit files (no .pak peer) are also recorded (missions-only entries).
    """
    entries: List[Dict[str, Any]] = []
    seen: Dict[str, bool] = {}  # name -> processed

    # Collect candidate mission dirs: base + active mod
    candidate_dirs = []
    base_missions = os.path.join(game_dir, "data", "missions")
    if os.path.isdir(base_missions):
        candidate_dirs.append((base_missions, "base"))
    if config.active_mod and config.mod_index:
        # Find mod mission dirs from the mod index
        for key, (abs_path, mod_id) in config.mod_index.items():
            if key.startswith("data/missions/") and key.endswith(".fit"):
                # Extract mission name
                name = os.path.splitext(os.path.basename(key))[0]
                if name not in seen:
                    seen[name] = True
                    fit_path = abs_path
                    pak_key = key.replace(".fit", ".pak")
                    pak_result = resolve("data/missions/" + os.path.basename(pak_key), config)
                    pak_path = pak_result["path"] if pak_result["layer"] != "MISS" else ""
                    num_pkts, mv = (0, False)
                    if pak_path and os.path.isfile(pak_path):
                        num_pkts, mv = _packet_header_walk(pak_path)
                        inputs.append(_input_record(pak_path))
                    inputs.append(_input_record(fit_path))
                    entries.append({
                        "name": name,
                        "fit": fit_path.replace("\\", "/"),
                        "pak": pak_path.replace("\\", "/") if pak_path else "",
                        "pakPackets": num_pkts,
                        "movePacketPresent": mv,
                        "providedBy": mod_id,
                    })

    # Scan base missions dir (names not already provided by mod)
    if os.path.isdir(base_missions):
        for fname in sorted(os.listdir(base_missions)):
            if not fname.lower().endswith(".fit"):
                continue
            if fname.startswith("."):
                continue
            name = os.path.splitext(fname)[0]
            if name in seen:
                continue
            seen[name] = True
            fit_path = os.path.join(base_missions, fname)
            pak_path_candidate = os.path.join(base_missions, name + ".pak")
            pak_exists = os.path.isfile(pak_path_candidate)
            num_pkts, mv = (0, False)
            if pak_exists:
                num_pkts, mv = _packet_header_walk(pak_path_candidate)
                inputs.append(_input_record(pak_path_candidate))
            inputs.append(_input_record(fit_path))
            entries.append({
                "name": name,
                "fit": fit_path.replace("\\", "/"),
                "pak": pak_path_candidate.replace("\\", "/") if pak_exists else "",
                "pakPackets": num_pkts,
                "movePacketPresent": mv,
                "providedBy": "base",
            })

    return entries


def _scan_appearances(
    game_dir: str,
    config: ResolverConfig,
    inputs: List[Dict],
) -> List[Dict[str, Any]]:
    """
    Enumerate appearance .ini files from data/tgl/ (base + mod overlay).
    Each entry: {name, ini, tgl, providedBy}.
    Appearance class inference is heuristic (name prefix); identity key is name.
    """
    entries: List[Dict[str, Any]] = []
    seen: set = set()

    def _class_for(name: str) -> str:
        n = name.lower()
        if n.startswith("mech") or any(k in n for k in ("omni", "atlas", "timber")):
            return "MECH"
        if any(k in n for k in ("tank", "apc", "gv", "vehicle")):
            return "GV"
        if any(k in n for k in ("bldg", "building", "tower", "base")):
            return "BLDG"
        return "UNKNOWN"

    def _add(ini_abs: str, provided_by: str) -> None:
        fname = os.path.basename(ini_abs)
        if not fname.lower().endswith(".ini"):
            return
        name = os.path.splitext(fname)[0].lower()
        if name in seen:
            return
        seen.add(name)
        tgl_candidate = os.path.splitext(ini_abs)[0] + ".tgl"
        has_tgl = os.path.isfile(tgl_candidate)
        inputs.append(_input_record(ini_abs))
        entries.append({
            "name": name,
            "ini": ini_abs.replace("\\", "/"),
            "tgl": has_tgl,
            "class": _class_for(name),
            "providedBy": provided_by,
        })

    # Mod overlay appearances first (first-wins)
    if config.active_mod and config.mod_index:
        for key, (abs_path, mod_id) in sorted(config.mod_index.items()):
            if key.startswith("data/tgl/") and key.endswith(".ini"):
                _add(abs_path, mod_id)

    # Base appearances
    base_tgl = os.path.join(game_dir, "data", "tgl")
    if os.path.isdir(base_tgl):
        for fname in sorted(os.listdir(base_tgl)):
            if fname.startswith("."):
                continue
            if not fname.lower().endswith(".ini"):
                continue
            _add(os.path.join(base_tgl, fname), "base")

    return entries


def _scan_terrain_texture_fits(
    game_dir: str,
    config: ResolverConfig,
    inputs: List[Dict],
) -> List[Dict[str, Any]]:
    """
    Enumerate terrain texture-list FIT files (files containing MaxTerrainTypes
    or MaxTerrainTextures blocks).  Typically data/textures/textures.fit.
    Records: {fit, tileset, maxTerrainTypes, maxTerrainTextures, terrainTypeCount,
              providedBy}.
    V2-DEFER: per-TerrainType texture name list.
    """
    entries: List[Dict[str, Any]] = []
    candidate_fit = os.path.join(game_dir, "data", "textures", "textures.fit")

    def _scan_fit(fit_path: str, provided_by: str) -> None:
        if not os.path.isfile(fit_path):
            return
        inputs.append(_input_record(fit_path))
        blocks = _parse_fit_blocks(fit_path)
        main = blocks.get("Main", blocks.get("main", {}))
        max_types = int(main.get("maxterraintypes", main.get("MaxTerrainTypes".lower(), "0")) or "0")
        max_tex = int(main.get("maxterraintextures", main.get("MaxTerrainTextures".lower(), "0")) or "0")
        # Count TerrainType blocks
        terrain_type_count = sum(
            1 for k in blocks if k.lower().startswith("terraintype")
        )
        tset = os.path.splitext(os.path.basename(fit_path))[0]
        entries.append({
            "fit": fit_path.replace("\\", "/"),
            "tileset": tset,
            "maxTerrainTypes": max_types,
            "maxTerrainTextures": max_tex,
            "terrainTypeCount": terrain_type_count,
            "providedBy": provided_by,
            # V2-DEFER: per-type texture name list
        })

    # Check mod overlay first
    tfit_key = normalize_key("data/textures/textures.fit")
    if config.mod_index and tfit_key in config.mod_index:
        abs_path, mod_id = config.mod_index[tfit_key]
        _scan_fit(abs_path, mod_id)
    else:
        _scan_fit(candidate_fit, "base")

    return entries


def _scan_fastfiles(
    game_dir: str,
    config: ResolverConfig,
    inputs: List[Dict],
) -> List[Dict[str, Any]]:
    """
    Enumerate FastFile archives (.fst) in game_dir root.
    For each archive, check for a .fst.txt listing sidecar and consume it
    (or note that it's missing and can be generated by fst_listing.py).
    Each entry: {archive, listing, memberCount, providedBy}.
    """
    entries: List[Dict[str, Any]] = []
    gd = Path(game_dir)
    for fst_path in sorted(gd.glob("*.fst")):
        if fst_path.name.startswith("."):
            continue
        fst_str = str(fst_path).replace("\\", "/")
        inputs.append(_input_record(fst_str))
        listing = fst_str + ".txt"
        member_count = 0
        listing_present = os.path.isfile(listing)
        if listing_present:
            inputs.append(_input_record(listing))
            try:
                with open(listing, "r", encoding="utf-8", errors="replace") as f:
                    member_count = sum(
                        1 for ln in f
                        if ln.strip() and not ln.strip().startswith("#")
                    )
            except OSError:
                pass
        entries.append({
            "archive": fst_str,
            "listing": listing if listing_present else "",
            "memberCount": member_count,
            "listingPresent": listing_present,
            "providedBy": "base",
        })
    return entries


def _scan_object_types(
    game_dir: str,
    config: ResolverConfig,
    inputs: List[Dict],
) -> List[Dict[str, Any]]:
    """
    Enumerate object2.pak via header-only walk.
    Returns one opaque entry per pak file (not per packet).
    V2-DEFER: per-packet name/type requires decompressing pak payloads.
    """
    entries: List[Dict[str, Any]] = []
    pak_rel = "data/objects/object2.pak"
    pak_key = normalize_key(pak_rel)

    def _record(pak_path: str, provided_by: str) -> None:
        if not os.path.isfile(pak_path):
            return
        inputs.append(_input_record(pak_path))
        num_pkts, _ = _packet_header_walk(pak_path)
        entries.append({
            "pak": pak_path.replace("\\", "/"),
            "packetCount": num_pkts,
            "providedBy": provided_by,
            # V2-DEFER: per-packet {id, name, size, nonzero} list
            "note": "V2-DEFER: per-entry enumeration requires payload decompression",
        })

    # Check mod overlay
    if config.mod_index and pak_key in config.mod_index:
        abs_path, mod_id = config.mod_index[pak_key]
        _record(abs_path, mod_id)
    else:
        base_pak = os.path.join(game_dir, "data", "objects", "object2.pak")
        _record(base_pak, "base")

    return entries


def _scan_mods(
    game_dir: str,
    config: ResolverConfig,
    inputs: List[Dict],
) -> List[Dict[str, Any]]:
    """
    Enumerate mods present under game_dir/mods/, reading mod.json for each.
    Each entry: {id, name, type, dependencies, modDir, active, providedBy}.
    """
    entries: List[Dict[str, Any]] = []
    mods_root = os.path.join(game_dir, "mods")
    if not os.path.isdir(mods_root):
        return entries
    for entry in sorted(os.listdir(mods_root)):
        if entry.startswith("."):
            continue
        mod_dir = os.path.join(mods_root, entry)
        if not os.path.isdir(mod_dir):
            continue
        json_path = os.path.join(mod_dir, "mod.json")
        if os.path.isfile(json_path):
            inputs.append(_input_record(json_path))
            try:
                with open(json_path, "r", encoding="utf-8") as f:
                    data = json.load(f)
            except (OSError, json.JSONDecodeError):
                data = {}
        else:
            data = {}
        mod_id = str(data.get("id", entry))
        entries.append({
            "id": mod_id,
            "name": str(data.get("name", entry)),
            "type": str(data.get("type", "unknown")),
            "dependencies": data.get("dependencies", []),
            "modDir": mod_dir.replace("\\", "/"),
            "active": mod_id == config.active_mod,
            "providedBy": "mods-root",
        })
    return entries


def _scan_conflicts(config: ResolverConfig) -> List[Dict[str, Any]]:
    """
    Build conflicts[] from resolver state.
    Two conflict kinds:
      "mod-dup"       -- two or more mods provide the same data/ key
                         (from config.shadowed, which records mod-layer losers)
      "mod-over-base" -- a mod key shadows a base-game loose file
                         (detected by checking whether the base file exists
                          for each mod-index entry; same logic as resolver.py's
                          resolve() step 2 shadowed_list check at line 384-386)
    Each conflict: {domain, key, winner, losers, kind}.
    """

    def _domain_for(key: str) -> str:
        if "/missions/" in key:
            return "missions"
        if "/tgl/" in key:
            return "appearances"
        if "/objects/" in key:
            return "objectTypes"
        if "/textures/" in key:
            return "terrainTypes"
        return "unknown"

    conflicts: List[Dict[str, Any]] = []
    seen_keys: set = set()

    # Kind 1: mod-vs-mod (config.shadowed: key -> [loser mod_ids])
    for key, loser_ids in config.shadowed.items():
        if not loser_ids:
            continue
        winner_id = config.mod_index.get(key, ("", "MISS"))[1]
        conflicts.append({
            "domain": _domain_for(key),
            "key": key,
            "winner": winner_id,
            "losers": list(loser_ids),
            "kind": "mod-dup",
        })
        seen_keys.add(key)

    # Kind 2: mod shadows base-game loose file
    # For every key in the mod index, check if the base file also exists.
    # This mirrors the shadowed_list logic in resolver.resolve() lines 382-386.
    for key, (abs_path, mod_id) in config.mod_index.items():
        if key in seen_keys:
            continue  # already captured as mod-dup
        base_path = os.path.join(config.game_dir, key.replace("/", os.sep))
        if os.path.isfile(base_path):
            conflicts.append({
                "domain": _domain_for(key),
                "key": key,
                "winner": mod_id,
                "losers": ["base-loose"],
                "kind": "mod-over-base",
            })

    return conflicts


# ---------------------------------------------------------------------------
# Index build / check / query
# ---------------------------------------------------------------------------

def _index_path(game_dir: str) -> str:
    return os.path.join(game_dir, INDEX_SUBDIR, INDEX_FILENAME).replace("\\", "/")


def build_index(game_dir: str, active_mod: Optional[str] = None) -> Dict[str, Any]:
    """
    Scan game_dir and return the complete registry-index dict.
    """
    t0 = time.time()
    game_dir = game_dir.rstrip("/\\")
    config = load_config(game_dir, active_mod=active_mod)

    inputs: List[Dict] = []

    missions = _scan_missions(game_dir, config, inputs)
    appearances = _scan_appearances(game_dir, config, inputs)
    terrain_types = _scan_terrain_texture_fits(game_dir, config, inputs)
    fastfiles = _scan_fastfiles(game_dir, config, inputs)
    object_types = _scan_object_types(game_dir, config, inputs)
    mods = _scan_mods(game_dir, config, inputs)
    conflicts = _scan_conflicts(config)

    elapsed = time.time() - t0

    index = {
        "schema": SCHEMA_ID,
        "builtAt": datetime.now(timezone.utc).isoformat(),
        "toolVersion": TOOL_VERSION,
        "deployRoot": game_dir.replace("\\", "/"),
        "modChain": [active_mod] if active_mod else [],
        "buildElapsedSecs": round(elapsed, 3),
        "inputs": inputs,
        "domains": {
            "missions": missions,
            "appearances": appearances,
            "terrainTypes": terrain_types,
            "fastfiles": fastfiles,
            "objectTypes": object_types,
            "mods": mods,
        },
        "conflicts": conflicts,
    }
    return index


def write_index(game_dir: str, index: Dict[str, Any]) -> str:
    """Write index JSON to <game-dir>/.registry/registry-index.json."""
    out_path = _index_path(game_dir)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(index, f, indent=2)
        f.write("\n")
    return out_path


def load_index(game_dir: str) -> Optional[Dict[str, Any]]:
    """Load existing index or return None if not present / malformed."""
    path = _index_path(game_dir)
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError):
        return None


def check_staleness(index: Dict[str, Any]) -> List[str]:
    """
    Compare recorded inputs[] against current disk state.
    Returns list of stale-reason strings (empty = clean).
    """
    stale: List[str] = []
    for rec in index.get("inputs", []):
        path = rec.get("path", "")
        if not path:
            continue
        try:
            mtime, size = _mtime_size(path)
        except OSError:
            stale.append(f"MISSING: {path}")
            continue
        if abs(mtime - rec.get("mtime", 0)) > 1.0:
            stale.append(
                f"MTIME_CHANGED: {path}  recorded={rec.get('mtime'):.3f} disk={mtime:.3f}"
            )
        elif size != rec.get("size", -1):
            stale.append(
                f"SIZE_CHANGED: {path}  recorded={rec.get('size')} disk={size}"
            )
    return stale


def _print_stale_report(reasons: List[str], index_path: str) -> None:
    print("=" * 70, file=sys.stderr)
    print("STALE INDEX DETECTED", file=sys.stderr)
    print(f"  index: {index_path}", file=sys.stderr)
    print(f"  {len(reasons)} changed input(s):", file=sys.stderr)
    for r in reasons[:20]:
        print(f"    {r}", file=sys.stderr)
    if len(reasons) > 20:
        print(f"    ... and {len(reasons) - 20} more", file=sys.stderr)
    print("Run with --rebuild to regenerate.", file=sys.stderr)
    print("=" * 70, file=sys.stderr)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _print_domain_summary(index: Dict[str, Any]) -> None:
    domains = index.get("domains", {})
    print(f"schema:      {index.get('schema')}")
    print(f"builtAt:     {index.get('builtAt')}")
    print(f"deployRoot:  {index.get('deployRoot')}")
    print(f"modChain:    {index.get('modChain')}")
    print(f"inputs:      {len(index.get('inputs', []))} files")
    print(f"elapsed:     {index.get('buildElapsedSecs', '?')}s")
    print("domains:")
    for domain, entries in sorted(domains.items()):
        print(f"  {domain:20s}: {len(entries)} entries")
    print(f"conflicts:   {len(index.get('conflicts', []))}")


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="MC2 Registry Index Builder (S6)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--game-dir", required=True,
                        help="Deploy root (the directory mc2.exe runs from).")
    parser.add_argument("--active-mod", default=None,
                        help="Active mod ID (or read from MC2_ACTIVE_MOD env).")
    parser.add_argument("--check", action="store_true",
                        help="Exit nonzero with a loud report if the index is stale.")
    parser.add_argument("--rebuild", action="store_true",
                        help="Rebuild even if the index exists and is fresh.")
    parser.add_argument("--out-dir", default=None,
                        help="Write index to this directory instead of <game-dir>/.registry/.")
    parser.add_argument("--query", default=None,
                        metavar="DOMAIN",
                        help="Print entries for a domain (missions|appearances|"
                             "terrainTypes|fastfiles|objectTypes|mods|conflicts).")
    parser.add_argument("--quiet", action="store_true",
                        help="Suppress informational output; only print query results.")
    args = parser.parse_args(argv)

    game_dir = os.path.abspath(args.game_dir)
    if not os.path.isdir(game_dir):
        print(f"ERROR: --game-dir not found: {game_dir}", file=sys.stderr)
        return 1

    active_mod = args.active_mod or os.environ.get("MC2_ACTIVE_MOD") or None

    index_path = (
        os.path.join(args.out_dir, INDEX_FILENAME)
        if args.out_dir
        else _index_path(game_dir)
    )

    # --check mode: load existing index, check staleness, exit
    if args.check:
        existing = load_index(game_dir) if not args.out_dir else _load_from(index_path)
        if existing is None:
            print(f"STALE: no index at {index_path}", file=sys.stderr)
            return 1
        reasons = check_staleness(existing)
        if reasons:
            _print_stale_report(reasons, index_path)
            return 1
        if not args.quiet:
            print(f"OK: index is fresh ({len(existing.get('inputs', []))} inputs checked)")
        return 0

    # Load existing if not rebuilding
    if not args.rebuild:
        existing = load_index(game_dir) if not args.out_dir else _load_from(index_path)
        if existing is not None:
            reasons = check_staleness(existing)
            if not reasons:
                if not args.quiet:
                    print(f"Index is fresh; use --rebuild to force regeneration.")
                    _print_domain_summary(existing)
                if args.query:
                    _run_query(existing, args.query)
                return 0
            elif not args.quiet:
                print(f"Index is stale ({len(reasons)} input(s) changed); rebuilding ...",
                      file=sys.stderr)

    # Build
    if not args.quiet:
        print(f"Building registry index for {game_dir} ...")
    index = build_index(game_dir, active_mod=active_mod)

    # Write
    if args.out_dir:
        os.makedirs(args.out_dir, exist_ok=True)
        out_path = os.path.join(args.out_dir, INDEX_FILENAME)
    else:
        out_path = write_index(game_dir, index)

    if args.out_dir:
        with open(out_path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(index, f, indent=2)
            f.write("\n")

    if not args.quiet:
        print(f"Written: {out_path}")
        _print_domain_summary(index)

    if args.query:
        _run_query(index, args.query)

    return 0


def _load_from(path: str) -> Optional[Dict[str, Any]]:
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError):
        return None


def _run_query(index: Dict[str, Any], domain: str) -> None:
    """Print all entries for a domain (or 'conflicts') as JSON."""
    if domain == "conflicts":
        entries = index.get("conflicts", [])
    else:
        entries = index.get("domains", {}).get(domain)
    if entries is None:
        print(f"ERROR: unknown domain '{domain}'. "
              f"Available: {', '.join(sorted(index.get('domains', {}).keys()))}, conflicts",
              file=sys.stderr)
        return
    print(json.dumps(entries, indent=2))


if __name__ == "__main__":
    sys.exit(main())
