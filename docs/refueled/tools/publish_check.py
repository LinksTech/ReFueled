#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# refueled: publish check for the public ReFueled repository (pre-push hook).
"""Checks everything a push would publish for game files, derived game data, build output, SDK
material and personal data, and fails on any finding.

What is checked: every commit and every blob reachable from the pushed tips that is not already
public. "Already public" is everything reachable from the upstream refs (default: SacredFire's
refs/remotes/origin/main) and from the target remote's own refs, i.e. history that is on GitHub
anyway. On the first push to an empty or new remote that means our whole own history.

Rules (a hit blocks the push; nothing is rewritten, fix or allow by hand):
  path    game files, game asset formats, images, build output, dumps, caches, logs, *.bin
  magic   ELF/SELF/PKG/SFO/DDS/IGZ/minidump/PNG/JPEG headers in any blob
  size    binary blobs over 1 MiB
  game    blob identical to a file in the local game folder (--game-dir, default $BB_GAME_DIR)
  user    home-directory paths with a user name (C:\\Users\\x, /home/x, /c/Users/x)
  private terms from the local environment (user name, git identity, $PUBLISH_CHECK_PRIVATE,
          <git-common-dir>/publish_private.txt); read at run time so this file never contains them
  email   e-mail addresses except placeholders and GitHub noreply addresses
  sdk     Sony SDK markers (confidential headers, SIE copyright lines)
  ident   commit author/committer e-mail outside the allowed placeholders
Warnings (do not block): local machine paths without a user name, first names from the git
identity or `warn:` lines. Allowed exceptions live in publish_allow.txt next to this file: one
`<rule> <path-glob>  # reason` per line, decided and reviewed by hand.

Usage:
  publish_check.py --hook REMOTE URL          (pre-push; ref lines on stdin)
  publish_check.py --range TIP [--remote R]   (dry run for one tip, e.g. HEAD)
  publish_check.py --install                  (writes .git/hooks/pre-push)
"""
import argparse
import fnmatch
import hashlib
import os
import re
import subprocess
import sys
from pathlib import Path, PurePosixPath

HERE = Path(__file__).resolve().parent
ALLOW_FILE = HERE / 'publish_allow.txt'
ZERO = '0' * 40
MiB = 1024 * 1024
TEXT_SCAN_LIMIT = 32 * MiB

GAME_NAMES = {'eboot.bin', 'param.sfo', 'icon0.png', 'icon0.dds', 'pic0.png', 'pic1.png', 'pic1.dds',
              'snd0.at9', 'npbind.dat', 'nptitle.dat', 'license.dat', 'license.info', 'playgo-chunk.dat',
              'chunks.igx', 'memoryconfiguration.igz', 'keystone', 'last_run.log', 'perf.data',
              'profile.bin', 'vkd3d-proton.cache', 'bbport.ini', 'bbport_gnm_table.inc'}
GAME_DIRS = {'sce_sys', 'sce_module', 'out', 'build', 'dist', 'orig', 'user', 'logs', 'pgo',
             'shader_cache', 'cache', 'dumps', 'savedata'}
