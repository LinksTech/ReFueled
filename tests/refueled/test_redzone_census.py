# SPDX-License-Identifier: GPL-2.0-or-later
# refueled: RZ1 offline - unit test for tools/redzone_census.py on synthetic code (no game data).
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent.parent / 'tools'))
import rz_synth as S  # noqa: E402
import redzone_funcs as R  # noqa: E402
import redzone_census as C  # noqa: E402


def fn(body):
    return body + b'\xcc' * (-len(body) % 16)


FUNCS = [
    # 0x200 leaf: mov [rsp-8],rax ; mov rax,[rsp-8] ; ret
    fn(b'\x48\x89\x44\x24\xf8' + b'\x48\x8b\x44\x24\xf8' + b'\xc3'),
    # 0x210 sub rsp,0x20 ; mov [rsp],rax ; add rsp,0x20 ; ret  (no red zone)
    fn(b'\x48\x83\xec\x20' + b'\x48\x89\x04\x24' + b'\x48\x83\xc4\x20' + b'\xc3'),
    # 0x220 short insn: mov [rsp-8],eax (4 bytes) ; ret
    fn(b'\x89\x44\x24\xf8' + b'\xc3'),
    # 0x230 mov [rsp-16],rax ; jmp rax
    fn(b'\x48\x89\x44\x24\xf0' + b'\xff\xe0'),
    # 0x240 push rax ; mov [rsp-8],rax ; ret
    fn(b'\x50' + b'\x48\x89\x44\x24\xf8' + b'\xc3'),
    # 0x250 invalid byte 0x06 in 64-bit mode ; ret
    fn(b'\x06' + b'\xc3'),
]


def run(code, n_funcs):
    starts_v = [S.BASE + S.CODE_OFF + 0x10 * i for i in range(n_funcs)]
    with tempfile.TemporaryDirectory() as d:
        p = Path(d) / 'x.elf'
        p.write_bytes(S.build(code, starts_v))
        starts, image, ranges, _info = R.function_starts(p)
    return starts, C.census(starts, image, ranges)


class CensusTest(unittest.TestCase):
    def test_counts(self):
        starts, res = run(b''.join(FUNCS), len(FUNCS))
        self.assertEqual(starts, [0x200 + 0x10 * i for i in range(6)])
        c = res['counts']
        want = dict(functions=6, rz_functions=4, rz_fn_write_only=3, rz_fn_both=1, rz_insns=5,
                    rz_insns_lt5=1, rz_insn_read=1, rz_insn_write=4, rz_fn_with_indirect_jmp=1,
                    rz_fn_no_alloc_before=3, rz_fn_no_alloc_before_no_call=3, decode_errors=1,
                    functions_with_decode_error=1)
        for k, v in want.items():
            self.assertEqual(c.get(k, 0), v, k)
        self.assertEqual(res['insn_length_hist'], {'4': 1, '5': 4})
        self.assertEqual(res['max_disp_hist'], {'-8': 3, '-16': 1})
        self.assertNotIn('rz_insns_beyond_128', c)

    def test_lea_and_positive_disp_not_counted(self):
        # lea rax,[rsp-8] ; mov [rsp+8],rax ; ret
        _s, res = run(fn(b'\x48\x8d\x44\x24\xf8' + b'\x48\x89\x44\x24\x08' + b'\xc3'), 1)
        self.assertEqual(res['counts'].get('rz_insns', 0), 0)
        self.assertEqual(res['counts']['functions'], 1)


if __name__ == '__main__':
    unittest.main()
