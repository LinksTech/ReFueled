#!/usr/bin/env bash
# DEPRECATED (2026-10-13): replaced by .github/workflows/linux-build.yml; WSL is no longer used.
# Kept only for reference, do not extend.
# refueled F8: Linux regression run of one commit, inside WSL.
#
#   linux-regress.sh <commit-or-branch> [--ref]
#
# Creates a detached git worktree of the repository at /mnt/c/ReFueled/repo under
# /mnt/c/ReFueled/wt/regress-<shorthash>, runs there
#   1. build-test   : bash build.sh --test   (build.sh enters nix-shell shell.nix by itself)
#   2. py-unittest  : python3 -m unittest discover -s tests -p 'test_*.py'   (as CONTRIBUTING.md / CI)
#   3. shell-syntax : bash -n on the shell scripts CI checks
# writes the full log to /mnt/c/ReFueled/wt/regress-logs/<date>-<shorthash>.log, prints
# "REGRESS PASS" or "REGRESS FAIL <step>" and removes the worktree again.
#   --ref : additionally keeps the log as regress-logs/ref-<shorthash>.log (reference for later runs).
#
# No game data: BB_GAME_DIR is neither needed nor set (it is removed from the environment).
# Exit codes: 0 pass, 1 a step failed, 2 build environment missing (no Nix, no native toolchain),
#             3 usage or setup error (bad commit, worktree exists, ...).
#
# Untested until Nix is installed in the WSL (docs/refueled/TOOLCHAIN.md section 3).
set -uo pipefail

REPO=${REGRESS_REPO:-/mnt/c/ReFueled/repo}
WT_ROOT=${REGRESS_WT_ROOT:-/mnt/c/ReFueled/wt}
LOG_DIR=$WT_ROOT/regress-logs

usage() { echo "usage: $0 <commit-or-branch> [--ref]" >&2; exit 3; }

rev=""; keep_ref=0
for arg in "$@"; do
    case $arg in
        --ref) keep_ref=1 ;;
        -h|--help) usage ;;
        -*) echo "unknown option: $arg" >&2; usage ;;
        *) if [[ -n $rev ]]; then usage; fi; rev=$arg ;;
    esac
done
[[ -n $rev ]] || usage

mkdir -p "$LOG_DIR" || { echo "cannot create $LOG_DIR" >&2; exit 3; }
if ! full=$(git -C "$REPO" rev-parse --verify --quiet "$rev^{commit}"); then
    echo "REGRESS: '$rev' is not a commit in $REPO" >&2; exit 3
fi
short=$(git -C "$REPO" rev-parse --short=9 "$full")
log=$LOG_DIR/$(date +%Y-%m-%d)-$short.log
wt=$WT_ROOT/regress-$short
: > "$log" || { echo "cannot write $log" >&2; exit 3; }

# Everything below goes to the screen and to the log.
exec > >(tee -a "$log") 2>&1

say() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }
say "linux-regress: $rev -> $full (regress-$short), repo $REPO"
say "host: $(uname -srm); WSL=${WSL_DISTRO_NAME:-no}"

# ---- build environment ------------------------------------------------------------------------
# build.sh needs either nix-shell (shell.nix) or a native toolchain: pkg-config with vulkan and
# sdl3, cmake and ninja. Source a single-user Nix profile if there is one.
for profile in "$HOME/.nix-profile/etc/profile.d/nix.sh" /etc/profile.d/nix.sh; do
    if [[ -r $profile ]] && ! command -v nix-shell >/dev/null 2>&1; then
        # shellcheck disable=SC1090
        . "$profile"
    fi
done
have_nix=0; have_native=0
command -v nix-shell >/dev/null 2>&1 && have_nix=1
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists vulkan sdl3 \
    && command -v cmake >/dev/null 2>&1 && command -v ninja >/dev/null 2>&1; then
    have_native=1
fi
if (( ! have_nix && ! have_native )); then
    cat <<'EOF'
REGRESS: the build environment is missing in this WSL (no nix-shell, and no native
         pkg-config(vulkan, sdl3) + cmake + ninja).
         Install Nix (single user) as described in docs/refueled/TOOLCHAIN.md section 3:
           curl --proto '=https' --tlsv1.2 -L https://nixos.org/nix/install | sh -s -- --no-daemon
         Then open a new WSL shell (or: . ~/.nix-profile/etc/profile.d/nix.sh) and run this again.
         The first nix-shell run downloads a few GB into /nix inside the WSL.
REGRESS ENVIRONMENT MISSING (exit 2)
EOF
    exit 2
fi
say "environment: nix-shell=$have_nix native-toolchain=$have_native"

# ---- worktree ---------------------------------------------------------------------------------
if [[ -e $wt ]]; then
    echo "REGRESS: $wt exists already (earlier run aborted?). Remove it with:" >&2
    echo "         git -C $REPO worktree remove --force $wt" >&2
    exit 3
fi
cleaned=0
cleanup() {
    (( cleaned )) && return
    cleaned=1
    cd / || true
    if [[ -e $wt ]]; then
        say "removing worktree $wt"
        git -C "$REPO" worktree remove --force "$wt" >/dev/null 2>&1 || {
            say "worktree remove failed; trying rm -rf and prune"
            rm -rf "$wt"; git -C "$REPO" worktree prune
        }
    fi
}
trap cleanup EXIT
trap 'say "interrupted"; cleanup; exit 130' INT TERM

say "git worktree add --detach $wt $short"
if ! git -C "$REPO" worktree add --detach "$wt" "$full"; then
    echo "REGRESS: cannot create worktree" >&2; exit 3
fi

# ---- steps ------------------------------------------------------------------------------------
unset BB_GAME_DIR
failed=""
run_step() {
    local name=$1; shift
    [[ -z $failed ]] || { say "skipped $name (after failure of $failed)"; return; }
    say "=== step $name: $*"
    local t0=$SECONDS
    ( cd "$wt" && "$@" )
    local rc=$?
    say "=== step $name finished: exit $rc after $((SECONDS - t0)) s"
    if (( rc != 0 )); then failed=$name; fi
}

run_step build-test bash build.sh --test
run_step py-unittest python3 -m unittest discover -s tests -p 'test_*.py' -v
run_step shell-syntax bash -n run.sh build.sh launcher/bb-launcher.sh launcher/install-desktop.sh packaging/appimage.sh

# ---- result -----------------------------------------------------------------------------------
if [[ -z $failed ]]; then
    result="REGRESS PASS"; code=0
else
    result="REGRESS FAIL $failed"; code=1
fi
say "$short"; echo "$result"
cleanup
if (( keep_ref )); then
    sleep 1   # let tee flush before the copy
    cp -f "$log" "$LOG_DIR/ref-$short.log" && say "reference log: $LOG_DIR/ref-$short.log"
fi
exit $code
