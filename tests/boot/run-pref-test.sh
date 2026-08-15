#!/usr/bin/env bash
# Does a preference survive a reboot?
#
# The settings store had nine machine keys, a GUI, a host test -- and no way
# for an application, ported or native, to remember ANYTHING about itself.
# /bin/pref is the coreutil front end onto the "app." half of that store (see
# the SYS_SETTING_ENUM comment in include/abi/logit_abi.h); this is the
# harness that proves the half of it that matters is real: a value written in
# one boot is still there in the next.
#
# NO -snapshot, two real boots against ONE disk image (a scratch copy, so the
# build artifact is never touched) -- the same idiom as run-fsmount-test.sh
# and run-durability-test.sh, for the identical reason stated there: every
# OTHER harness in this tree throws the disk away on exit, which is exactly
# how "corrupts after repeated non-snapshot boots" sat unnoticed for as long
# as it did. A store that has only ever been read back inside the boot that
# wrote it has not been shown to remember anything.
#
#   boot 1  pref write an app.-namespace key, read it back (same boot), list
#           it back by prefix, and -- root being the only caller a serial
#           shell CAN be -- write a MACHINE (schema) key and confirm the
#           write succeeds. (The REFUSAL half of "the key namespace is the
#           permission model" needs a non-root caller, which this shell is
#           not; that half belongs in a host-side test that can fabricate a
#           non-root credential (tests/unit/settings_test.c, which owns
#           settings.c's host coverage, does not exercise settings_syscall()'s
#           permission gate as of this writing -- CONFIRMED by grep, not
#           assumed), not in a boot harness that only ever runs as root.
#   boot 2  the SAME disk, cold: read the app key back. This is the
#           headline -- a preference that does not survive a boot is not a
#           preference, it is a variable.
#
# Assertions are exact-line matches on the bare read output ("hello42", not
# "...hello42...") wherever that output could otherwise be confused with the
# write command's own echo, which also contains the value.

set -u

# Wait for /bin/sh to exist before typing at it, rather than sleeping a
# guessed number of seconds. See tests/boot/bootwait.sh for why a longer
# sleep is the same bug with a bigger number.
. "$(dirname "$0")/bootwait.sh"

ISO="${1:?usage: run-pref-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-pref-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"

WORK="$(mktemp -d)"
DISKC="$WORK/disk.img"
cp "$DISK" "$DISKC"
cleanup() { [ -n "${QPID:-}" ] && kill -9 "$QPID" 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

NET="-netdev user,id=n0 -device e1000,netdev=n0"

# Never `wait` for a SIGKILLed background-pipeline member under WSL: the
# SIGCHLD is never delivered and bash's do_wait wedges until some other child
# dies. Kill, then poll for the process to vanish (bounded; a stale zombie
# holds no open files and cannot touch the image the next boot is about to
# open).
reap() {
    [ -n "${QPID:-}" ] || return 0
    kill -9 "$QPID" 2>/dev/null
    for _ in $(seq 1 100); do
        kill -0 "$QPID" 2>/dev/null || break
        sleep 0.1
    done
    QPID=""
}

# One boot against the persistent copy. $1 = what to type, $2 = log,
# $3 = settle seconds after the DONE marker (writes go straight through
# virtio-blk with no cache, so this is settle time for the shell's own
# printing, not a race with the disk).
boot() {
    local cmds="$1" log="$2" settle="$3" done_marker="$4"
    { logit_wait_for_shell "$log" 120; printf '%s' "$cmds"; sleep "$settle"; } | \
      "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
        -drive file="$DISKC",format=raw,if=none,id=hd0 -device virtio-blk-pci,drive=hd0 \
        -boot d -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
        $NET -serial stdio -display none -no-reboot >"$log" 2>/dev/null &
    QPID=$!
    local waited=0
    while kill -0 "$QPID" 2>/dev/null && [ "$waited" -lt 180 ]; do
        grep -aq "$done_marker" "$log" 2>/dev/null && break
        sleep 1; waited=$((waited + 1))
    done
    sleep 1
    reap
    tr -d '\r' <"$log" >"$log.n" && mv "$log.n" "$log"   # serial is CRLF
}

fail() {
    echo "FAIL: $1"
    for f in "$WORK"/b*.log; do
        [ -f "$f" ] || continue
        echo "----- $(basename "$f") -----"
        grep -aE "PREF|panic|fault|cannot" "$f" | tail -25
    done
    echo "  (full logs were in $WORK, now removed; re-run to inspect)"
    exit 1
}

# ---- boot 1: write, read back, list, and a root machine-key write --------
boot "pref write app.probe.k1 hello42
pref read app.probe.k1
pref list app.probe.
pref write ui.dark 1
echo PREF-BOOT1-DONE
" "$WORK/b1.log" 8 "PREF-BOOT1-DONE"

grep -aq "PREF-BOOT1-DONE" "$WORK/b1.log" || fail "boot 1 never finished its commands"
grep -aq "PREF-WRITE-OK app.probe.k1=hello42" "$WORK/b1.log" \
    || fail "boot 1: 'pref write app.probe.k1 hello42' was not confirmed"
grep -aqx "hello42" "$WORK/b1.log" \
    || fail "boot 1: 'pref read app.probe.k1' did not echo the value back"
grep -aq "^app.probe.k1=hello42$" "$WORK/b1.log" \
    || fail "boot 1: 'pref list app.probe.' did not show the key"
grep -aq "PREF-WRITE-OK ui.dark=1" "$WORK/b1.log" \
    || fail "boot 1: a machine-key write from the (root) serial shell was refused -- it must succeed; root is allowed. (The non-root REFUSAL case needs a login this harness does not have, and does not yet have host-side coverage either -- see the file header comment.)"

# ---- boot 2: same disk, cold -- does the preference survive a reboot? ----
boot "pref read app.probe.k1
pref read ui.dark
echo PREF-BOOT2-DONE
" "$WORK/b2.log" 8 "PREF-BOOT2-DONE"

grep -aq "PREF-BOOT2-DONE" "$WORK/b2.log" || fail "boot 2 never finished its commands"
grep -aqx "hello42" "$WORK/b2.log" \
    || fail "boot 2: app.probe.k1 did NOT survive the reboot -- this is the whole point of the test"
grep -aqx "1" "$WORK/b2.log" \
    || fail "boot 2: ui.dark (written by root in boot 1) did not survive the reboot either"

echo "PASS: /bin/pref -- write/read/list all confirmed in boot 1, and"
echo "      app.probe.k1=hello42 (an app-namespace preference) plus ui.dark=1"
echo "      (a machine key, written by root) both survived a real reboot with"
echo "      no -snapshot."
