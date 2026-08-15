#!/usr/bin/env bash
# Can this machine be ASKED to stop?
#
# Before this, LogitOS could only be killed from outside -- ACPI was parsed
# solely for CPU discovery (MADT), and no syscall meant "power off". This
# harness is the one gate in the tree that can assert THE QEMU PROCESS EXITS
# BY ITSELF: every other boot test kills qemu in cleanup, which is silent
# about whether the guest was still running or had already gone down.
#
# Three real boots against ONE scratch disk (a copy, so the build artifact is
# never touched, and boot 2 has to see boot 1's write -- no -snapshot):
#
#   boot 1  drive the serial shell: write a known pattern to /pwprobe.txt,
#           then `poweroff`. Assert "[power] syncing" then "[power] going
#           down" appear IN ORDER on serial, then wait for the qemu PROCESS
#           to exit on its own. A kill in cleanup that finds it already gone
#           is the whole point.
#   boot 2  THE DISCRIMINATOR. Same scratch disk: the shell comes up, the
#           write from boot 1 reads back byte-for-byte, and -- the assertion
#           only a journaled filesystem earns -- "[fs] log: replayed" does
#           NOT appear. run-fscrash-test.sh counts that exact string's
#           PRESENCE after a SIGKILL; this harness counts its ABSENCE after
#           a clean shutdown. The two bracket the same property from both
#           sides, which is why a clean poweroff is worth a filesystem
#           assertion and not just a process-exit assertion.
#   boot 3  `reboot`, run WITHOUT -no-reboot so a real guest reset restarts
#           the same qemu PROCESS rather than ending it. The boot banner
#           (LOGIT_BOOT_OK) has to appear TWICE in the one serial log.
#
# THE CONTROL (--negative): proves boot 2's no-replay assertion is capable of
# failing, rather than being true for any kernel that happens not to crash.
# Same write, but SIGKILLed instead of asked to power off; the next boot MUST
# see a replay or an fsck finding. See run_negative_control() below for why
# the kill window is a retry loop rather than one fixed delay.

set -u

# Wait for /bin/sh to exist before typing at it, rather than sleeping a
# guessed number of seconds. See tests/boot/bootwait.sh for why a longer
# sleep is the same bug with a bigger number.
. "$(dirname "$0")/bootwait.sh"

ISO="${1:?usage: run-power-test.sh <iso> <disk.img> [--negative]}"
DISK="${2:?usage: run-power-test.sh <iso> <disk.img> [--negative]}"
NEGATIVE=0
[ "${3:-}" = "--negative" ] && NEGATIVE=1
QEMU="${QEMU:-qemu-system-x86_64}"

WORK="$(mktemp -d)"
cleanup() { [ -n "${QPID:-}" ] && kill -9 "$QPID" 2>/dev/null; [ -n "${PWTEST_KEEP_WORK:-}" ] || rm -rf "$WORK"; }
trap cleanup EXIT

NET="-netdev user,id=n0 -device e1000,netdev=n0"
QARGS="-cpu ${QEMU_CPU:-max} -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci $NET"

normlog() { tr -d '\r' <"$1" >"$1.n" && mv "$1.n" "$1"; }   # serial is CRLF