BAD_EXT = {
    # game executables, packages and system files
    '.elf', '.self', '.prx', '.sprx', '.pkg', '.sfo', '.bin', '.dat', '.plt', '.sig',
    # game asset formats (CTR: Alchemy igz/igx/pak, textures, audio, video)
    '.igz', '.igx', '.pak', '.arc', '.dds', '.gnf', '.at9', '.bnk', '.wem', '.bik', '.mp4',
    # images (screenshots and icons are game material until reviewed)
    '.png', '.jpg', '.jpeg', '.bmp', '.gif', '.webp', '.tga',
    # build output
    '.exe', '.dll', '.so', '.o', '.obj', '.a', '.lib', '.pdb', '.ilk', '.exp', '.dylib',
    # derived data: shader caches, memory images, dumps, recordings, logs
    '.spv', '.cache', '.dmp', '.mdmp', '.core', '.mem', '.img', '.raw', '.log', '.trace', '.rec',
}
CORE_RE = re.compile(r'^core(\.\d+)?$')
MAGIC = [
    (b'\x7fELF', 'ELF'), (b'\x4f\x15\x3d\x1d', 'SELF'), (b'\x7fCNT', 'PKG'), (b'\x00PSF', 'SFO'),
    (b'DDS ', 'DDS'), (b'\x01ZGI', 'IGZ'), (b'IGZ\x01', 'IGZ'), (b'MDMP', 'minidump'),
    (b'\x89PNG', 'PNG'), (b'\xff\xd8\xff', 'JPEG'), (b'MZ', 'PE'),
]
USER_PATH_RES = [
    re.compile(r'(?i)\b[a-z]:[\\/]{1,2}(?:users|dokumente und einstellungen|documents and settings)'
               r'[\\/]{1,2}(?!public\b|default\b|all users\b)([^\\/\s"\'<>%$*…�]+)'),
    re.compile(r'(?i)/(?:c|mnt/c)/users/(?!public\b|default\b)([^/\s"\'<>%$*…�]+)'),
    re.compile(r'/home/(?!runner\b|user\b|builder\b)([A-Za-z0-9._-]+)'),
    re.compile(r'(?<![\w.])/Users/(?!Shared\b)([A-Za-z0-9._-]+)'),
]
LOCAL_PATH_RE = re.compile(r'(?i)\b[a-z]:[\\/]{1,2}(?!users\b|windows\b|program files|msys64\b|vulkansdk\b)'
                           r'[^\\/\s"\'<>|*?]{2,40}')
EMAIL_RE = re.compile(r'[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*\.[A-Za-z]{2,}')
EMAIL_OK = re.compile(r'(?i)^(refueled@localhost|noreply@github\.com|[^@]+@users\.noreply\.github\.com|'
                      r'[^@]+@example\.(?:com|org|net))$')
IDENT_OK = re.compile(r'(?i)^(refueled@localhost|noreply@github\.com|[^@]+@users\.noreply\.github\.com)$')
SDK_RES = [
    re.compile(r'SCE CONFIDENTIAL'),
    re.compile(r'(?i)copyright\s*\(c\)\s*\d{4}(?:-\d{4})?\s*sony (?:interactive|computer) entertainment'),
    re.compile(r'(?i)playstation\(r\)\s*4 programmer tool'),
]


def git(*args, inp=None, check=True):
    r = subprocess.run(['git', *args], input=inp, capture_output=True, check=False)
    if check and r.returncode:
        sys.exit(f'publish_check: git {" ".join(args)} failed: {r.stderr.decode(errors="replace").strip()}')
    return r.stdout


def private_terms():
    """Terms that must not appear: local user name, git identity, extra terms from the environment."""
    terms, first = set(), set()
    for env in ('USERNAME', 'USER'):
        v = os.environ.get(env, '').strip()
        if len(v) >= 3:
            terms.add(v)
    name = git('config', 'user.name', check=False).decode(errors='replace').strip()
    email = git('config', 'user.email', check=False).decode(errors='replace').strip()
    if name:
        parts = name.split()
        terms.add(name)
        terms.update(p for p in parts[1:] if len(p) >= 3)
        if parts and len(parts[0]) >= 3:
            first.add(parts[0])
    if email and not IDENT_OK.match(email):
        terms.add(email)
        terms.add(email.split('@')[0])
    for v in os.environ.get('PUBLISH_CHECK_PRIVATE', '').split(','):
        if len(v.strip()) >= 3:
            terms.add(v.strip())
    # Local list, never committed: <git-common-dir>/publish_private.txt, one term per line,
    # `warn:<term>` for terms that only warn (e.g. a first name used in plan documents).
    local = Path(git('rev-parse', '--git-common-dir').decode().strip()) / 'publish_private.txt'
    if local.exists():
        for v in local.read_text(encoding='utf-8').splitlines():
            v = v.strip()
            if v.startswith('warn:') and len(v) >= 8:
                first.add(v[5:].strip())
            elif len(v) >= 3 and not v.startswith('#'):
                terms.add(v)
    first -= terms
    mk = lambda ts: [re.compile(r'(?i)(?<![A-Za-z0-9])' + re.escape(t) + r'(?![A-Za-z0-9])') for t in sorted(ts)]
    return mk(terms), mk(first)


