#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# refueled: builds and runs every platform test on Linux against all pf_*_linux.c + common sources.
# Usage: sh docs/refueled/tools/linux-platform-tests.sh [REPO_DIR]   (default: current directory = repo root)
# Output goes to <repo>/out/platform-tests/. Exit != 0 if any test fails to build or run.
set -e
cd "${1:-.}"
out="out/platform-tests"; mkdir -p "$out"
srcs="src/platform/pf_common.c src/platform/pf_lowheap.c $(ls src/platform/pf_*_linux.c)"
fail=0
for t in tests/refueled/test_*.c; do
  n=$(basename "$t" .c)
  case $n in test_stubs) echo "SKIP $n"; continue;; esac
  extra=""; [ "$n" = test_sysv_abi ] && extra="tests/refueled/sysv_caller.S"
  if [ "$n" = test_sysv_abi ]; then s=""; else s="$srcs"; fi
  if gcc -std=c11 -D_GNU_SOURCE -O2 -g -Wall -Wextra -Werror -pthread -rdynamic -Isrc -Isrc/platform "$t" $extra $s -ldl -o "$out/$n" && (cd "$out" && "./$n" >"$n.out" 2>&1); then echo "PASS $n: $(tail -1 "$out/$n.out")"; else echo "FAIL $n"; tail -5 "$out/$n.out" 2>/dev/null; fail=1; fi
done
exit $fail
