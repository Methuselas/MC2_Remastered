#!/usr/bin/env python3
"""tools/mc2texcook/tests/test_cook_terrain_layers.py -- Unit tests for
cook_terrain_layers.py (TERRAIN-MATERIAL-TEXTURE-REMAP-1 prep).

Covers:
  1. terrain_layer_manifest.json schema validation (the real, checked-in manifest).
  2. Synthetic cook round-trip: normalize_to_png / pack_height_into_normal_alpha
     end to end on small in-memory-generated PNGs (no ktx.exe / real downloaded
     assets required for these). If ktx.exe is present, also exercises the full
     PNG -> BC7 KTX2 cook and validates the resulting header.

Run:
    py -3 tools/mc2texcook/tests/test_cook_terrain_layers.py
    (or via pytest from repo root)
"""
from __future__ import annotations

import json
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

_TOOLS_DIR = Path(__file__).resolve().parent.parent
if str(_TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(_TOOLS_DIR))

import cook_pbr_maps as pbr          # noqa: E402
import cook_terrain_layers as ctl    # noqa: E402

MANIFEST_PATH = _TOOLS_DIR / 'terrain_layer_manifest.json'

KTX2_MAGIC = b"\xabKTX 20\xbb\r\n\x1a\n"
REQUIRED_CHANNELS = ('rock', 'grass', 'dirt', 'concrete', 'snow', 'cliff')
OPTIONAL_CHANNELS = ('shore_sand', 'wet_rock')


def _make_test_image(width=8, height=8, mode='RGB'):
    from PIL import Image
    img = Image.new(mode, (width, height))
    n = len(mode)
    if n == 3:
        pixels = [(x * 16 % 256, y * 16 % 256, 128) for y in range(height) for x in range(width)]
    else:
        pixels = [(x * 16 % 256, y * 16 % 256, 128, 255) for y in range(height) for x in range(width)]
    img.putdata(pixels)
    return img


# ---------------------------------------------------------------------------
# 1. Manifest schema validation
# ---------------------------------------------------------------------------

class TestManifestSchema(unittest.TestCase):

    def setUp(self):
        with open(MANIFEST_PATH, 'r', encoding='utf-8') as f:
            self.manifest = json.load(f)

    def test_manifest_is_valid(self):
        errors = ctl.validate_manifest(self.manifest)
        self.assertEqual(errors, [], f'manifest validation errors: {errors}')

    def test_output_size_is_2048(self):
        self.assertEqual(self.manifest['output_size'], 2048)

    def test_required_channels_present(self):
        for ch in REQUIRED_CHANNELS:
            self.assertIn(ch, self.manifest['layers'], f'missing required channel "{ch}"')

    def test_optional_shoreline_channels_present(self):
        for ch in OPTIONAL_CHANNELS:
            self.assertIn(ch, self.manifest['layers'], f'missing optional channel "{ch}"')

    def test_every_layer_has_albedo_and_normal(self):
        for channel, entry in self.manifest['layers'].items():
            maps = entry['maps']
            self.assertTrue(maps.get('albedo'), f'{channel}: no albedo map')
            self.assertTrue(maps.get('normal'), f'{channel}: no normal map')

    def test_every_layer_has_license_note(self):
        for channel, entry in self.manifest['layers'].items():
            self.assertIn('license', entry, f'{channel}: missing license note')
            self.assertTrue(entry['license'].strip(), f'{channel}: empty license note')

    def test_cliff_height_is_null(self):
        # marble_cliff_01 ships no displacement map; height must be explicitly
        # null (not missing) so the packer knows to fall back to flat alpha=255.
        self.assertIsNone(self.manifest['layers']['cliff']['maps'].get('height'))

    def test_wet_rock_flagged_needs_better_source(self):
        # No true wet-rock/tide-pool pack exists in the current downloads;
        # this must stay flagged until a better source is found.
        self.assertTrue(self.manifest['layers']['wet_rock'].get('needs_better_source'))

    def test_naming_convention_matches_driver_output(self):
        # Sanity: driver writes "<channel>_<map>.ktx2" -- confirm the manifest
        # documents the same convention it's actually consumed with.
        self.assertIn('<channel>_<map>.ktx2', self.manifest['naming_convention'])


# ---------------------------------------------------------------------------
# 2. Bad-manifest validation (negative cases, synthetic)
# ---------------------------------------------------------------------------