def load_allow():
    allow = []
    if ALLOW_FILE.exists():
        for line in ALLOW_FILE.read_text(encoding='utf-8').splitlines():
            line = line.split('#', 1)[0].strip()
            if line:
                rule, glob = line.split(None, 1)
                allow.append((rule, glob.strip()))
    return allow


def allowed(allow, rule, path):
    return any(r in (rule, '*') and fnmatch.fnmatchcase(path, g) for r, g in allow)


def exempt_refs(remote, upstream):
    refs = list(upstream)
    if remote:
        out = git('for-each-ref', '--format=%(refname)', f'refs/remotes/{remote}/').decode().split()
        refs += out
    good = []
    for r in refs:
        if git('rev-parse', '--verify', '--quiet', r + '^{commit}', check=False).strip():
            good.append(r)
    return good


def scan_text(text, private, first):
    """(rule, detail) hits and warnings for one text."""
    hits, warns = [], []
    for rx in USER_PATH_RES:
        for m in rx.finditer(text):
            hits.append(('user', m.group(0)[:80]))
    for m in EMAIL_RE.finditer(text):
        if not EMAIL_OK.match(m.group(0)):
            hits.append(('email', m.group(0)))
    for rx in SDK_RES:
        m = rx.search(text)
        if m:
            hits.append(('sdk', m.group(0)[:80]))
    for rx in private:
        n = len(rx.findall(text))
        if n:
            hits.append(('private', f'{n}x private term (pattern #{private.index(rx)})'))
    for rx in first:
        n = len(rx.findall(text))
        if n:
            warns.append(('firstname', f'{n}x first name of the git identity'))
    locs = sorted({m.group(0) for m in LOCAL_PATH_RE.finditer(text)})
    if locs:
        warns.append(('localpath', ', '.join(locs[:4]) + (' ...' if len(locs) > 4 else '')))
    return hits, warns


def path_rule(path):
    p = PurePosixPath(path)
    name = p.name.lower()
    dirs = [d.lower() for d in p.parts[:-1]]
    if name in GAME_NAMES:
        return f'game/derived file name {p.name}'
    if CORE_RE.match(name):
        return 'core dump'
    for d in dirs:
        if d in GAME_DIRS:
            return f'directory {d}/'
    ext = p.suffix.lower()
    if ext in BAD_EXT:
        return f'file type {ext}'
    return None


def game_index(game_dir, sizes):
    """sha1 (git blob id) -> game file, for game files whose size matches a blob to be pushed."""
    idx = {}
    if not game_dir or not Path(game_dir).is_dir():
        return idx, False
    for root, _dirs, files in os.walk(game_dir):
        for f in files:
            fp = Path(root) / f
            try:
                size = fp.stat().st_size
            except OSError:
                continue
            if size in sizes:
                h = hashlib.sha1(b'blob %d\0' % size)
                with open(fp, 'rb') as fh:
                    for chunk in iter(lambda: fh.read(MiB), b''):
                        h.update(chunk)
                idx[h.hexdigest()] = fp.name
    return idx, True


