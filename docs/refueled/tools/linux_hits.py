#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# refueled: Linux/POSIX host API hits outside the platform layer (src/platform/).
# Derived from the read-only inventory script of the 2026-10-12 survey (sf_inventory.py).
#
# Usage: python linux_hits.py [--repo DIR] [--ref SHADPS4_CHECKOUT] [--list] [--json FILE]
#
# Counts code only: comments and string literals are blanked before matching (except the
# "procfs" category, which matches paths inside strings). In gpu/shadps4 only lines that differ
# from the vendored upstream commit (b4e7ae7, --ref) count, i.e. bbport's own changes; without
# --ref gpu/shadps4 is skipped and the report says so.
import argparse, difflib, json, os, re, sys
from collections import Counter, defaultdict

# Category -> (pattern, toolchain note). "posix" categories compile unchanged with MinGW-w64
# (winpthreads, dirent.h, clock_gettime); "linux" categories need the platform layer either way.
CATEGORIES = [
    ("mmap",        r"\bmmap\s*\(|\bmunmap\s*\(|\bmprotect\s*\(|<sys/mman\.h>|\bMAP_(FIXED(_NOREPLACE)?|SHARED|PRIVATE|ANONYMOUS|NORESERVE|FAILED)\b|\bPROT_(READ|WRITE|EXEC|NONE)\b", "linux"),
    ("memfd",       r"\bmemfd_create\b|\bMFD_\w+|\bftruncate\s*\(", "linux"),
    ("fallocate",   r"\bfallocate\s*\(|\bFALLOC_FL_\w+", "linux"),
    ("mremap",      r"\bmremap\s*\(", "linux"),
    ("arch_prctl",  r"\barch_prctl\b|\bARCH_[SG]ET_[FG]S\b|<asm/prctl\.h>", "linux"),
    ("signals",     r"\bsigaction\b|\bsig(empty|fill|add|del)set\s*\(|\bpthread_sigmask\b|\bsigjmp_buf\b|\bsig(set|long)jmp\b|\bSIG(SEGV|BUS|ILL|TRAP|USR1|USR2|ALRM|PROF)\b|\braise\s*\(\s*SIG|\balarm\s*\(", "linux"),
    ("ucontext",    r"\bucontext(_t)?\b|<ucontext\.h>|\buc_mcontext\b|\bREG_(R[A-Z0-9]+|EFL)\b|\bgreg_t\b", "linux"),
    ("userfaultfd", r"userfaultfd|\bUFFD\w*", "linux"),
    ("futex",       r"\bfutex\b|\bFUTEX_\w+|\bSYS_futex\b", "linux"),
    ("syscall",     r"\bsyscall\s*\(|\bSYS_\w+|<sys/syscall\.h>", "linux"),
    ("thread-id",   r"\bgettid\s*\(|\btgkill\b|\bsched_(get|set)affinity\b|\bcpu_set_t\b|\bCPU_(SET|ISSET|ZERO|COUNT)\b|\bsched_setscheduler\b|\bSCHED_(IDLE|BATCH)\b|\bsetpriority\s*\(|\bprctl\s*\(", "linux"),
    ("pthread-np",  r"\bpthread_\w+_np\b|\bpthread_attr_setstack\b|\bpthread_condattr_setclock\b", "linux"),
    ("getdents",    r"\bgetdents(64)?\s*\(|\bSYS_getdents64\b", "linux"),
    ("ftw",         r"\bn?ftw\s*\(|<ftw\.h>|\bFTW_\w+|\bstruct\s+FTW\b", "linux"),
    ("dl",          r"\bdl(addr|sym|open|close|error)\s*\(|\bDl_info\b|<dlfcn\.h>", "linux"),
    ("misc-linux",  r"\bprocess_vm_readv\b|\bclose_range\b|\bexeclp\s*\(|\bbacktrace\s*\(|<execinfo\.h>|\bmallopt\s*\(|\bM_(ARENA_MAX|MMAP_THRESHOLD)\b|<malloc\.h>|<sys/uio\.h>", "linux"),
    ("dma-buf",     r"DMA_BUF|DmaBuf|\b(vk)?[Gg]etMemoryFdKHR\b|MemoryFdKHR|\bdma_buf\w*", "linux"),
    ("procfs",      r"\"/proc/", "linux"),
    ("pthread",     r"\bpthread_\w+|<pthread\.h>|PTHREAD_\w+", "posix"),
    ("posix-dir",   r"\b(opendir|readdir|closedir|rewinddir|fdopendir)\s*\(|\bDIR\s*\*|\bstruct\s+dirent\b|<dirent\.h>", "posix"),
    ("posix-time",  r"\bclock_gettime\s*\(|\bCLOCK_(MONOTONIC(_RAW)?|REALTIME|BOOTTIME)\b|\bnanosleep\s*\(|\busleep\s*\(", "posix"),
]
COMPILED = [(n, re.compile(p), k) for n, p, k in CATEGORIES]
EXT = (".c", ".h", ".cpp", ".hpp", ".inc")
PRIMARY = ["src", "gpu/shim", "gpu/bbgpu.h", "gpu/tools"]
SECONDARY = ["tests", "tools"]
VENDORED = "gpu/shadps4"
# The platform layer itself (headers and every pf_*_<platform> file) is the abstraction, not a hit.
PLATFORM_DIR = re.compile(r"^src/platform/")


