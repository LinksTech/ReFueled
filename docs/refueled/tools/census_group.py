#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# refueled: groups the errors of a `build-windows.sh --keep-going` run by package owner
# (docs/refueled/tools/owners.txt) and writes <census>/summary.md and summary.tsv.
# Input: the logs build-windows.sh leaves in out/win/census/:
#   unit=<path, '/' as '__'>.log   one failed C/asm translation unit or test (clang output)
#   gpu-configure.log, gpu-build.log  CMake configure / `ninja -k 0` of the GPU library
#   link.log                        the sacredfire.exe link
# Only build logs are read; no game file passes through here.
import argparse
import fnmatch
import os
import re
import sys
from collections import defaultdict

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..')).replace('\\', '/')
# clang: "path:line:col: error: text" / "fatal error:"; the path may start with a drive letter.
DIAG = re.compile(r'^(?P<path>(?:[A-Za-z]:)?[^:\s][^:]*):(?P<line>\d+):(?:\d+:)?\s+(?:fatal )?error:\s*(?P<text>.*)$')
# lld / clang driver: "ld.lld: error: undefined symbol: x", "clang: error: linker command failed"
TOOL = re.compile(r'^(?:ld\.lld|lld|lld-link|clang|clang\+\+)(?:\.exe)?: error: (?P<text>.*)$')
REF = re.compile(r'>>> referenced by (?P<where>.+)$')
CMAKE = re.compile(r'^CMake Error at (?P<path>[^:]+):(?P<line>\d+)')
FAILED = re.compile(r'^FAILED: (?P<target>.+)$')
SOURCE = re.compile(r'\s-c\s+(?P<src>\S+\.(?:c|cc|cpp|cxx|S|s))(?:\s|$)')


def load_owners(path):
    rules = []
    for raw in open(path, encoding='utf-8'):
        line = raw.split('#', 1)[0].strip()
        if line:
            pattern, owner = line.split(None, 1)
            rules.append((pattern, owner.strip()))
    return rules


def normalize(path):
    path = path.strip().replace('\\', '/')
    for prefix in (ROOT + '/', '/c/ReFueled/repo/'):
        if path.lower().startswith(prefix.lower()):
            return path[len(prefix):]
    # GPU objects are built in out/win/gpu with sources given relative to it.
    while path.startswith('../'):
        path = path[3:]
    if path.startswith('out/win/gpu/'):
        return path
    return path


def owner_of(rules, path):
    path = normalize(path)
    low = path.lower()
    if low.startswith('/clang64/') or '/msys64/' in low:
        return 'F5-toolchain'
    for pattern, owner in rules:
        if fnmatch.fnmatchcase(path, pattern):
            return owner
    return 'Opus-unbekannt'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('census', help='out/win/census')
    parser.add_argument('--owners', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), 'owners.txt'))
    args = parser.parse_args()
    rules = load_owners(args.owners)
    errors = defaultdict(list)       # owner -> [(path, line, text)]
    failed_units = defaultdict(set)  # owner -> {translation unit}
    for name in sorted(os.listdir(args.census)):
        if not name.endswith('.log'):
            continue
        if name.startswith('unit='):
            unit = normalize(name[5:-4].replace('__', '/'))
            failed_units[owner_of(rules, unit)].add(unit)
        last_link = None
        for raw in open(os.path.join(args.census, name), encoding='utf-8', errors='replace'):
            line = raw.rstrip('\r\n')
            m = FAILED.match(line)
            if m:
                continue
            m = SOURCE.search(line)
            if m and name.startswith('gpu'):
                unit = normalize(m.group('src'))
                failed_units[owner_of(rules, unit)].add(unit)
                continue
            m = DIAG.match(line)
            if m:
                errors[owner_of(rules, m.group('path'))].append((normalize(m.group('path')), m.group('line'), m.group('text')))
                continue
            m = CMAKE.match(line)
            if m:
                errors[owner_of(rules, m.group('path'))].append((normalize(m.group('path')), m.group('line'), 'CMake Error'))
                continue
            m = TOOL.match(line)
            if m:
                if m.group('text').startswith('linker command failed'):
                    continue
                errors['link'].append(('(link)', '', m.group('text')))
                last_link = len(errors['link']) - 1
                continue
            m = REF.search(line)
            if m and last_link is not None:
                path, _, text = errors['link'][last_link]
                if '[referenced by' not in text:
                    where = m.group('where')
                    inner = re.search(r'\(([^)]+)\)\s*$', where)
                    where = inner.group(1) if inner else where
                    where = re.sub(r':\d+$', '', where.strip())
                    errors['link'][last_link] = (path, '', f'{text} [referenced by {normalize(where)} -> {owner_of(rules, where)}]')
    owners = sorted(set(errors) | set(failed_units))
    total = sum(len(v) for v in errors.values())
    out_md = os.path.join(args.census, 'summary.md')
    with open(out_md, 'w', encoding='utf-8', newline='\n') as md, \
         open(os.path.join(args.census, 'summary.tsv'), 'w', encoding='utf-8', newline='\n') as tsv:
        md.write('| Paket | Fehler | Dateien mit Fehler | Einheiten fehlgeschlagen |\n|---|---|---|---|\n')
        tsv.write('owner\terrors\tfiles\tunits\n')
        for owner in owners:
            files = sorted({p for p, _, _ in errors[owner]})
            md.write(f'| {owner} | {len(errors[owner])} | {len(files)} | {len(failed_units[owner])} |\n')
            tsv.write(f'{owner}\t{len(errors[owner])}\t{len(files)}\t{len(failed_units[owner])}\n')
        md.write(f'| **gesamt** | {total} | | {sum(len(v) for v in failed_units.values())} |\n\n')
        for owner in owners:
            md.write(f'## {owner}\n\n')
            for unit in sorted(failed_units[owner]):
                md.write(f'- fehlgeschlagen: `{unit}`\n')
            for path, line, text in errors[owner][:300]:
                md.write(f'- `{path}:{line}` {text}\n')
            if len(errors[owner]) > 300:
                md.write(f'- … {len(errors[owner]) - 300} weitere\n')
            md.write('\n')
    print(f'{total} errors in {len(owners)} groups -> {out_md}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
