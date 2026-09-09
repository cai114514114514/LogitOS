#!/usr/bin/env bash
# run-shapes.sh -- the differential for tests/jssem/shapes/.
#
# Same shape as tests/jsmicro/run.sh and tests/jssem/run-objweak.sh, and it
# reuses tests/jsmicro/micro_run.c as the engine runner because that runner
# already does the two things this needs and the other host runners do not:
# it DRAINS the job queue after evaluation (QuickJS never runs a queued job on
# its own) and it installs the browser's exact queueMicrotask prelude. A
# scheduler case run without those measures the harness.
#
# node is the ORACLE. Every case is a program that PRINTS and the two stdouts
# are diffed byte for byte.
#
# THE CONTROL is s99_CONTROL.js, whose output differs by construction. If it
# does not show up as a difference this harness is not an instrument and its
# greens mean nothing (CLAUDE.md rule 5).
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ENGINE="${ENGINE:-$ROOT/build-jssem/rerun/jsmicro_brow}"
NODE="${NODE:-node}"
OUT="${OUT:-$ROOT/build-jssem/rerun/shapes-out}"
DIR="$ROOT/tests/jssem/shapes"
mkdir -p "$OUT"
[ -x "$ENGINE" ] || { echo "FAIL: no engine at $ENGINE (run tests/jssem/rebuild-hosts.sh)"; exit 2; }

pass=0; fail=0; ctl_seen=0; ctl_fired=0
for f in "$DIR"/*.js; do
    b="$(basename "$f" .js)"
    "$NODE" "$ROOT/tests/jsmicro/node_run.js" "$f" > "$OUT/$b.node" 2>"$OUT/$b.node.err"
    "$ENGINE" "$f" > "$OUT/$b.ours" 2>"$OUT/$b.ours.err"
    case "$b" in *_CONTROL) ctl_seen=1 ;; esac
    if cmp -s "$OUT/$b.node" "$OUT/$b.ours"; then
        pass=$((pass+1)); printf 'ok    %s\n' "$b"
    else
        fail=$((fail+1)); printf 'DIFF  %s\n' "$b"
        case "$b" in *_CONTROL) ctl_fired=1 ;; esac
        diff -u "$OUT/$b.node" "$OUT/$b.ours" | sed 's/^/      /'
    fi
done
printf '\n%d ok, %d differ\n' "$pass" "$fail"
if [ "$ctl_seen" = 1 ] && [ "$ctl_fired" = 0 ]; then
    printf 'HARNESS BROKEN: the control did not report a difference.\n'; exit 2
fi
[ "$ctl_seen" = 1 ] && printf 'control: fired (this harness can report a difference)\n'
exit 0