def strip_code(text):
    """Blank comments and string/char literals, keeping line structure. Returns
    (code_lines, raw_lines)."""
    out, i, n = [], 0, len(text)
    state = None  # None, '//', '/*', '"', "'"
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if state is None:
            if c == "/" and nxt == "/": state = "//"; out.append("  "); i += 2; continue
            if c == "/" and nxt == "*": state = "/*"; out.append("  "); i += 2; continue
            if c == '"':
                # raw string literal R"delim( ... )delim"
                if i > 0 and text[i - 1] == "R":
                    m = re.match(r'"([^(\s]{0,16})\(', text[i:])
                    if m:
                        end = text.find(")" + m.group(1) + '"', i)
                        end = n if end < 0 else end + len(m.group(1)) + 2
                        out.append('""' + "".join("\n" if ch == "\n" else " " for ch in text[i + 2:end]))
                        i = end; continue
                state = '"'; out.append('"'); i += 1; continue
            if c == "'": state = "'"; out.append("'"); i += 1; continue
            out.append(c); i += 1; continue
        if state == "//":
            if c == "\n": state = None; out.append("\n")
            else: out.append(" ")
            i += 1; continue
        if state == "/*":
            if c == "*" and nxt == "/": state = None; out.append("  "); i += 2; continue
            out.append("\n" if c == "\n" else " "); i += 1; continue
        # inside a literal
        if c == "\\" and i + 1 < n: out.append("  " if nxt != "\n" else " \n"); i += 2; continue
        if c == state: state = None; out.append(c); i += 1; continue
        if c == "\n": state = None; out.append("\n"); i += 1; continue  # unterminated: resync
        out.append(" "); i += 1
    return "".join(out).split("\n"), text.split("\n")


def walk(repo, roots):
    for r in roots:
        p = os.path.join(repo, r)
        if os.path.isfile(p):
            if p.endswith(EXT): yield r.replace("\\", "/")
            continue
        for d, dirs, fs in os.walk(p):
            dirs[:] = sorted(x for x in dirs if x not in ("third_party", ".git"))
            for f in sorted(fs):
                if f.endswith(EXT):
                    yield os.path.relpath(os.path.join(d, f), repo).replace("\\", "/")


def changed_lines(path, ref_path):
    """1-based line numbers of `path` that are not identical in `ref_path` (bbport's changes)."""
    with open(path, encoding="utf-8", errors="replace") as f: a = f.read().split("\n")
    if not ref_path or not os.path.isfile(ref_path): return set(range(1, len(a) + 1)), True
    with open(ref_path, encoding="utf-8", errors="replace") as f: b = f.read().split("\n")
    keep = set()
    for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(None, b, a, autojunk=False).get_opcodes():
        if tag in ("replace", "insert"): keep.update(range(j1 + 1, j2 + 1))
    return keep, False


def scan(repo, rel, only=None):
    with open(os.path.join(repo, rel), encoding="utf-8", errors="replace") as f:
        code, raw = strip_code(f.read())
    hits = []
    for n, (cl, rl) in enumerate(zip(code, raw), 1):
        if only is not None and n not in only: continue
        for name, rx, kind in COMPILED:
            if rx.search(rl if name == "procfs" else cl):
                hits.append(dict(category=name, kind=kind, file=rel, line=n, code=rl.strip()[:200]))
    return hits


