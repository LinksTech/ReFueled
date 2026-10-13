#!/usr/bin/env bash
# refueled: Windows build (MSYS2 CLANG64). Output goes to out/win/:
#   1. libpf.a            the platform layer (src/platform/pf_*_win32.{c,S}, pf_common.c, pf_lowheap.c)
#   2. obj/common/*.o     the common sources listed in src/platform/WIN32_SOURCES.txt
#   3. libatrac9.a        third_party/LibAtrac9
#   4. gpu/               the GPU library (CMake + Ninja), static libbbgpu.a
#   5. sacredfire.res.o   manifest + resources (llvm-windres)
#   6. sacredfire.exe     the link of all of it, plus out/win/VERSION
# Options: --test            build and run tests/refueled/test_*.c against libpf.a
#          --no-gpu          skip stages 4 and 6 (platform + runtime objects only)
#          --skip-STAGE      skip one stage, STAGE = libpf, common, atrac9, gpu, resource, link
#                            (a skipped stage uses what an earlier run left in out/win/)
# The Linux build stays in build.sh.
set -euo pipefail
cd -- "$(dirname -- "$0")"

if [[ ${MSYSTEM:-} != CLANG64 ]]; then
    echo "build-windows.sh must run in the MSYS2 CLANG64 shell (MSYSTEM=${MSYSTEM:-unset}); see docs/refueled/TOOLCHAIN.md." >&2
    exit 1
fi

