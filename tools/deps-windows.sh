#!/usr/bin/env bash
# ReFueled (Windows, MSYS2 CLANG64): dependency of gpu/ that MSYS2 does not carry (miniz).
# Companion of tools/deps.sh, which stays unchanged for Linux; run both on Windows (deps.sh
# fetches magic_enum, VulkanMemoryAllocator and xbyak). The commit is the one shadPS4 pins for
# externals/miniz (b4e7ae7, the base of gpu/shadps4). It is fetched into out/deps/src and
# installed with its own CMake into out/deps/prefix, the same prefix as deps.sh, which the
# build adds to CMAKE_PREFIX_PATH. Idempotent: an installed package is skipped.
# DEPS_PREFIX overrides the prefix.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
prefix=${DEPS_PREFIX:-$PWD/out/deps/prefix}
src=$PWD/out/deps/src
mkdir -p "$prefix" "$src"
# Like deps.sh, but pinned by commit (the shadPS4 submodule is not at a tag).
fetch_and_install_commit() {
    local name=$1 url=$2 commit=$3 marker=$4; shift 4
    if [[ -e $prefix/$marker ]]; then echo "deps: $name present"; return; fi
    if [[ ! -d $src/$name/.git ]]; then
        git init -q "$src/$name"
        git -C "$src/$name" remote add origin "$url"
    fi
    git -C "$src/$name" fetch -q --depth 1 origin "$commit"
    git -C "$src/$name" checkout -q --detach FETCH_HEAD
    cmake -S "$src/$name" -B "$src/$name/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$prefix" "$@" >/dev/null
    cmake --build "$src/$name/build" >/dev/null
    cmake --install "$src/$name/build" >/dev/null
    echo "deps: $name ${commit:0:9} installed"
}
# Static library (gpu links it into bbgpu, which is static on Windows, D11); no examples or fuzzers.
fetch_and_install_commit miniz https://github.com/richgel999/miniz.git \
    174573d60290f447c13a2b1b3405de2b96e27d6c lib/libminiz.a \
    -DBUILD_SHARED_LIBS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_FUZZERS=OFF -DINSTALL_PROJECT=ON
echo "deps: prefix $prefix"