def vendored_map(rel):
    # gpu/shadps4/<p> was copied from upstream src/<p> (gpu/VENDOR.txt); externals live elsewhere.
    return "src/" + rel[len(VENDORED) + 1:]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument("--repo", default=os.path.normpath(os.path.join(here, "..", "..", "..")))
    ap.add_argument("--ref", help="shadPS4 checkout at the vendored commit (b4e7ae7)")
    ap.add_argument("--list", action="store_true", help="print every hit")
    ap.add_argument("--json", help="write the full result as JSON")
    a = ap.parse_args()
    repo = a.repo
    result = dict(repo=repo, ref=a.ref, scopes={})

    primary = []
    for rel in walk(repo, PRIMARY):
        if PLATFORM_DIR.match(rel): continue
        primary += scan(repo, rel)
    vendored, new_files = [], []
    if a.ref:
        for rel in walk(repo, [VENDORED]):
            ref_path = os.path.join(a.ref, vendored_map(rel))
            lines, is_new = changed_lines(os.path.join(repo, rel), ref_path)
            if is_new: new_files.append(rel)
            if lines: vendored += scan(repo, rel, lines)
    secondary = []
    for rel in walk(repo, SECONDARY):
        secondary += scan(repo, rel)
    ifdefs = 0
    for rel in walk(repo, PRIMARY):
        if rel.startswith("src/platform/"): continue
        with open(os.path.join(repo, rel), encoding="utf-8", errors="replace") as f:
            ifdefs += sum(1 for l in f if re.match(r"\s*#\s*(if|elif).*(_WIN32|__linux__|_MSC_VER|__MINGW)", l))

    def summary(hits):
        by = defaultdict(Counter)
        for h in hits: by[h["category"]][h["file"]] += 1
        total = {k: sum(by[k].values()) for k, _, _ in CATEGORIES}
        linux = sum(v for (k, _, kind) in CATEGORIES for kk, v in total.items() if kk == k and kind == "linux")
        posix = sum(v for (k, _, kind) in CATEGORIES for kk, v in total.items() if kk == k and kind == "posix")
        files = len({h["file"] for h in hits})
        return dict(total=len(hits), linux_only=linux, posix=posix, files=files, by_category=total)

    result["scopes"]["primary"] = summary(primary)
    result["scopes"]["vendored_bbport_lines"] = summary(vendored) if a.ref else None
    result["scopes"]["secondary_tests_tools"] = summary(secondary)
    result["vendored_new_files"] = new_files
    result["platform_ifdefs_outside_platform"] = ifdefs
    vend = result["scopes"]["vendored_bbport_lines"] or {}
    metric = result["scopes"]["primary"]["linux_only"] + vend.get("linux_only", 0)
    total = result["scopes"]["primary"]["total"] + vend.get("total", 0)
    result["metric"] = metric
    result["metric_with_posix"] = total

    print("refueled linux_hits: Linux/POSIX host API lines outside src/platform/")
    print(f"  METRIC  linux-only lines (primary + bbport lines in gpu/shadps4) = {metric}   (target 0)"
          + ("" if a.ref else "   [gpu/shadps4 skipped: no --ref]"))
    print(f"  INFO    including the POSIX subset (stays in common code with MinGW) = {total}")
    for scope, s in result["scopes"].items():
        if not s: continue
        print(f"  {scope}: {s['total']} lines ({s['linux_only']} linux-only, {s['posix']} posix) in {s['files']} files")
        print("    " + ", ".join(f"{k}={v}" for k, v in s["by_category"].items() if v))
    print(f"  platform #if/#elif lines outside src/platform: {ifdefs}")
    if a.list:
        for scope, hits in (("primary", primary), ("vendored", vendored), ("secondary", secondary)):
            for h in hits:
                print(f"{scope}\t{h['category']}\t{h['file']}:{h['line']}\t{h['code']}")
    if a.json:
        result["hits"] = dict(primary=primary, vendored=vendored, secondary=secondary)
        with open(a.json, "w", encoding="utf-8", newline="\n") as f: json.dump(result, f, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