class TestManifestValidationNegative(unittest.TestCase):

    def test_missing_layers_key(self):
        errors = ctl.validate_manifest({'output_size': 2048})
        self.assertTrue(any('layers' in e for e in errors))

    def test_empty_layers(self):
        errors = ctl.validate_manifest({'layers': {}})
        self.assertTrue(any('empty' in e for e in errors))

    def test_non_power_of_two_size(self):
        manifest = {
            'output_size': 2000,
            'layers': {'rock': {'source_pack': 'x', 'source_dir': '.', 'license': 'CC0',
                                 'maps': {'albedo': 'a.png', 'normal': 'n.png'}}},
        }
        errors = ctl.validate_manifest(manifest)
        self.assertTrue(any('power-of-two' in e for e in errors))

    def test_missing_albedo_map(self):
        manifest = {
            'output_size': 2048,
            'layers': {'rock': {'source_pack': 'x', 'source_dir': '.', 'license': 'CC0',
                                 'maps': {'normal': 'n.png'}}},
        }
        errors = ctl.validate_manifest(manifest)
        self.assertTrue(any('albedo' in e for e in errors))

    def test_nonexistent_source_dir(self):
        manifest = {
            'output_size': 2048,
            'layers': {'rock': {'source_pack': 'x', 'source_dir': 'Z:/does/not/exist',
                                 'license': 'CC0', 'maps': {'albedo': 'a.png', 'normal': 'n.png'}}},
        }
        errors = ctl.validate_manifest(manifest)
        self.assertTrue(any('does not exist' in e for e in errors))


# ---------------------------------------------------------------------------
# 3. Synthetic cook round-trip (normalize + height-alpha packing, no ktx.exe)
# ---------------------------------------------------------------------------

class TestSyntheticCookRoundTrip(unittest.TestCase):

    def setUp(self):
        self.tmpdir = tempfile.mkdtemp(prefix='test_cook_terrain_')

    def tearDown(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def test_normalize_png_passthrough(self):
        from PIL import Image
        src = os.path.join(self.tmpdir, 'src.png')
        _make_test_image(mode='RGB').save(src)

        out = os.path.join(self.tmpdir, 'out.png')
        pbr.normalize_to_png(src, out, srgb=True)

        self.assertTrue(os.path.exists(out))
        img = Image.open(out)
        self.assertEqual(img.size, (8, 8))

    def test_normalize_jpg_to_png(self):
        from PIL import Image
        src = os.path.join(self.tmpdir, 'src.jpg')
        _make_test_image(mode='RGB').save(src, format='JPEG', quality=95)

        out = os.path.join(self.tmpdir, 'out.png')
        pbr.normalize_to_png(src, out, srgb=True)

        self.assertTrue(os.path.exists(out))
        img = Image.open(out)
        self.assertEqual(img.format, 'PNG')
        self.assertEqual(img.size, (8, 8))

    def test_pack_height_into_normal_alpha(self):
        from PIL import Image
        normal_src = os.path.join(self.tmpdir, 'normal.png')
        height_src = os.path.join(self.tmpdir, 'height.png')
        _make_test_image(mode='RGB').save(normal_src)
        Image.new('L', (8, 8), 200).save(height_src)

        out = os.path.join(self.tmpdir, 'packed.png')
        pbr.pack_height_into_normal_alpha(normal_src, height_src, out, size=16)

        packed = Image.open(out)
        self.assertEqual(packed.mode, 'RGBA')
        self.assertEqual(packed.size, (16, 16))
        r, g, b, a = packed.split()
        # Height was a flat 200-gray image -- alpha channel should be ~200
        # everywhere after resize (LANCZOS may introduce tiny edge ringing,
        # so check the center pixel where no edge effects apply).
        self.assertAlmostEqual(a.getpixel((8, 8)), 200, delta=2)

    def test_pack_height_none_fills_alpha_255(self):
        from PIL import Image
        normal_src = os.path.join(self.tmpdir, 'normal.png')
        _make_test_image(mode='RGB').save(normal_src)

        out = os.path.join(self.tmpdir, 'packed.png')
        pbr.pack_height_into_normal_alpha(normal_src, None, out, size=16)

        packed = Image.open(out)
        r, g, b, a = packed.split()
        self.assertEqual(a.getpixel((8, 8)), 255)

    def test_resize_to_square(self):
        from PIL import Image
        src = os.path.join(self.tmpdir, 'src.png')
        _make_test_image(width=4, height=16, mode='RGBA').save(src)

        out = os.path.join(self.tmpdir, 'square.png')
        pbr.resize_to_square(src, out, size=32)

        img = Image.open(out)
        self.assertEqual(img.size, (32, 32))

    @unittest.skipUnless(os.path.exists(pbr.DEFAULT_KTX_EXE), 'ktx.exe not available in this environment')
    def test_full_synthetic_cook_to_bc7(self):
        """End-to-end: synthetic PNG -> BC7 KTX2, validate header bytes."""
        src = os.path.join(self.tmpdir, 'albedo.png')
        _make_test_image(width=32, height=32, mode='RGBA').save(src)

        out = os.path.join(self.tmpdir, 'albedo.ktx2')
        ok = pbr.cook_source_to_bc7(
            pbr.DEFAULT_KTX_EXE, src, out,
            vk_format='R8G8B8A8_SRGB', oetf='srgb', tmpdir=self.tmpdir,
        )
        self.assertTrue(ok)
        self.assertTrue(os.path.exists(out))

        with open(out, 'rb') as f:
            data = f.read(64)
        self.assertEqual(data[:12], KTX2_MAGIC)


if __name__ == '__main__':
    unittest.main()
