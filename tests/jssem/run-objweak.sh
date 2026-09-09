#!/usr/bin/env bash
# Differential: node is the ORACLE, this tree's QuickJS is the subject.
# Each case is a program that PRINTS; the two stdouts are diffed byte for byte,
# the c/apps/libc gate shape. No hand-written expectations anywhere.
#
#   run-objweak.sh <engine-binary> [outdir]
#
# The engine binary takes one argument, a .js file, and evals it with a global
# `print`. TZ is pinned so Date rows are about parsing and not about the host.
set -u

ENGINE="${1:?usage: run-objweak.sh <engine-binary> [outdir]}"
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${2:-$HERE/out}"
NODE="${NODE:-node}"
mkdir -p "$OUT"
export TZ=UTC

# The prelude gives node the same `print` the engine's C harness installs, and
# NOTHING ELSE. It emits no output of its own, so a byte diff of stdout is a
# diff of the case.
PRELUDE='globalThis.print=function(){var a=Array.prototype.slice.call(arguments),s="";for(var i=0;i<a.length;i++){s+=typeof a[i]==="string"?a[i]:String(a[i]);if(i+1<a.length)s+=" ";}process.stdout.write(s+"\n");};'

fail=0
for c in "$HERE"/cases/*.js; do
  name="$(basename "$c" .js)"

  # The control's planted difference is injected by the HARNESS, one value per
  # side, so the diff it produces is guaranteed and is about the instrument.
  nplant=''; eplant=''
  case "$name" in
    99-control)
      nplant='var deliberate_difference="ORACLE-SIDE";'
      eplant='var deliberate_difference="SUBJECT-SIDE";'
      ;;
  esac

  { printf '%s\n' "$PRELUDE$nplant"; cat "$c"; } > "$OUT/$name.node.js"
  { printf '%s\n' "$eplant"; cat "$c"; }            > "$OUT/$name.engine.js"

  "$NODE" "$OUT/$name.node.js"   >"$OUT/$name.node.out"   2>"$OUT/$name.node.err"
  echo "exit=$?" >> "$OUT/$name.node.err"
  "$ENGINE" "$OUT/$name.engine.js" >"$OUT/$name.engine.out" 2>"$OUT/$name.engine.err"
  echo "exit=$?" >> "$OUT/$name.engine.err"

  # STRIP THE HARNESS FRAMING BEFORE COMPARING, and this is a fix rather than a
  # convenience. js_sem_probe.c prints "=== BEGIN <path>" / "=== END <path>"
  # around each case and one "JSSEM-DONE" at exit, and its own comment says why:
  # "the BEGIN/END markers exist so the guest run can be sliced out of a serial
  # log". node prints none of them. A raw diff of the two therefore reported
  # every case as DIFF -- 19 of 19, forever, three or four phantom lines each --
  # so this gate COULD NOT GO GREEN and its per-case "changed lines" count was
  # inflated by the instrument. That is CLAUDE.md rule 1 in its usual shape: the
  # measurement was right and the frame around it sent the reader somewhere
  # else. The markers are load-bearing for the guest and are kept; they are
  # removed here, on the comparison, where they were never part of the case.
  sed -e '/^=== BEGIN /d' -e '/^=== END /d' -e '/^JSSEM-DONE$/d' \
      "$OUT/$name.engine.out" > "$OUT/$name.engine.cmp"
  cp "$OUT/$name.node.out" "$OUT/$name.node.cmp"
  if diff -u "$OUT/$name.node.cmp" "$OUT/$name.engine.cmp" > "$OUT/$name.diff" 2>&1; then
    echo "SAME  $name"
    [ "$name" = "99-control" ] && { echo "  !! CONTROL DID NOT DIFFER -- THIS HARNESS DETECTS NOTHING"; fail=1; }
  else
    n=$(grep -c '^[-+][^-+]' "$OUT/$name.diff" || true)
    echo "DIFF  $name  ($n changed lines)"
    [ "$name" = "99-control" ] || fail=1
  fi
done
exit $fail
