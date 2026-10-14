#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# refueled: Windows launcher (MSYS2 CLANG64 bash), the counterpart of run.sh. Same steps in the same
# order (game check, prepare, link, patches, content profile, start); everything Linux-only is gone
# (see docs/refueled/log/T2c.md). Untested until CLANG64 and a 1.21 game folder exist.
#
# Required: BB_GAME_DIR, the game folder (C:\... or /c/...). It is only ever READ.
# BB_DRY_RUN=1: print the commands, run nothing, write nothing.
# BB_GUARD_SELFTEST=1: check the deletion guard against BB_GAME_DIR and exit.
set -euo pipefail
cd -- "$(dirname -- "$0")"

dry=${BB_DRY_RUN:-0}

# ---- environment ------------------------------------------------------------------------------
if [[ ${MSYSTEM:-} != CLANG64 ]]; then
    if [[ $dry == 1 ]]; then
        echo "run-windows.sh: warning: MSYSTEM=${MSYSTEM:-unset}, not CLANG64 (continuing because BB_DRY_RUN=1)." >&2
    else
        echo "run-windows.sh must run in the MSYS2 CLANG64 shell (MSYSTEM=${MSYSTEM:-unset}); see docs/refueled/TOOLCHAIN.md." >&2
        exit 1
    fi
fi

# Print (dry run) or execute one command. Every state-changing command goes through here.
run() {
    if [[ $dry == 1 ]]; then
        printf '+'; printf ' %q' "$@"; printf '\n'
    else
        "$@"
    fi
}

# Native (Windows) programs get Windows paths: arguments are passed exactly as written here, so the
# automatic MSYS path conversion is switched off and every path is converted with cygpath instead.
export MSYS2_ARG_CONV_EXCL='*'
export PYTHONUTF8=1                # scripts read/write text as UTF-8 whatever the console code page is
export PYTHONDONTWRITEBYTECODE=1   # no __pycache__ in the checkout
posix() { cygpath -u -- "$1"; }    # for bash itself: tests, mkdir, realpath
winp() { cygpath -w -- "$1"; }     # for sacredfire.exe and python.exe: backslash form, no ambiguity

# ---- paths ------------------------------------------------------------------------------------
# All of these may arrive as C:\... or /c/...; from here on they are absolute MSYS paths.
data=$(realpath -m -- "$(posix "${BB_DATA_DIR:-.}")")
out=$data/out
user=$(realpath -m -- "$(posix "${BB_USER_DIR:-$data/user}")")
export BB_CONFIG=$(realpath -m -- "$(posix "${BB_CONFIG:-$data/bbport.ini}")")

if [[ -z ${BB_GAME_DIR:-} ]]; then
    echo 'BB_GAME_DIR is required (the game folder with eboot.bin, e.g. C:\path\to\CUSA13795); there is no default.' >&2
    exit 1
fi
game=$(realpath -m -- "$(posix "$BB_GAME_DIR")")

