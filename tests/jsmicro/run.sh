#!/usr/bin/env bash
# tests/jsmicro/run.sh -- the differential.  One case = one .js file.  Run it
# under node (the ORACLE) and under this tree's QuickJS, diff the two stdouts
# BYTE FOR BYTE, and print the diff when they disagree.
#
# The only assertion in this harness is "these two byte strings are equal".
# There is no expected-output file anywhere, on purpose: an expectation file
# records what its author believed, and the whole reason this gate exists is
# that nobody's belief about microtask ordering is reliable.
#
# The CONTROL is a case named *_CONTROL.js whose output has been deliberately
# made to differ.  If it does not appear in the failure list, this harness is
# not an instrument and its greens mean nothing.  Rule 5.
set -u
ENGINE="${ENGINE:-build/micro_run}"
NODE="${NODE:-node}"
DIR="$(cd "$(dirname "$0")" && pwd)"
OUT="${OUT:-build/jsmicro-out}"
mkdir -p "$OUT"

fail=0; pass=0; ctl_seen=0; ctl_fired=0
for f in "$DIR"/cases/*.js; do
    b="$(basename "$f" .js)"
    "$NODE" "$DIR/node_run.js" "$f" > "$OUT/$b.node" 2>"$OUT/$b.node.err"
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
    printf 'HARNESS BROKEN: the control did not report a difference.\n'
    exit 2
fi
[ "$ctl_seen" = 1 ] && printf 'control: fired (this harness can report a difference)\n'
exit 0
