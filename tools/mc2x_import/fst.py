"""
tools/mc2x_import/fst.py -- FST/pak read helpers, ported verbatim from the proven
legacy scripts A:/tmp/cveg_min_files.py (FST/pak helpers, appearance parsing) and
A:/tmp/fst_extract.py (lzw_decomp). Two edits only vs the originals:
  (a) lzw_decomp is inlined here (legacy imported it from fst_extract via sys.path);
  (b) module-level hardcoded install-path constants are dropped (paths now passed in).
Everything else is byte-identical proven code.
"""
import struct, os, re, sys, shutil, zlib, argparse


# ---- Classic variable-width LZW decompressor (from A:/tmp/fst_extract.py) ----
# Direct port of LZDecompClassicLZW_ from lzdecomp.cpp (LINUX_BUILD path).
# Bit stream: LSB-first across byte boundaries.

HASH_CLEAR = 256
HASH_EOF   = 257
HASH_FREE  = 258
MAX_BITS   = 12
BASE_BITS  = 9


def lzw_decomp(src: bytes, dest_max: int) -> bytes:
    """Decompress Microsoft MC2 LZW-packed data.

    Returns the decompressed bytes, or raises ValueError on corrupt input.
    """
    parent = [0] * 4096   # parentTbl: 16-bit parent code
    suffix = [0] * 4096   # suffixTbl: 8-bit terminal byte

    src_len = len(src)
    src_pos  = 0
    bit_buf  = 0
    bits_in  = 0
    dest     = bytearray()

    def read_code(nbits):
        nonlocal src_pos, bit_buf, bits_in
        while bits_in < nbits:
            if src_pos >= src_len:
                return -1
            bit_buf |= src[src_pos] << bits_in
            bits_in += 8
            src_pos += 1
        code = bit_buf & ((1 << nbits) - 1)
        bit_buf >>= nbits
        bits_in -= nbits
        return code

    code_width = BASE_BITS
    max_code   = 1 << code_width   # 512 initially
    free_idx   = HASH_FREE          # 258
    prev_code  = -1
    prev_first = 0

    stk = []

    while True:
        code = read_code(code_width)
        if code < 0 or code == HASH_EOF:
            break
        if code == HASH_CLEAR:
            code_width = BASE_BITS
            max_code   = 1 << code_width
            free_idx   = HASH_FREE
            prev_code  = -1
            stk.clear()
            continue

        # Expand code to byte sequence onto stack
        c = code
        stk.clear()

        if c < free_idx:
            # Code already in dictionary
            while c >= HASH_FREE:
                stk.append(suffix[c])
                c = parent[c]
            first_byte = c & 0xFF
            stk.append(first_byte)
        elif c == free_idx and prev_code >= 0:
            # KwKwK special case: code == freeIdx
            stk.append(prev_first)
            pc = prev_code
            while pc >= HASH_FREE:
                stk.append(suffix[pc])
                pc = parent[pc]
            first_byte = pc & 0xFF
            stk.append(first_byte)
        else:
            raise ValueError(
                "LZW: invalid code {} (free_idx={}, prev_code={})".format(
                    code, free_idx, prev_code))

        # Emit in reverse (stack is LIFO -> actual order)
        while stk:
            if len(dest) >= dest_max:
                return bytes(dest)
            dest.append(stk.pop())

        # Add new dictionary entry
        if prev_code >= 0 and free_idx < (1 << MAX_BITS):
            parent[free_idx] = prev_code
            suffix[free_idx] = first_byte
            free_idx += 1

            # Grow code width when dictionary is full
            if free_idx >= max_code and code_width < MAX_BITS:
                code_width += 1
                max_code = 1 << code_width

        prev_code  = code
        prev_first = first_byte

    return bytes(dest)


# -- helpers (from A:/tmp/cveg_min_files.py) ----------------------------------

def read_pak_packet(pak_data, idx):
    magic, count = struct.unpack_from('<II', pak_data, 0)
    assert magic == 0xFEEDFACE, f"bad pak magic {magic:#010x}"
    entry = struct.unpack_from('<I', pak_data, 8 + idx*4)[0]
    ptype  = (entry >> 29) & 0x7
    offset = entry & 0x1FFFFFFF
    nxt    = struct.unpack_from('<I', pak_data, 8 + (idx+1)*4)[0] & 0x1FFFFFFF
    return pak_data[offset:nxt].decode('ascii', errors='replace')

def pak_count(pak_data):
    return struct.unpack_from('<I', pak_data, 4)[0]

