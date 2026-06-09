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

_VERTEX_DTYPE = np.dtype([
    ("normal",        "<f4", (3,)),
    ("elevation",     "<f4"),
    ("textureData",   "<u4"),
    ("localRGBLight", "<u4"),
    ("terrainType",   "<u4"),
    ("selected",      "u1"),
    ("water",         "u1"),
    ("shadow",        "u1"),
    ("highlighted",   "u1"),
])

assert _VERTEX_DTYPE.itemsize == _VERTEX.size


class PakExporter:
    def _normals_array(self, height: np.ndarray, max_elevation: float) -> np.ndarray:
        """Vectorized finite-difference normals for all vertices (Y-up, MC2 convention).

        height in [0,1]; elevation = height * max_elevation world units.
        Horizontal vertex spacing = 128 wu (worldUnitsPerVertex).
        Returns array shape (N, N, 3) of normal vectors.
        """
        h = np.asarray(height, dtype=np.float32)
        scale = np.float32(max_elevation / 256.0)

        dx = np.empty_like(h, dtype=np.float32)
        dz = np.empty_like(h, dtype=np.float32)

        # Interior: centered difference (h[x+1] - h[x-1]) / 2
        # Edges: clamp to same edge cell (one-sided difference)
        dx[:, 1:-1] = (h[:, 2:] - h[:, :-2]) * scale
        dx[:, 0]    = (h[:, 1] - h[:, 0]) * scale
        dx[:, -1]   = (h[:, -1] - h[:, -2]) * scale

        dz[1:-1, :] = (h[2:, :] - h[:-2, :]) * scale
        dz[0, :]    = (h[1, :] - h[0, :]) * scale
        dz[-1, :]   = (h[-1, :] - h[-2, :]) * scale

        n = np.empty((*h.shape, 3), dtype=np.float32)
        n[..., 0] = -dx
        n[..., 1] = 1.0
        n[..., 2] = -dz

        length = np.sqrt(np.maximum(np.sum(n * n, axis=2), np.float32(1e-16)))
        n /= length[..., None]
        return n

    def build_packet0(
        self,
        height: np.ndarray,
        masks: TerrainMasks,
        recipe: TerrainRecipe,
    ) -> bytes:
        """Build raw PostcompVertex[] bytes for Packet 0 (row-major, y outer loop)."""
        N = recipe.size
        h_p = recipe.height

        if height.shape != (N, N):
            raise ValueError(f"height shape {height.shape} does not match recipe size {N}")
        if masks.terrain_type.shape != (N, N):
            raise ValueError(f"terrain_type shape {masks.terrain_type.shape} does not match recipe size {N}")

        out = np.zeros((N, N), dtype=_VERTEX_DTYPE)

        out["normal"] = self._normals_array(height, h_p.max_elevation)
        out["elevation"] = (
            height.astype(np.float32) * np.float32(h_p.max_elevation)
            + np.float32(h_p.min_elevation)
        ).astype("<f4")

        out["textureData"] = np.uint32(0)
        out["localRGBLight"] = np.uint32(_NEUTRAL_LIGHT)
        out["terrainType"] = masks.terrain_type.astype("<u4", copy=False)

        return out.tobytes(order="C")

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
