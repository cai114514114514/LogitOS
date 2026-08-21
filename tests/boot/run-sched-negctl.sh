#!/usr/bin/env bash
# The negative control for the weighted scheduler.
#
# Same disk, same /bin/schedtest, a kernel built with -DSCHED_IGNORE_WEIGHT --
# which is one line in prio_apply() (c/kernel/sched/sched.c): the weight is
# still looked up in the table, still stored, still reported by
# SCHEDCTL_GET_WEIGHT and still readable through getpriority(), and only the
# reciprocal the CHARGE uses stops depending on it.
#
# THE REQUIREMENT IS NOT "IT FAILS". This script requires a specific pattern,
# because "the whole suite went red" would also be satisfied by a kernel that
# did not boot:
#
#   nice=0/0    must still PASS   -- equal weights, so ignoring the weight
#                                    changes nothing, and this is what proves
#                                    the build still runs and still measures
#   nice=0/10   must FAIL         -- expected 2.000, must come back near 1.000
#   nice=-10/10 must FAIL         -- expected 4.000, must come back near 1.000
#   SCHED-RESULT must be 4/6      -- the four API checks are untouched
#
# The "near 1.000" half is checked explicitly rather than inferred from FAIL: a
# case that failed because the child crashed and reported work=0 would also be
# a FAIL, and would prove nothing about the weight. So the ratio of each
# reddened case is required to land inside [0.85, 1.15].
#
# IT IS THE cpu= RATIO THAT IS CHECKED, NOT work=, and that was a correction
# made by watching this script fail on a correct control. The two quantities do
# not have the same noise: work= is iterations completed, and work per
# nanosecond varies with how long a thread's bursts are and with host load. In
# the run that exposed this, the reddened cases read work=1.159 and 1.126 --
# outside a +-15% band -- while THE EQUAL-WEIGHT CASE IN THE SAME RUN read
# work=1.130, i.e. the whole excursion was present with the feature under test
# contributing nothing at all. The cpu= column for the same three cases read
# 0.999, 1.004 and 1.000. Bounding the noisy number would have made this
# control flaky in the direction that matters least.

set -u

ISO="${1:?usage: run-sched-negctl.sh <iso> <disk.img>}"
DISK="${2:?usage: run-sched-negctl.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
LOG="$(mktemp)"
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null; rm -f "$LOG" "$LOG.disk"; }
trap cleanup EXIT

# WORK FROM A PRIVATE COPY OF THE DISK, not from build/disk.img itself. This is
# not caution: THIS script's first run reported "/bin/schedtest: permission
# denied (not executable)" for a program that had executed correctly ten
# minutes earlier on the same image, because another session ran `make
# build/disk.img` while QEMU had the file open. -snapshot stops the GUEST
# writing to the image and does nothing at all about the HOST rewriting it
# underneath. A 64 MiB copy costs well under a second and makes the run
# reproducible whatever else the tree happens to be doing.
DISKCOPY="$LOG.disk"
cp "$DISK" "$DISKCOPY"
DISK="$DISKCOPY"

NET="-netdev user,id=n0 -device e1000,netdev=n0"
{ sleep 11; printf '/bin/schedtest 4000\n'; sleep 180; printf 'exit\n'; sleep 2; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
    -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
    -m 512M -smp 2 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
    $NET -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

for _ in $(seq 1 3000); do
    grep -aq "SCHED-RESULT" "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done

if ! grep -aq "SCHED-RESULT" "$LOG"; then
    echo "FAIL: the control build produced no SCHED-RESULT at all -- it must still RUN"
    tail -80 "$LOG"
    exit 1
fi

grep -a -E "^SCHED-(CASE|RAW|FAIL|RESULT)" "$LOG"

fail=0
# tr -d '\r' everywhere a value is EXTRACTED: the guest console is a serial tty,
# so every line arrives CRLF-terminated and the last field of any line carries a
# trailing CR. The positive gate reported FAIL once for exactly that reason,
# against its own output saying it had passed.
line_for() { grep -a "^SCHED-CASE nice=$1 " "$LOG" | tail -1 | tr -d '\r'; }
verdict()  { echo "$1" | awk '{print $NF}'; }
# cpu=X.YYY -> XYYY, so a shell integer compare can bound it
cpu_x1000() { echo "$1" | tr ' ' '\n' | grep '^cpu=' | sed 's/cpu=//; s/\.//'; }

L=$(line_for "0/0")
[ -n "$L" ] || { echo "FAIL: no 0/0 case in the output"; exit 1; }
if [ "$(verdict "$L")" != "PASS" ]; then
    echo "FAIL: the equal-weight case must still PASS under the control"
    fail=1
fi

for c in "0/10" "-10/10"; do
    L=$(line_for "$c")
    [ -n "$L" ] || { echo "FAIL: no $c case in the output"; fail=1; continue; }
    if [ "$(verdict "$L")" != "FAIL" ]; then
        echo "FAIL: case $c did NOT redden under -DSCHED_IGNORE_WEIGHT"
        fail=1
    fi
    W=$(cpu_x1000 "$L")
    if [ -z "$W" ] || [ "$W" -lt 850 ] || [ "$W" -gt 1150 ]; then
        echo "FAIL: case $c reddened, but its CPU-share ratio ($W/1000) is not ~1.000 --"
        echo "      a crashed child would also redden and would prove nothing"
        fail=1
    fi
done

R=$(grep -a "^SCHED-RESULT" "$LOG" | tail -1 | tr -d '\r' | awk '{print $2}')
if [ "$R" != "4/6" ]; then
    echo "FAIL: expected exactly 4/6 under the control (2 ratio cases red, 4 API checks green), got $R"
    fail=1
fi

if [ "$fail" = 0 ]; then
    echo "PASS: -DSCHED_IGNORE_WEIGHT collapses 2:1 and 4:1 to 1:1 and reddens exactly those 2 of 6"
    exit 0
fi
exit 1
