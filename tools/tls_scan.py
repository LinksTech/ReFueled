#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# refueled: static scan of every fs:/gs: segment-override access in the game's executable code,
# compared against what the port's TLS rewrite covers (link_libc.py / link_modules.py FS_LOAD and
# pf_tls_patch_image: exactly the 9-byte `mov rax, fs:[0]`).
#
# Method: parse eboot.bin / PRX with scripts/prepare.py, build an in-memory ELF holding only the
# executable PT_LOAD segments, pipe it into llvm-objdump (stdin, nothing written to disk), read the
# listing from a pipe and keep only normalized instruction forms. The tool prints counts, forms,
# displacements, segment indices and image-relative offsets only: never game bytes or disassembly.
#
# Usage: python tools/tls_scan.py --game C:\CUSA13795 [--json out.json] [--objdump PATH]
import argparse
import collections
import json
import re
import struct
import subprocess
import sys
import threading
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'scripts'))
from prepare import parse_self, sfo  # noqa: E402

DEFAULT_MODULES = ('libc.prx', 'libSceFios2.prx', 'libSceNpToolkit2.prx')
OBJDUMP_CANDIDATES = (r'C:\msys64\clang64\bin\llvm-objdump.exe', 'llvm-objdump')
BIG = 1 << 40  # displacement magnitude above 0xffff is reported as 'large' (not a value)
FS_LOAD_LEN = 9  # 64 48 8b 04 25 00 00 00 00

REG64 = {'rax', 'rbx', 'rcx', 'rdx', 'rsi', 'rdi', 'rbp', 'rsp', 'rip'} | {f'r{i}' for i in range(8, 16)}
REG32 = {'eax', 'ebx', 'ecx', 'edx', 'esi', 'edi', 'ebp', 'esp'} | {f'r{i}d' for i in range(8, 16)}
REG16 = {'ax', 'bx', 'cx', 'dx', 'si', 'di', 'bp', 'sp'} | {f'r{i}w' for i in range(8, 16)}
REG8 = {'al', 'bl', 'cl', 'dl', 'ah', 'bh', 'ch', 'dh', 'sil', 'dil', 'bpl', 'spl'} | {f'r{i}b' for i in range(8, 16)}
SEG_RE = re.compile(r'(?:(\w+) ptr )?\b(fs|gs):(.*)$')
SIZES = {'byte': '8', 'word': '16', 'dword': '32', 'qword': '64', 'xmmword': '128', 'ymmword': '256',
         'zmmword': '512', 'tbyte': '80', 'fword': '48', 'oword': '128'}


def fmt_disp(d):
    return 'large' if d == BIG else hex(d)


def reg_class(tok):
    if tok in REG64: return 'r64'
    if tok in REG32: return 'r32'
    if tok in REG16: return 'r16'
    if tok in REG8: return 'r8'
    if re.fullmatch(r'[xyz]mm\d+', tok): return tok[:3]
    if re.fullmatch(r'(st\(\d\)|st|mm\d|k\d|[cdefgs]s|cr\d+|dr\d+)', tok): return tok.rstrip('0123456789()') or tok
    return None


def split_ops(text):
    out, depth, cur = [], 0, ''
    for ch in text:
        if ch == '[': depth += 1
        elif ch == ']': depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip()); cur = ''
        else:
            cur += ch
    if cur.strip(): out.append(cur.strip())
    return out


def seg_operand(op):
    """Normalize `qword ptr fs:[rbx + 0x8]` -> (segment, 'm64 fs:[base+disp]', disp)."""
    m = SEG_RE.search(op)
    if not m: return None
    size, seg, rest = m.groups()
    rest = rest.strip()
    if rest.startswith('[') and rest.endswith(']'): rest = rest[1:-1]
    regs, disp, sign = [], 0, 1
    for tok in re.findall(r'[+-]|[^+\-\s]+', rest):
        if tok == '+': sign = 1
        elif tok == '-': sign = -1
        else:
            base = tok.split('*')[0]
            if reg_class(base) or base in ('rip',):
                regs.append(('idx' if '*' in tok else 'base') + ('*' + tok.split('*')[1] if '*' in tok else ''))
                sign = 1
            else:
                try:
                    disp += sign * int(tok, 0)
                except ValueError:
                    regs.append('?')
                sign = 1
    parts = [r for r in regs]
    if disp or not parts: parts.append('disp')
    mem = 'm' + SIZES.get(size, '?') if size else 'm'
    return seg, f'{mem} {seg}:[{"+".join(parts)}]', disp


