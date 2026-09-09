#!/usr/bin/env bash
# rebuild-hosts.sh -- rebuild EVERY host engine binary in this workflow family
# from ONE source state, so the four differentials that share third_party/quickjs
# are all measuring the same engine.
#
# WHY THIS EXISTS. Four independent lines patched quickjs.c in this session, and
# each left behind a host binary built at the moment its own patch landed. Those
# binaries have four different mtimes and therefore four different engines
# inside them. Re-running the differentials against them measures a browser that
# never existed -- the jsfb BASELINE header records exactly this failure one
# layer up ("`make` had relinked the probe against a js_dom.c edited four
# minutes earlier"). Everything below is built here, now, from the same bytes,
# and the md5 of the sources is recorded beside them.
#
# NOT a substitute for the guest. Every binary here is darwin/arm64. The guest
# run is a separate step and no finding is reported from these alone.
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT="${OUT:-build-jssem/rerun}"
mkdir -p "$OUT"

QJS=(third_party/quickjs/quickjs.c third_party/quickjs/cutils.c
     third_party/quickjs/libregexp.c third_party/quickjs/libunicode.c
     third_party/quickjs/libbf.c)
INC=(-Ithird_party/quickjs -DCONFIG_VERSION='"logit-2024"')
# The guest's define set, from Makefile:888 $(JS_CF). NDEBUG is added because
# the .aex is built with it; a host binary without it has live assertions the
# shipped engine does not.
BROW=(-DLOGIT_OS -DCONFIG_STACK_CHECK -DNDEBUG)

md5 "${QJS[@]}" third_party/quickjs/*.h > "$OUT/qjs.md5" 2>/dev/null \
  || md5sum "${QJS[@]}" third_party/quickjs/*.h > "$OUT/qjs.md5"

build () {           # build <out> <runner.c> [extra defines...]
    local out="$1" src="$2"; shift 2
    printf 'building %-24s ' "$(basename "$out")" >&2
    cc -O1 -w "${INC[@]}" "$@" -o "$out" "$src" "${QJS[@]}" -lm
    printf 'ok\n' >&2
}

# The three runners, each the one its own line's cases are written against.
build "$OUT/jsmicro_brow"    tests/jsmicro/micro_run.c   "${BROW[@]}"
build "$OUT/js_sem_brow"     tests/unit/js_sem.c         "${BROW[@]}"
build "$OUT/js_sem_probe"    tests/unit/js_sem_probe.c   "${BROW[@]}"
# One runner built WITHOUT the guest defines. If this disagrees with its
# browser-flag twin, that outranks every finding in the differential, because it
# means the gate and the product are not the same engine.
build "$OUT/js_sem_host"     tests/unit/js_sem.c
build "$OUT/jsmicro_host"    tests/jsmicro/micro_run.c

echo "all host engines rebuilt into $OUT" >&2
