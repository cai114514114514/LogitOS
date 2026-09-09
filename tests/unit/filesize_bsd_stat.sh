#!/usr/bin/env sh
# A deterministic BSD-only stat for tests/filesize.mk.  It rejects the GNU
# spelling even on Linux, so the negative control can be watched failing on
# every host instead of depending on which developer happens to run it.
set -eu

if [ "$#" -eq 4 ] && [ "$1" = "-f" ] && [ "$2" = "%z" ] && [ "$3" = "--" ]; then
    wc -c < "$4" | tr -d ' '
    exit 0
fi

echo "bsd-stat-control: unsupported spelling: $*" >&2
exit 2