def normalize(line_text):
    """line_text: 'mnemonic operands'. Return (segment, form, disp) or None."""
    pieces = line_text.split(None, 1)
    mn = pieces[0]
    ops = split_ops(pieces[1]) if len(pieces) > 1 else []
    seg = None
    norm = []
    disp = 0
    for op in ops:
        s = seg_operand(op)
        if s:
            seg, text, disp = s
            norm.append(text)
        elif reg_class(op):
            norm.append(reg_class(op))
        elif '[' in op:
            sz = re.match(r'(\w+) ptr', op)
            norm.append('m' + (SIZES.get(sz.group(1), '?') if sz else ''))
        elif re.fullmatch(r'-?(0x[0-9a-f]+|\d+)', op):
            norm.append('imm')
        else:
            norm.append('?')
    if seg is None:
        # bare prefix printed as its own token (e.g. `fs` before another mnemonic)
        for p in (mn, *ops):
            if p in ('fs', 'gs'):
                return p, f'{p}-prefix without parsed memory operand', 0, ''
        return None
    return seg, f'{mn} {", ".join(norm)}', disp, (ops[0] if ops else '')


def build_elf(chunks):
    """chunks: list of (vaddr, bytes). Minimal ET_EXEC with one executable section each."""
    names = b'\0'
    name_off = []
    for i in range(len(chunks)):
        name_off.append(len(names)); names += f'.t{i}'.encode() + b'\0'
    shstr_off = len(names); names += b'.shstrtab\0'
    body = b''.join(c for _, c in chunks)
    data_off = 64
    names_off = data_off + len(body)
    shoff = (names_off + len(names) + 7) & ~7
    sec = b'\0' * 64
    pos = data_off
    for (va, c), no in zip(chunks, name_off):
        sec += struct.pack('<IIQQQQIIQQ', no, 1, 6, va, pos, len(c), 0, 0, 16, 0)
        pos += len(c)
    sec += struct.pack('<IIQQQQIIQQ', shstr_off, 3, 0, 0, names_off, len(names), 0, 0, 1, 0)
    eh = b'\x7fELF\x02\x01\x01' + b'\0' * 9 + struct.pack('<HHIQQQIHHHHHH', 2, 62, 1, 0, 0, shoff, 0, 64, 0, 0, 64,
                                                       len(chunks) + 2, len(chunks) + 1)
    return eh + body + names + b'\0' * (shoff - names_off - len(names)) + sec


LINE_RE = re.compile(r'^\s*([0-9a-f]+):\s*(.*)$')


