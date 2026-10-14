#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Merge loose-file mods through links, preserving the original game and mod files."""
import argparse
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile

# Top-level folders of the game's dvdroot_ps4: a mod made of these is a dvdroot_ps4 itself.
GAME_FOLDERS = {'action', 'chr', 'event', 'facegen', 'font', 'map', 'menu', 'movie', 'msg', 'mtd',
                'obj', 'other', 'param', 'paramdef', 'parts', 'remo', 'script', 'sfx', 'shader',
                'sound'}


def child(folder, name):
    """`name` inside `folder`, matching an existing entry case-insensitively (mods made on
    Windows often differ from the game's lower-case names)."""
    folder = Path(folder)
    for entry in (folder.iterdir() if folder.is_dir() else ()):
        if entry.name.casefold() == name.casefold():
            return entry
    return folder / name


def content_root(folder):
    """(root, prefix): where a mod's files are and the game path they go to, or None.

    Accepted layouts: <mod>/dvdroot_ps4, <mod>/app0/dvdroot_ps4, <mod>/CUSA03173/dvdroot_ps4,
    one wrapper folder around any of these (an archive extracted into a folder of its name), and
    the game's folders without dvdroot_ps4 (<mod>/chr, <mod>/parts, ...)."""
    folder = Path(folder)
    if not folder.is_dir():
        return None
    for wrapper in ('', 'app0', 'CUSA03173'):
        base = child(folder, wrapper) if wrapper else folder
        dvdroot = child(base, 'dvdroot_ps4')
        if dvdroot.is_dir():
            return base, ''
    entries = [e for e in folder.iterdir() if not e.name.startswith('.')]
    folders = [e for e in entries if e.is_dir()]
    if folders and all(e.name.casefold() in GAME_FOLDERS for e in folders):
        return folder, 'dvdroot_ps4'
    if len(folders) == 1 and not [e for e in entries if e.is_file() and
                                  e.suffix.casefold() not in ('.txt', '.md', '.jpg', '.png', '.ini')]:
        return content_root(folders[0])
    return None


def discover(root):
    """Named mods: folders of `root` with an accepted layout (content_root)."""
    root = Path(root)
    if not root.is_dir():
        return []
    return sorted((p.name for p in root.iterdir() if p.is_dir() and content_root(p)),
                  key=lambda name: (name.casefold(), name))


def selected(root, config):
    available = discover(root)
    if not config or not Path(config).is_file():
        return available
    settings = json.loads(Path(config).read_text(encoding='utf-8'))
    disabled_names = settings.get('disabled', [])
    if not isinstance(disabled_names, list) or not all(isinstance(n, str) for n in disabled_names):
        raise ValueError('Disabled mods must be a list of folder names')
    disabled = set(disabled_names)
    order = settings.get('order', [])
    if not isinstance(order, list) or not all(isinstance(n, str) for n in order):
        raise ValueError('Mod order must be a list of folder names')
    # New folders are enabled automatically and appended in alphabetical order.
    return list(dict.fromkeys(n for n in [*order, *available]
                             if n in available and n not in disabled))


def mod_files(folder):
    """(game path, source) of every file a mod replaces or adds under dvdroot_ps4."""
    layout = content_root(folder)
    if not layout:
        raise ValueError(f'{folder}: expected dvdroot_ps4 (or chr/, parts/, ...) inside the mod folder')
    root, prefix = layout
    root = root.resolve()
    for directory, folders, files in os.walk(root, followlinks=False):
        directory = Path(directory)
        for name in [*folders, *files]:
            if (directory / name).is_symlink():
                raise ValueError(f'Mod symlinks are unsupported: {directory / name}')
        for name in sorted(files):
            source = directory / name
            relative = Path(prefix) / source.relative_to(root) if prefix else source.relative_to(root)
            # Readme/metadata stay outside the mounted game. This loader handles assets;
            # executable patches use the existing patch compiler, with address validation.
            if relative.parts[0].casefold() != 'dvdroot_ps4':
                if relative.parts[0].casefold() in ('eboot.bin', 'sce_module', 'sce_sys'):
                    raise ValueError(f'Executable/system replacement is unsupported: {source}')
                continue
            if not source.is_file():
                raise ValueError(f'Not a regular mod file: {source}')
            yield relative, source


def expand(directory):
    """Materialize one directory level; never write through a directory link."""
    if directory.is_symlink():
        target = directory.resolve(strict=True)
        if not target.is_dir():
            raise ValueError(f'File/directory conflict at {directory.name}')
        directory.unlink()
        directory.mkdir()
        for entry in target.iterdir():
            (directory / entry.name).symlink_to(entry, target_is_directory=entry.is_dir())
    elif directory.exists() and not directory.is_dir():
        raise ValueError(f'File/directory conflict at {directory.name}')
    else:
        directory.mkdir(exist_ok=True)


