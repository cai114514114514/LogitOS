#!/usr/bin/env bash
# ci.sh -- build LogitOS from a CLEAN CLONE OF HEAD and run the suites.
#
# Why a clean clone and not `make` in place: on 2026-08-08, 14 of 30 commits in
# this tree did not build from a clean clone of themselves. Every one of them
# built fine in the working tree, because the working tree held a generated
# file, an untracked source, or another session's edit. Building where nothing
# is untracked is the ONLY way that class of breakage is visible.
#
# The clone and the build happen on the SAME side of the WSL boundary, in one
# process. A clean-clone check that clones under Git Bash and builds under WSL
# verifies nothing -- /tmp is a different directory in each, and that mistake
# has already produced one false pass here.
#
# Usage:
#   tools/ci.sh                 audit + clean-clone build + host suites
#   tools/ci.sh --boot          the above, then the QEMU boot suites
#   tools/ci.sh --boot-only     skip the host suites
#   tools/ci.sh --here          use the working tree, no clone (fast, weaker)
#   tools/ci.sh --only 'a b c'  run exactly these targets
#   CI_DIR=/path                where to clone (default /tmp/logitos-ci)
#   CI_JOBS=n                   -j for the build (default nproc)
#
# Exit status is the number of failures, capped at 1. Nothing is skipped
# quietly: every suite lands in the summary as PASS, FAIL or TIMEOUT.

set -u

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CI_DIR="${CI_DIR:-/tmp/logitos-ci}"
CI_JOBS="${CI_JOBS:-$(nproc 2>/dev/null || echo 4)}"
SUITE_TIMEOUT="${SUITE_TIMEOUT:-900}"
BUILD_TIMEOUT="${BUILD_TIMEOUT:-3600}"

do_host=1; do_boot=0; do_clone=1; only=""
while [ $# -gt 0 ]; do
    case "$1" in
        --boot)      do_boot=1 ;;
        --boot-only) do_boot=1; do_host=0 ;;
        --here)      do_clone=0 ;;
        --only)      shift; only="$1" ;;
        *) echo "ci.sh: unknown argument $1" >&2; exit 2 ;;
    esac
    shift
done

case "$(uname -s)" in
    Linux) ;;
    *) echo "ci.sh: this build only works inside WSL/Linux; got $(uname -s)" >&2
       exit 2 ;;
esac

# ---- 1. the audit ---------------------------------------------------------
# It runs against the SOURCE tree, before the clone: it is a lint over the
# Makefile, and it is the cheapest thing here, so it should fail first.
echo "=============================================================="
echo "ci: test audit (tests that cannot fail)"
echo "=============================================================="
audit_rc=0
python3 "$SRC/tools/audit_tests.py" || audit_rc=$?

# ---- 2. the clean clone ---------------------------------------------------
if [ "$do_clone" = 1 ]; then
    head_sha="$(git -C "$SRC" rev-parse HEAD)"
    echo
    echo "=============================================================="
    echo "ci: clean clone of $head_sha -> $CI_DIR"
    echo "=============================================================="
    rm -rf "$CI_DIR"
    # --no-hardlinks so a later `clean` in the clone can never reach back into
    # the source object store. --shared would be faster and is exactly the
    # wrong trade for a test whose subject is isolation.
    git clone --quiet --no-hardlinks "$SRC" "$CI_DIR" || exit 1
    git -C "$CI_DIR" checkout --quiet "$head_sha" || exit 1
    # Report, do not copy: an untracked file the build needs is the bug this
    # target exists to find, and listing them names it precisely.
    untracked="$(git -C "$SRC" ls-files --others --exclude-standard | wc -l)"
    echo "ci: source tree has $untracked untracked file(s), none of which were copied"
    WORK="$CI_DIR"
else
    echo "ci: --here, building in the working tree (a clean-clone failure CANNOT be seen this way)"
    WORK="$SRC"
fi

# ---- 3. the build ---------------------------------------------------------
declare -a names=() verdicts=() secs=()

record() { names+=("$1"); verdicts+=("$2"); secs+=("$3"); }

run_step() {
    local name="$1" tmo="$2"; shift 2
    local log="$CI_LOG/$name.log" t0 t1 rc
    t0=$(date +%s)
    timeout "$tmo" "$@" >"$log" 2>&1; rc=$?
    t1=$(date +%s)
    if   [ $rc -eq 0   ]; then record "$name" PASS    $((t1-t0)); printf '  PASS    %-28s %4ds\n' "$name" $((t1-t0))
    elif [ $rc -eq 124 ]; then record "$name" TIMEOUT $((t1-t0)); printf '  TIMEOUT %-28s %4ds  %s\n' "$name" $((t1-t0)) "$log"
    else                       record "$name" FAIL    $((t1-t0)); printf '  FAIL    %-28s %4ds  %s\n' "$name" $((t1-t0)) "$log"
    fi
    return $rc
}

CI_LOG="${CI_LOG:-$WORK/build/ci-logs}"
mkdir -p "$CI_LOG"

echo
echo "=============================================================="
echo "ci: build (make -j$CI_JOBS) in $WORK"
echo "=============================================================="
if ! run_step "build" "$BUILD_TIMEOUT" make -C "$WORK" -j"$CI_JOBS"; then
    echo
    echo "ci: the build failed; suites cannot mean anything. Tail of the log:"
    tail -n 40 "$CI_LOG/build.log"
    echo
    echo "SUMMARY: build FAIL"
    exit 1
fi
# A build step that exits 0 without producing the artefact is a test that
# cannot fail -- the exact shape this whole target exists to find. Check the
# products, not the status.
for art in build/logit.iso build/disk.img; do
    if [ ! -s "$WORK/$art" ]; then
        echo "ci: make exited 0 but $art is missing or empty"
        record "build-artifacts" FAIL 0
        echo "SUMMARY: build FAIL (no $art)"
        exit 1
    fi
done
echo "  artifacts: $(du -h "$WORK/build/logit.iso" | cut -f1) iso, $(du -h "$WORK/build/disk.img" | cut -f1) disk"

# ---- 4. the suites --------------------------------------------------------
suites=""
if [ -n "$only" ]; then
    suites="$only"
else
    [ "$do_host" = 1 ] && suites="$(python3 "$SRC/tools/audit_tests.py" --suites=host | tr '\n' ' ')"
    [ "$do_boot" = 1 ] && suites="$suites $(python3 "$SRC/tools/audit_tests.py" --suites=boot | tr '\n' ' ')"
fi

echo
echo "=============================================================="
echo "ci: $(echo $suites | wc -w) suites"
echo "=============================================================="
for s in $suites; do
    run_step "$s" "$SUITE_TIMEOUT" make -C "$WORK" "$s"
done

# ---- 5. the summary -------------------------------------------------------
echo
echo "=============================================================="
echo "ci: SUMMARY"
echo "=============================================================="
fail=0
for i in "${!names[@]}"; do
    [ "${verdicts[$i]}" = PASS ] && continue
    printf '  %-8s %-28s %s\n' "${verdicts[$i]}" "${names[$i]}" "$CI_LOG/${names[$i]}.log"
    fail=$((fail+1))
done
pass=$(( ${#names[@]} - fail ))
echo
echo "ci: $pass passed, $fail failed, of ${#names[@]}"
[ "$audit_rc" -ne 0 ] && echo "ci: the test audit reported findings (see the top of this log)"

# The audit is advisory in the exit status until its findings are worked off --
# but it prints every time, so it cannot be forgotten, and `make test-audit`
# gates on it directly for anyone who wants that gate today.
[ "$fail" -gt 0 ] && exit 1
exit 0
