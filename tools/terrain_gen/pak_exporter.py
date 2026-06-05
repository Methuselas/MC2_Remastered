# tools/terrain_gen/pak_exporter.py
from __future__ import annotations
import struct
import math
import numpy as np

from terrain_gen.terrain_recipe import TerrainRecipe
from terrain_gen.material_classifier import TerrainMasks

# PostcompVertex: 3 floats normal + float elevation + uint32 textureData +
#                 uint32 localRGBLight + uint32 terrainType + 4 bytes flags
_VERTEX = struct.Struct('<3f f I I I 4B')
_NEUTRAL_LIGHT = 0x00CCCCCC  # aRGB: moderate warm-white pre-baked lighting
_MAGIC = 0xFEEDFACE


class PakExporter:
    def build_packet0(
        self,
        height: np.ndarray,
        masks: TerrainMasks,
        recipe: TerrainRecipe,
    ) -> bytes:
        """Build raw PostcompVertex[] bytes for Packet 0 (row-major, y outer loop)."""
        N   = recipe.size
        h_p = recipe.height
        buf = bytearray(N * N * _VERTEX.size)
        off = 0
        for y in range(N):
            for x in range(N):
                nx, ny, nz = self._normal(height, x, y, h_p.max_elevation)
                elev = float(height[y, x]) * h_p.max_elevation + h_p.min_elevation
                tt   = int(masks.terrain_type[y, x])
                _VERTEX.pack_into(buf, off,
                    nx, ny, nz,
                    elev,
                    0,              # textureData — engine derives from terrainType
                    _NEUTRAL_LIGHT, # localRGBLight
                    tt,             # terrainType
                    0, 0, 0, 0,    # selected, water, shadow, highlighted
                )
                off += _VERTEX.size
        return bytes(buf)

    def _normal(self, height: np.ndarray, x: int, y: int, max_elevation: float) -> tuple[float, float, float]:
        """
        Finite-difference surface normal (Y-up, MC2 convention).
        height in [0,1]; elevation = height * max_elevation world units.
        Horizontal vertex spacing = 128 wu (worldUnitsPerVertex).
        dx = (elev_right - elev_left) / (2 * 128)
           = ((h_right - h_left) * max_elevation) / 256
        """
        N  = height.shape[0]
        hl = height[y, max(x-1, 0)]
        hr = height[y, min(x+1, N-1)]
        hu = height[max(y-1, 0), x]
        hd = height[min(y+1, N-1), x]
        dx = ((hr - hl) * max_elevation) / 256.0
        dz = ((hd - hu) * max_elevation) / 256.0
        nx, ny, nz = -dx, 1.0, -dz
        length = math.sqrt(nx*nx + ny*ny + nz*nz)
        if length > 1e-8:
            nx /= length; ny /= length; nz /= length
        return nx, ny, nz

    def patch_pak(self, template_path: str, out_path: str, packet0_data: bytes) -> None:
        """
        Copy template_path, replace Packet 0 with packet0_data, write to out_path.
        Requires same-size replacement (raises ValueError otherwise).
        """
        with open(template_path, 'rb') as f:
            raw = bytearray(f.read())

        magic = struct.unpack_from('<I', raw, 0)[0]
        if magic != _MAGIC:
            raise ValueError(f"Not a PacketFile (magic={magic:#010x}, expected={_MAGIC:#010x})")

        fpo = struct.unpack_from('<I', raw, 4)[0]
        n_pkt = fpo // 4 - 2
        if n_pkt < 2:
            raise ValueError(f"PacketFile needs at least 2 seek entries, got {n_pkt}")

        seek = list(struct.unpack_from(f'<{n_pkt}I', raw, 8))  # unsigned — file offsets are never negative
        start = seek[0]
        end   = seek[1]
        expected = end - start

        if len(packet0_data) != expected:
            raise ValueError(
                f"Packet 0 size mismatch: template={expected} bytes, "
                f"new data={len(packet0_data)} bytes. Grids must match."
            )

        raw[start:end] = packet0_data
        with open(out_path, 'wb') as f:
            f.write(raw)
