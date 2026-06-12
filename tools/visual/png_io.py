"""Self-contained PNG decode/encode for the MC2 visual comparator.

Python 3 stdlib only (zlib, struct). NO numpy / PIL. Handles 8-bit RGB and
RGBA PNGs (color types 2 and 6), with any DEFLATE compression level
(zlib.decompress copes with the stored-DEFLATE goldens and normal zlib alike).

This module exists so the comparator and triptych writer share one decoder and
one (minimal) writer instead of pulling in an imaging dependency the smoke /
gate environment does not have.
"""

import struct
import zlib

PNG_SIG = b"\x89PNG\r\n\x1a\n"


class PngError(Exception):
    pass


def _paeth(a, b, c):
    p = a + b - c
    pa = abs(p - a)
    pb = abs(p - b)
    pc = abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def decode_png(data):
    """Decode a PNG byte string.

    Returns (width, height, channels, pixels) where pixels is a flat
    bytearray of length width*height*channels (8-bit samples).
    channels is 3 (RGB) or 4 (RGBA).
    """
    if data[:8] != PNG_SIG:
        raise PngError("not a PNG (bad signature)")
    off = 8
    width = height = bit_depth = color_type = None
    idat = bytearray()
    while off + 8 <= len(data):
        (length,) = struct.unpack(">I", data[off : off + 4])
        ctype = data[off + 4 : off + 8]
        body = data[off + 8 : off + 8 + length]
        off += 12 + length  # skip length, type, body, CRC
        if ctype == b"IHDR":
            width, height, bit_depth, color_type = struct.unpack(">IIBB", body[:10])
        elif ctype == b"IDAT":
            idat += body
        elif ctype == b"IEND":
            break
    if width is None:
        raise PngError("no IHDR")
    if bit_depth != 8:
        raise PngError("unsupported bit depth %r (need 8)" % bit_depth)
    if color_type == 2:
        channels = 3
    elif color_type == 6:
        channels = 4
    else:
        raise PngError("unsupported color type %r (need 2 RGB or 6 RGBA)" % color_type)

    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    expected = (stride + 1) * height
    if len(raw) != expected:
        raise PngError("decompressed size %d != expected %d" % (len(raw), expected))

    out = bytearray(stride * height)
    prev = bytearray(stride)
    pos = 0
    for y in range(height):
        ft = raw[pos]
        pos += 1
        line = bytearray(raw[pos : pos + stride])
        pos += stride
        if ft == 0:
            pass
        elif ft == 1:  # Sub
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif ft == 2:  # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ft == 3:  # Average
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ft == 4:  # Paeth
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                c = prev[i - channels] if i >= channels else 0
                line[i] = (line[i] + _paeth(a, prev[i], c)) & 0xFF
        else:
            raise PngError("unknown filter type %d" % ft)
        out[y * stride : (y + 1) * stride] = line
        prev = line
    return width, height, channels, out


def encode_png(width, height, channels, pixels):
    """Encode an 8-bit RGB(3) or RGBA(4) image to a PNG byte string.

    Uses filter type 0 (none) on every scanline; zlib does the compression.
    Output is a standard zlib-compressed PNG (browsers / any decoder read it).
    """
    if channels not in (3, 4):
        raise PngError("channels must be 3 or 4")
    color_type = 2 if channels == 3 else 6
    stride = width * channels
    raw = bytearray()
    for y in range(height):
        raw.append(0)  # filter: none
        raw += pixels[y * stride : (y + 1) * stride]
    comp = zlib.compress(bytes(raw), 9)

    def chunk(tag, body):
        return (
            struct.pack(">I", len(body))
            + tag
            + body
            + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)
        )

    ihdr = struct.pack(">IIBBBBB", width, height, 8, color_type, 0, 0, 0)
    return PNG_SIG + chunk(b"IHDR", ihdr) + chunk(b"IDAT", comp) + chunk(b"IEND", b"")