def collect_replacements(mods):
    """The selection shared by the link tree and the manifest: (game path, source) of every mod
    file in load order (a later mod wins), with the conflict and count messages on stderr."""
    replacements = []
    owners = {}
    for name, root in mods:
        count = 0
        for relative, source in mod_files(root):
            key = relative.as_posix().casefold()
            if key in owners:
                print(f'Mods: {relative}: {owners[key]} -> {name}', file=sys.stderr)
            owners[key] = name
            replacements.append((relative, source))
            count += 1
        print(f'Mods: {name}: {count} files', file=sys.stderr)
    return replacements


MANIFEST_HEADER = '# refueled mod manifest 1'


def manifest_entries(game, replacements):
    """(entries, replaced, added): `entries` maps the guest path (relative to /app0, '/' separated)
    to the absolute host path of the mod file; the last mod for a path wins. The walk is the one
    of build_overlay on names instead of links: each component takes the game's spelling when it
    exists in another case, otherwise the spelling of the first mod that created it."""
    game = Path(game).resolve(strict=True)
    entries = {}
    created = {}    # (spelled parent, casefolded name) -> (name, is_dir): only mods have it
    replaced = added = 0

    def lookup(spelled, part):
        """(name, is_dir or None, in_game) of `part` inside the overlay folder `spelled`."""
        parent = game.joinpath(*spelled)
        found = child(parent, part) if parent.is_dir() else None
        if found is not None and found.exists():
            return found.name, found.is_dir(), True
        key = ('/'.join(spelled), part.casefold())
        if key in created:
            return (*created[key], False)
        return part, None, False

    for relative, source in replacements:
        spelled = []
        for part in relative.parts[:-1]:
            name, is_dir, _ = lookup(spelled, part)
            if is_dir is False:
                raise ValueError(f'File/directory conflict at {name}')
            if is_dir is None:
                created[('/'.join(spelled), part.casefold())] = (name, True)
            spelled.append(name)
        name, is_dir, in_game = lookup(spelled, relative.parts[-1])
        if is_dir:
            raise ValueError(f'File/directory conflict: {relative}')
        guest = '/'.join([*spelled, name])
        if guest not in entries:
            if in_game:
                replaced += 1
            else:
                added += 1
                created[('/'.join(spelled), name.casefold())] = (name, False)
        entries[guest] = str(source)
    return entries, replaced, added


def write_manifest(game, mods, target):
    """Write the manifest of the selection (format: docs/refueled/MODLOADER.md); no links."""
    entries, replaced, added = manifest_entries(game, collect_replacements(mods))
    lines = [MANIFEST_HEADER]
    for guest in sorted(entries):
        for text in (guest, entries[guest]):
            if '\t' in text or '\n' in text or '\r' in text:
                raise ValueError(f'Tab or line break in a path (not possible in a manifest): {text!r}')
        lines.append(f'{guest}\t{entries[guest]}')
    print(f'Mods: {replaced} game files replaced, {added} added', file=sys.stderr)
    Path(target).write_bytes(('\n'.join(lines) + '\n').encode('utf-8'))


def build_overlay(game, out, mods):
    game = Path(game).resolve(strict=True)
    replacements = collect_replacements(mods)
    if not replacements:
        return game
    out = Path(out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    overlay = Path(tempfile.mkdtemp(prefix='mod-game-', dir=out))
    try:
        for entry in game.iterdir():
            (overlay / entry.name).symlink_to(entry, target_is_directory=entry.is_dir())
        replaced = added = 0
        for relative, source in replacements:
            # Each component takes the game's spelling when it exists in another case.
            parent = overlay
            for part in relative.parts[:-1]:
                parent = child(parent, part)
                expand(parent)
            destination = child(parent, relative.parts[-1])
            if destination.is_symlink():
                replaced += destination.resolve().is_relative_to(game)
                destination.unlink()
            elif destination.exists():
                raise ValueError(f'File/directory conflict: {relative}')
            else:
                added += 1
            destination.symlink_to(source)
        print(f'Mods: {replaced} game files replaced, {added} added', file=sys.stderr)
        return overlay
    except BaseException:
        shutil.rmtree(overlay)
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--mods-dir', required=True, type=Path)
    parser.add_argument('--config', type=Path)
    parser.add_argument('--enabled', choices=('0', '1'), default='1')
    parser.add_argument('--manifest', type=Path, help='write the selection as a manifest file (no links, no overlay folder)')
    args = parser.parse_args()
    layers = []
    if args.enabled == '1':
        # shadPS4's loose overlay convention, beside the original game.
        legacy = Path(str(args.game.resolve()) + '-mods')
        if legacy.is_dir():
            layers.append((legacy.name, legacy))
        # A directly selected loose overlay is also accepted.
        if child(args.mods_dir, 'dvdroot_ps4').is_dir():
            layers.append((args.mods_dir.name, args.mods_dir))
        else:
            for name in selected(args.mods_dir, args.config):
                layers.append((name, args.mods_dir / name))
    if args.manifest:
        write_manifest(args.game, layers, args.manifest)
    else:
        print(build_overlay(args.game, args.out, layers))


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, TypeError) as error:
        print(f'Mods: {error}', file=sys.stderr)
        sys.exit(1)
