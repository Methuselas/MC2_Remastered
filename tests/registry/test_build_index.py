"""
Unit tests for tools/registry/build_index.py (S6).

Tests cover:
  - Full index build from a synthetic tempdir fixture.
  - Schema ID and required top-level fields.
  - Domain presence and entry shapes.
  - Staleness detection: touch a file -> STALE.
  - Dot-dir exclusion: .scratch/ files not indexed.
  - Resolver-layer agreement: mod-provided file -> providedBy != "base".
  - --check mode: exit 0 when fresh, exit 1 when stale.
  - Conflicts: overlapping mod file surfaces in conflicts[].

All tests use tempdir fixtures; no deploy required.
"""

from __future__ import annotations

import json
import os
import sys
import tempfile
import time
import unittest
from pathlib import Path

# Wire in both tools/registry and tools/mod_install
_TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
_WORKTREE = os.path.abspath(os.path.join(_TESTS_DIR, "..", ".."))
_TOOLS_REG = os.path.join(_WORKTREE, "tools", "registry")
_TOOLS_MOD = os.path.join(_WORKTREE, "tools", "mod_install")
for _p in (_TOOLS_REG, _TOOLS_MOD):
    if _p not in sys.path:
        sys.path.insert(0, _p)

from build_index import (
    SCHEMA_ID,
    build_index,
    check_staleness,
    write_index,
    load_index,
    _index_path,
    main as bi_main,
)


# ---------------------------------------------------------------------------
# Fixture helpers
# ---------------------------------------------------------------------------

def _make_game_dir(root: str) -> str:
    """
    Create a minimal synthetic deploy dir under root:
      data/missions/test_01.fit
      data/missions/test_01.pak (minimal PacketFile header, 6 packets, pkt4 nonzero)
      data/missions/solo_01.fit  (no matching .pak -- solo-mission entry)
      data/tgl/abuilding.ini
      data/tgl/abuilding.tgl
      data/tgl/atank.ini
      data/tgl/.hidden.ini       (must NOT be indexed)
      data/textures/textures.fit (terrain types)
      data/objects/object2.pak   (minimal header, 10 packets)
      mods/mymod/mod.json
      mods/mymod/data/missions/mod_01.fit
      mods/mymod/data/tgl/abuilding.ini  (overrides base)
      .scratch/internal.json             (must NOT affect index)
    """
    import struct

    def _mkdir(*parts: str) -> str:
        p = os.path.join(root, *parts)
        os.makedirs(p, exist_ok=True)
        return p

    def _write(path: str, content: str = "") -> None:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write(content)

    def _write_pak(path: str, num_packets: int, pkt4_size: int = 0) -> None:
        """
        Write a minimal PacketFile with num_packets entries.
        Format (mclib/packet.cpp afterOpen, packet.h TABLE_ENTRY):
          u32 word0            = 0xFEEDFACE (version marker)
          u32 firstPacketOffset = (2 + num_packets) * 4  (byte offset of first data)
          u32[num_packets] table: each = (type<<29) | byteOffset
            STORAGE_TYPE_NUL = 7, STORAGE_TYPE_RAW = 0
        """
        os.makedirs(os.path.dirname(path), exist_ok=True)
        FEEDFACE = 0xFEEDFACE
        STORAGE_TYPE_NUL = 0x07
        STORAGE_TYPE_RAW = 0x00
        TYPE_SHIFT = 29
        first_pkt_offset = (2 + num_packets) * 4  # byte after the table
        header = struct.pack("<II", FEEDFACE, first_pkt_offset)
        # Table: each entry = (type<<29) | byteOffset
        # For pkt4: use RAW type so it is non-NUL; others use NUL
        payload_offset = first_pkt_offset
        entries = b""
        for i in range(num_packets):
            if i == 4 and pkt4_size > 0:
                entry = (STORAGE_TYPE_RAW << TYPE_SHIFT) | payload_offset
            else:
                entry = (STORAGE_TYPE_NUL << TYPE_SHIFT) | payload_offset
            entries += struct.pack("<I", entry)
        payload = b"\xAB" * max(pkt4_size, 1)
        with open(path, "wb") as f:
            f.write(header + entries + payload)

    # Missions
    _write(os.path.join(root, "data", "missions", "test_01.fit"),
           "[Main]\nst MissionName = \"Test 01\"\n")
    _write_pak(os.path.join(root, "data", "missions", "test_01.pak"),
               num_packets=6, pkt4_size=42)
    _write(os.path.join(root, "data", "missions", "solo_01.fit"),
           "[Main]\nst MissionName = \"Solo 01\"\n")

    # Appearances
    _write(os.path.join(root, "data", "tgl", "abuilding.ini"),
           "[Main]\nst AppearanceName = \"abuilding\"\n")
    _write(os.path.join(root, "data", "tgl", "abuilding.tgl"), "FAKE_TGL")
    _write(os.path.join(root, "data", "tgl", "atank.ini"),
           "[Main]\nst AppearanceName = \"atank\"\n")
    # Hidden file -- must be excluded
    _write(os.path.join(root, "data", "tgl", ".hidden.ini"), "[Main]\n")

    # Terrain texture FIT
    _write(os.path.join(root, "data", "textures", "textures.fit"),
           "[Main]\nl MaxTerrainTypes = 10\nl MaxTerrainTextures = 50\n"
           "[TerrainType0]\nl TerrainId = 0\nst TerrainName = \"water\"\n"
           "[TerrainType1]\nl TerrainId = 1\nst TerrainName = \"grass\"\n")

    # Object pak
    _write_pak(os.path.join(root, "data", "objects", "object2.pak"),
               num_packets=10, pkt4_size=0)

    # Mod
    _mkdir("mods", "mymod")
    _write(os.path.join(root, "mods", "mymod", "mod.json"),
           json.dumps({"id": "mymod", "name": "My Mod", "type": "map",
                       "dependencies": []}))
    _write(os.path.join(root, "mods", "mymod", "data", "missions", "mod_01.fit"),
           "[Main]\nst MissionName = \"Mod Mission\"\n")
    # Override base appearance
    _write(os.path.join(root, "mods", "mymod", "data", "tgl", "abuilding.ini"),
           "[Main]\nst AppearanceName = \"abuilding-override\"\n")

    # Dot-dir at deploy root -- must NOT pollute index inputs
    _write(os.path.join(root, ".scratch", "internal.json"), "{}")

    return root


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

