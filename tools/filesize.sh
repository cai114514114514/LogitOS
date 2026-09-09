#!/usr/bin/env sh
# Print one file's size in bytes on both supported development-host families.
#
# This is deliberately one helper instead of another local `stat` probe in
# each producer.  GNU stat spells this `-c %s`; the macOS/BSD stat on the
# documented Apple-Silicon host spells it `-f %z` and reports `-c` as an
# illegal option.  The old video-matrix recipes ignored that failed command
# substitution and printed an empty byte count while still stamping the
# generated corpus complete.  A benchmark with an unmeasured input is worse
# than a loud failure, so both spellings live here and failure of both is
# fatal.  FILESIZE_STAT exists for tests to supply a one-dialect stat; normal
# callers must leave it unset.
set -eu

[ "$#" -eq 1 ] || {
    echo "usage: filesize.sh FILE" >&2
    exit 2
}

stat_cmd="${FILESIZE_STAT:-stat}"
if size="$($stat_cmd -f %z -- "$1" 2>/dev/null)"; then
    printf '%s\n' "$size"
elif size="$($stat_cmd -c %s -- "$1" 2>/dev/null)"; then
    printf '%s\n' "$size"
else
    echo "filesize: neither BSD 'stat -f %z' nor GNU 'stat -c %s' can size: $1" >&2
    exit 1
fi