def scan_image(path, objdump):
    data = Path(path).read_bytes()
    elf, _hdr, ph, _s, _m = parse_self(data)
    del data
    segs = [(i, p) for i, p in enumerate(ph) if p['type'] == 1 and p['flags'] & 1 and p['filesz']]
    chunks = [(p['vaddr'], bytes(elf[p['offset']:p['offset'] + p['filesz']])) for _, p in segs]
    ends = sorted((p['vaddr'] + p['filesz']) for _, p in segs)
    # plausibility: the raw 9-byte pattern, as link_libc.py counts it
    pat = bytes.fromhex('64488b042500000000')
    raw_count = 0
    for _, c in chunks:
        at = c.find(pat)
        while at >= 0:
            raw_count += 1
            at = c.find(pat, at + len(pat))
    blob = build_elf(chunks)
    fde, fde_note = (lambda: fde_ranges(elf, ph))() if True else ([], '')
    del elf
    proc = subprocess.Popen([objdump, '-d', '-M', 'intel', '--no-show-raw-insn', '-'],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    def feed():
        try:
            proc.stdin.write(blob)
        finally:
            proc.stdin.close()
    threading.Thread(target=feed, daemon=True).start()
    err_chunks = []
    threading.Thread(target=lambda: err_chunks.append(proc.stderr.read()), daemon=True).start()
    hits = []   # dicts
    pend = None
    insns = bad = 0
    seg_of = lambda a: next((i for i, p in segs if p['vaddr'] <= a < p['vaddr'] + p['filesz']), -1)
    for raw in proc.stdout:
        line = raw.decode('latin-1')
        m = LINE_RE.match(line)
        if not m:
            continue
        addr = int(m.group(1), 16)
        text = m.group(2).strip()
        if pend is not None:
            pend['len'] = addr - pend['addr']
            hits.append(pend); pend = None
        insns += 1
        if '(bad)' in text:
            bad += 1
        if 'fs' in text or 'gs' in text:
            if not re.search(r'\b[fg]s\b', text):
                continue
            r = normalize(text)
            if r:
                pend = dict(addr=addr, seg=r[0], form=r[1], disp=(r[2] if abs(r[2]) <= 0xffff else BIG), dst=r[3], segidx=seg_of(addr))
    if pend is not None:
        pend['len'] = 0
        hits.append(pend)
    proc.wait()
    for h in hits:
        h['covered'] = (h['seg'] == 'fs' and h['form'] == 'mov r64, m64 fs:[disp]' and h['disp'] == 0
                        and h['len'] == FS_LOAD_LEN and h['dst'] == 'rax')
    return dict(fde=fde, fde_note=fde_note, fde_starts=[x[0] for x in fde], insns=insns, bad=bad, raw_fs_load=raw_count, hits=hits,
                exec_segments=[dict(index=i, vaddr=p['vaddr'], size=p['filesz']) for i, p in segs],
                objdump_stderr_lines=len(b''.join(err_chunks).splitlines()))



def rate(seg, form, covered, in_fde):
    if covered:
        return 'ja (Port: fs->gs bzw. TLS-Slot)'
    if not in_fde:
        return 'nein, aber ausserhalb aller FDE-Funktionsbereiche: vermutlich Daten/Desync, kein Code'
    if seg == 'gs':
        return 'nein: Original-GS im Code; Linux: Kollision mit Gast-TCB-GS, Windows: GS gehoert dem TEB'
    return 'nein: Linux: FS gehoert glibc (falsche Daten), Windows: FS-Basis 0 (Zugriffsfehler)'


def uleb(b, p):
    r = s = 0
    while True:
        x = b[p]; p += 1
        r |= (x & 0x7f) << s; s += 7
        if not x & 0x80: return r, p


def read_va(elf, ph, va, n):
    for p in ph:
        if p['type'] == 1 and p['vaddr'] <= va and va + n <= p['vaddr'] + p['filesz']:
            o = p['offset'] + va - p['vaddr']
            return elf[o:o + n]
    raise ValueError('va not file-backed')


def fde_ranges(elf, ph):
    """Function ranges from PT_GNU_EH_FRAME's binary search table (+ FDE pc_range). Returns (ranges, note)."""
    eh = next((p for p in ph if p['type'] == 0x6474e550), None)
    if not eh: return [], 'kein PT_GNU_EH_FRAME'
    hv = eh['vaddr']
    hdr = read_va(elf, ph, hv, 4)
    if hdr[0] != 1: return [], 'eh_frame_hdr version'
    pe, ce, te = hdr[1], hdr[2], hdr[3]
    if te != 0x3b or ce != 0x03: return [], f'eh_frame_hdr Kodierung {pe:#x}/{ce:#x}/{te:#x} nicht unterstuetzt'
    p = hv + 4
    # eh_frame_ptr (skip, size by encoding low nibble)
    p += {3: 4, 0xb: 4, 4: 8, 0xc: 8}[pe & 0xf]
    count, = struct.unpack('<I', bytes(read_va(elf, ph, p, 4))); p += 4
    table = bytes(read_va(elf, ph, p, count * 8))
    ranges = []
    cie_cache = {}
    for i in range(count):
        initial, fde_rel = struct.unpack_from('<ii', table, i * 8)
        fde = hv + fde_rel
        head = bytes(read_va(elf, ph, fde, 8))
        length, cie_ptr = struct.unpack('<II', head)
        if length in (0, 0xffffffff): continue
        cie = fde + 4 - cie_ptr
        if cie not in cie_cache:
            raw = bytes(read_va(elf, ph, cie, 64))
            q = 9
            aug_end = raw.index(0, q); aug = raw[q:aug_end].decode('latin-1'); q = aug_end + 1
            _, q = uleb(raw, q); _, q = uleb(raw, q); q += 1  # code align, data align (sleb ok for skip), ra reg
            enc = 0
            if aug.startswith('z'):
                _, q = uleb(raw, q)
                for ch in aug[1:]:
                    if ch == 'R': enc = raw[q]; q += 1
                    elif ch == 'L': q += 1
                    elif ch == 'P':
                        e = raw[q]; q += 1 + {3: 4, 0xb: 4, 4: 8, 0xc: 8, 0: 8}[e & 0xf]
            cie_cache[cie] = enc
        enc = cie_cache[cie]
        size = {3: 4, 0xb: 4, 4: 8, 0xc: 8, 0: 8}[enc & 0xf]
        rng = bytes(read_va(elf, ph, fde + 8 + size, size))
        rlen = int.from_bytes(rng, 'little')
        ranges.append((hv + initial, hv + initial + rlen))
    ranges.sort()
    merged = []
    for a, b in ranges:
        if merged and a <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], b)
        else:
            merged.append([a, b])
    return merged, f'{count} FDE'


