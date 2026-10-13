#!/bin/sh
# refueled: interim check until Nix (F8) is there. Builds and runs every platform test on Linux against all pf_*_linux.c + common sources.
set -e
cd "${1:-/mnt/c/nitro-ps4-port}"
out="/mnt/c/CTR Reload/tmp/refueled/linux-tests"; mkdir -p "$out"
srcs="src/platform/pf_common.c src/platform/pf_lowheap.c $(ls src/platform/pf_*_linux.c)"
fail=0
for t in tests/refueled/test_*.c; do
  n=$(basename "$t" .c)
  case $n in test_stubs|test_memory) echo "SKIP $n"; continue;; esac
  extra=""; [ "$n" = test_sysv_abi ] && extra="tests/refueled/sysv_caller.S"
  if [ "$n" = test_sysv_abi ]; then s=""; else s="$srcs"; fi
  if gcc -std=c11 -D_GNU_SOURCE -O2 -g -Wall -Wextra -Werror -pthread -rdynamic -Isrc -Isrc/platform "$t" $extra $s -ldl -o "$out/$n" && (cd "$out" && "./$n" >"$n.out" 2>&1); then echo "PASS $n: $(tail -1 "$out/$n.out")"; else echo "FAIL $n"; tail -5 "$out/$n.out" 2>/dev/null; fail=1; fi
done
exit $fail
