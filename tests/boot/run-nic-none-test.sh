#!/usr/bin/env bash
# The case real hardware hits first: a machine whose network card this kernel
# cannot drive. It must boot to a usable desktop and shell with networking
# simply absent -- not hang in a probe, not fault, and above all not bind a
# driver to a card it does not understand.
#
# Three cases, because they fail differently:
#
#   no-nic       nothing on the PCI bus to find.
#   unknown-nic  `-device ne2k_pci`: a REAL NIC we deliberately do not claim.
#                This is the interesting one. It proves the match tables reject
#                an unrecognised card rather than some driver grabbing it by
#                class and then receiving nothing -- the failure mode that looks
#                like working hardware and is much harder to diagnose than "no
#                network".
#   nonet-fetch  a fetch ATTEMPTED with no NIC returns an error and leaves the
#                shell usable. Run under -smp 1; see the note below.
#
# WHY nonet-fetch IS -smp 1
# -------------------------
# Under -smp 4 a fetch with no NIC deadlocks the machine, and it does so on the
# UNMODIFIED kernel too -- measured at the same commit: 0/3 completions with
# -smp 4, 2/2 with -smp 1, against a tree with none of the NIC work in it. The
# mechanism, from `info registers` on a hung guest: the fetch lands on an
# application processor and holds the big kernel lock while it waits in a
# timer_ticks()-bounded loop; meanwhile the BSP takes its timer interrupt, ticks
# once, and then blocks in spin_lock_irqsave(&g_bkl) with IF=0, so it can never
# take another timer interrupt. Only the BSP advances timer_ticks (see the
# me->index == 0 test in interrupts.c), so the clock stops, the AP's timeout
# never expires, and the two wait on each other forever.
#
# That is a pre-existing SMP bug in the blocking-fetch path (c/net + the BKL),
# not a NIC-driver bug, and it is not this test's business to hide it or to
# fail on it. Running the fetch case on one CPU tests the thing that IS in
# scope -- that the network stack refuses cleanly when there is no interface --
# and the deadlock is written down here rather than lost.
#
# Usage: run-nic-none-test.sh <iso> <disk.img>

set -u

ISO="${1:?usage: run-nic-none-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-nic-none-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
TMP="$(mktemp -d)"

cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

LOG=

fail() {
    echo "FAIL[$LABEL]: $1"
    echo "----- serial output -----"; cat "$LOG"; echo "-------------------------"
    exit 1
}

# boot <label> <smp-args> <script-file> <done-marker> -- <qemu net args>
boot() {
    LABEL="$1"; SMPARGS="$2"; SCRIPT="$3"; MARKER="$4"; shift 5
    LOG="$TMP/$LABEL.log"
    bash "$SCRIPT" | \
      "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
        -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
        -device virtio-blk-pci,drive=hd0 -boot d -snapshot -m 512M $SMPARGS \
        -vga none -device virtio-gpu-pci "$@" \
        -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
    QPID=$!
    for _ in $(seq 1 500); do
        grep -aq "$MARKER" "$LOG" && break
        kill -0 "$QPID" 2>/dev/null || break
        sleep 0.1
    done
    kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=
}

# Common assertions: the kernel came up, found nothing it could drive, bound
# nothing, faulted nowhere, and the shell runs.
assert_degraded() {
    grep -aq "LOGIT_BOOT_OK" "$LOG" || fail "kernel did not reach 64-bit C"
    grep -aq "\[net\] no supported NIC found" "$LOG" \
        || fail "kernel did not report that it found no drivable NIC"
    if grep -aq "\[net\] NIC bound:" "$LOG"; then
        fail "a driver bound a card that should not have matched"
    fi
    if grep -aqE "\[fault\]|PANIC|panic:" "$LOG"; then
        fail "the boot faulted or panicked with no NIC present"
    fi
    grep -aq "NIC_NONE_SHELL_ALIVE" "$LOG" || fail "the shell never came up"
    grep -aq "net: no network interface" "$LOG" \
        || fail "'net info' did not report the missing interface cleanly"
}

cat >"$TMP/boot.sh" <<'SH'
sleep 14
printf 'echo NIC_NONE_SHELL_ALIVE\n'
printf 'net info\n'
sleep 4
printf 'echo NIC_NONE_DONE\n'
sleep 4
SH

cat >"$TMP/fetch.sh" <<'SH'
sleep 14
printf 'echo NIC_NONE_SHELL_ALIVE\n'
printf 'net info\n'
sleep 3
printf 'net get http://10.0.2.2:1/nothing\n'
sleep 20
printf 'echo NIC_NONE_DONE\n'
sleep 5
SH

boot "no-nic" "-smp 4 -accel tcg,thread=multi" "$TMP/boot.sh" NIC_NONE_DONE -- -nic none
assert_degraded
echo "PASS[no-nic]: booted clean with no network device, networking absent, shell alive"

boot "unknown-nic" "-smp 4 -accel tcg,thread=multi" "$TMP/boot.sh" NIC_NONE_DONE -- \
     -netdev user,id=n0 -device ne2k_pci,netdev=n0
assert_degraded
echo "PASS[unknown-nic]: an ne2k_pci is left unclaimed instead of mis-bound; boot is clean"

boot "nonet-fetch" "-smp 1" "$TMP/fetch.sh" NIC_NONE_DONE -- -nic none
assert_degraded
grep -aq "net: fetch failed" "$LOG" \
    || fail "a fetch with no NIC did not return an error (see the -smp 1 note at the top)"
grep -aq "NIC_NONE_DONE" "$LOG" \
    || fail "the shell stopped responding after a fetch attempt with no NIC"
echo "PASS[nonet-fetch]: a fetch with no NIC fails cleanly and the shell survives it"

echo "PASS: a machine with no drivable NIC boots and degrades to no networking"
exit 0
