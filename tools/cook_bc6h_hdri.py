#!/usr/bin/env python3
"""
cook_bc6h_hdri.py -- Cook an EXR HDRI into a BC6H_UFLOAT_BLOCK KTX2 file.

Implements BC6H mode 11 (unsigned float, no partition, 10-bit endpoints).
Uses fully vectorized numpy for the heavy lifting, then calls ktx.exe --raw
to produce a standards-compliant KTX2 file.

VK_FORMAT_BC6H_UFLOAT_BLOCK = 143 (Vulkan 1.4 spec)
VK_FORMAT_BC6H_SFLOAT_BLOCK = 144

Usage:
    python cook_bc6h_hdri.py <input.exr> <output.ktx2> [<path_to_ktx.exe>]
"""

import sys
import os
import subprocess
import tempfile
import time
import struct
import numpy as np

try:
    import OpenEXR
    import Imath
except ImportError:
    print("ERROR: OpenEXR Python module required.", file=sys.stderr)
    sys.exit(1)

# BC6H 4-bit palette interpolation weights (D3D10 BC6H spec)
# 16-entry palette: weight[i] = round(i * 64 / 15)
BC6H_WEIGHTS_4 = np.array([0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64],
                           dtype=np.float32)

VK_FORMAT_BC6H_UFLOAT_BLOCK = 143  # Vulkan spec, verified against SDK header


def quantize_ep10(vals_f32):
    """
    Quantize float32 -> 10-bit BC6H_UFLOAT endpoint (vectorized).
    BC6H_UFLOAT: endpoint = clamp(half_float_bits >> 6, 0, 1023)
    """
    clamped = np.clip(vals_f32, 0.0, 65504.0).astype(np.float32)
    h_bits = clamped.astype(np.float16).view(np.uint16).astype(np.uint32)
    return np.clip(h_bits >> 6, 0, 0x3FF).astype(np.uint16)


def reconstruct_ep10(ep10):
    """Reconstruct float32 from 10-bit BC6H_UFLOAT endpoint."""
    h_bits = (ep10.astype(np.uint32) << 6).astype(np.uint16)
    return h_bits.view(np.float16).astype(np.float32)


def encode_bc6h_mode11(pixels, width, height):
    """
    Encode a (height, width, 3) float32 image to BC6H mode 11 blocks.
    Returns bytes of length blocks_wide * blocks_high * 16.

    BC6H mode 11 layout (128 bits per 4x4 block):
      [4:0]   = 5'b11110  (mode identifier)
      [14:5]  = Rw0[9:0]  endpoint 0 red
      [24:15] = Rw1[9:0]  endpoint 1 red
      [34:25] = Gw0[9:0]  endpoint 0 green
      [44:35] = Gw1[9:0]  endpoint 1 green
      [54:45] = Bw0[9:0]  endpoint 0 blue
      [64:55] = Bw1[9:0]  endpoint 1 blue
      [127:65] = 63 index bits (anchor=3 bits, 15 x 4 bits)
    """
    bw = (width + 3) // 4
    bh = (height + 3) // 4
    n_blocks = bw * bh

    # Pad to multiple of 4
    ph, pw = bh * 4, bw * 4
    if ph != height or pw != width:
        padded = np.zeros((ph, pw, 3), dtype=np.float32)
        padded[:height, :width] = pixels[:, :, :3]
        if height < ph:
            padded[height:, :width] = pixels[height-1:height, :, :3]
        if width < pw:
            padded[:, width:] = padded[:, width-1:width]
    else:
        padded = np.ascontiguousarray(pixels[:, :, :3], dtype=np.float32)

    # Reshape to (n_blocks, 16, 3): texels per block
    blocks = padded.reshape(bh, 4, bw, 4, 3).transpose(0, 2, 1, 3, 4)
    flat = blocks.reshape(n_blocks, 16, 3)  # (n_blocks, 16, 3)

    r = flat[:, :, 0]  # (n_blocks, 16)
    g = flat[:, :, 1]
    b = flat[:, :, 2]

    # Quantize min/max endpoints
    ep0_r = quantize_ep10(r.min(axis=1))  # (n_blocks,) uint16
    ep1_r = quantize_ep10(r.max(axis=1))
    ep0_g = quantize_ep10(g.min(axis=1))
    ep1_g = quantize_ep10(g.max(axis=1))
    ep0_b = quantize_ep10(b.min(axis=1))
    ep1_b = quantize_ep10(b.max(axis=1))

    # Build palette for all blocks: (n_blocks, 16)
    def palette(ep0, ep1):
        f0 = reconstruct_ep10(ep0)[:, np.newaxis]   # (n_blocks, 1)
        f1 = reconstruct_ep10(ep1)[:, np.newaxis]
        w = BC6H_WEIGHTS_4[np.newaxis, :]            # (1, 16)
        return (f0 * (64.0 - w) + f1 * w + 32.0) / 64.0

    pal_r = palette(ep0_r, ep1_r)  # (n_blocks, 16)
    pal_g = palette(ep0_g, ep1_g)
    pal_b = palette(ep0_b, ep1_b)

    # Nearest palette entry for each texel: (n_blocks, 16_texels, 16_palette)
    # Use chunked processing to keep peak RAM manageable
    chunk = 8192
    indices = np.empty((n_blocks, 16), dtype=np.uint8)
    for start in range(0, n_blocks, chunk):
        end = min(start + chunk, n_blocks)
        er = (r[start:end, :, np.newaxis] - pal_r[start:end, np.newaxis, :]) ** 2
        eg = (g[start:end, :, np.newaxis] - pal_g[start:end, np.newaxis, :]) ** 2
        eb = (b[start:end, :, np.newaxis] - pal_b[start:end, np.newaxis, :]) ** 2
        indices[start:end] = (er + eg + eb).argmin(axis=2).astype(np.uint8)

    # BC6H anchor constraint: index[0] must be < 8 (3-bit representation)
    needs_swap = indices[:, 0] >= 8
    if needs_swap.any():
        sw = needs_swap
        ep0_r[sw], ep1_r[sw] = ep1_r[sw].copy(), ep0_r[sw].copy()
        ep0_g[sw], ep1_g[sw] = ep1_g[sw].copy(), ep0_g[sw].copy()
        ep0_b[sw], ep1_b[sw] = ep1_b[sw].copy(), ep0_b[sw].copy()
        indices[sw] = 15 - indices[sw]

    # Pack bits into 16-byte blocks
    # We can vectorize the packing partially using uint64 pairs
    # Block layout in two 64-bit words:
    #   word0[63:0]:  mode(5) + Rw0(10) + Rw1(10) + Gw0(10) + Gw1(10) + Bw0(10) + Bw1(10) + idx0_bits2:0(3)
    #                 = 5+10+10+10+10+10+10+3 = 68 bits -> overflows 64... need big int or different split

    # Split as: bytes 0-7 and bytes 8-15 using separate uint64 arrays
    # Byte packing using Python ints in a batch loop is the simplest correct approach.
    # At 524k blocks with simple int ops, this takes ~3-5 seconds.

    output = bytearray(n_blocks * 16)

    ep0_r_np = ep0_r  # all uint16 arrays (n_blocks,)
    ep1_r_np = ep1_r
    ep0_g_np = ep0_g
    ep1_g_np = ep1_g
    ep0_b_np = ep0_b
    ep1_b_np = ep1_b

    for i in range(n_blocks):
        # mode bits [4:0] = 0x1E (= 11110b, mode 11)
        bits = 0x1E
        bits |= int(ep0_r_np[i]) << 5
        bits |= int(ep1_r_np[i]) << 15
        bits |= int(ep0_g_np[i]) << 25
        bits |= int(ep1_g_np[i]) << 35
        bits |= int(ep0_b_np[i]) << 45
        bits |= int(ep1_b_np[i]) << 55
        # Anchor index: 3 bits at [67:65]
        bits |= (int(indices[i, 0]) & 0x7) << 65
        # Remaining 15 indices: 4 bits each at [68+j*4 : 65+j*4+4]
        pos = 68
        for j in range(1, 16):
            bits |= (int(indices[i, j]) & 0xF) << pos
            pos += 4
        output[i*16:(i+1)*16] = bits.to_bytes(16, 'little')

    return bytes(output)