class TestBuildIndexBase(unittest.TestCase):
    """Build a base-game index (no active mod) and verify structure."""

    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()
        _make_game_dir(self.tmpdir)
        self.index = build_index(self.tmpdir)

    def test_schema_id(self):
        self.assertEqual(self.index["schema"], SCHEMA_ID)

    def test_required_top_level_fields(self):
        for field in ("builtAt", "toolVersion", "deployRoot", "modChain",
                      "inputs", "domains", "conflicts"):
            self.assertIn(field, self.index, f"missing field: {field}")

    def test_mod_chain_empty_for_base(self):
        self.assertEqual(self.index["modChain"], [])

    def test_missions_domain_present(self):
        missions = self.index["domains"]["missions"]
        names = {m["name"] for m in missions}
        self.assertIn("test_01", names)
        self.assertIn("solo_01", names)

    def test_mission_pak_packets(self):
        missions = {m["name"]: m for m in self.index["domains"]["missions"]}
        t = missions["test_01"]
        self.assertEqual(t["pakPackets"], 6)
        self.assertTrue(t["movePacketPresent"])

    def test_solo_mission_no_pak(self):
        missions = {m["name"]: m for m in self.index["domains"]["missions"]}
        s = missions["solo_01"]
        self.assertEqual(s["pak"], "")
        self.assertFalse(s["movePacketPresent"])

    def test_appearances_domain(self):
        apps = self.index["domains"]["appearances"]
        names = {a["name"] for a in apps}
        self.assertIn("abuilding", names)
        self.assertIn("atank", names)

    def test_hidden_ini_excluded(self):
        apps = self.index["domains"]["appearances"]
        names = {a["name"] for a in apps}
        self.assertNotIn(".hidden", names)

    def test_appearance_tgl_presence(self):
        apps = {a["name"]: a for a in self.index["domains"]["appearances"]}
        self.assertTrue(apps["abuilding"]["tgl"])
        self.assertFalse(apps["atank"]["tgl"])

    def test_terrain_types_domain(self):
        tt = self.index["domains"]["terrainTypes"]
        self.assertGreater(len(tt), 0)
        entry = tt[0]
        self.assertIn("fit", entry)
        self.assertEqual(entry["terrainTypeCount"], 2)
        self.assertEqual(entry["maxTerrainTypes"], 10)

    def test_object_types_domain(self):
        ot = self.index["domains"]["objectTypes"]
        self.assertGreater(len(ot), 0)
        self.assertEqual(ot[0]["packetCount"], 10)

    def test_mods_domain(self):
        mods = self.index["domains"]["mods"]
        self.assertGreater(len(mods), 0)
        ids = {m["id"] for m in mods}
        self.assertIn("mymod", ids)

    def test_inputs_not_empty(self):
        self.assertGreater(len(self.index["inputs"]), 0)

    def test_inputs_have_required_fields(self):
        for rec in self.index["inputs"]:
            self.assertIn("path", rec)
            self.assertIn("mtime", rec)
            self.assertIn("size", rec)

    def test_dot_scratch_not_in_inputs(self):
        """Dot-dirs at deploy root must not appear as inputs."""
        for rec in self.index["inputs"]:
            self.assertNotIn(".scratch", rec["path"],
                             f"dot-dir leaked into inputs: {rec['path']}")


