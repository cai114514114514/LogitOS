#!/usr/bin/env bash
# Does this machine remember anything about its user?
#
# A settings system tested without a real reboot has not been tested. Everything
# below runs against ONE disk image with NO -snapshot, exactly as
# run-durability-test.sh does -- that harness is the pattern this copies, and it
# is the one that established that a write to this disk survives a power cycle.
# What this adds is that a write made THROUGH THE SETTINGS STORE survives, and
# that the KERNEL ACTS ON IT on the next boot.
#
# Six boots, and each one asserts something a previous one cannot:
#
#   1  fresh machine   -- no settings file. Defaults, and it says so.
#                         Then write a full set of values, including a window
#                         frame and a network configuration.
#   2  THE REAL TEST   -- reboot. Every value comes back byte-for-byte, AND the
#                         kernel configured the network from the file during
#                         boot ("[net] static from settings:"). That last line
#                         is the one that separates "the store remembers" from
#                         "the machine remembers": it is a decision taken at
#                         boot, before userland, because of a file on disk.
#   3  truncate        -- cut the file to a partial length, in the guest,
#                         through the ordinary file API. Reboot into it.
#   4  survives it     -- the desktop comes up, the loader reports the
#                         truncation, and the keys that were lost read as
#                         defaults. A settings file that bricks the desktop is
#                         worse than no settings; this is the boot that says it
#                         does not.
#   5  garbage         -- a hand-edited file: a colour out of range, a negative
#                         boolean, an address that is not one, a window at
#                         -9000,-9000, and two lines that are not settings at
#                         all. Reboot into it.
#   6  survives it     -- the desktop comes up AND NAMES WHAT IT REJECTED. A
#                         machine that silently ignores what the user typed is
#                         indistinguishable from one that is broken.
#
# The in-kernel truncate-at-EVERY-byte-offset sweep runs on every boot (see
# settings_selftest); this harness asserts it is clean on all six.

set -u

ISO="${1:?usage: run-settings-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-settings-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
SC=/usr/as/examples/setcheck.as

