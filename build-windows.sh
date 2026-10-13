#!/usr/bin/env bash
# refueled: Windows build (MSYS2 CLANG64). Builds the platform layer into out/win/libpf.a plus the
# common sources listed in src/platform/WIN32_SOURCES.txt; `--test` builds and runs every
# tests/refueled/test_*.c against libpf.a. The Linux build stays in build.sh.
set -euo pipefail
cd -- "$(dirname -- "$0")"

if [[ ${MSYSTEM:-} != CLANG64 ]]; then
    echo "build-windows.sh must run in the MSYS2 CLANG64 shell (MSYSTEM=${MSYSTEM:-unset}); see docs/refueled/TOOLCHAIN.md." >&2
    exit 1
fi

run_tests=0
for argument in "$@"; do
    case $argument in
        --test) run_tests=1 ;;
        *) echo "Unknown argument: $argument (usage: build-windows.sh [--test])" >&2; exit 1 ;;
    esac
done

CC=${CC:-clang}
out=out/win
mkdir -p "$out/obj"

cflags=(-std=c11 -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -O2 -g -gcodeview -Wall -Wextra -Werror -Isrc -I.)
if [[ -f src/platform/pf_posix_gap_win32.h ]]; then
    cflags+=(-include src/platform/pf_posix_gap_win32.h)
fi
ldflags=(-fuse-ld=lld -Wl,--disable-high-entropy-va -Wl,--disable-dynamicbase)

# ---- Platform layer -> out/win/libpf.a ---------------------------------------------------------
platform_sources=()
for pattern in src/platform/pf_*_win32.c src/platform/pf_*_win32.S src/platform/pf_common.c src/platform/pf_lowheap.c; do
    for file in $pattern; do
        [[ -f $file ]] && platform_sources+=("$file")
    done
done
if ((${#platform_sources[@]} == 0)); then echo 'No platform sources found.' >&2; exit 1; fi

objects=()
for file in "${platform_sources[@]}"; do
    object=$out/obj/$(basename -- "${file%.*}").o
    "$CC" "${cflags[@]}" -c "$file" -o "$object"
    objects+=("$object")
done

# ---- Common sources (WIN32_SOURCES.txt) --------------------------------------------------------
while IFS= read -r line || [[ -n $line ]]; do
    line=${line%%#*}
    line=${line//$'\r'/}
    line=${line#"${line%%[![:space:]]*}"}
    line=${line%"${line##*[![:space:]]}"}
    [[ -z $line ]] && continue
    object=$out/obj/$(basename -- "${line%.*}").o
    "$CC" "${cflags[@]}" -c "$line" -o "$object"
    objects+=("$object")
done < src/platform/WIN32_SOURCES.txt

rm -f "$out/libpf.a"
llvm-ar rcs "$out/libpf.a" "${objects[@]}"
echo "built $out/libpf.a (${#objects[@]} objects)"

# ---- GPU library and sacredfire.exe: not in this package (F5 / G2 / G3) ------------------------
# TODO(F5/G2/G3): GPU library build and the sacredfire.exe link go here.

# ---- Tests -------------------------------------------------------------------------------------
if ((run_tests)); then
    failed=0
    count=0
    for source in tests/refueled/test_*.c; do
        [[ -f $source ]] || continue
        name=$(basename -- "${source%.c}")
        count=$((count + 1))
        exe=$out/$name.exe
        if "$CC" "${cflags[@]}" "$source" "$out/libpf.a" "${ldflags[@]}" "-Wl,--pdb=$out/$name.pdb" -lpthread -o "$exe" \
            && "$exe"; then
            echo "PASS $name"
        else
            echo "FAIL $name"
            failed=$((failed + 1))
        fi
    done
    echo "tests: $count run, $failed failed"
    ((failed == 0)) || exit 1
fi
