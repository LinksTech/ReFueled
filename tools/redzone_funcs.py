#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# refueled: RZ2 - function start list from PT_GNU_EH_FRAME (.eh_frame_hdr search table) for the
# red-zone work (docs/refueled/audit/REDZONE.md). Stand-alone tool, not wired into prepare.py yet.
"""Write redzone.bin: sorted, unique function starts (image offsets) of an eboot.bin / .prx.

Usage: redzone_funcs.py INPUT -o OUT.bin [--walk] [--table-only] [--summary]

Default: table plus the FDE starts of a full .eh_frame walk (the CTR search table misses functions
behind its last entry, docs/refueled/log/RZ2.md); --table-only uses the search table alone.

redzone.bin (little endian): "RZFL", u16 version=1, u16 reserved=0, u32 count, u32 image_size,
then count * u32 offsets, ascending. Offset = vaddr - lowest PT_LOAD vaddr.
"""
import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'scripts'))
from prepare import parse_self, span  # noqa: E402

PT_LOAD = 1
PT_SCE_LOAD = 0x61000010  # also part of the loaded image (same as scripts/game_check.py)
PT_GNU_EH_FRAME = 0x6474e550
PF_X = 1
MAGIC = b'RZFL'


def read_uleb(data, pos):
    value = shift = 0
    while True:
        b = data[pos]
        pos += 1
        value |= (b & 0x7f) << shift
        shift += 7
        if not b & 0x80:
            return value, pos


def read_sleb(data, pos):
    value = shift = 0
    while True:
        b = data[pos]
        pos += 1
        value |= (b & 0x7f) << shift
        shift += 7
        if not b & 0x80:
            if b & 0x40:
                value -= 1 << shift
            return value, pos


_FIXED = {0x02: '<H', 0x03: '<I', 0x04: '<Q', 0x0a: '<h', 0x0b: '<i', 0x0c: '<q'}


def read_encoded(data, pos, enc, vaddr_of_pos, datarel_base):
    """Decode a DW_EH_PE pointer at data[pos]; returns (value, new_pos). vaddr_of_pos = address of data[pos]."""
    if enc == 0xff:
        raise ValueError("DW_EH_PE_omit pointer where a value is required")
    if enc & 0x80:
        raise ValueError(f"indirect pointer encoding {enc:#x} unsupported")
    fmt = enc & 0x0f
    if fmt == 0x00:
        value, size = struct.unpack_from('<Q', data, pos)[0], 8
    elif fmt == 0x01:
        value, new = read_uleb(data, pos)
        size = new - pos
    elif fmt == 0x09:
        value, new = read_sleb(data, pos)
        size = new - pos
    elif fmt in _FIXED:
        value, size = struct.unpack_from(_FIXED[fmt], data, pos)[0], struct.calcsize(_FIXED[fmt])
    else:
        raise ValueError(f"unsupported pointer format {enc:#x}")
    app = enc & 0x70
    if app == 0x00:
        pass
    elif app == 0x10:
        value += vaddr_of_pos
    elif app == 0x30:
        value += datarel_base
    else:
        raise ValueError(f"unsupported pointer application {enc:#x}")
    return value & 0xffffffffffffffff, pos + size


def load_image(path):
    """Returns (elf, ph, loads, base, image) for a SELF or ELF file."""
    elf, _header, ph, _seg, _missing = parse_self(Path(path).read_bytes())
    loads = [p for p in ph if p['type'] in (PT_LOAD, PT_SCE_LOAD)]
    if not loads:
        raise ValueError("no PT_LOAD segment")
    base = min(p['vaddr'] for p in loads)
    size = max(p['vaddr'] + p['memsz'] for p in loads) - base
    if size > 0xffffffff:
        raise ValueError("image larger than 4 GiB")
    image = bytearray(size)
    for p in loads:
        image[p['vaddr'] - base:p['vaddr'] - base + p['filesz']] = span(elf, p['offset'], p['filesz'])
    return elf, ph, loads, base, image


def parse_hdr(elf, ph):
    """Parse .eh_frame_hdr. Returns (eh_frame_vaddr, hdr_vaddr, entries[(start_vaddr, fde_vaddr)])."""
    hdrs = [p for p in ph if p['type'] == PT_GNU_EH_FRAME]
    if not hdrs:
        raise ValueError("no PT_GNU_EH_FRAME segment")
    h = hdrs[0]
    data = bytes(span(elf, h['offset'], h['filesz']))
    hva = h['vaddr']
    if len(data) < 4 or data[0] != 1:
        raise ValueError("unsupported .eh_frame_hdr version")
    _ver, pe, ce, te = data[:4]
    pos = 4
    eh_frame, pos = read_encoded(data, pos, pe, hva + pos, hva)
    if ce == 0xff or te == 0xff:
        raise ValueError(".eh_frame_hdr has no search table")
    count, pos = read_encoded(data, pos, ce, hva + pos, hva)
    entries = []
    for _ in range(count):
        start, pos = read_encoded(data, pos, te, hva + pos, hva)
        fde, pos = read_encoded(data, pos, te, hva + pos, hva)
        entries.append((start, fde))
    return eh_frame, hva, entries


