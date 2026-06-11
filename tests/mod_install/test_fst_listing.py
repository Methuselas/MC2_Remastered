"""
Unit tests for tools/mod_install/fst_listing.py (S4b)
Tests: binary .fst parsing (both versions), entry-table bounds validation,
       listing emission, resolver auto-discovery integration.
All tests build synthetic archives in tempdirs -- no deploy required.
"""

import os
import struct
import sys
import tempfile
import unittest

_TOOLS = os.path.join(os.path.dirname(__file__), "..", "..", "tools", "mod_install")
if _TOOLS not in sys.path:
    sys.path.insert(0, os.path.abspath(_TOOLS))

from fst_listing import (
    parse_fst,
    write_listing,
    normalize_key,
    FASTFILE_VERSION,
    FASTFILE_VERSION_LZ,
    ENTRY_SIZE,
    ENTRY_TABLE_START,
)
from resolver import ResolverConfig, _build_fst_set, _discover_fst_listings


def make_archive(path, names, version=FASTFILE_VERSION):
    """Build a synthetic .fst with one 4-byte payload per name."""
    num = len(names)
    payload_start = ENTRY_TABLE_START + ENTRY_SIZE * num
    with open(path, "wb") as f:
        f.write(struct.pack("<II", version, num))
        for i, name in enumerate(names):
            f.write(struct.pack(
                "<IIII250s",
                payload_start + 4 * i, 4, 4, 0xDEAD + i,
                name.encode("ascii"),
            ))
        f.write(b"\x00" * (4 * num))
    return path


class TestParseFst(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name

    def tearDown(self):
        self.tmp.cleanup()

    def test_parse_basic(self):
        p = make_archive(os.path.join(self.dir, "a.fst"),
                         ["data/art/foo.tga", "data/tgl/bar.ini"])
        entries = parse_fst(p)
        self.assertEqual([e["name"] for e in entries],
                         ["data/art/foo.tga", "data/tgl/bar.ini"])
        self.assertEqual(entries[0]["size"], 4)
        self.assertEqual(entries[0]["realSize"], 4)
        self.assertEqual(entries[0]["hash"], 0xDEAD)

    def test_parse_lz_version(self):
        p = make_archive(os.path.join(self.dir, "lz.fst"), ["data/x.abl"],
                         version=FASTFILE_VERSION_LZ)
        self.assertEqual(len(parse_fst(p)), 1)

    def test_bad_version_rejected(self):
        p = os.path.join(self.dir, "bad.fst")
        with open(p, "wb") as f:
            f.write(struct.pack("<II", 0x12345678, 0))
        with self.assertRaises(ValueError):
            parse_fst(p)

    def test_truncated_table_rejected(self):
        p = os.path.join(self.dir, "trunc.fst")
        with open(p, "wb") as f:
            f.write(struct.pack("<II", FASTFILE_VERSION, 5))
            f.write(b"\x00" * 10)
        with self.assertRaises(ValueError):
            parse_fst(p)

    def test_payload_out_of_bounds_rejected(self):
        p = make_archive(os.path.join(self.dir, "oob.fst"), ["data/y.tga"])
        # Truncate the payload region
        with open(p, "r+b") as f:
            f.truncate(ENTRY_TABLE_START + ENTRY_SIZE)
        with self.assertRaises(ValueError):
            parse_fst(p)

    def test_empty_archive(self):
        p = make_archive(os.path.join(self.dir, "empty.fst"), [])
        self.assertEqual(parse_fst(p), [])


class TestListingAndDiscovery(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name

    def tearDown(self):
        self.tmp.cleanup()

    def test_listing_normalized_and_discovered(self):
        # Stored names with mixed case / backslashes must come out normalized
        make_archive(os.path.join(self.dir, "art.fst"),
                     ["Data\\Art\\Foo.TGA", "data/tgl/bar.ini"])
        n = write_listing(os.path.join(self.dir, "art.fst"),
                          out_dir=None, as_json=False, dry_run=False)
        self.assertEqual(n, 2)

        listing = os.path.join(self.dir, "art.fst.txt")
        self.assertTrue(os.path.isfile(listing))

        # resolver auto-discovery: *.fst.txt found, binary art.fst skipped
        discovered = _discover_fst_listings(self.dir)
        self.assertIn(listing, discovered)
        self.assertNotIn(os.path.join(self.dir, "art.fst"), discovered)

        members = _build_fst_set(discovered)
        self.assertEqual(members,
                         {"data/art/foo.tga", "data/tgl/bar.ini"})

    def test_resolver_config_picks_up_listing(self):
        make_archive(os.path.join(self.dir, "m.fst"), ["data/missions/a.fit"])
        write_listing(os.path.join(self.dir, "m.fst"),
                      out_dir=None, as_json=False, dry_run=False)
        cfg = ResolverConfig(game_dir=self.dir)
        self.assertIn("data/missions/a.fit", cfg.fst_set)

    def test_dry_run_writes_nothing(self):
        p = make_archive(os.path.join(self.dir, "d.fst"), ["data/z.tga"])
        write_listing(p, out_dir=None, as_json=False, dry_run=True)
        self.assertFalse(os.path.exists(p + ".txt"))


if __name__ == "__main__":
    unittest.main()