def check(tips, remote, upstream, game_dir):
    allow = load_allow()
    private, first = private_terms()
    refs = exempt_refs(remote, upstream)
    nots = ['--not', *refs] if refs else []
    commits = git('rev-list', *tips, *nots).decode().split()
    objs = git('rev-list', '--objects', *tips, *nots).decode(errors='replace').splitlines()
    paths = {}
    for line in objs:
        sha, _, path = line.partition(' ')
        if path:
            paths.setdefault(sha, set()).add(path)
    info = git('cat-file', '--batch-check=%(objectname) %(objecttype) %(objectsize)',
               inp='\n'.join(paths).encode()).decode().splitlines()
    blobs = {}
    for line in info:
        sha, typ, size = line.split()
        if typ == 'blob':
            blobs[sha] = int(size)
    hits, warns = [], []

    def hit(rule, where, detail):
        if not allowed(allow, rule, where):
            hits.append((rule, where, detail))

    # commit metadata and messages
    for c in commits:
        meta = git('show', '-s', '--format=%ae%n%ce%n%B', c).decode(errors='replace')
        ae, ce, msg = meta.split('\n', 2)
        for e in (ae, ce):
            if not IDENT_OK.match(e):
                hits.append(('ident', f'commit {c[:12]}', e))
        h, w = scan_text(msg, private, first)
        hits += [(r, f'commit {c[:12]} message', d) for r, d in h]
        warns += [(r, f'commit {c[:12]} message', d) for r, d in w]

    gidx, game_ok = game_index(game_dir, set(blobs.values()))
    for sha, size in blobs.items():
        for path in sorted(paths[sha]):
            why = path_rule(path)
            if why:
                hit('path', path, why)
            if sha in gidx:
                hit('game', path, f'identical to game file {gidx[sha]}')
        path = sorted(paths[sha])[0]
        if size == 0:
            continue
        data = git('cat-file', 'blob', sha) if size <= TEXT_SCAN_LIMIT else None
        head = data[:16] if data is not None else git('cat-file', 'blob', sha)[:16]
        for magic, what in MAGIC:
            if head.startswith(magic) and not (what == 'PE' and size < 64):
                hit('magic', path, f'{what} header')
        binary = data is None or b'\0' in data[:8000]
        if binary and size > MiB:
            hit('size', path, f'binary blob {size / MiB:.1f} MiB')
        if data is not None and not binary:
            h, w = scan_text(data.decode('utf-8', errors='replace'), private, first)
            for r, d in h:
                hit(r, path, d)
            warns += [(r, path, d) for r, d in w if not allowed(allow, r, path)]

    print(f'publish_check: {len(commits)} commits, {len(blobs)} blobs checked '
          f'(exempt: {", ".join(refs) or "nothing"}; game folder '
          f'{"compared" if game_ok else "NOT compared (no --game-dir/BB_GAME_DIR)"})')
    seen = set()
    for r, where, d in warns:
        if (r, where) not in seen:
            seen.add((r, where))
    wcount = {}
    for r, _w in seen:
        wcount[r] = wcount.get(r, 0) + 1
    if wcount:
        print('  warnings (not blocking): ' + ', '.join(f'{r} in {n} places' for r, n in sorted(wcount.items())))
    for r, where, d in sorted(set(hits)):
        print(f'  HIT {r:8} {where}: {d}')
    if hits:
        print(f'publish_check: {len(set(hits))} hits - push blocked. Nothing was rewritten.')
        return 1
    print('publish_check: clean')
    return 0


def install():
    common = Path(git('rev-parse', '--git-common-dir').decode().strip()).resolve()
    hook = common / 'hooks' / 'pre-push'
    hook.write_text('#!/bin/sh\n# refueled: publish check (docs/refueled/tools/publish_check.py)\n'
                    'top=$(git rev-parse --show-toplevel) || exit 1\n'
                    'exec python "$top/docs/refueled/tools/publish_check.py" --hook "$@"\n',
                    encoding='utf-8', newline='\n')
    print(f'installed {hook}')
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--hook', nargs=2, metavar=('REMOTE', 'URL'))
    ap.add_argument('--range', metavar='TIP', action='append')
    ap.add_argument('--remote', help='target remote whose refs count as public (with --range)')
    ap.add_argument('--upstream', action='append', help='public upstream ref (default refs/remotes/origin/main)')
    ap.add_argument('--game-dir', default=os.environ.get('BB_GAME_DIR'))
    ap.add_argument('--install', action='store_true')
    a = ap.parse_args()
    if a.install:
        return install()
    upstream = a.upstream or ['refs/remotes/origin/main']
    if a.hook:
        remote = a.hook[0]
        tips = []
        for line in sys.stdin.read().splitlines():
            parts = line.split()
            if len(parts) == 4 and parts[1] != ZERO:
                tips.append(parts[1])
        if not tips:
            return 0
        return check(tips, remote, upstream, a.game_dir)
    if a.range:
        return check(a.range, a.remote, upstream, a.game_dir)
    ap.print_help()
    return 2


if __name__ == '__main__':
    sys.exit(main())