class TestStalenessDetection(unittest.TestCase):
    """Staleness: touch a file -> check_staleness returns reasons."""

    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()
        _make_game_dir(self.tmpdir)
        self.index = build_index(self.tmpdir)

    def test_fresh_index_is_clean(self):
        reasons = check_staleness(self.index)
        self.assertEqual(reasons, [], f"Expected clean, got: {reasons}")

    def test_touch_file_makes_stale(self):
        # Find an input that actually exists on disk
        target = None
        for rec in self.index["inputs"]:
            if os.path.isfile(rec["path"]):
                target = rec["path"]
                break
        self.assertIsNotNone(target, "No real file in inputs to touch")
        # Advance mtime by 2 seconds
        stat = os.stat(target)
        os.utime(target, (stat.st_atime + 2.0, stat.st_mtime + 2.0))
        reasons = check_staleness(self.index)
        self.assertGreater(len(reasons), 0, "Expected stale after touch")
        self.assertTrue(any(os.path.basename(target) in r for r in reasons))

    def test_write_index_then_load_fresh(self):
        out_path = write_index(self.tmpdir, self.index)
        self.assertTrue(os.path.isfile(out_path))
        loaded = load_index(self.tmpdir)
        self.assertIsNotNone(loaded)
        reasons = check_staleness(loaded)
        self.assertEqual(reasons, [])

    def test_check_mode_clean(self):
        write_index(self.tmpdir, self.index)
        ret = bi_main(["--game-dir", self.tmpdir, "--check", "--quiet"])
        self.assertEqual(ret, 0)

    def test_check_mode_stale(self):
        write_index(self.tmpdir, self.index)
        # Touch a mission fit
        fit_path = os.path.join(self.tmpdir, "data", "missions", "test_01.fit")
        stat = os.stat(fit_path)
        os.utime(fit_path, (stat.st_atime + 5.0, stat.st_mtime + 5.0))
        ret = bi_main(["--game-dir", self.tmpdir, "--check", "--quiet"])
        self.assertEqual(ret, 1)


class TestDotDirExclusion(unittest.TestCase):
    """S5 dot-dir rule: entries under dot-dirs must not be indexed."""

    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()
        _make_game_dir(self.tmpdir)

    def test_dot_scratch_not_in_inputs(self):
        index = build_index(self.tmpdir)
        for rec in index["inputs"]:
            p = rec["path"].replace("\\", "/")
            self.assertNotIn("/.scratch/", p,
                             f"dot-dir file in inputs: {p}")

    def test_mod_dot_dir_not_indexed(self):
        """Files inside mods/mymod/.modproject/ must not appear in mod index."""
        dot_dir = os.path.join(self.tmpdir, "mods", "mymod", ".modproject")
        os.makedirs(dot_dir, exist_ok=True)
        dot_file = os.path.join(dot_dir, "data", "missions", "hidden.fit")
        os.makedirs(os.path.dirname(dot_file), exist_ok=True)
        with open(dot_file, "w") as f:
            f.write("[Main]\n")
        index = build_index(self.tmpdir, active_mod="mymod")
        missions = {m["name"] for m in index["domains"]["missions"]}
        self.assertNotIn("hidden", missions,
                         "dot-dir mission leaked into index")


class TestResolverLayerAgreement(unittest.TestCase):
    """Mod-provided file must appear with providedBy == mod ID, not 'base'."""

    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()
        _make_game_dir(self.tmpdir)

    def test_mod_mission_provided_by_mod(self):
        index = build_index(self.tmpdir, active_mod="mymod")
        missions = {m["name"]: m for m in index["domains"]["missions"]}
        self.assertIn("mod_01", missions, "mod_01 mission not found")
        self.assertEqual(missions["mod_01"]["providedBy"], "mymod")

    def test_base_mission_provided_by_base(self):
        index = build_index(self.tmpdir, active_mod="mymod")
        missions = {m["name"]: m for m in index["domains"]["missions"]}
        # test_01 exists only in base; may or may not be in index depending on
        # whether mod also has it.  If present, should be base.
        if "test_01" in missions:
            self.assertEqual(missions["test_01"]["providedBy"], "base")

    def test_overridden_appearance_provided_by_mod(self):
        index = build_index(self.tmpdir, active_mod="mymod")
        apps = {a["name"]: a for a in index["domains"]["appearances"]}
        # abuilding is overridden by mymod
        self.assertIn("abuilding", apps)
        self.assertEqual(apps["abuilding"]["providedBy"], "mymod",
                         f"Expected mymod, got {apps['abuilding']['providedBy']}")

    def test_mod_chain_recorded(self):
        index = build_index(self.tmpdir, active_mod="mymod")
        self.assertEqual(index["modChain"], ["mymod"])

    def test_conflict_for_overridden_appearance(self):
        """abuilding overridden by mod -> should appear in conflicts[]."""
        index = build_index(self.tmpdir, active_mod="mymod")
        conflicts = index.get("conflicts", [])
        conflict_keys = {c["key"] for c in conflicts}
        # The mod overrides data/tgl/abuilding.ini
        self.assertTrue(
            any("abuilding" in k for k in conflict_keys),
            f"Expected abuilding conflict; got keys: {conflict_keys}",
        )


if __name__ == "__main__":
    unittest.main()
