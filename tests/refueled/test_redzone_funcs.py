# SPDX-License-Identifier: GPL-2.0-or-later
# refueled: RZ2 - unit test for tools/redzone_funcs.py on synthetic ELF64 images (no game data).
import struct
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent.parent / 'tools'))
import rz_synth as S  # noqa: E402
import redzone_funcs as R  # noqa: E402


class FuncsTest(unittest.TestCase):
    def run_tool(self, data, **kw):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'x.elf'
            p.write_bytes(data)
            return R.function_starts(p, **kw)

    def test_datarel_sdata4_sorted_unique_and_dropped(self):
        code = b'\xc3' * 0x40
        v = S.BASE
        # 4 entries: 3 in text, 1 inside the rw- segment (not executable, must be dropped)
        fdes = [v + 0x230, v + 0x200, v + 0x210, S.DATA_VADDR + 0x10]
        data = S.build(code, fdes)
        starts, image, ranges, info = self.run_tool(data, walk=True)
        self.assertEqual(starts, [0x200, 0x210, 0x230])
        self.assertEqual(ranges, [(0, S.TEXT_END)])
        self.assertEqual(len(image), 0xc00)
        self.assertEqual(info['table_entries'], 4)
        self.assertEqual(info['dropped'], 1)
        self.assertEqual(info['duplicates'], 0)
        self.assertEqual(info['walk_count'], 4)
        self.assertEqual(info['walk_extra'], 0)

    def test_duplicate_table_entries_are_merged(self):
        s = S.BASE + 0x200
        data = S.build(b'\xc3' * 0x20, [s], table_starts=[s, s])
        starts, _i, _r, info = self.run_tool(data)
        self.assertEqual(starts, [0x200])
        self.assertEqual(info['duplicates'], 1)

    def test_udata8_absptr_table_and_udata4_eh_frame_ptr(self):
        fdes = [S.BASE + 0x220, S.BASE + 0x200]
        data = S.build(b'\xc3' * 0x40, fdes, table_enc=0x04, count_enc=0x03, ptr_enc=0x03)
        starts, _img, _r, info = self.run_tool(data, walk=True)
        self.assertEqual(starts, [0x200, 0x220])
        self.assertEqual(info['walk_count'], 2)

    def test_pcrel_sdata4_table(self):
        fdes = [S.BASE + 0x240, S.BASE + 0x200]
        data = S.build(b'\xc3' * 0x60, fdes, table_enc=0x1b)
        starts, *_ = self.run_tool(data)
        self.assertEqual(starts, [0x200, 0x240])

    def test_walk_finds_fdes_missing_from_table(self):
        fdes = [S.BASE + 0x200, S.BASE + 0x210, S.BASE + 0x220]
        data = S.build(b'\xc3' * 0x40, fdes, table_starts=fdes[:2])
        starts, _i, _r, info = self.run_tool(data, walk=True)
        self.assertEqual(starts, [0x200, 0x210])
        self.assertEqual((info['walk_count'], info['walk_extra']), (3, 1))
        starts, _i, _r, info = self.run_tool(data, union=True)
        self.assertEqual(starts, [0x200, 0x210, 0x220])

    def test_file_format_byte_exact(self):
        blob = R.pack([0x200, 0x210, 0x230], 0xc00)
        want = b'RZFL' + struct.pack('<HHII', 1, 0, 3, 0xc00) + struct.pack('<III', 0x200, 0x210, 0x230)
        self.assertEqual(blob, want)
        self.assertEqual(len(blob), 16 + 12)
        self.assertEqual(R.pack([], 0x10), b'RZFL' + struct.pack('<HHII', 1, 0, 0, 0x10))

    def test_pack_rejects_u32_overflow(self):
        with self.assertRaises(ValueError):
            R.pack([1 << 32], 0x10)
        with self.assertRaises(ValueError):
            R.pack([1], 1 << 32)

    def test_missing_eh_frame_is_an_error(self):
        with self.assertRaises(ValueError):
            self.run_tool(S.build_without_eh_frame())

    def test_main_writes_file(self):
        with tempfile.TemporaryDirectory() as d:
            src, out = Path(d) / 'a.elf', Path(d) / 'rz.bin'
            src.write_bytes(S.build(b'\xc3' * 0x20, [S.BASE + 0x200, S.BASE + 0x210]))
            self.assertEqual(R.main([str(src), '-o', str(out), '--walk', '--summary']), 0)
            self.assertEqual(out.read_bytes(), R.pack([0x200, 0x210], 0xc00))


if __name__ == '__main__':
    unittest.main()
