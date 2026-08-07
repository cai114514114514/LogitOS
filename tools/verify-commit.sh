#!/usr/bin/env bash
# The commit gate: does what is COMMITTED build and run?
#
#   tools/verify-commit.sh                 # gate HEAD
#   tools/verify-commit.sh --rev <rev>     # gate some other commit
#   tools/verify-commit.sh --quick         # build only, skip the boot test
#   tools/verify-commit.sh --in-place      # build+test the current tree, no clone
#
# Three commits landed in one day whose own clean clone did not compile, twice
# from the same mistake: a file was committed and the header it includes was
# not. Nothing in the tree could see that, because every check ran against a
# WORKING TREE that still had the missing file sitting there untracked. The only
# way to ask the question is to leave the working tree entirely.
#
#   git -c core.autocrlf=false clone --no-hardlinks
#
# core.autocrlf=false because this repo is worked on from an NTFS mount: a
# clone that converts line endings turns every shell script and every .as file
# into something the guest cannot run, and the failure looks like a code bug.
# --no-hardlinks so the clone's object store cannot be damaged by anything the
# build does. No `git worktree`, for the same line-ending reason.
#
# And it builds BOTH artefacts. `make` alone produces the ISO, which is the
# kernel; every ring-3 program lives on the disk image and is built by
# `make build/disk.img`. A gate that stops at the ISO is blind to the entire
# userland -- which is precisely how a c/lib/video file that did not compile
# reached HEAD.
#
# Exit codes are chosen for `git bisect run`:
#   0    built and passed
#   1    built, but the full-system test failed
#   125  could not be tested here (build failed and --build-failure=skip, or a
#        tool is missing) -- bisect skips these instead of blaming them
#   2    build failed and --build-failure=bad (the default for a plain gate)

set -u

REV=""
QUICK=0
IN_PLACE=0
KEEP=0
BUILD_FAILURE="bad"
JOBS="$( (command -v nproc >/dev/null && nproc) || echo 4)"

while [ $# -gt 0 ]; do
    case "$1" in
        --rev)            REV="${2:?--rev needs a revision}"; shift 2 ;;
        --quick)          QUICK=1; shift ;;
        --in-place)       IN_PLACE=1; shift ;;
        --keep)           KEEP=1; shift ;;
        --jobs)           JOBS="${2:?--jobs needs a number}"; shift 2 ;;
        --build-failure)  BUILD_FAILURE="${2:?--build-failure needs bad|skip}"; shift 2 ;;
        --build-failure=*) BUILD_FAILURE="${1#*=}"; shift ;;
        -h|--help)        sed -n '2,40p' "$0"; exit 0 ;;
        *) echo "verify-commit: unknown argument '$1'" >&2; exit 125 ;;
    esac
done

case "$BUILD_FAILURE" in bad|skip) ;; *)
    echo "verify-commit: --build-failure must be bad or skip" >&2; exit 125 ;;
esac

say() { printf '\n== %s\n' "$*"; }

for t in git make python3; do
    command -v "$t" >/dev/null 2>&1 || {
        echo "verify-commit: $t not found -- cannot test here" >&2; exit 125; }
done

REPO="$(git rev-parse --show-toplevel 2>/dev/null)" || {
    echo "verify-commit: not inside a git repository" >&2; exit 125; }

# ---------------------------------------------------------------- the tree --
if [ "$IN_PLACE" = 1 ]; then
    WORK="$REPO"
    SHA="$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo '?')"
    say "verifying the WORKING TREE at $SHA (--in-place; nothing was cloned)"
else
    WORK="$(mktemp -d "${TMPDIR:-/tmp}/logit-verify-XXXXXX")"
    cleanup() { [ "$KEEP" = 1 ] || rm -rf "$WORK"; }
    trap cleanup EXIT
    SHA="$(git -C "$REPO" rev-parse --short "${REV:-HEAD}")" || exit 125
    say "cloning ${REV:-HEAD} ($SHA) to $WORK"
    git -c core.autocrlf=false clone --no-hardlinks --quiet "$REPO" "$WORK" || {
        echo "verify-commit: clone failed" >&2; exit 125; }
    git -C "$WORK" -c advice.detachedHead=false checkout --quiet \
        --detach "${REV:-HEAD}" || {
        echo "verify-commit: could not check out ${REV:-HEAD} in the clone" >&2
        exit 125; }
    # Prove the clone really is clean. An untracked file that the gate somehow
    # inherited would defeat the entire point of cloning.
    if [ -n "$(git -C "$WORK" status --porcelain)" ]; then
        echo "verify-commit: the fresh clone is not clean -- refusing to gate" >&2
        git -C "$WORK" status --porcelain >&2
        exit 125
    fi
fi

# ----------------------------------------------------------------- building --
# ISO *and* disk image, named explicitly. `make` alone stops at the ISO.
say "building the ISO and the disk image (-j$JOBS)"
BUILD_LOG="$WORK/verify-build.log"
if ! ( cd "$WORK" && make -j"$JOBS" build/logit.iso build/disk.img ) \
        >"$BUILD_LOG" 2>&1; then
    echo "FAIL: the commit does not build from a clean clone"
    echo "----- build output (tail) -----"
    tail -n 60 "$BUILD_LOG"
    echo "-------------------------------"
    [ "$BUILD_FAILURE" = skip ] && exit 125
    exit 2
fi
echo "ok: a clean clone of $SHA builds both build/logit.iso and build/disk.img"

if [ "$QUICK" = 1 ]; then
    say "--quick: skipping the full-system test"
    exit 0
fi

# ------------------------------------------------------------------ testing --
say "running the full-system test against that build"
# Through the Makefile target, not the script directly: the list of .aex packed
# at the LogitFS root is derived from $(APPS) there, and a second copy of that
# list here is a second thing to forget when an app is added.
if ( cd "$WORK" && make --no-print-directory test-fullsystem ); then
    say "PASS: $SHA builds from a clean clone and the whole system works"
    exit 0
fi
say "FAIL: $SHA builds, but the full-system test failed"
exit 1
