#!/usr/bin/env bash
# Exercise tools/filesize.sh and the exact GNU-only failure that used to leave
# blank byte counts in generated-video evidence.  The control uses a fake
# BSD-only stat, so it is observable on both macOS and Linux.
set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:?usage: filesize_test.sh OUTDIR [--negctl]}"
mkdir -p "$OUT"
fixture="$OUT/seven-bytes"
printf '1234567' > "$fixture"

bsd_stat="$ROOT/tests/unit/filesize_bsd_stat.sh"
if [ "${2:-}" = "--negctl" ]; then
    if "$bsd_stat" -c %s -- "$fixture" > "$OUT/legacy.out" 2> "$OUT/legacy.err"; then
        echo "FAIL: GNU-only stat unexpectedly worked through the BSD control" >&2
        exit 1
    fi
    grep -q "unsupported spelling" "$OUT/legacy.err" || {
        echo "FAIL: the control failed somewhere other than the stat dialect" >&2
        cat "$OUT/legacy.err" >&2
        exit 1
    }
    echo "filesize-negctl: old GNU-only 'stat -c %s' is caught by the BSD host control"
    exit 0
fi

native="$(bash "$ROOT/tools/filesize.sh" "$fixture")"
[ "$native" = 7 ] || {
    echo "FAIL: native stat path reported '$native', expected 7" >&2
    exit 1
}

bsd="$(FILESIZE_STAT="$bsd_stat" bash "$ROOT/tools/filesize.sh" "$fixture")"
[ "$bsd" = 7 ] || {
    echo "FAIL: BSD stat path reported '$bsd', expected 7" >&2
    exit 1
}

if bash "$ROOT/tools/filesize.sh" "$OUT/missing" > "$OUT/missing.out" 2> "$OUT/missing.err"; then
    echo "FAIL: a missing input produced a plausible byte count" >&2
    exit 1
fi
grep -q "neither BSD" "$OUT/missing.err" || {
    echo "FAIL: missing input was not refused at the measurement boundary" >&2
    exit 1
}

echo "filesize: native and BSD-only paths report 7 bytes; missing input is loud"
