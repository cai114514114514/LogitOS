#!/usr/bin/env bash
# sysroot_tccsnap.sh -- build the HOST tcc the sysroot gates use, from a
# SNAPSHOT of third_party/tcc that is known to compile.
#
# WHY A SNAPSHOT AND NOT THE LIVE TREE. third_party/tcc is untracked by git
# and is being patched in place by another line (tests/tcc.mk: the TCC_LOGIT
# defaults). On 2026-08-21 the first `make sysroot` built a host tcc from it
# at 09:46 and the second, forty minutes later, died in tccelf.c:1719 on a
# half-written TLS change. A gate whose toolchain compiles only between
# somebody else's keystrokes is not a gate. So: the live tree is COPIED and
# try-built on every run; if it compiles, the copy becomes the snapshot and
# the new binary the host tcc; if it does not, the last good snapshot stays
# and the run says so, loudly, with the first error. Only when there is no
# snapshot at all is the stock copy at build/tcc/pristine (another line's,
# read-only) used as the seed. Nothing here writes into third_party/tcc or
# build/tcc.
#
# The host tcc is stock tcc (no -DTCC_LOGIT: that patch may be mid-flight in
# the snapshot) with three paths compiled in, all rooted at build/sysroot:
#   CONFIG_SYSROOT             every default search path gets this prefix
#   CONFIG_TCCDIR              {B} = <sysroot>/usr/lib/tcc (libtcc1.a, include/)
#   CONFIG_TCC_SYSINCLUDEPATHS <sysroot>/usr/include first, then {B}/include
#                              -- the ORDER tests/tcc.mk builds tcc.aex with
#                              (libc first), which is also the order this libc
#                              was written against; upstream's default is the
#                              reverse and tests/unit/sysroot_hdr_test.py
#                              measures both
# Everything downstream (libtcc1.a's sources, tcc's include/*.h) is taken from
# the SAME snapshot, so the headers, the runtime library and the compiler
# that the gates exercise are one version of tcc.
#
# usage: sysroot_tccsnap.sh <cc> <live-tcc-dir> <work-dir> <sysroot-abs> [pristine-dir]
set -u
CC="$1"; LIVE="$2"; WORK="$3"; SYSROOT_ABS="$4"; PRISTINE="${5:-}"
CAND="$WORK/tccsrc.cand"; SNAP="$WORK/tccsrc"; HOST="$WORK/hosttcc"
mkdir -p "$HOST"

copy_src() {   # $1 = source dir -> $CAND (only what a build reads)
    rm -rf "$CAND"; mkdir -p "$CAND"
    cp "$1"/*.c "$1"/*.h "$1"/*.def "$1"/VERSION "$CAND"/ 2>/dev/null
    cp -r "$1"/include "$1"/lib "$CAND"/
}

try_build() {  # $CAND -> $HOST/tcc.new ; 0 on success, log in $HOST/build.log
    local ver
    ver="$(tr -d '\r\n' < "$CAND/VERSION")"
    printf '#define TCC_VERSION "%s"\n' "$ver" > "$HOST/config.h"
    "$CC" -O1 -w -I"$HOST" -DTCC_TARGET_X86_64 -DONE_SOURCE=1 \
        -DCONFIG_TCCDIR="\"$SYSROOT_ABS/usr/lib/tcc\"" \
        -DCONFIG_SYSROOT="\"$SYSROOT_ABS\"" \
        -DCONFIG_TCC_SYSINCLUDEPATHS="\"$SYSROOT_ABS/usr/include:{B}/include\"" \
        -o "$HOST/tcc.new" "$CAND/tcc.c" -lm -ldl -lpthread > "$HOST/build.log" 2>&1
}

promote() {    # $1 = a description of where the snapshot came from
    rm -rf "$SNAP"; mv "$CAND" "$SNAP"; mv "$HOST/tcc.new" "$HOST/tcc"
    {
        echo "source: $1"
        echo "taken:  $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "sources md5: $(cd "$SNAP" && cat *.c *.h include/*.h lib/* | md5sum | cut -d' ' -f1)"
        echo "host tcc: $("$HOST/tcc" -v 2>&1 | head -1)"
    } > "$SNAP/SNAPSHOT.txt"
    echo "sysroot: host tcc built from $1 -> $HOST/tcc ($(stat -c %s "$HOST/tcc") bytes)"
}

copy_src "$LIVE"
if try_build; then
    promote "$LIVE (the live tree)"
    exit 0
fi
echo "sysroot: WARNING: $LIVE does not compile right now (another line is editing it); first errors:"
grep -m3 'error:' "$HOST/build.log" | sed 's/^/    /'
rm -rf "$CAND"
if [ -x "$HOST/tcc" ] && [ -f "$SNAP/SNAPSHOT.txt" ]; then
    echo "sysroot: keeping the last good snapshot:"
    sed 's/^/    /' "$SNAP/SNAPSHOT.txt"
    exit 0
fi
if [ -n "$PRISTINE" ] && [ -d "$PRISTINE" ]; then
    echo "sysroot: no snapshot yet -- seeding from $PRISTINE (stock tcc, copied, never written)"
    copy_src "$PRISTINE"
    if try_build; then
        promote "$PRISTINE (stock copy; the live tree did not compile)"
        exit 0
    fi
    echo "sysroot: $PRISTINE does not compile either:"
    grep -m3 'error:' "$HOST/build.log" | sed 's/^/    /'
fi
echo "sysroot: FAIL: no buildable tcc source; see $HOST/build.log"
exit 1
