#!/usr/bin/env bash
# tools/coldcode/run.sh -- drive every corpus in the tree through the
# instrumented probes and leave one merged .profdata behind.
#
#   tools/coldcode/run.sh <bindir> <workdir>
#
# CONTINUOUS MODE (%c) IS LOAD-BEARING, NOT A TUNING KNOB.
# wpt_test isolates every file in a forked child that ends with `_exit(0)`
# (tests/unit/wpt_test.c:1387, with its own comment about stdio). `_exit` does
# not run atexit handlers, and the LLVM profile writer IS an atexit handler --
# so in the ordinary %m mode a child's counters are DISCARDED and every
# function reached only inside a child reads COLD. Measured on a five-line
# fork/_exit program: %m reported `child_only` at 0 after it had demonstrably
# run and printed; %c reported it at 1. That is this instrument's own worst
# failure -- a function that ran, reported as never having run -- and it would
# have been indistinguishable from a finding.
#
# %c mmaps the counters into the file, so nothing has to run at exit. Verified
# to accumulate exactly: 5 sequential runs -> 5, and 20 CONCURRENT runs -> 20,
# with no lost updates, while the cold control stayed at 0 throughout.
#
# COLD_FULL=1 adds jsfb (221 implementations) and WPT (9,181 harness files).
# Without it only the three captured-site corpora run, which takes about five
# minutes instead of about forty. THE DEFAULT IS THE QUICK ONE ON PURPOSE:
# the per-change gate (make cold-what-ran) is meant to be run before a commit,
# and a gate that takes forty minutes is a gate nobody runs -- which is this
# tree's rule 4 arriving by a different road. The corpus manifest this writes
# names exactly which half ran, so a NEVER-RAN finding is always read against
# the corpus that produced it rather than against an assumed one.
set -uo pipefail

BIN=${1:?usage: run.sh <bindir> <workdir>}
WORK=${2:?usage: run.sh <bindir> <workdir>}
FULL=${COLD_FULL:-}
RAW=$WORK/raw
mkdir -p "$RAW"
: > "$WORK/corpus.txt"
note() { echo "$*" >> "$WORK/corpus.txt"; }

pf() { echo "LLVM_PROFILE_FILE=$RAW/$1-%c-%m.profraw"; }

run_corpus() {   # name, then the command
    local name=$1; shift
    echo "=== corpus: $name"
    mkdir -p "$RAW/$name"
    ( export LLVM_PROFILE_FILE="$RAW/$name/p-%c-%m.profraw"; "$@" ) \
        > "$WORK/$name.log" 2>&1
    local rc=$?
    local n
    n=$(ls "$RAW/$name" 2>/dev/null | wc -l | tr -d ' ')
    echo "    exit=$rc  profraw=$n  log=$WORK/$name.log"
    if [ "$n" = "0" ]; then
        echo "    WARNING: no profile written -- this corpus contributed NOTHING."
        note "$name: NOTHING -- no profile was written (exit=$rc)."
    elif [ "$rc" != "0" ]; then
        # exit!=0 is not automatically a failure here (a probe that reports
        # page errors exits non-zero), but a KILLED corpus is a partial one and
        # a finding read against it is a finding read against less than it
        # claims. jsfb was Terminated (143) on the 2026-08-29 full run and the
        # headline said "221 applications" anyway. Never again silently.
        note "$name: ran, exit=$rc -- PARTIAL OR ERRORED, read findings accordingly"
    fi
}

cd "$(dirname "$0")/../.." || exit 1

