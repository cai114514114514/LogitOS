#!/usr/bin/env bash
# The four silent truncations, ON THE MACHINE -- each at its limit (must work,
# whole) and one past it (must be refused, out loud, and NOT run).
#
#   (a) /bin/sh's argv bound     MAXARG words run; MAXARG+1 refused, no echo
#   (b) /bin/sh's line bound     LINE-1 bytes run; LINE bytes refused
#   (c) the 60-byte file name    59 creates and lists; 60 is refused by touch
#                                with a non-zero status, and never listed
#   (d) the kernel's argv bound  257 entries -> LOGIT_EXEC_E2BIG (via /bin/as,
#                                which builds the vector by hand; the shell
#                                cannot overshoot its own bound)
#
# Measured before the fix (2026-08-20): echo with 40 arguments printed 31; a
# 600-byte line ran as 506; touch of a 60-byte name returned 0 and the file
# was not there; a 49-entry execve ran with 48. None of them printed a thing.
#
# Every assertion is shaped so that the OLD behaviour fails it, not merely so
# the new one passes: the "past the limit" cases look for the ABSENCE of the
# output the truncated command would have produced, anchored so an echo of the
# typed line (if the console ever echoes) cannot satisfy them.

set -u

. "$(dirname "$0")/bootwait.sh"

ISO="${1:?usage: run-argv-limits-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-argv-limits-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
LOG="$(mktemp)"
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null; rm -f "$LOG"; }
trap cleanup EXIT

# The limits, as the build defines them. Read from the headers rather than
# typed here, so this harness cannot quietly test yesterday's numbers.
ARG_MAX=$(sed -n 's/^#define LOGIT_ARG_MAX[[:space:]]*\([0-9]*\).*/\1/p' include/abi/logit_exec.h)
LINE=$(sed -n 's/^#define LINE[[:space:]]*\([0-9]*\).*/\1/p' c/apps/coreutils/sh.c)
NAME=$(sed -n 's/^#define LFS_NAME_MAX[[:space:]]*\([0-9]*\).*/\1/p' c/fs/logitfs_fmt.h)
[ -n "$ARG_MAX" ] && [ -n "$LINE" ] && [ -n "$NAME" ] || { echo "FAIL: could not read the limits from the headers"; exit 1; }
NAMEMAX=$((NAME - 1))

rep() { local s="" i; for ((i = 1; i <= $2; i++)); do s="$s$1"; done; printf '%s' "$s"; }
words() { local s="" i; for ((i = 1; i <= $2; i++)); do s="$s $1$i"; done; printf '%s' "$s"; }

# (a) "echo" + (ARG_MAX-1) words = ARG_MAX words, the most one command may carry.
P=$(words P $((ARG_MAX - 1)))
N=$(words N "$ARG_MAX")
# (b) "echo " + Y + " | wc" = LINE-1 bytes exactly; one more y is one past.
YOK=$(rep y $((LINE - 1 - 5 - 5)))
YNO=$(rep y $((LINE - 5 - 5)))
# (c)
AOK=$(rep a "$NAMEMAX")
BNO=$(rep b $((NAMEMAX + 1)))

SCRIPT="echo ARGV-START
echo$P | wc
echo$N | wc
echo ARGS-STATUS \$?
echo $YOK | wc
echo $YNO | wc
echo LINE-STATUS \$?
mkdir /nmtest
touch /nmtest/$AOK
echo TOUCH-OK-STATUS \$?
touch /nmtest/$BNO
echo TOUCH-LONG-STATUS \$?
ls /nmtest
/bin/as /usr/as/examples/argvlimit.as
echo ARGV-END
exit
"

