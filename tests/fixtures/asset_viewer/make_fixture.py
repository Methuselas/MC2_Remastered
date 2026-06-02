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