def fst_entries(fst_path):
    """Return list of (offset, comp_sz, real_sz, name) for a FST."""
    with open(fst_path, 'rb') as f:
        data = f.read()
    magic, count = struct.unpack_from('<II', data, 0)
    is_lz = (magic == 0xFADDECAF)   # else 0xCADDECAF = zlib
    entries = []
    for i in range(count):
        base = 8 + i*266
        off, comp, real, h = struct.unpack_from('<IIII', data, base)
        name_raw = data[base+16:base+266]
        null = name_raw.find(b'\x00')
        name = name_raw[:null].decode('latin-1','replace') if null>=0 else ''
        entries.append((off, comp, real, name, is_lz, data))
    return entries

def decompress_entry(off, comp, real, is_lz, data):
    raw = data[off:off+comp]
    if comp == real:
        return raw   # stored uncompressed
    if is_lz:
        return lzw_decomp(raw, real)
    else:
        return zlib.decompress(raw)

def extract_fst_file(fst_path, target_rel, out_path):
    """Extract a single file from an FST into out_path."""
    target_lower = target_rel.lower().replace('\\','/')
    for off, comp, real, name, is_lz, data in fst_entries(fst_path):
        if name.lower().replace('\\','/') == target_lower:
            content = decompress_entry(off, comp, real, is_lz, data)
            os.makedirs(os.path.dirname(out_path), exist_ok=True)
            with open(out_path, 'wb') as f:
                f.write(content)
            return True
    return False

def fst_has(fst_path, target_rel):
    target_lower = target_rel.lower().replace('\\','/')
    for off, comp, real, name, is_lz, data in fst_entries(fst_path):
        if name.lower().replace('\\','/') == target_lower:
            return True
    return False

# -- Step 1: Find all CVE-G-exclusive mech/vehicle types from object2.pak -----

def get_cveg_types(pak_path):
    """Return list of dicts: {obj_num, obj_type, appearance, profile}"""
    with open(pak_path, 'rb') as f:
        pak_data = f.read()
    count = pak_count(pak_data)
    types = []
    for i in range(count):
        text = read_pak_packet(pak_data, i)
        obj_type = appearance = profile = None
        for line in text.split('\n'):
            l = line.strip()
            if 'ObjectTypeNum' in l:
                m = re.search(r'=\s*(\d+)', l)
                if m: obj_type = int(m.group(1))
            if 'AppearanceName' in l:
                m = re.search(r'=\s*"([^"]+)"', l)
                if m: appearance = m.group(1).strip()
            if 'ProfileName' in l:
                m = re.search(r'=\s*"([^"]+)"', l)
                if m: profile = m.group(1).strip()
        if appearance:
            types.append({'idx': i, 'type': obj_type, 'appearance': appearance, 'profile': profile or appearance})
    return types

# -- Step 2: For each mech appearance, compute required tgl/ files -------------

MECH_ANIM_NAMES = [
    'StandToPark','ParkToStand','','STtoWK','Walk','StandToPark',
    'WKtoRN','Run','RNToWK','Walk','WKtoST','LimpLeft','LimpRight',
    'Idle','FallBackward','FallForward','HitFront','HitBack','HitLeft',
    'HitRight','Jump','GetupBack','GetupFront','FallForward','FallBackward',
    'FallBackwardDam','FallForwardDam',
]

def mech_tgl_files(appearance):
    """All data/tgl/ files needed for a mech appearance."""
    base = appearance.lower()
    needed = set()
    # Main ini
    needed.add(f'data/tgl/{base}.ini')
    # Left/right arm ini (may be 151-154 bytes stub with just bounds -- still needed)
    needed.add(f'data/tgl/{base}leftarm.ini')
    needed.add(f'data/tgl/{base}rightarm.ini')
    # Main AGL (even if empty header)
    needed.add(f'data/tgl/{base}.agl')
    # Animations
    for anim in MECH_ANIM_NAMES:
        if anim:
            needed.add(f'data/tgl/{base}{anim.lower()}.agl')
    # TGL shapes (main + LODs + shadow + arms + damage)
    for suffix in ['', 'l1', 'x', 'leftarm', 'rightarm', 'fallforwarddam', 'fallbackwarddam']:
        needed.add(f'data/tgl/{base}{suffix}.tgl')
    return needed

def vehicle_tgl_files(appearance):
    """Vehicles need fewer files -- just ini and tgl."""
    base = appearance.lower()
    return {f'data/tgl/{base}.ini', f'data/tgl/{base}.tgl'}

# -- Step 3: CSV files for mech stats -----------------------------------------

def mech_csv_files(profile):
    return {f'data/objects/{profile.lower()}.csv'}