def walk_eh_frame(image, base, eh_frame_vaddr):
    """Walk .eh_frame CIE/FDE records; returns the list of FDE initial locations (vaddrs)."""
    pos = eh_frame_vaddr - base
    cies = {}
    starts = []
    n = len(image)
    while pos + 4 <= n:
        length = struct.unpack_from('<I', image, pos)[0]
        if length == 0:
            break
        hdr = 4
        if length == 0xffffffff:
            length = struct.unpack_from('<Q', image, pos + 4)[0]
            hdr = 12
        body = pos + hdr
        end = body + length
        if end > n:
            raise ValueError(".eh_frame record exceeds the image")
        cie_id = struct.unpack_from('<I', image, body)[0]
        if cie_id == 0:
            cies[pos] = parse_cie(image, body + 4)
        else:
            cie = cies.get(body - cie_id)
            if cie is None:
                raise ValueError("FDE refers to an unknown CIE")
            loc, _ = read_encoded(image, body + 4, cie, base + body + 4, 0)
            starts.append(loc)
        pos = end
    return starts


def parse_cie(image, pos):
    """Returns the FDE pointer encoding (R augmentation, default absptr)."""
    version = image[pos]
    pos += 1
    aug_end = image.index(0, pos)
    aug = bytes(image[pos:aug_end]).decode('ascii')
    pos = aug_end + 1
    if aug.startswith('eh'):
        pos += 8
    _, pos = read_uleb(image, pos)
    _, pos = read_sleb(image, pos)
    if version == 1:
        pos += 1
    else:
        _, pos = read_uleb(image, pos)
    enc = 0x00
    if aug.startswith('z'):
        _, pos = read_uleb(image, pos)
        for ch in aug[1:]:
            if ch == 'R':
                enc = image[pos]
                pos += 1
            elif ch == 'L':
                pos += 1
            elif ch == 'P':
                penc = image[pos]
                pos += 1
                _, pos = read_encoded(image, pos, penc & 0x7f, 0, 0)
            elif ch in 'SB':
                pass
            else:
                break
    return enc


def function_starts(path, walk=False, union=False):
    """Returns (starts, image, exec_ranges, info).

    starts: sorted unique function starts as image offsets inside executable ranges.
    image: the loaded image (bytearray, offset 0 = lowest PT_LOAD vaddr).
    exec_ranges: sorted [(lo, hi)] image-offset ranges of executable PT_LOADs.
    info: dict with table_entries, dropped (outside exec), duplicates, walk_count (or None),
    walk_extra (FDE starts found by the walk but absent from the table, or None).
    union=True also adds the walked FDE starts that the table lacks (implies walk).
    """
    elf, ph, loads, base, image = load_image(path)
    eh_frame, _hva, entries = parse_hdr(elf, ph)
    exec_ranges = sorted((p['vaddr'] - base, p['vaddr'] - base + p['memsz'])
                         for p in loads if p['flags'] & PF_X)
    seen = set()
    dropped = 0
    walked = walk_eh_frame(image, base, eh_frame) if (walk or union) else None
    table = [s for s, _ in entries]
    extra = sorted(set(walked) - set(table)) if walked is not None else None
    for start in table + (extra if union else []):
        off = start - base
        if any(lo <= off < hi for lo, hi in exec_ranges):
            if off > 0xffffffff:
                raise ValueError("offset exceeds u32")
            seen.add(off)
        else:
            dropped += 1
    starts = sorted(seen)
    n_in = len(table) + (len(extra) if union else 0)
    info = dict(table_entries=len(entries), dropped=dropped, duplicates=n_in - dropped - len(starts),
                walk_count=None if walked is None else len(walked),
                walk_extra=None if extra is None else len(extra))
    return starts, image, exec_ranges, info


def pack(starts, image_size):
    if image_size > 0xffffffff or (starts and starts[-1] > 0xffffffff):
        raise ValueError("image or offset does not fit u32")
    return MAGIC + struct.pack('<HHII', 1, 0, len(starts), image_size) + struct.pack(f'<{len(starts)}I', *starts)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('input')
    ap.add_argument('-o', '--output', required=True)
    ap.add_argument('--walk', action='store_true', help='also walk .eh_frame and compare the FDE count')
    ap.add_argument('--table-only', action='store_true', help='only the .eh_frame_hdr search table (default: table plus .eh_frame walk)')
    ap.add_argument('--summary', action='store_true', help='print numbers only')
    args = ap.parse_args(argv)
    union = not args.table_only
    starts, image, _ranges, info = function_starts(args.input, args.walk, union)
    Path(args.output).write_bytes(pack(starts, len(image)))
    print(f"functions: {len(starts)}")
    if starts:
        print(f"offset min/max: {starts[0]:#x} / {starts[-1]:#x}")
    print(f"table entries: {info['table_entries']}, dropped (outside exec): {info['dropped']}, "
          f"duplicates: {info['duplicates']}, image size: {len(image):#x}")
    if args.walk or union:
        ok = info['walk_count'] == info['table_entries']
        print(f".eh_frame walk: {info['walk_count']} FDEs, table {info['table_entries']}, "
              f"walk-only {info['walk_extra']}: {'match' if ok else 'MISMATCH'}")
        if not ok:
            return 0 if union else 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
