#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# refueled: RZ1 offline - static red-zone census (docs/refueled/audit/REDZONE.md 3.3 item 2).
# Counts only; no rewriting, no liveness (liveness is RZ3). Prints numbers, never bytes or listings.
"""Count red-zone accesses ([rsp+disp], disp < 0, no index) per function of an eboot.bin / .prx.

Usage: redzone_census.py INPUT [--table-only] [--json]
"""
import argparse
import collections
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import capstone  # noqa: E402
from capstone import x86  # noqa: E402
from redzone_funcs import function_starts  # noqa: E402

MAX_EXAMPLES = 5
DISP_BUCKET = 8
DISP_LIMIT = 128


def function_bounds(starts, exec_ranges, image_len):
    """Yield (start, end): end = next start or end of the executable range holding start."""
    ri = 0
    for i, s in enumerate(starts):
        while exec_ranges[ri][1] <= s:
            ri += 1
        end = exec_ranges[ri][1]
        if i + 1 < len(starts) and starts[i + 1] < end:
            end = starts[i + 1]
        yield s, min(end, image_len)


def rz_operand(insn):
    """Return the negative displacement if insn has a red-zone memory operand, else None."""
    if insn.id in (x86.X86_INS_LEA, x86.X86_INS_NOP):
        return None
    for op in insn.operands:
        if op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RSP and op.mem.index == 0 \
                and op.mem.segment == 0 and op.mem.disp < 0:
            return op.mem.disp, op.access
    return None


def census(starts, image, exec_ranges):
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    t0 = time.perf_counter()
    c = collections.Counter()
    lengths = collections.Counter()
    max_disp_hist = collections.Counter()
    examples = []
    view = bytes(image)
    for start, end in function_bounds(starts, exec_ranges, len(image)):
        c['functions'] += 1
        code = view[start:end]
        pos = 0
        has_rz = reads = writes = has_ijmp = has_call = False
        saw_alloc = False
        first_alloc = None  # saw sub rsp/push before the first red-zone access?
        max_neg = 0
        decode_err = False
        while pos < len(code):
            for insn in md.disasm(code[pos:], start + pos):
                pos = insn.address - start + insn.size
                iid = insn.id
                if iid == x86.X86_INS_PUSH:
                    saw_alloc = True
                elif iid == x86.X86_INS_SUB and insn.operands and insn.operands[0].type == x86.X86_OP_REG \
                        and insn.operands[0].reg == x86.X86_REG_RSP:
                    saw_alloc = True
                elif iid == x86.X86_INS_CALL:
                    has_call = True
                elif iid == x86.X86_INS_JMP and insn.operands and insn.operands[0].type != x86.X86_OP_IMM:
                    has_ijmp = True
                hit = rz_operand(insn)
                if hit:
                    disp, access = hit
                    has_rz = True
                    c['rz_insns'] += 1
                    lengths[insn.size] += 1
                    if insn.size < 5:
                        c['rz_insns_lt5'] += 1
                    r, w = bool(access & capstone.CS_AC_READ), bool(access & capstone.CS_AC_WRITE)
                    reads |= r
                    writes |= w
                    c['rz_insn_read' if r and not w else 'rz_insn_write' if w and not r
                      else 'rz_insn_rw' if r and w else 'rz_insn_noaccess'] += 1
                    if first_alloc is None:
                        first_alloc = saw_alloc
                    max_neg = max(max_neg, -disp)
                    if disp < -DISP_LIMIT:
                        c['rz_insns_beyond_128'] += 1
                    if len(examples) < MAX_EXAMPLES and not examples.count(start):
                        examples.append(start)
            if pos >= len(code):
                break
            # md.disasm stopped before the end: undecodable byte, count and skip one byte
            c['decode_errors'] += 1
            decode_err = True
            pos += 1
        if decode_err:
            c['functions_with_decode_error'] += 1
        if has_rz:
            c['rz_functions'] += 1
            c['rz_fn_write_only' if writes and not reads else 'rz_fn_read_only' if reads and not writes
              else 'rz_fn_both' if reads and writes else 'rz_fn_noaccess'] += 1
            if has_ijmp:
                c['rz_fn_with_indirect_jmp'] += 1
            if not first_alloc:
                c['rz_fn_no_alloc_before'] += 1
                if not has_call:
                    c['rz_fn_no_alloc_before_no_call'] += 1
            bucket = (max_neg + DISP_BUCKET - 1) // DISP_BUCKET * DISP_BUCKET
            max_disp_hist[-bucket if bucket <= DISP_LIMIT else -(DISP_LIMIT + 1)] += 1
    c['ms'] = int((time.perf_counter() - t0) * 1000)
    return dict(counts=dict(c), insn_length_hist={str(k): lengths[k] for k in sorted(lengths)},
                max_disp_hist={('<-128' if k < -DISP_LIMIT else str(k)): v
                               for k, v in sorted(max_disp_hist.items(), reverse=True)},
                example_function_offsets=[hex(e) for e in examples])


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('input')
    ap.add_argument('--table-only', action='store_true', help='function starts only from the search table (default: table plus .eh_frame walk)')
    ap.add_argument('--json', action='store_true')
    args = ap.parse_args(argv)
    starts, image, ranges, info = function_starts(args.input, union=not args.table_only)
    res = census(starts, image, ranges)
    res['source'] = dict(function_starts=len(starts), table_entries=info['table_entries'],
                         walk_extra=info['walk_extra'], union=not args.table_only)
    if args.json:
        print(json.dumps(res, indent=1))
        return 0
    print(f"function starts: {len(starts)} (table {info['table_entries']}, union={not args.table_only})")
    for k, v in res['counts'].items():
        print(f"{k}: {v}")
    print("rz instruction length histogram (bytes: count):", res['insn_length_hist'])
    print("largest negative displacement per function (upper bucket bound: functions):", res['max_disp_hist'])
    print("example function offsets (max 5):", ' '.join(res['example_function_offsets']))
    print("note: liveness of the red zone is RZ3, not computed here")
    return 0


if __name__ == '__main__':
    sys.exit(main())