NET="-netdev user,id=n0 -device e1000,netdev=n0"
{ logit_wait_for_shell "$LOG" 120; printf '%s' "$SCRIPT"; sleep 15; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
    -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
    -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
    $NET -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

for _ in $(seq 1 600); do
    grep -aq "ARGV-END" "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done

fail=0
need()  { if grep -aqE "$1" "$LOG"; then echo "  ok: $2"; else echo "  FAIL: $2   (no match for /$1/)"; fail=1; fi; }
count() { local n; n=$(grep -acE "$1" "$LOG"); if [ "$n" -eq "$2" ]; then echo "  ok: $3 ($n)"; else echo "  FAIL: $3   (want $2 matches of /$1/, got $n)"; fail=1; fi; }

echo "argv/line/name limits on the machine: ARG_MAX $ARG_MAX, LINE $LINE, NAME_MAX $NAMEMAX"
# What an exec COSTS on this boot, from the kernel's own accounting -- printed
# pass or fail, because the eager stack window is now sized from the argv and
# this is the line that would show it being sized wrong.
grep -a '^\[exec\] [0-9]* execs' "$LOG" | sed 's/^/  cost: /'
grep -a '^\[execve\] .*too big\|^\[fs\] .*name refused\|open refused by' "$LOG" | sed 's/^/  kernel: /'
need  "ARGV-START" "the script reached the shell"

# (a) wc prints "lines words bytes". ARG_MAX-1 words through echo; the refused
#     line must not produce a SECOND such count -- that is what a dropped
#     word would look like.
count "^1 $((ARG_MAX - 1)) [0-9]+" 1           "(a) $ARG_MAX words (the limit) ran whole, once"
need  "too many words in one command \(limit $ARG_MAX " "(a) $((ARG_MAX + 1)) words refused, naming the limit"
need  "^ARGS-STATUS 1"                         "(a) the refusal is status 1, not 0"

# (b) LINE-1 bytes through wc; LINE bytes refused. A prefix-run would print a
#     line of y's to the console (the `| wc` is what gets truncated off).
# (the console ends its lines with CR LF, so nothing here anchors on $)
count "^1 1 $((LINE - 1 - 5 - 5 + 1))([^0-9]|\$)" 1 "(b) a $((LINE - 1))-byte line (the limit) ran whole, once"
need  "line too long \(limit $((LINE - 1)) bytes\)" "(b) a $LINE-byte line refused, naming the limit"
# (a line of NOTHING but y's is echo's output; the console's echo of the typed
#  line ends in "| wc" and so does not count)
count "^y+\r?\$" 0                             "(b) no prefix of the refused line ran"
need  "^LINE-STATUS 1"                         "(b) the refusal is status 1"

# (c) The listing is the proof; lines naming /nmtest/ are the typed commands
#     and are excluded so an echo of them cannot count.
need  "^TOUCH-OK-STATUS 0"                     "(c) touch of a $NAMEMAX-byte name succeeds"
if grep -av '/nmtest/' "$LOG" | grep -aqE "a{$NAMEMAX}"; then echo "  ok: (c) the $NAMEMAX-byte name is listed"; else echo "  FAIL: (c) the $NAMEMAX-byte name is not listed"; fail=1; fi
need  "touch: cannot create /nmtest/b{$((NAMEMAX + 1))}" "(c) touch of a $((NAMEMAX + 1))-byte name is refused out loud"
need  "^TOUCH-LONG-STATUS 1"                   "(c) ...with status 1 (it used to be 0)"
if grep -av '/nmtest/' "$LOG" | grep -aqE "b{$((NAMEMAX + 1))}"; then echo "  FAIL: (c) the $((NAMEMAX + 1))-byte name appears in a listing"; fail=1; else echo "  ok: (c) the $((NAMEMAX + 1))-byte name is not listed"; fi
need  "open refused by may_create: /nmtest/b{$((NAMEMAX + 1))}" "(c) the kernel named the gate that refused it"

# (d) The kernel's own bound, reached by a hand-built vector.
need  "argvlimit: 257 entries -> rc -7 E2BIG: true"  "(d) $((ARG_MAX + 1)) entries -> LOGIT_EXEC_E2BIG"
need  "argvlimit: 16 KiB string -> rc -7 E2BIG: true" "(d) a string past LOGIT_ARG_BYTES -> LOGIT_EXEC_E2BIG"
need  "k ARGVLIMIT-LAST-256"                   "(d) $ARG_MAX entries run and the LAST one arrives"
need  "argvlimit: 256 entries exit 0"          "(d) ...and the child exited 0"
need  "\[execve\] /bin/true: argv too big -- the limit is $ARG_MAX entries" "(d) the kernel log names the limit"
need  "argvlimit ok"                           "(d) the script ran to the end"

need  "ARGV-END" "the script ran to the end"

if [ "$fail" -eq 0 ]; then
    echo "PASS: every limit runs whole at the bound and refuses, out loud, one past it"
    exit 0
fi
echo "FAIL: see above"
echo "----- serial output (tail) -----"
tail -c 12000 "$LOG"
echo "--------------------------------"
exit 1
