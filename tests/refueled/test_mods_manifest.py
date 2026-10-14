# SPDX-License-Identifier: GPL-2.0-or-later
# refueled: scripts/mods.py --manifest (virtual mod overlay, package MO). Synthetic game and mods
# only. Run: python3 -m unittest discover -s tests/refueled -p "test_mods_manifest.py"
import contextlib
import importlib.util
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('bbmods_manifest', ROOT / 'scripts/mods.py')
mods = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mods)

HEADER = '# refueled mod manifest 1\n'


def symlinks_work(folder):
    target = folder / 'probe-target'
    target.write_bytes(b'x')
    try:
        (folder / 'probe-link').symlink_to(target)
    except (OSError, NotImplementedError):
        return False
    return True


class ManifestTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.game = self.root / 'CUSA03173'
        (self.game / 'dvdroot_ps4' / 'chr').mkdir(parents=True)
        (self.game / 'dvdroot_ps4' / 'msg').mkdir()
        (self.game / 'eboot.bin').write_bytes(b'exe')
        (self.game / 'dvdroot_ps4' / 'chr' / 'a.dcx').write_bytes(b'game-a')
        (self.game / 'dvdroot_ps4' / 'chr' / 'b.dcx').write_bytes(b'game-b')
        (self.game / 'dvdroot_ps4' / 'msg' / 'x.txt').write_bytes(b'game-x')
        self.moddir = self.root / 'mods'

    def put(self, path, data=b'mod'):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def host(self, path):
        return str(Path(path).resolve())

    def manifest(self, layers):
        target = self.root / 'm.manifest'
        with contextlib.redirect_stderr(io.StringIO()) as err:
            mods.write_manifest(self.game, layers, target)
        return target.read_bytes().decode('utf-8'), err.getvalue()

    def test_no_mods_header_only(self):
        text, _ = self.manifest([])
        self.assertEqual(text, HEADER)

    def test_two_mods_conflict_last_wins_byte_exact(self):
        a = self.moddir / 'A'
        b = self.moddir / 'B'
        fa = self.put(a / 'dvdroot_ps4' / 'chr' / 'a.dcx', b'A')
        self.put(a / 'dvdroot_ps4' / 'chr' / 'new.dcx', b'new')
        fb = self.put(b / 'dvdroot_ps4' / 'chr' / 'a.dcx', b'B')
        text, err = self.manifest([('A', a), ('B', b)])
        expected = (HEADER
                    + 'dvdroot_ps4/chr/a.dcx\t' + self.host(fb) + '\n'
                    + 'dvdroot_ps4/chr/new.dcx\t' + self.host(a / 'dvdroot_ps4' / 'chr' / 'new.dcx') + '\n')
        self.assertEqual(text, expected)
        self.assertIn('A -> B', err)                      # same conflict message as the link mode
        self.assertIn('Mods: A: 2 files', err)
        self.assertIn('Mods: B: 1 files', err)
        self.assertIn('Mods: 1 game files replaced, 1 added', err)
        self.assertNotEqual(self.host(fa), self.host(fb))
        # order matters: B first, A last -> A wins
        text, _ = self.manifest([('B', b), ('A', a)])
        self.assertIn('dvdroot_ps4/chr/a.dcx\t' + self.host(fa) + '\n', text)

    def test_casefold_takes_the_games_spelling(self):
        a = self.moddir / 'A'
        f = self.put(a / 'dvdroot_ps4' / 'CHR' / 'A.DCX')
        g = self.put(a / 'dvdroot_ps4' / 'Msg' / 'Fresh' / 'F.bin')
        text, _ = self.manifest([('A', a)])
        # existing names take the game's case; names the game lacks keep the mod's
        self.assertEqual(text, HEADER
                         + 'dvdroot_ps4/chr/a.dcx\t' + self.host(f) + '\n'
                         + 'dvdroot_ps4/msg/Fresh/F.bin\t' + self.host(g) + '\n')

    def test_two_mods_spell_a_new_folder_the_same(self):
        a, b = self.moddir / 'A', self.moddir / 'B'
        fa = self.put(a / 'dvdroot_ps4' / 'NewDir' / 'one.bin')
        fb = self.put(b / 'dvdroot_ps4' / 'newdir' / 'two.bin')
        text, _ = self.manifest([('A', a), ('B', b)])
        self.assertEqual(text, HEADER
                         + 'dvdroot_ps4/NewDir/one.bin\t' + self.host(fa) + '\n'
                         + 'dvdroot_ps4/NewDir/two.bin\t' + self.host(fb) + '\n')

    def test_layouts(self):
        layouts = {
            'plain': 'dvdroot_ps4/chr/p.dcx',
            'app': 'app0/dvdroot_ps4/chr/q.dcx',
            'sku': 'CUSA03173/dvdroot_ps4/chr/r.dcx',
            'wrapper': 'Pack/dvdroot_ps4/chr/s.dcx',
            'folders': 'chr/t.dcx',                    # the game's folders without dvdroot_ps4
        }
        layers, expected = [], []
        for name, path in layouts.items():
            f = self.put(self.moddir / name / path)
            layers.append((name, self.moddir / name))
            guest = 'dvdroot_ps4/chr/' + path.rsplit('/', 1)[1]
            expected.append((guest, self.host(f)))
        text, _ = self.manifest(layers)
        self.assertEqual(text, HEADER + ''.join(f'{g}\t{h}\n' for g, h in sorted(expected)))

    def test_metadata_outside_dvdroot_is_not_listed(self):
        a = self.moddir / 'A'
        f = self.put(a / 'dvdroot_ps4' / 'chr' / 'a.dcx')
        self.put(a / 'readme.txt')
        text, _ = self.manifest([('A', a)])
        self.assertEqual(text, HEADER + 'dvdroot_ps4/chr/a.dcx\t' + self.host(f) + '\n')

    def test_file_directory_conflicts_are_errors(self):
        a = self.moddir / 'A'
        self.put(a / 'dvdroot_ps4' / 'chr' / 'a.dcx' / 'inside')    # a.dcx is a game file
        with self.assertRaises(ValueError), contextlib.redirect_stderr(io.StringIO()):
            mods.write_manifest(self.game, [('A', a)], self.root / 'm.manifest')

    def test_tab_or_newline_in_a_path_is_refused(self):
        real = mods.collect_replacements
        try:
            for bad in ('dvdroot_ps4/chr/a\tb', 'dvdroot_ps4/chr/a\nb'):
                mods.collect_replacements = lambda layers, bad=bad: [(Path(bad), Path(self.root / 'src.bin'))]
                with self.assertRaises(ValueError) as caught, contextlib.redirect_stderr(io.StringIO()):
                    mods.write_manifest(self.game, [], self.root / 'm.manifest')
                self.assertIn('Tab or line break', str(caught.exception))
        finally:
            mods.collect_replacements = real

    def test_cli_writes_only_the_file(self):
        a = self.moddir / 'A'
        f = self.put(a / 'dvdroot_ps4' / 'chr' / 'a.dcx')
        out = self.root / 'out'
        target = self.root / 'cli.manifest'
        done = subprocess.run([sys.executable, '-I', str(ROOT / 'scripts/mods.py'), str(self.game), '--out', str(out),
                               '--mods-dir', str(self.moddir), '--manifest', str(target)],
                              capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(done.stdout, '')
        self.assertEqual(target.read_bytes().decode('utf-8'),
                         HEADER + 'dvdroot_ps4/chr/a.dcx\t' + self.host(f) + '\n')
        self.assertFalse(out.exists())                    # no overlay folder, no links
        self.assertEqual((self.game / 'dvdroot_ps4' / 'chr' / 'a.dcx').read_bytes(), b'game-a')

    def test_same_selection_as_the_link_mode(self):
        """The link tree and the manifest both start from collect_replacements, so the selection
        (layouts, order, last mod wins, messages) is the same code. Where symlinks work the two
        results are also compared; otherwise (no privilege on Windows) only the shared function."""
        a, b = self.moddir / 'A', self.moddir / 'B'
        self.put(a / 'dvdroot_ps4' / 'CHR' / 'A.dcx', b'A')
        self.put(a / 'dvdroot_ps4' / 'Fresh' / 'F.bin', b'F')
        self.put(b / 'chr' / 'a.dcx', b'B')
        layers = [('A', a), ('B', b)]
        with contextlib.redirect_stderr(io.StringIO()):
            selection = mods.collect_replacements(layers)
        keys = {rel.as_posix().casefold() for rel, _ in selection}
        text, _ = self.manifest(layers)
        guests = [line.split('\t')[0] for line in text.splitlines()[1:]]
        self.assertEqual({g.casefold() for g in guests}, keys)
        if not symlinks_work(self.root):
            self.skipTest('symlinks need privileges here: selection compared through the shared function only')
        with contextlib.redirect_stderr(io.StringIO()):
            tree = mods.build_overlay(self.game, self.root / 'out', layers)
        linked = {}
        for path in (tree / 'dvdroot_ps4').rglob('*'):
            if path.is_file():
                linked[path.relative_to(tree).as_posix()] = str(path.resolve())
        listed = {line.split('\t')[0]: line.split('\t')[1] for line in text.splitlines()[1:]}
        game_files = {p.relative_to(self.game).as_posix(): str(p.resolve())
                      for p in (self.game / 'dvdroot_ps4').rglob('*') if p.is_file()}
        merged = {**game_files, **listed}
        self.assertEqual(linked, merged)


if __name__ == '__main__':
    unittest.main()