fail() {
    echo "FAIL: $1"
    for f in "$WORK"/*.log; do
        [ -f "$f" ] || continue
        echo "----- $(basename "$f") -----"
        grep -aE "\[power\]|\[fs\]|\[fsck\]|LOGIT_BOOT_OK|PWTEST|PWNEG|panic|fault|cannot" "$f" | tail -25
    done
    exit 1
}

# Never `wait` for a SIGKILLed background-pipeline member: under WSL the
# SIGCHLD is never delivered and bash's do_wait wedges until some OTHER child
# dies (see run-fscrash-test.sh for the same note). Kill, then poll for the
# pid to vanish -- which is a no-op, and the point, when it already has.
reap() {
    [ -n "${QPID:-}" ] || return 0
    kill -9 "$QPID" 2>/dev/null
    for _ in $(seq 1 100); do
        kill -0 "$QPID" 2>/dev/null || break
        sleep 0.1
    done
    QPID=""
}

# An ordinary boot: type $1, wait for completion marker $5 (breaking out as
# soon as it appears rather than always spending the full cap -- the naive
# version of this loop just polls kill -0 for 150s straight, which is what
# run-durability-test.sh's boot() does and is fine for 5 boots but not for a
# retry loop that may need several), settle $3 more seconds, then WE kill it.
# Used for boot 2 and for the post-kill boot inside the negative control --
# neither of those expects the machine to stop on its own.
boot() {
    local cmds="$1" log="$2" settle="$3" disk="$4" marker="$5"
    { logit_wait_for_shell "$log" 120; printf '%s' "$cmds"; sleep 600; } | \
      "$QEMU" $QARGS -cdrom "$ISO" \
        -drive file="$disk",format=raw,if=none,id=hd0 -device virtio-blk-pci,drive=hd0 \
        -boot d -serial stdio -display none -no-reboot >"$log" 2>/dev/null &
    QPID=$!
    local waited=0
    while kill -0 "$QPID" 2>/dev/null && [ "$waited" -lt 150 ]; do
        grep -aq "$marker" "$log" 2>/dev/null && break
        sleep 1; waited=$((waited + 1))
    done
    sleep "$settle"
    reap
    normlog "$log"
}

# $1 before $2, both present, in $3. A missing marker and an out-of-order one
# are different bugs (kernel never syncs vs. syncs after it has already told
# ACPI to go down) and get different messages.
assert_order() {
    local m1="$1" m2="$2" log="$3"
    local l1 l2
    l1="$(grep -anF "$m1" "$log" | head -1 | cut -d: -f1)"
    l2="$(grep -anF "$m2" "$log" | head -1 | cut -d: -f1)"
    [ -n "$l1" ] || fail "'$m1' never appeared on serial -- poweroff did not even start syncing"
    [ -n "$l2" ] || fail "'$m2' never appeared on serial -- poweroff synced but never told ACPI to go down"
    [ "$l1" -lt "$l2" ] || fail "'$m1' (line $l1) did not come before '$m2' (line $l2)"
}

# ==============================================================================
# THE NEGATIVE CONTROL
# ==============================================================================
# Same write as boot 1, but SIGKILLed instead of asked to power off. Boot 2's
# assertion ("[fs] log: replayed" absent) is only meaningful if the identical
# shape of test CAN see it present -- otherwise a kernel that never journaled
# anything would pass boot 2 for the wrong reason.
#
# THERE IS NO ARMED MARKER HERE the way run-fscrash-test.sh has one (its `as
# durcheck.as crashwrite` prints CRASH-WRITE-ARMED the instant the write
# begins, so that harness can kill at a precisely-known offset into it). A
# plain `echo > file` has no such instrumentation and adding one is outside
# this unit's owned files.
#
# THE DELAYS ARE MEASURED FROM THE SHELL BANNER, NOT FROM THE WRITE, AND THAT
# 0.5s IS NOT SLACK -- IT IS THE FLOOR. logit_wait_for_shell() sleeps 0.5s
# after the banner appears before this function -- and, independently, the
# piped side that actually types the command -- proceed (see bootwait.sh:
# "one scheduling quantum"). Both sides wait it out, so the write is never
# issued before T=0.5s; a delay list that stayed under 0.5s (an early version
# of this loop tried 0.01s-0.05s and burned two attempts finding out) can
# only ever kill before the write starts, which is a miss by construction,
# not evidence the window is narrow. So the list starts AT the floor and
# fans out past it, on the theory that a one-block file write under TCG
# virtio-blk finishes within some tens of milliseconds of starting -- an
# estimate, not a measurement, which is exactly why it is a spread of eight
# instead of one guess.
run_negative_control() {
    echo "=== negative control: SIGKILL instead of poweroff ==="
    local pattern="POWERTEST-NEG-$$-$(date +%s)"
    local attempt ok=0
    local delays="0.50 0.55 0.60 0.70 0.85 1.05 1.30 1.60"
    local n=0

    for d in $delays; do
        n=$((n + 1)); attempt=$n
        local diskn="$WORK/diskneg$attempt.img"
        local cn="$WORK/neg$attempt.log"
        local vn="$WORK/negv$attempt.log"
        cp "$DISK" "$diskn"

        { logit_wait_for_shell "$cn" 120; printf 'echo %s > /pwprobe.txt\n' "$pattern"; sleep 600; } | \
          "$QEMU" $QARGS -cdrom "$ISO" \
            -drive file="$diskn",format=raw,if=none,id=hd0 -device virtio-blk-pci,drive=hd0 \
            -boot d -serial stdio -display none -no-reboot >"$cn" 2>/dev/null &
        QPID=$!

        # Poll the raw banner directly (NOT logit_wait_for_shell, which would
        # add its own 0.5s on top of ours) so `d` is the real elapsed time
        # since the banner, matching what the piped side is also measuring.
        local waited=0
        while [ "$waited" -lt 1200 ]; do
            grep -aq "$LOGIT_SHELL_BANNER" "$cn" 2>/dev/null && break
            kill -0 "$QPID" 2>/dev/null || break
            sleep 0.1; waited=$((waited + 1))
        done
        sleep "$d"
        reap
        normlog "$cn"

        boot "echo PWNEG-BOOT2-DONE
" "$vn" 3 "$diskn" "PWNEG-BOOT2-DONE"
        if ! grep -aq "PWNEG-BOOT2-DONE" "$vn"; then
            echo "  attempt $attempt (+${d}s): boot 2 never finished (kill may have wedged the disk) -- retrying"
            continue
        fi
        if grep -aq "\[fs\] log: replayed" "$vn"; then
            echo "  attempt $attempt (+${d}s): caught it -- the journal replayed an interrupted transaction"
            ok=1; break
        fi
        if grep -aqE "\[fsck\] mount check: [0-9]+ problem" "$vn"; then
            echo "  attempt $attempt (+${d}s): caught it -- mount-time fsck reported a finding"
            ok=1; break
        fi
        echo "  attempt $attempt (+${d}s): nothing to replay and fsck was clean -- the kill missed the window"
    done

    if [ "$ok" != 1 ]; then
        echo "FAIL: attempts at +${delays// /s, +}s past the shell banner never landed"
        echo "      inside a journal transaction. Reporting honestly rather than dropping"
        echo "      the control: boot 2's no-replay assertion has not been shown capable of"
        echo "      failing on this host, and the delay list needs hand-tuning further."
        for f in "$WORK"/neg*.log "$WORK"/negv*.log; do
            [ -f "$f" ] || continue
            echo "----- $(basename "$f") -----"
            grep -aE "\[power\]|\[fs\]|\[fsck\]|PWNEG|panic|fault" "$f" | tail -15
        done
        exit 1
    fi
    echo "PASS: negative control -- a SIGKILL in the write window leaves the journal"
    echo "      with something to replay (or an fsck finding), proving the positive"
    echo "      test's no-replay assertion is a real assertion and not a tautology"
    exit 0
}

[ "$NEGATIVE" = 1 ] && run_negative_control

# ==============================================================================
# BOOT 1: poweroff -- the headline. Work on a scratch copy (cp, not
# -snapshot): boot 2 below must see this boot's write.
# ==============================================================================
DISKC="$WORK/disk.img"
cp "$DISK" "$DISKC"
PATTERN="POWERTEST-$$-$(date +%s)"
b1="$WORK/b1.log"

{ logit_wait_for_shell "$b1" 120; printf 'echo %s > /pwprobe.txt\npoweroff\n' "$PATTERN"; sleep 600; } | \
  "$QEMU" $QARGS -cdrom "$ISO" \
    -drive file="$DISKC",format=raw,if=none,id=hd0 -device virtio-blk-pci,drive=hd0 \
    -boot d -serial stdio -display none -no-reboot >"$b1" 2>/dev/null &
QPID=$!

# Sync with the piped side's own wait so the 20s clock below times the
# machine's response to `poweroff`, not this script's boot-time guess.
logit_wait_for_shell "$b1" 120

exited=0
for _ in $(seq 1 200); do   # ~20s
    if ! kill -0 "$QPID" 2>/dev/null; then exited=1; break; fi
    sleep 0.1
done
reap
normlog "$b1"

[ "$exited" = 1 ] || fail "boot 1: qemu did not exit within ~20s of poweroff being typed -- either the syscall never reached ACPI, or it reached it and QEMU did not honour S5"
echo "PASS: qemu exited on its own after poweroff"
assert_order "[power] syncing" "[power] going down" "$b1"
echo "PASS: [power] syncing then [power] going down, in order"

# ==============================================================================
# BOOT 2: the discriminator, same scratch disk.
# ==============================================================================
b2="$WORK/b2.log"
boot "cat /pwprobe.txt
echo PWTEST-BOOT2-DONE
" "$b2" 3 "$DISKC" "PWTEST-BOOT2-DONE"

grep -aq "PWTEST-BOOT2-DONE" "$b2" || fail "boot 2: never finished its commands -- did the machine come back up at all?"
grep -aq "\[fs\] mounted" "$b2" || fail "boot 2: filesystem did not mount"
grep -aqF "$PATTERN" "$b2" || fail "boot 2: /pwprobe.txt did not read back the pattern written in boot 1 -- the write did not survive the poweroff"
echo "PASS: /pwprobe.txt survived the poweroff, byte-for-byte"

if grep -aq "\[fs\] log: replayed" "$b2"; then
    fail "boot 2: '[fs] log: replayed' appeared -- a CLEAN poweroff left the journal something to finish, so the shutdown was not actually clean"
fi
echo "PASS: no journal replay after a clean poweroff (contrast run-fscrash-test.sh, which counts this string's PRESENCE after a SIGKILL)"

# ==============================================================================
# BOOT 3: reboot. Run WITHOUT -no-reboot so a guest-triggered reset restarts
# the same qemu PROCESS instead of ending it -- the boot banner must then
# appear twice in the one serial log. Fresh disk: this boot writes nothing
# and asserting on it would only add a second thing that could fail.
# ==============================================================================
DISKR="$WORK/diskr.img"
cp "$DISK" "$DISKR"
b3="$WORK/b3.log"

{ logit_wait_for_shell "$b3" 120; printf 'reboot\n'; sleep 600; } | \
  "$QEMU" $QARGS -cdrom "$ISO" \
    -drive file="$DISKR",format=raw,if=none,id=hd0 -device virtio-blk-pci,drive=hd0 \
    -boot d -serial stdio -display none >"$b3" 2>/dev/null &
QPID=$!

waited=0
while [ "$waited" -lt 900 ]; do   # up to 90s: two boots plus the reset itself
    n="$(grep -ac "LOGIT_BOOT_OK" "$b3" 2>/dev/null || true)"
    [ -n "$n" ] || n=0
    [ "$n" -ge 2 ] && break
    kill -0 "$QPID" 2>/dev/null || break   # qemu died instead of resetting
    sleep 0.1
    waited=$((waited + 1))
done
reap
normlog "$b3"

n="$(grep -ac "LOGIT_BOOT_OK" "$b3")"
[ "$n" -ge 2 ] || fail "boot 3: boot banner appeared $n time(s), expected >= 2 -- reboot did not actually reset the guest"
echo "PASS: boot banner appeared $n times in one serial log -- reboot really reset the guest"

echo
echo "PASS: make test-power -- poweroff syncs, goes down, and QEMU exits by itself;"
echo "      the write survives and the journal has nothing to replay; reboot"
echo "      round-trips through a real guest reset"