WEBAPI_DIRS=$(ls -d tests/fixtures/webapi/*/ 2>/dev/null)
FW_DIRS=$(ls -d tests/fixtures/frameworks/*/ 2>/dev/null | grep -v '/_')
CSSWEB_DIRS=$(ls -d tests/fixtures/cssweb/*/ 2>/dev/null)

# The three captured-site corpora go through the probe the same way
# `make probe-webapi` does: a list of directories, each with an index.html.
# --errors is added so channel 2 runs too -- the UNWRAPPED pass, which is the
# code path browser.c actually takes. Without it the only measurement is of
# scripts running inside the `with (probeScope)` Proxy, which is not the
# browser's execution path and would systematically cold-report whatever only
# the real path reaches.
# shellcheck disable=SC2086
run_corpus webapi     "$BIN/webapi_probe" --errors $WEBAPI_DIRS
note "webapi:     $(echo "$WEBAPI_DIRS" | grep -c .) captured-site fixtures"
# shellcheck disable=SC2086
run_corpus frameworks "$BIN/webapi_probe" --errors $FW_DIRS
note "frameworks: $(echo "$FW_DIRS" | grep -c .) captured framework pages"
# shellcheck disable=SC2086
run_corpus cssweb     "$BIN/webapi_probe" --errors $CSSWEB_DIRS
note "cssweb:     $(echo "$CSSWEB_DIRS" | grep -c .) captured CSS pages"

# jsfb: --include-built is the whole corpus (221 implementations), not the
# build-free subset the baseline gate uses. jsfb_matrix.py spawns the probe
# per implementation, so LLVM_PROFILE_FILE is inherited by every child.
if [ -z "$FULL" ]; then
    echo "=== corpus: jsfb  NOT RUN -- quick mode. add COLD_FULL=1 for it."
    note "jsfb:       NOT RUN (quick mode; COLD_FULL=1 adds it)"
elif [ -d build/jsfb ]; then
    run_corpus jsfb python3 tests/unit/jsfb_matrix.py "$BIN/webapi_probe" \
        --root build/jsfb --include-built
    note "jsfb:       $(ls -d build/jsfb/*/ 2>/dev/null | wc -l | tr -d ' ') implementations"
else
    echo "=== corpus: jsfb  SKIPPED -- build/jsfb absent. settle it with: make jsfb-fetch"
    note "jsfb:       ABSENT -- build/jsfb does not exist (make jsfb-fetch)"
fi

# WPT: WEBAPI_FILE_ROOT SET. tests/cache.mk:145 records what omitting it costs
# -- js_webapi.c's host build answers GETs out of that directory, and without
# it every subresource fetch fails instead of being served, which changes what
# the corpus exercises rather than merely how fast it runs.
if [ -z "$FULL" ]; then
    echo "=== corpus: wpt  NOT RUN -- quick mode. add COLD_FULL=1 for it."
    note "wpt:        NOT RUN (quick mode; COLD_FULL=1 adds it, ~35 min)"
elif [ -d build/wpt ]; then
    echo "=== corpus: wpt"
    mkdir -p "$RAW/wpt"
    ( export LLVM_PROFILE_FILE="$RAW/wpt/p-%c-%m.profraw"
      export WEBAPI_FILE_ROOT=build/wpt
      "$BIN/wpt_test" --root build/wpt -b tests/unit/wpt_expected_fail.txt \
          --jobs 8 ) > "$WORK/wpt.log" 2>&1
    echo "    exit=$?  profraw=$(ls "$RAW/wpt" | wc -l | tr -d ' ')  log=$WORK/wpt.log"
    note "wpt:        $(grep -c '^' tests/unit/wpt_expected_fail.txt 2>/dev/null || echo '?') baselined; harness files under build/wpt"
else
    echo "=== corpus: wpt  SKIPPED -- build/wpt absent. settle it with: make wpt-fetch"
    note "wpt:        ABSENT -- build/wpt does not exist (make wpt-fetch)"
fi

echo "=== merging"
find "$RAW" -name '*.profraw' > "$WORK/profraw.list"
echo "    $(wc -l < "$WORK/profraw.list" | tr -d ' ') profraw files"
PROFDATA=$( (xcrun -f llvm-profdata 2>/dev/null) || echo /opt/homebrew/opt/llvm/bin/llvm-profdata )
"$PROFDATA" merge -sparse -f "$WORK/profraw.list" -o "$WORK/all.profdata" \
    && echo "    -> $WORK/all.profdata"

# The manifest is written LAST and describes what actually happened, not what
# was intended. CLAUDE.md rule 3: the corpus description and the corpus run
# are one jar, and a hand-typed sentence in a Makefile would be a second door
# onto it -- which is how a headline came to say "221 jsfb applications" about
# a run that had been Terminated.
{
    echo "corpus run $(date '+%Y-%m-%d %H:%M') -- $( [ -n "$FULL" ] && echo FULL || echo QUICK )"
    sort "$WORK/corpus.txt"
} > "$WORK/corpus.note"
mv "$WORK/corpus.note" "$WORK/corpus.txt"
echo "=== corpus manifest"
sed 's/^/    /' "$WORK/corpus.txt"
