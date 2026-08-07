#!/usr/bin/env bash
# "When did apps stop launching?" as a command instead of an afternoon.
#
#   tools/bisect-boot.sh --good <rev> --bad <rev> [options]
#
# Around 400 commits landed in this repository in a single day. Finding which
# one broke the machine by hand means building and booting by hand, and the
# thing being looked for -- ring-3 programs failing to load -- is invisible to
# every existing check. This wraps `git bisect run` around tools/verify-commit.sh,
# which builds the ISO *and* the disk image and then runs the full-system test.
#
# It bisects in a CLONE, never in your working tree.
#
# That is not caution for its own sake. `git bisect` checks out ~9 commits over
# the run; doing that in a tree another session is editing destroys their work,
# and this repository is routinely worked on by several at once. `git worktree`
# is not the alternative either -- a new worktree on this NTFS mount re-applies
# CRLF to every shell script and every .as file, and then the bisect measures
# line endings. So: clone once with core.autocrlf=false, bisect there, and leave
# the answer as a commit id you can look at in your own tree.
#
# Options
#   --good <rev>        a revision known to work            (required)
#   --bad  <rev>        a revision known to be broken       (default HEAD)
#   --cmd  <command>    what to run per commit, from the clone's root.
#                       Default: tools/verify-commit.sh --in-place
#                       --build-failure skip
#   --quick             per-commit check is BUILD ONLY -- use this to bisect a
#                       compile break; it is many times faster
#   --build-failure bad|skip   how to treat a commit that does not compile when
#                       you are bisecting a RUNTIME regression (default: skip)
#   --dir <path>        clone here instead of a temp dir (kept afterwards)
#   --jobs <n>          make -j
#
# The per-commit exit codes are `git bisect run`'s: 0 good, 1 bad, 125 skip.
# verify-commit.sh already speaks them.

set -u

GOOD=""
BAD="HEAD"
CMD=""
QUICK=0
BUILD_FAILURE="skip"
DIR=""
KEEPDIR=0
JOBS="$( (command -v nproc >/dev/null && nproc) || echo 4)"

while [ $# -gt 0 ]; do
    case "$1" in
        --good) GOOD="${2:?--good needs a revision}"; shift 2 ;;
        --bad)  BAD="${2:?--bad needs a revision}"; shift 2 ;;
        --cmd)  CMD="${2:?--cmd needs a command}"; shift 2 ;;
        --quick) QUICK=1; shift ;;
        --build-failure) BUILD_FAILURE="${2:?bad|skip}"; shift 2 ;;
        --build-failure=*) BUILD_FAILURE="${1#*=}"; shift ;;
        --dir)  DIR="${2:?--dir needs a path}"; KEEPDIR=1; shift 2 ;;
        --jobs) JOBS="${2:?--jobs needs a number}"; shift 2 ;;
        -h|--help) sed -n '2,40p' "$0"; exit 0 ;;
        *) echo "bisect-boot: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

[ -n "$GOOD" ] || { echo "bisect-boot: --good <rev> is required" >&2; exit 2; }

REPO="$(git rev-parse --show-toplevel)" || exit 2
GOOD_SHA="$(git -C "$REPO" rev-parse "$GOOD")" || exit 2
BAD_SHA="$(git -C "$REPO" rev-parse "$BAD")" || exit 2

if [ -z "$CMD" ]; then
    CMD="tools/verify-commit.sh --in-place --jobs $JOBS --build-failure $BUILD_FAILURE"
    [ "$QUICK" = 1 ] && CMD="$CMD --quick"
fi

if [ -z "$DIR" ]; then
    DIR="$(mktemp -d "${TMPDIR:-/tmp}/logit-bisect-XXXXXX")"
fi
mkdir -p "$DIR"

# An existing clone is reused, so a bisect that was interrupted can be resumed
# without paying for the clone (and the object store) again.
if [ ! -d "$DIR/.git" ]; then
    echo "== cloning $REPO -> $DIR (core.autocrlf=false, --no-hardlinks)"
    git -c core.autocrlf=false clone --no-hardlinks --quiet "$REPO" "$DIR" || exit 2
else
    echo "== reusing the clone in $DIR"
    git -C "$DIR" fetch --quiet origin || true
fi

cd "$DIR" || exit 2
git bisect reset --quiet >/dev/null 2>&1 || true

echo "== bisecting"
echo "   good : $(git rev-parse --short "$GOOD_SHA")  $(git log -1 --format=%s "$GOOD_SHA")"
echo "   bad  : $(git rev-parse --short "$BAD_SHA")  $(git log -1 --format=%s "$BAD_SHA")"
echo "   test : $CMD"
echo

git bisect start "$BAD_SHA" "$GOOD_SHA" || exit 2

LOG="$DIR/bisect-run.log"
set +e
# shellcheck disable=SC2086
git bisect run $CMD 2>&1 | tee "$LOG"
STATUS=${PIPESTATUS[0]}
set -e

echo
echo "================= result ================="
FIRST_BAD="$(git rev-parse --verify --quiet refs/bisect/bad)"
if [ -n "$FIRST_BAD" ]; then
    git --no-pager log -1 --format='first bad commit: %H%n  %s%n  %an, %ad' "$FIRST_BAD"
    # Skipped commits (exit 125 -- typically "this one does not even compile")
    # leave git unable to narrow the last step, and it says so loudly. The
    # commit above is still the earliest one PROVEN bad; the untestable ones
    # before it are the remaining candidates. Saying which of the two answers
    # you got matters: one is a commit, the other is a commit and a range.
    if grep -q "cannot bisect more" "$LOG"; then
        echo
        echo "NOTE: some commits could not be tested (exit 125) and were skipped,"
        echo "      so the true culprit may be any of them up to the commit above."
        git --no-pager log --oneline "$FIRST_BAD" -1 >/dev/null
    fi
    STATUS=0
else
    echo "bisect did not converge on a single commit (see $LOG)"
    [ "$STATUS" = 0 ] && STATUS=1
fi
echo "=========================================="
echo "full log: $LOG"
[ "$KEEPDIR" = 1 ] && echo "clone kept at: $DIR"

git bisect reset --quiet >/dev/null 2>&1 || true
[ "$KEEPDIR" = 1 ] || echo "note: $DIR is a temp clone; remove it when done"
exit "$STATUS"