# ---- deletion guard ---------------------------------------------------------------------------
# Nothing in this script may delete the game folder. The only deletions are of (a) a private
# "mod-game-*" view directly below out/ and (b) the shader cache user/cache/CUSA13795. Both are
# checked by real path, and a path that is, contains or lies inside the game folder is refused.
inside() { [[ $1 == "$2" || $1 == "$2"/* ]]; }   # is $1 equal to or below $2 (both real paths)
refuses_game() {
    local target=$1
    inside "$target" "$game" || inside "$game" "$target"
}
may_delete_overlay() {   # out/mod-game-*
    local target out_real
    target=$(realpath -m -- "$1"); out_real=$(realpath -m -- "$out")
    [[ $(dirname -- "$target") == "$out_real" && $(basename -- "$target") == mod-game-* ]] && ! refuses_game "$target"
}
may_delete_cache() {   # <user>/cache/CUSA13795
    local target user_real
    target=$(realpath -m -- "$1"); user_real=$(realpath -m -- "$user")
    [[ $target == "$user_real/cache/CUSA13795" ]] && ! refuses_game "$target"
}
guarded_rm() {   # guarded_rm overlay|cache PATH
    local check=may_delete_$1
    if "$check" "$2"; then
        run rm -rf -- "$(realpath -m -- "$2")"
    else
        echo "run-windows.sh: refusing to delete '$2' (not an allowed target; the game folder is never deleted)." >&2
        return 1
    fi
}
if [[ ${BB_GUARD_SELFTEST:-0} == 1 ]]; then
    check() {   # check EXPECTED KIND PATH
        local got=refused
        "may_delete_$2" "$3" && got=allowed
        printf '%-8s %-8s %-8s %s\n' "$([[ $got == "$1" ]] && echo ok || echo FAIL)" "$got" "$2" "$3"
        [[ $got == "$1" ]] || selftest_failed=1
    }
    selftest_failed=0
    check allowed overlay "$out/mod-game-abc123"
    check refused overlay "$game"
    check refused overlay "$out"
    check refused overlay "$out/other"
    check refused overlay "$out/mod-game-abc123/sub"
    check refused overlay "$game/mod-game-abc123"
    check refused overlay "$out/../mod-game-abc123"
    check allowed cache "$user/cache/CUSA13795"
    check refused cache "$user/cache"
    check refused cache "$game"
    check refused cache "$game/cache/CUSA13795"
    exit "$selftest_failed"
fi

if [[ ${1:-} == --software ]]; then
    echo '--software (Lavapipe) is a Linux option and is ignored on Windows.' >&2
    shift
fi

# ---- Python (MSYS2's own, never the Windows Store placeholder) --------------------------------
if [[ -z ${PYTHON:-} ]]; then
    for candidate in "${MSYSTEM_PREFIX:-/clang64}/bin/python3" "${MSYSTEM_PREFIX:-/clang64}/bin/python" /clang64/bin/python3 /clang64/bin/python /usr/bin/python3; do
        if [[ -x $candidate ]]; then PYTHON=$candidate; break; fi
    done
    if [[ -z ${PYTHON:-} ]]; then
        found=$(command -v python3 || true)
        if [[ -n $found && $found != *WindowsApps* ]]; then PYTHON=$found; fi
    fi
fi
if [[ -z ${PYTHON:-} ]]; then
    if [[ $dry == 1 ]]; then PYTHON=python3
    else echo 'No MSYS2 Python found (pacman -S mingw-w64-clang-x86_64-python) or set PYTHON.' >&2; exit 1; fi
fi
if [[ $dry != 1 ]] && ! "$PYTHON" -c 'import sys; sys.exit(sys.version_info < (3, 10))' 2> /dev/null; then
    echo "$PYTHON is not a working Python >= 3.10 (the Windows Store placeholder does not count); set PYTHON." >&2
    exit 1
fi

# ---- game folder: read-only checks ------------------------------------------------------------
if [[ $dry != 1 && ! -f $game/eboot.bin ]]; then
    echo "No eboot.bin in $game (set BB_GAME_DIR)." >&2
    exit 1
fi
game_w=$(winp "$game")
out_w=$(winp "$(realpath -m -- "$out")")
user_w=$(winp "$(realpath -m -- "$user")")
config_w=$(winp "$(realpath -m -- "$BB_CONFIG")")

# ---- temporary files stay below out/ (TMP/TEMP of every started program) ----------------------
tmp=$out/win/tmp
run mkdir -p -- "$out" "$tmp"
export TMP TEMP
TMP=$(winp "$(realpath -m -- "$tmp")"); TEMP=$TMP

# ---- BB_SAVE_LOG: this run's output also goes to <data>/logs/<time>.log (CRs of the exe dropped) -
if [[ ${BB_SAVE_LOG:-} == 1 ]]; then
    logs=$data/logs
    run mkdir -p -- "$logs"
    printf -v stamp '%(%Y%m%d_%H%M%S)T' -1
    export BB_FRAME_STATS=1 BB_FRAME_LOG=${BB_FRAME_LOG:-$(winp "$(realpath -m -- "$logs")")\\$stamp.frames.csv}
    export BB_READBACK_LOG=${BB_READBACK_LOG:-$(winp "$(realpath -m -- "$logs")")\\$stamp.readbacks.csv}
    echo "Log: $logs/$stamp.log"
    if [[ $dry != 1 ]]; then
        exec 3> "$logs/$stamp.log"
        exec > >(while IFS= read -r line || [[ -n $line ]]; do line=${line%$'\r'}; printf '%s\n' "$line"; printf '%s\n' "$line" >&3; done) 2>&1
    fi
fi

# ---- game check (scripts/game_check.py): prints the profile name, or fails with the reason ----
if [[ $dry == 1 ]]; then
    printf '+ profile=$('; printf ' %q' "$PYTHON" scripts/game_check.py "$game_w"; printf ' )\n'
    profile=ctr
else
    # stderr (the reason, e.g. missing_update) is not captured: it goes straight to the terminal.
    profile=$("$PYTHON" scripts/game_check.py "$game_w" | tr -d '\r') || exit 2
fi
export BB_GAME_PROFILE=${profile:-ctr}
echo "Game profile: $BB_GAME_PROFILE"

# ---- mods: virtual overlay, no links (docs/refueled/MODLOADER.md section 3) --------------------
# Symbolic links need rights an ordinary user does not have, so run.sh's link tree is not used here:
# scripts/mods.py --manifest writes <out>\mods.manifest (same selection) and the runtime resolves
# /app0 through it (BB_MOD_MANIFEST). Same condition as run.sh: mods on (BB_MODS_ENABLED not 0)
# and a mods folder or the shadPS4-style <game>-mods folder. Otherwise the variable stays unset.
mods_dir=$(realpath -m -- "$(posix "${BB_MODS_DIR:-$data/mods}")")
if [[ ${BB_MODS_ENABLED:-1} != 0 && ( -d $mods_dir || -d $game-mods ) ]]; then
    run "$PYTHON" scripts/mods.py "$game_w" --out "$out_w" --mods-dir "$(winp "$mods_dir")" \
        --config "$(winp "$(realpath -m -- "$(posix "${BB_MODS_CONFIG:-$data/mods.json}")")")" \
        --enabled "${BB_MODS_ENABLED:-1}" --manifest "$out_w\\mods.manifest"
    export BB_MOD_MANIFEST="$out_w\\mods.manifest"
    echo "Mods: virtual overlay, BB_MOD_MANIFEST=$BB_MOD_MANIFEST"
else
    unset BB_MOD_MANIFEST
fi

# ---- prepare and link (all write only into out/) ----------------------------------------------
run "$PYTHON" scripts/prepare.py "$game_w" --out "$out_w"
run "$PYTHON" scripts/link_libc.py "$game_w" --out "$out_w"
run "$PYTHON" scripts/link_modules.py "$game_w" --out "$out_w" --modules libc.prx libSceFios2.prx libSceNpToolkit2.prx
# Symbol names for this eboot's host-side imports. refueled: from the local copy of shadPS4's
# aerolib.inl (third_party/shadps4-aerolib, shadPS4 commit b4e7ae7, GPL-2.0-or-later) instead of a
# download; run.sh keeps fetching it into out/aerolib.inl.
run "$PYTHON" tools/gen_import_names.py --out "$out_w" --aerolib "$(winp "$PWD/third_party/shadps4-aerolib/aerolib.inl")"
run "$PYTHON" scripts/content_profile.py "$game_w" --out "$out_w" --sku "${BB_CONTENT_SKU:-full}"
fps=${BB_FPS:-30}
run "$PYTHON" scripts/patches.py --patches-dir "$(winp "$(realpath -m -- "${BB_PATCHES_DIR:-patches}")")" \
    --patches-config "$(winp "$(realpath -m -- "${BB_PATCHES_CONFIG:-$data/patches.json}")")" \
    --app-version 01.21 --title-id CUSA13795 --out "$out_w" --settings "$config_w"
patch_args=(--patches "$out_w\\patches.bin")

# ---- runtime switches (as run.sh; Linux-only ones are fixed or dropped) -----------------------
export BB_NEO=${BB_NEO:-0}
echo "PS4 Pro mode: ${BB_NEO} (BB_NEO=1 turns it on)"
if [[ ${BB_CLEAR_SHADER_CACHE:-0} == 1 ]]; then
    guarded_rm cache "$user/cache/CUSA13795"
    echo "Shader cache: cleared for this launch (BB_CLEAR_SHADER_CACHE=1)"
fi
# bbport.ini is written by the game in text mode (CRLF on Windows): drop the CRs before grep -x.
if [[ -f $BB_CONFIG ]] && tr -d '\r' < "$BB_CONFIG" | grep -qx 'patch_60fps=1'; then
    export BB_VBLANK_HZ=${BB_VBLANK_HZ:-480}
    echo "60 FPS patch on: virtual vblank ${BB_VBLANK_HZ} Hz"
fi
export BB_PREUPLOAD=${BB_PREUPLOAD:-1}
export BB_GUEST_IN_PLACE=0         # dma-buf guest memory is Linux/AMD only (D12); BB_PC_MODEL/BB_AS_0_3 are ignored
export BB_UFFD=0                   # userfaultfd is Linux only (D13)
export BB_COPY_GPU_BUFFERS=${BB_COPY_GPU_BUFFERS:-1}
export BB_GPU_WRITE_TWINS=${BB_GPU_WRITE_TWINS:-1} BB_GPU_WRITE_TWINS_MAX=${BB_GPU_WRITE_TWINS_MAX:-65536}
if [[ -z ${BB_VBLANK_HZ:-} ]]; then
    case $fps in uncap) export BB_VBLANK_HZ=480 ;; 90) export BB_VBLANK_HZ=90 ;; *) export BB_VBLANK_HZ=60 ;; esac
fi

# ---- the game executable (out/win/sacredfire.exe, built by build-windows.sh) -------------------
if [[ -n ${BB_PREBUILT:-} ]]; then
    probe=${BB_PROBE:-bin/sacredfire.exe}
    version=$(cat VERSION 2> /dev/null | tr -d '\r' || true)
else
    if [[ ${BB_SKIP_BUILD:-0} != 1 ]]; then run bash build-windows.sh; fi
    probe=${BB_PROBE:-out/win/sacredfire.exe}
    version=$(cat out/win/VERSION 2> /dev/null | tr -d '\r' || true)
fi
if [[ $dry != 1 && ! -f $probe ]]; then
    echo "$probe not found (build it with: bash build-windows.sh)." >&2
    exit 1
fi
echo "SacredFire ${version:-unknown version}"

# The user directory (saves, shader cache) must exist and be named to the GPU library before the game starts.
export BB_GPU_USER_DIR=$user_w
run mkdir -p -- "$user"
probe_args=("$out_w\\boot-linked.bin" --import-names "$out_w\\import_names.txt" --content-profile "$out_w\\content.bin"
    "${patch_args[@]}" --app0 "$game_w" --user "$user_w" --timeout "${BB_TIMEOUT:-0}" "$@")
if [[ $dry == 1 ]]; then
    printf '+ exec'; printf ' %q' "$probe" "${probe_args[@]}"; printf '\n'
    exit 0
fi
# refueled (DBG): fatal faults print registers, stack and rbp chain (docs/refueled/DEBUG-M2.md); BB_CRASH_DETAIL=0 turns it off.
export BB_CRASH_DETAIL="${BB_CRASH_DETAIL:-1}"
# refueled: the in-game settings menu restarts the game through this script (patches for a new
# render resolution are generated here); sacredfire.exe starts BB_RESTART_COMMAND
# (pf_restart_through_launcher) instead of only its own command line.
export BB_RESTART_COMMAND="\"$(cygpath -w /usr/bin/bash.exe)\" -lc \"cd '$(pwd)' && exec ./run-windows.sh\""
exec "$probe" "${probe_args[@]}"