run_tests=0
declare -A skip=()
for argument in "$@"; do
    case $argument in
        --test) run_tests=1 ;;
        --no-gpu) skip[gpu]=1; skip[link]=1 ;;
        --skip-libpf|--skip-common|--skip-atrac9|--skip-gpu|--skip-resource|--skip-link) skip[${argument#--skip-}]=1 ;;
        *) echo "Unknown argument: $argument (usage: build-windows.sh [--test] [--no-gpu] [--skip-libpf|common|atrac9|gpu|resource|link])" >&2; exit 1 ;;
    esac
done

CC=${CC:-clang}
CXX=${CXX:-clang++}
out=out/win
mkdir -p "$out/obj/common"

cflags=(-std=c11 -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -O2 -g -gcodeview -Wall -Wextra -Werror -Isrc -I.)
if [[ -f src/platform/pf_posix_gap_win32.h ]]; then
    cflags+=(-include src/platform/pf_posix_gap_win32.h)
fi
ldflags=(-fuse-ld=lld -Wl,--disable-high-entropy-va -Wl,--disable-dynamicbase)

# ---- 1. Platform layer -> out/win/libpf.a ------------------------------------------------------
if [[ -z ${skip[libpf]:-} ]]; then
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
    rm -f "$out/libpf.a"
    llvm-ar rcs "$out/libpf.a" "${objects[@]}"
    echo "built $out/libpf.a (${#objects[@]} objects)"
fi

# ---- 2. Common sources (WIN32_SOURCES.txt) -----------------------------------------------------
# Same flags plus the Vulkan/SDL3 include paths; the objects stay out of libpf.a.
read -r -a includes <<< "$(pkg-config --cflags vulkan sdl3)"
common_objects=()
while IFS= read -r line || [[ -n $line ]]; do
    line=${line%%#*}
    line=${line//$'\r'/}
    line=${line#"${line%%[![:space:]]*}"}
    line=${line%"${line##*[![:space:]]}"}
    [[ -z $line ]] && continue
    object=$out/obj/common/$(basename -- "${line%.*}").o
    if [[ -z ${skip[common]:-} ]]; then
        "$CC" "${cflags[@]}" "${includes[@]}" -c "$line" -o "$object"
    fi
    common_objects+=("$object")
done < src/platform/WIN32_SOURCES.txt
if [[ -z ${skip[common]:-} ]]; then echo "built ${#common_objects[@]} common objects in $out/obj/common"; fi

# ---- 3. LibAtrac9 -> out/win/libatrac9.a -------------------------------------------------------
# Third-party decoder: compiled once, without this project's -Werror policy and the gap include.
if [[ -z ${skip[atrac9]:-} ]]; then
    atrac9=(third_party/LibAtrac9/C/src/*.c)
    if [[ ! -f $out/libatrac9.a || -n $(find third_party/LibAtrac9/C/src -newer "$out/libatrac9.a" -name '*.c') ]]; then
        rm -rf "$out/atrac9" && mkdir -p "$out/atrac9"
        for source in "${atrac9[@]}"; do "$CC" -std=c99 -O2 -g -gcodeview -w -c "$source" -o "$out/atrac9/$(basename "${source%.c}").o"; done
        rm -f "$out/libatrac9.a"
        llvm-ar rcs "$out/libatrac9.a" "$out"/atrac9/*.o
    fi
    echo "built $out/libatrac9.a"
fi

# ---- 4. GPU library -> out/win/gpu/libbbgpu.a --------------------------------------------------
# From build.sh, verbatim. bbport 0.51 (issue 124): a checkout updated from an earlier version
# keeps that version's patch applied, and the new one applies neither way ("patch does not
# apply"): the submodule's tree holds nothing but these patches, so it is reset and they are
# applied again.
fsr_patches_apply() {
    for patch in gpu/patches/fsr-vulkan/*.patch; do
        if ! git -C gpu/third_party/fsr-vulkan apply --reverse --check "$PWD/$patch" 2>/dev/null; then
            git -C gpu/third_party/fsr-vulkan apply "$@" "$PWD/$patch" || return 1
        fi
    done
}
if [[ -z ${skip[gpu]:-} ]]; then
    # Header-only dependencies (magic_enum, VulkanMemoryAllocator, xbyak) and miniz, which MSYS2
    # does not carry: both scripts install into out/deps/prefix.
    if [[ ! -f out/deps/prefix/include/vk_mem_alloc.h ]]; then bash tools/deps.sh; fi
    bash tools/deps-windows.sh
    # Submodules and this port's changes to FSR-Vulkan, as build.sh.
    if [[ ! -f gpu/third_party/fsr-vulkan/CMakeLists.txt || ! -f gpu/third_party/imgui/imgui.h ]]; then
        git submodule update --init --recursive
    fi
    if ! fsr_patches_apply --check 2>/dev/null; then
        echo 'FSR-Vulkan: an earlier version of the port changed it; resetting it and patching again'
        git -C gpu/third_party/fsr-vulkan checkout -- .
    fi
    fsr_patches_apply
    # No PGO on the first runs; LTO off unless BB_LTO says otherwise. VULKAN_SDK is unset so that
    # CMake does not pick the SDK's MSVC libraries.
    env -u VULKAN_SDK CMAKE_PREFIX_PATH="$PWD/out/deps/prefix${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}" \
        cmake -S gpu -B "$out/gpu" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBB_PGO=off \
        -DBB_LTO="${BB_LTO:-OFF}" -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ >/dev/null
    echo "GPU library: PGO off, LTO ${BB_LTO:-OFF}"
    # A failed GPU build must stop here: an older libbbgpu.a would otherwise be linked silently.
    if ! env -u VULKAN_SDK ninja -C "$out/gpu" bbgpu > "$out/gpu-build.log" 2>&1; then
        grep -v '^\[' "$out/gpu-build.log" | tail -40 >&2
        echo "GPU library build failed (full log: $out/gpu-build.log)" >&2; exit 1
    fi
    echo "built GPU library in $out/gpu"
fi

# ---- 5. Manifest + resources -> out/win/sacredfire.res.o ---------------------------------------
if [[ -z ${skip[resource]:-} ]]; then
    resource_object=$PWD/$out/sacredfire.res.o
    (cd packaging/windows && llvm-windres --input sacredfire.rc --output "$resource_object" --output-format=coff)
    echo "built $out/sacredfire.res.o"
fi

# ---- 6. Link -> out/win/sacredfire.exe ---------------------------------------------------------
if [[ -z ${skip[link]:-} ]]; then
    read -r -a libraries <<< "$(pkg-config --libs vulkan sdl3)"
    read -r -a gpu_pkg_libraries <<< "$(pkg-config --libs fmt libxxhash libavformat libavcodec libavutil libswscale libswresample)"
    # bbgpu is static and CMake does not bundle its static dependencies (sirit, FSR-Vulkan, ...),
    # so every archive of the GPU build tree and of the deps prefix goes into one link group.
    gpu_archives=("$out/gpu/libbbgpu.a")
    [[ -f ${gpu_archives[0]} ]] || { echo "Missing ${gpu_archives[0]} (find $out/gpu -name 'libbbgpu.a')." >&2; exit 1; }
    while IFS= read -r archive; do
        [[ $archive == "${gpu_archives[0]}" ]] || gpu_archives+=("$archive")
    done < <(find "$out/gpu" out/deps/prefix/lib -name '*.a' 2>/dev/null | sort)
    zydis=()
    if ! printf '%s\n' "${gpu_archives[@]}" | grep -qi 'zydis'; then zydis=(-lZydis -lZycore); fi
    "$CXX" -O2 -g -gcodeview "${ldflags[@]}" -Wl,--pdb="$out/sacredfire.pdb" \
        "${common_objects[@]}" "$out/libpf.a" "$out/libatrac9.a" "$out/sacredfire.res.o" \
        -Wl,--start-group "${gpu_archives[@]}" "${zydis[@]}" -Wl,--end-group \
        "${gpu_pkg_libraries[@]}" "${libraries[@]}" -lpthread -lm -o "$out/sacredfire.exe"
    echo "Built $PWD/$out/sacredfire.exe"
    # out/win/VERSION: the source revision this build came from (as build.sh): the nearest v* tag,
    # else the commit; "-dirty" when tracked files differ from it. The FSR-Vulkan patch applied
    # inside its submodule does not count as a change.
    version=$(git describe --tags --match 'v*' --always 2>/dev/null || echo unknown)
    git update-index -q --refresh 2>/dev/null || true
    if ! git diff-index --quiet --ignore-submodules=dirty HEAD -- 2>/dev/null; then version="$version-dirty"; fi
    printf '%s\n' "$version" > "$out/VERSION"
    echo "Version: $version ($out/VERSION)"
fi

# ---- Tests -------------------------------------------------------------------------------------
# The platform tests only; the existing tests/test_*.c follow with T1.
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