def cook(exr_path, ktx2_path, ktx_exe):
    """Cook EXR -> BC6H KTX2."""
    print(f"Reading EXR: {exr_path}", flush=True)
    exr = OpenEXR.InputFile(exr_path)
    hdr = exr.header()
    dw = hdr['dataWindow']
    w = dw.max.x - dw.min.x + 1
    h = dw.max.y - dw.min.y + 1
    print(f"Dimensions: {w}x{h}", flush=True)

    pt = Imath.PixelType(Imath.PixelType.FLOAT)
    R = np.frombuffer(exr.channel('R', pt), dtype=np.float32).reshape(h, w)
    G = np.frombuffer(exr.channel('G', pt), dtype=np.float32).reshape(h, w)
    B = np.frombuffer(exr.channel('B', pt), dtype=np.float32).reshape(h, w)
    pixels = np.stack([R, G, B], axis=2)

    print(f"Encoding BC6H mode 11 ({(w+3)//4}x{(h+3)//4} = {((w+3)//4)*((h+3)//4)} blocks)...", flush=True)
    t0 = time.perf_counter()
    raw_blocks = encode_bc6h_mode11(pixels, w, h)
    t1 = time.perf_counter()
    print(f"Encoding done in {t1-t0:.1f}s. Block data: {len(raw_blocks)/(1024*1024):.2f} MB", flush=True)

    # Write raw blocks to temp file and create KTX2 via ktx.exe --raw
    with tempfile.NamedTemporaryFile(suffix='.raw', delete=False) as tf:
        tf.write(raw_blocks)
        raw_path = tf.name

    try:
        print(f"Creating KTX2 via ktx.exe --raw...", flush=True)
        cmd = [
            ktx_exe,
            'create',
            '--format', 'BC6H_UFLOAT_BLOCK',
            '--raw',
            '--width', str(w),
            '--height', str(h),
            '--assign-tf', 'linear',
            raw_path,
            ktx2_path,
        ]
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            print(f"ktx.exe error: {result.stderr}", file=sys.stderr)
            sys.exit(1)
        print(f"KTX2 written: {ktx2_path}", flush=True)
        size_mb = os.path.getsize(ktx2_path) / (1024 * 1024)
        print(f"File size: {size_mb:.2f} MB", flush=True)
    finally:
        os.unlink(raw_path)


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <input.exr> <output.ktx2> [ktx.exe]", file=sys.stderr)
        sys.exit(1)

    exr_path = sys.argv[1]
    ktx2_path = sys.argv[2]
    ktx_exe = sys.argv[3] if len(sys.argv) > 3 else r'A:\Games\mc2-tools\ktx\ktx.exe'

    cook(exr_path, ktx2_path, ktx_exe)

    # Run ktx info to verify
    result = subprocess.run([ktx_exe, 'info', ktx2_path], capture_output=True, text=True)
    for line in result.stdout.split('\n')[:20]:
        print(line)


if __name__ == '__main__':
    main()
