import struct, zlib, os
os.makedirs(os.path.dirname(__file__), exist_ok=True)
w, h = 4, 2
rows = bytearray()
for y in range(h):
    rows.append(0)  # filter byte
    for x in range(w):
        rows += bytes([(x*60) % 256, (y*120) % 256, 128, 255])
def chunk(tag, data):
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)
png = b"\x89PNG\r\n\x1a\n"
png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
png += chunk(b"IDAT", zlib.compress(bytes(rows)))
png += chunk(b"IEND", b"")
open(os.path.join(os.path.dirname(__file__), "test_rgba.png"), "wb").write(png)
print("wrote test_rgba.png", w, h)

# ---- KTX2 fixtures (hand-written; no encoder dependency) ----
KTX2_MAGIC = bytes([0xab,0x4b,0x54,0x58,0x20,0x32,0x30,0xbb,0x0d,0x0a,0x1a,0x0a])

def ktx2(vk_format, w, h, levels, super_scheme=0):
    # levels: list of bytes, mip0 first. Header layout mirrors RenderCore/KtxLoader.cpp:
    #   magic(12) + 9*u32(36) + dfd/kvd(4*u32=16) + sgd(2*u64=16) = 80, then level index.
    type_size = 1
    hdr = struct.pack("<9I", vk_format, type_size, w, h, 0, 0, 1, len(levels), super_scheme)
    desc = struct.pack("<4I", 0, 0, 0, 0) + struct.pack("<2Q", 0, 0)
    data_start = 80 + 24 * len(levels)
    entries = b""; data = b""; off = data_start
    for lv in levels:
        entries += struct.pack("<3Q", off, len(lv), len(lv))
        data += lv; off += len(lv)
    return KTX2_MAGIC + hdr + desc + entries + data

def write(name, blob):
    open(os.path.join(os.path.dirname(__file__), name), "wb").write(blob)
    print("wrote", name, len(blob), "bytes")

# RGBA8 unorm 4x2, single mip (32 bytes payload).
rgba = bytes([(i*8) % 256 for i in range(4*2*4)])
write("tex_rgba8.ktx2", ktx2(37, 4, 2, [rgba]))                     # vkFormat 37 = R8G8B8A8_UNORM

# BC7 sRGB 4x2 -> 1 block (16 bytes). Block contents are not color-validated by
# the smoke (it asserts upload success + isCompressed + clean glGetError only).
bc7_block = bytes([0x40] + [0x00]*15)                               # mode-6-ish; any 16 bytes upload fine
write("tex_bc7.ktx2", ktx2(146, 4, 2, [bc7_block]))                # vkFormat 146 = BC7_SRGB_BLOCK

# Supercompressed (scheme=1) -> classify as Basis/unsupported.
write("tex_super.ktx2", ktx2(37, 4, 2, [rgba], super_scheme=1))

# Unknown vkFormat -> classify as unsupported format.
write("tex_badfmt.ktx2", ktx2(999, 4, 2, [rgba]))

# ---- resolution-tier fixtures (for --smoke-tiers) ----
import shutil
_here = os.path.dirname(__file__)
_src = os.path.join(_here, "tex_rgba8.ktx2")
for _tier in ("128", "256"):
    _d = os.path.join(_here, "tiers", _tier)
    os.makedirs(_d, exist_ok=True)
    shutil.copyfile(_src, os.path.join(_d, "sample.ktx2"))
shutil.copyfile(_src, os.path.join(_here, "tiers", "128", "only128.ktx2"))
print("wrote tiers/{128,256}/sample.ktx2 + tiers/128/only128.ktx2")

# ---- PBR material slot fixtures (for --smoke-texload) ----
def write_png_rgba(filename, w, h, pixels):
    """Write a minimal RGBA8 PNG. pixels: flat list of (r,g,b,a) tuples."""
    rows = bytearray()
    for y in range(h):
        rows.append(0)  # filter byte
        for x in range(w):
            r, g, b, a = pixels[y * w + x]
            rows += bytes([r, g, b, a])
    data = png + b""  # reset
    _png = b"\x89PNG\r\n\x1a\n"
    _png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    _png += chunk(b"IDAT", zlib.compress(bytes(rows)))
    _png += chunk(b"IEND", b"")
    open(os.path.join(_here, filename), "wb").write(_png)
    print("wrote", filename, w, h)

# mat_base.png: 4x4 sRGB-ish color (warm orange tones)
_here = os.path.dirname(__file__)
_base_pixels = [
    (200, 100,  50, 255), (210, 110,  55, 255), (190,  95,  45, 255), (205, 105,  52, 255),
    (195,  98,  48, 255), (208, 108,  53, 255), (202, 102,  50, 255), (198, 100,  49, 255),
    (207, 107,  54, 255), (193,  97,  47, 255), (203, 103,  51, 255), (199, 101,  49, 255),
    (204, 104,  52, 255), (196,  99,  48, 255), (206, 106,  53, 255), (201, 101,  50, 255),
]
write_png_rgba("mat_base.png", 4, 4, _base_pixels)

# mat_orm.png: 4x4 linear gray (AO=1, Roughness=0.5, Metallic=0 -> ~255, 128, 0)
_orm_pixels = [
    (255, 128, 0, 255), (255, 128, 0, 255), (255, 128, 0, 255), (255, 128, 0, 255),
    (255, 128, 0, 255), (255, 128, 0, 255), (255, 128, 0, 255), (255, 128, 0, 255),
    (255, 128, 0, 255), (255, 128, 0, 255), (255, 128, 0, 255), (255, 128, 0, 255),
    (255, 128, 0, 255), (255, 128, 0, 255), (255, 128, 0, 255), (255, 128, 0, 255),
]
write_png_rgba("mat_orm.png", 4, 4, _orm_pixels)

# ---- tangent validation fixtures (for --smoke-tangent) ----
# nrm_flat.png: 128x128 flat tangent-space normal (+Z, encodes as (128,128,255)).
_flat_pixels = [(128, 128, 255, 255)] * (128 * 128)
write_png_rgba("nrm_flat.png", 128, 128, _flat_pixels)

# nrm_tilt_u.png: 128x128 normal tilted toward +U (192,128,255 -> slight +X tilt).
_tilt_pixels = [(192, 128, 255, 255)] * (128 * 128)
write_png_rgba("nrm_tilt_u.png", 128, 128, _tilt_pixels)
