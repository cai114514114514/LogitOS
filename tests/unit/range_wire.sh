#!/bin/sh
# Build and run the on-the-wire range test (tests/unit/range_wire.c).
#
# Exit 77 from the program means "the network did not answer" -- reported as a
# SKIP rather than a pass, because a test that quietly succeeds when it could
# not reach anything is worse than no test.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${BUILD:-$ROOT/build}
CC=${CC:-cc}
mkdir -p "$BUILD"

"$CC" -O1 -g -w \
    -Itests/unit/h2stub -Iinclude/abi -Ic/apps/browser -Ic/net/http -Ic/lib/image \
    -o "$BUILD/range_wire" \
    tests/unit/range_wire.c c/apps/browser/browser_rt.c \
    c/net/http/http1.c c/net/http/http2.c c/net/http/hpack.c \
    c/net/http/hpool.c c/net/http/url.c

set +e
"$BUILD/range_wire" "$@"
rc=$?
set -e
if [ "$rc" = 77 ]; then
    echo "SKIP: test-range-wire needs network; nothing was asserted"
    exit 0
fi
exit $rc
