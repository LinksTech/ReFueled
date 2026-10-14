# SPDX-License-Identifier: GPL-2.0-or-later
# refueled: RZ2 - builds a synthetic x86-64 ELF64 with PT_LOAD r-x, PT_LOAD rw- and PT_GNU_EH_FRAME for the
# red-zone tool tests (no game data).
import struct

BASE = 0x400000
TEXT_END = 0x400        # r-x segment: file/image offsets 0 .. 0x400 (headers + code)
DATA_OFF = 0x800        # rw- segment: file offset 0x800, vaddr BASE + 0x800 (holds .eh_frame_hdr/.eh_frame)
DATA_VADDR = BASE + 0x800
HDR_OFF = 0x800         # .eh_frame_hdr
EH_OFF = 0xa00          # .eh_frame
CODE_OFF = 0x200


def phdr(p_type, flags, off, vaddr, filesz, memsz):
    return struct.pack('<IIQQQQQQ', p_type, flags, off, vaddr, vaddr, filesz, memsz, 0x1000)


def eh_frame(starts_vaddr):
    """CIE (zR, pcrel sdata4) + one FDE per start; returns (bytes, [fde offsets in the section])."""
    cie_body = struct.pack('<I', 0) + bytes([1]) + b'zR' + bytes(1) + bytes([1, 0x78, 16, 1, 0x1b])
    cie_body += bytes(1) * (-len(cie_body) % 4)
    out = struct.pack('<I', len(cie_body)) + cie_body
    fde_offs = []
    for s in starts_vaddr:
        pos = len(out)
        fde_offs.append(pos)
        field_vaddr = BASE + EH_OFF + pos + 8
        body = struct.pack('<I', pos + 4) + struct.pack('<ii', s - field_vaddr, 0x10) + bytes([0]) + b'\0' * 3
        out += struct.pack('<I', len(body)) + body
    out += struct.pack('<I', 0)
    return out, fde_offs


def build(code, starts_vaddr, table_enc=0x3b, count_enc=0x03, ptr_enc=0x1b, table_starts=None):
    """code: bytes placed at CODE_OFF. starts_vaddr: FDE initial locations written into .eh_frame.
    table_starts: starts listed in the hdr table (default = starts_vaddr)."""
    table_starts = starts_vaddr if table_starts is None else table_starts
    eh, fde_offs = eh_frame(starts_vaddr)
    fde_of = dict(zip(starts_vaddr, fde_offs))
    hva = BASE + HDR_OFF
    hdr = bytes([1, ptr_enc, count_enc, table_enc])
    eh_va = BASE + EH_OFF
    if ptr_enc == 0x1b:
        hdr += struct.pack('<i', eh_va - (hva + 4))
    else:  # 0x03 udata4 absptr
        hdr += struct.pack('<I', eh_va)
    hdr += struct.pack('<I', len(table_starts))
    for s in sorted(table_starts):
        f = BASE + EH_OFF + fde_of.get(s, 0)
        if table_enc == 0x3b:      # datarel | sdata4
            hdr += struct.pack('<ii', s - hva, f - hva)
        elif table_enc == 0x04:    # udata8 absptr
            hdr += struct.pack('<QQ', s, f)
        elif table_enc == 0x1b:    # pcrel | sdata4
            p = hva + len(hdr)
            hdr += struct.pack('<ii', s - p, f - (p + 4))
        else:
            raise ValueError(table_enc)
    n_ph = 3
    ehdr = b'\x7fELF\x02\x01\x01' + b'\0' * 9 + struct.pack('<HHIQQQIHHHHHH', 2, 62, 1, BASE + CODE_OFF, 64, 0, 0,
                                                              64, 56, n_ph, 0, 0, 0)
    img = bytearray(DATA_OFF + 0x400)
    img[:len(ehdr)] = ehdr
    ph = (phdr(1, 5, 0, BASE, TEXT_END, TEXT_END) + phdr(1, 6, DATA_OFF, DATA_VADDR, 0x400, 0x400)
          + phdr(0x6474e550, 4, HDR_OFF, hva, len(hdr), len(hdr)))
    img[64:64 + len(ph)] = ph
    img[HDR_OFF:HDR_OFF + len(hdr)] = hdr
    img[EH_OFF:EH_OFF + len(eh)] = eh
    img[CODE_OFF:CODE_OFF + len(code)] = code
    return bytes(img)


def build_without_eh_frame():
    img = bytearray(build(b'\xc3', [BASE + CODE_OFF]))
    struct.pack_into('<I', img, 64 + 56 * 2, 0x6474e551)  # not PT_GNU_EH_FRAME any more
    return bytes(img)