def in_ranges(ranges, starts, a):
    import bisect
    i = bisect.bisect_right(starts, a) - 1
    return i >= 0 and a < ranges[i][1]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--game', required=True, type=Path)
    ap.add_argument('--json', type=Path)
    ap.add_argument('--objdump')
    ap.add_argument('--modules', nargs='*', default=list(DEFAULT_MODULES))
    ap.add_argument('--max-list', type=int, default=200)
    a = ap.parse_args()
    objdump = a.objdump
    if not objdump:
        objdump = next((c for c in OBJDUMP_CANDIDATES if Path(c).exists()), OBJDUMP_CANDIDATES[-1])
    ver = '?'
    try:
        ver = sfo((a.game / 'sce_sys' / 'param.sfo').read_bytes()).get('APP_VER', '?')
    except Exception as e:  # noqa
        print('APP_VER unreadable:', type(e).__name__)
    print(f'APP_VER={ver}')
    files = [('eboot.bin', a.game / 'eboot.bin')] + [(m, a.game / 'sce_module' / m) for m in a.modules]
    result = dict(app_ver=ver, files={})
    uncovered = []
    for name, path in files:
        r = scan_image(path, objdump)
        for h in r['hits']:
            h['in_fde'] = in_ranges(r['fde'], r['fde_starts'], h['addr']) if r['fde'] else True
        forms = collections.OrderedDict()
        for h in r['hits']:
            k = (h['seg'], h['form'], h['covered'], h['in_fde'])
            e = forms.setdefault(k, dict(count=0, disp=collections.Counter(), lens=collections.Counter()))
            e['count'] += 1; e['disp'][h['disp']] += 1; e['lens'][h['len']] += 1
            if not h['covered'] and h['in_fde']:
                uncovered.append((name, h))
        inf = [h for h in r['hits'] if h['in_fde']]
        last_end = r['fde'][-1][1] if r['fde'] else 0
        for h in r['hits']:
            h['region'] = 'fde' if h['in_fde'] else ('tail' if h['addr'] >= last_end else 'gap')
        gaps = [h for h in r['hits'] if h['region'] == 'gap']
        print(f'== {name}: instructions={r["insns"]} (bad)={r["bad"]} exec_segments={[(s["index"], hex(s["vaddr"]), s["size"]) for s in r["exec_segments"]]}')
        print(f'   FDE: {r["fde_note"]}, function bytes={sum(b - a_ for a_, b in r["fde"])}')
        print(f'   outside FDE: behind last FDE range ({last_end:#x}, data tail)={len(r["hits"]) - len(inf) - len(gaps)}, in gaps between FDE ranges={len(gaps)}')
        print(f'   raw 9-byte mov rax,fs:[0] pattern count (link_*.py basis): {r["raw_fs_load"]}')
        print(f'   segment-override hits total={len(r["hits"])} inside FDE={len(inf)} (covered={sum(1 for h in inf if h["covered"])}, '
              f'not covered={sum(1 for h in inf if not h["covered"])}) outside FDE={len(r["hits"]) - len(inf)}')
        for (seg, form, cov, fde), e in sorted(forms.items(), key=lambda kv: (not kv[0][3], -kv[1]['count'])):
            if not fde and e['count'] < 0: continue
            top = ', '.join(f'{fmt_disp(d)}:{n}' for d, n in e['disp'].most_common(6))
            print(f'   {"FDE" if fde else "noFDE"} | {seg} | {form} | n={e["count"]} | covered={cov} | lens={dict(e["lens"])} | disp {{{top}}}')
        result['files'][name] = dict(insns=r['insns'], bad=r['bad'], raw_fs_load=r['raw_fs_load'], exec_segments=r['exec_segments'],
                                     fde_note=r['fde_note'], fde_function_bytes=sum(b - a_ for a_, b in r['fde']), last_fde_end=hex(last_end), gap_hits=len(gaps),
                                     forms=[dict(segment=k[0], form=k[1], covered=k[2], in_fde=k[3], count=e['count'],
                                                 disp={fmt_disp(d): n for d, n in e['disp'].items()}, lens=dict(e['lens']),
                                                 rating=rate(k[0], k[1], k[2], k[3]))
                                            for k, e in forms.items()],
                                     hits=[dict(vaddr=hex(h['addr']), seg=h['segidx'], segment=h['seg'], form=h['form'],
                                                disp=fmt_disp(h['disp']), len=h['len'], covered=h['covered'], in_fde=h['in_fde'], region=h['region'])
                                           for h in r['hits']])
    print(f'== not covered inside FDE ranges: {len(uncovered)} total (showing up to {a.max_list})')
    for name, h in uncovered[:a.max_list]:
        print(f'   {name} seg{h["segidx"]} {h["addr"]:#x} {h["seg"]} {h["form"]} disp={fmt_disp(h["disp"])} len={h["len"]}')
    if a.json:
        a.json.write_text(json.dumps(result, indent=1) + chr(10), encoding='utf-8')


if __name__ == '__main__':
    main()