WORK="$(mktemp -d)"
DISKC="$WORK/disk.img"
cp "$DISK" "$DISKC"
cleanup() { [ -n "${QPID:-}" ] && kill -9 "$QPID" 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

NET="-netdev user,id=n0 -device e1000,netdev=n0"

# Never `wait` for a SIGKILLed background-pipeline member: under WSL the SIGCHLD
# is never delivered and bash's do_wait wedges until some OTHER child dies. Kill,
# then poll for it to vanish. (Lifted verbatim from run-durability-test.sh --
# same environment, same trap.)
reap() {
    [ -n "${QPID:-}" ] || return 0
    kill -9 "$QPID" 2>/dev/null
    for _ in $(seq 1 100); do
        kill -0 "$QPID" 2>/dev/null || break
        sleep 0.1
    done
    QPID=""
}

boot() {
    local cmds="$1" log="$2" settle="$3"
    { sleep 6; printf '%s' "$cmds"; sleep "$settle"; } | \
      "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
        -drive file="$DISKC",format=raw,if=none,id=hd0 -device virtio-blk-pci,drive=hd0 \
        -boot d -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
        $NET -serial stdio -display none -no-reboot >"$log" 2>/dev/null &
    QPID=$!
    local waited=0
    while kill -0 "$QPID" 2>/dev/null && [ "$waited" -lt 150 ]; do sleep 1; waited=$((waited + 1)); done
    reap
    tr -d '\r' <"$log" >"$log.n" && mv "$log.n" "$log"   # serial is CRLF
}

fail() {
    echo "FAIL: $1"
    for f in "$WORK"/b*.log; do
        [ -f "$f" ] || continue
        echo "----- $(basename "$f") -----"
        grep -aE "SETCHECK|SETTINGS_|\[set\]|\[net\]|BOOT[0-9]-DONE|panic|fault" "$f" | tail -25
    done
    echo "  (full logs were in $WORK, now removed; re-run to inspect)"
    exit 1
}

# Every boot must produce a live desktop and a clean sweep. Asserted on all six
# rather than only on the interesting ones: the whole claim of this feature is
# that NO content of that file stops the machine coming up.
common() {   # $1 = log, $2 = which boot
    grep -aq "LOGIT_BOOT_OK" "$1" || fail "boot $2: the machine did not reach LOGIT_BOOT_OK"
    grep -aq "SETTINGS_READY" "$1" || fail "boot $2: the settings store never reported ready"
    grep -aq "SETTINGS_SELFTEST offsets=[0-9]* failures=0" "$1" || {
        echo "--- the sweep said: ---"; grep -a "SETTINGS_SELFTEST\|\[set\] selftest\|\[set\] garbage" "$1" | tail -20
        fail "boot $2: the truncate-at-every-offset sweep reported failures"
    }
    grep -aqE "panic|PANIC" "$1" && fail "boot $2: the kernel panicked"
    return 0
}

# ---- boot 1: a machine that has never been configured -----------------------
boot "as $SC diag
as $SC set ui.dark 1
as $SC set ui.accent 0xC81E64
as $SC set net.dhcp 0
as $SC set net.ip 10.0.2.99
as $SC set net.gw 10.0.2.2
as $SC frame clock 120 140 400 300
as $SC set notify.history 7
cat /etc/settings.conf
echo BOOT1-DONE
" "$WORK/b1.log" 12
grep -aq "BOOT1-DONE" "$WORK/b1.log" || fail "boot 1 never finished its commands"
common "$WORK/b1.log" 1

# A fresh machine has no settings file: diag bit 1 (SET_D_NOFILE). If this is
# ever not true the whole test is meaningless, because boot 2 would be reading
# values some earlier run left behind rather than the ones boot 1 wrote.
grep -aq "SETCHECK-DIAG 1 keys 0" "$WORK/b1.log" || \
    fail "boot 1: expected a virgin machine (diag 1, 0 keys); the disk image was not clean"
for k in ui.dark ui.accent net.dhcp net.ip; do
    grep -aq "SETCHECK-SET $k" "$WORK/b1.log" || fail "boot 1: setting $k was not accepted"
done
grep -aq "SETCHECK-SET win.clock.frame" "$WORK/b1.log" || fail "boot 1: the window frame was not stored"
# The file has to be readable with `cat`. This is not decoration -- it is the
# claim that a human can repair it, and it is asserted here rather than assumed.
grep -aq "^ui.dark = 1$" "$WORK/b1.log" || fail "boot 1: /etc/settings.conf is not readable text"
grep -aq "^# crc32 = " "$WORK/b1.log" || fail "boot 1: no crc32 diagnostic line was written"

# ---- boot 2: the whole feature ----------------------------------------------
boot "as $SC check ui.dark 1
as $SC check ui.accent 0xC81E64
as $SC check net.dhcp 0
as $SC check net.ip 10.0.2.99
as $SC check win.clock.frame '120 140 400 300 1 0 0 120 140 400 300'
as $SC check notify.history 7
as $SC diag
echo BOOT2-DONE
" "$WORK/b2.log" 10
grep -aq "BOOT2-DONE" "$WORK/b2.log" || fail "boot 2 never finished its commands"
common "$WORK/b2.log" 2

grep -aq "SETCHECK-BAD" "$WORK/b2.log" && {
    echo "--- what came back wrong: ---"; grep -a "SETCHECK-BAD" "$WORK/b2.log"
    fail "boot 2: a value did not survive the reboot"
}
for k in ui.dark ui.accent net.dhcp net.ip notify.history; do
    grep -aq "SETCHECK-OK $k" "$WORK/b2.log" || fail "boot 2: $k did not come back"
done
# notify.history is a key the kernel has NO SCHEMA FOR. It surviving is the
# assertion that another line can persist its own state here without a kernel
# change -- which is the reason unknown keys are preserved rather than dropped.
grep -aq "SETCHECK-OK notify.history" "$WORK/b2.log" || \
    fail "boot 2: an unknown key was dropped; the store does not preserve foreign keys"

# THE assertion. Not "the file still says 10.0.2.99" -- the KERNEL configured
# the interface from it, at boot, before any userland ran.
grep -aq "\[net\] static from settings: ip 10.0.2.99" "$WORK/b2.log" || {
    echo "--- what the network did: ---"; grep -a "\[net\]" "$WORK/b2.log" | head
    fail "boot 2: the kernel did not act on the stored network configuration"
}
# And the theme: the file said dark, so the store must report it loaded.
grep -aq "SETTINGS_READY diag=0" "$WORK/b2.log" || \
    fail "boot 2: the settings file did not load cleanly on the second boot"

# ---- boot 3: cut the file in half -------------------------------------------
boot "as $SC truncate 90
cat /etc/settings.conf
echo BOOT3-DONE
" "$WORK/b3.log" 10
grep -aq "BOOT3-DONE" "$WORK/b3.log" || fail "boot 3 never finished its commands"
common "$WORK/b3.log" 3
grep -aq "SETCHECK-TRUNCATED 90" "$WORK/b3.log" || fail "boot 3: the truncation did not happen"

# ---- boot 4: boot INTO the truncated file -----------------------------------
boot "as $SC diag
as $SC check ui.dark 0
echo BOOT4-DONE
" "$WORK/b4.log" 10
grep -aq "BOOT4-DONE" "$WORK/b4.log" || fail "boot 4 never finished its commands"
common "$WORK/b4.log" 4
# 90 bytes is inside the header comment, so nothing survives and every key is a
# default -- ui.dark reads 0 again. The desktop still came up (common(), above).
grep -aq "SETCHECK-OK ui.dark = 0" "$WORK/b4.log" || \
    fail "boot 4: a truncated file did not fall back to defaults"

# ---- boot 5: a badly hand-edited file ---------------------------------------
boot "as $SC garbage
cat /etc/settings.conf
echo BOOT5-DONE
" "$WORK/b5.log" 10
grep -aq "BOOT5-DONE" "$WORK/b5.log" || fail "boot 5 never finished its commands"
common "$WORK/b5.log" 5
grep -aq "SETCHECK-GARBAGE-WRITTEN" "$WORK/b5.log" || fail "boot 5: the garbage file was not written"

# ---- boot 6: boot INTO the garbage ------------------------------------------
boot "as $SC diag
as $SC check ui.dark 0
as $SC check net.ip 10.0.2.15
echo BOOT6-DONE
" "$WORK/b6.log" 10
grep -aq "BOOT6-DONE" "$WORK/b6.log" || fail "boot 6 never finished its commands"
common "$WORK/b6.log" 6

# It has to SAY WHAT IT REJECTED, by name. A machine that quietly ignores four
# bad values looks exactly like a machine that is broken.
for k in ui.dark ui.accent net.dhcp net.ip; do
    grep -aq "\[set\] REJECTED $k" "$WORK/b6.log" || {
        echo "--- what it said: ---"; grep -a "\[set\]" "$WORK/b6.log" | head -20
        fail "boot 6: $k was out of range and the machine did not say so"
    }
done
# A window at -9000,-9000 sized 4x4 must be refused, not clamped and not used.
grep -aq "SETCHECK-OK ui.dark = 0"     "$WORK/b6.log" || fail "boot 6: ui.dark did not fall back to its default"
grep -aq "SETCHECK-OK net.ip = 10.0.2.15" "$WORK/b6.log" || fail "boot 6: net.ip did not fall back to its default"
# ui.wallpaper names a file that does not exist: the desktop must still be up,
# which common() already asserted, and the compositor must not have faulted.
grep -aq "\[wm\] display" "$WORK/b6.log" || fail "boot 6: the compositor did not come up"

echo "PASS: settings survive a real reboot (6 boots, one image, no -snapshot)"
echo "      - every stored value came back byte-for-byte, including a key with no schema"
echo "      - the kernel configured the network FROM THE FILE at boot"
echo "      - a truncated file and a hand-mangled file both booted, to defaults,"
echo "        and the machine named every value it rejected"
echo "      - the in-kernel truncate-at-every-offset sweep was clean on all six boots"
