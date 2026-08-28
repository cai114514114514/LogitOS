#!/usr/bin/env bash
# run-netlock-test.sh -- step 4a of the BKL removal, asserted on the machine.
#
# WHY THIS CANNOT BE A HOST GATE, said once so nobody looks for one.
# =================================================================
# c/net/core/net.h degenerates net_lock() to a no-op under -DLOGIT_NET_HOST,
# and every host gate in this tree that touches the stack (tcp_test,
# net_proto_test, ip6_*, dhcp_test, the tcpstub headers) compiles that way and
# runs single-threaded. They cannot observe this lock existing, let alone being
# held. Only a boot can, which is why this file exists.
#
# WHAT IT ASSERTS
# ===============
#  1. THE LINE IS THERE AT ALL. `[netlock] acq N recursive R maxdepth D
#     violations V` is printed by c/net/core/net.c's netlock_report() on the
#     lock's own counter (first acquisition, then every 4096) rather than on the
#     receive path's frame count -- because it used to ride rx_report()'s
#     64-frame threshold and `make test-net-os` moves about two dozen frames, so
#     a complete 32 KiB fetch printed nothing at all and read as a machine with
#     nothing to say.
#
#  2. violations == 0. That counter is the whole point of the step. It counts
#     net_lock_assert_held() failing in eth_input()/eth_send() and net_unlock()
#     by a core that does not own the lock -- i.e. every place the old prose
#     contract ("Contract: callers must hold net_lock ... All current paths
#     (eth/ip/tcp/udp/icmp send) do", e1000.c:333) was simply wrong.
#
#  3. acq > 0. A build where net_lock() had been optimised into nothing would
#     satisfy (2) perfectly.
#
#  4. maxdepth >= 2. The lock is RECURSIVE, and that is structural rather than
#     convenient: the driver drain takes it and eth_input -> arp_input, and
#     tcp_output -> ip_output -> arp_output -> arp_resolve, re-take it three
#     levels down. A non-recursive lock self-deadlocks on the first ARP frame
#     this machine receives. maxdepth == 1 would mean the nesting stopped
#     happening and the recursion had stopped earning its keep -- which is a
#     real regression to catch, because the machine would still work.
#
# THE CONTROL is the same script with NETLOCK=negctl, against an ISO built
# `make NETNOTXLOCK=1` -- eth_send() without its acquisition, the shipped code
# on a -D switch rather than a broken version. There it REQUIRES violations > 0.
# Everything a person can see is identical between the two builds, which is
# exactly why the control has to be watched failing instead of argued about.
#
# Usage: run-netlock-test.sh <iso> <disk.img>
#        NETLOCK=negctl run-netlock-test.sh <negctl-iso> <disk.img>
set -u

ISO="${1:?usage: run-netlock-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-netlock-test.sh <iso> <disk.img>}"
MODE="${NETLOCK:-positive}"
QEMU="${QEMU:-qemu-system-x86_64}"
TMP="$(mktemp -d)"
LOG="$TMP/serial.log"

cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null
    [ -n "${KEEP:-}" ] && cp "$LOG" "$KEEP" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

# SLIRP with no host server needed: DHCP alone drives the broadcast transmit
# path this measures, and it is the one udp_send() reaches with nothing held.
# `-snapshot` is legitimate here -- nothing about this gate asserts survival.
"$QEMU" -cdrom "$ISO" -drive file="$DISK",format=raw,if=virtio -snapshot \
        -m 512M -smp 4 -accel tcg,thread=multi \
        -netdev user,id=n0 -device e1000,netdev=n0 \
        -display none -serial "file:$LOG" -no-reboot >/dev/null 2>&1 &
QPID=$!

# TWO WAITS, AND THE SECOND ONE IS THE WHOLE MEASUREMENT.
#
# The first waits for the stack to come up at all -- one [netlock] line means
# net_lock() exists and was taken. Stopping there is what the first version of
# this script did, and it read `acq 3 maxdepth 1` and failed the nesting
# assertion on a machine whose lock nests on every ARP frame: the FIRST
# acquisition is DHCP's TRANSMIT, which happens before a single frame has been
# received, and receiving is the only thing that nests. The line was accurate
# and the moment was wrong.
#
# So the second wait dwells until the report cadence (geometric: 1, 8, 64, 512,
# ...) has produced at least THREE lines, i.e. until the machine has been
# through some hundreds of acquisitions with the receive path running, and every
# assertion below is made against the LAST line rather than the first.
for _ in $(seq 1 45); do
    sleep 1
    grep -q '^\[netlock\]' "$LOG" 2>/dev/null && break
done
for _ in $(seq 1 60); do
    sleep 1
    [ "$(grep -c '^\[netlock\] acq' "$LOG" 2>/dev/null || echo 0)" -ge 3 ] && break
done
sleep 2
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=""

echo "--- [netlock] reports over the boot ---"
grep '^\[netlock\]' "$LOG" || true
echo "---"
LINE="$(grep '^\[netlock\] acq' "$LOG" | tail -1)"
if [ -z "$LINE" ]; then
    echo "FAIL: no [netlock] line in the boot log after 45 s"
    echo "      the stack may not have come up at all; last 20 lines:"
    tail -20 "$LOG"
    exit 1
fi
echo "$LINE"

ACQ="$(printf '%s' "$LINE"   | sed -n 's/.*acq \([0-9]*\).*/\1/p')"
DEPTH="$(printf '%s' "$LINE" | sed -n 's/.*maxdepth \([0-9]*\).*/\1/p')"
VIOL="$(printf '%s' "$LINE"  | sed -n 's/.*violations \([0-9]*\).*/\1/p')"
BUGS="$(grep -c '^\[netlock\] BUG' "$LOG" || true)"

rc=0
if [ "$MODE" = "negctl" ]; then
    # The control. eth_send() takes nothing, so udp_send()'s broadcast path --
    # DHCPDISCOVER, the first datagram this machine sends -- reaches eth_send
    # and eth_input (via the loopback branch) with no lock held.
    if [ "${VIOL:-0}" -gt 0 ] || [ "${BUGS:-0}" -gt 0 ]; then
        echo "negative control ok: violations=$VIOL bug-lines=$BUGS without eth_send's acquisition"
    else
        echo "NEGATIVE CONTROL FAILED: violations=$VIOL with eth_send NOT taking net_lock"
        echo "  either the assertion is not reached on this workload, or the"
        echo "  ISO was not built with NETNOTXLOCK=1 -- check which before"
        echo "  believing the positive run."
        rc=1
    fi
    exit $rc
fi

[ "${ACQ:-0}" -gt 0 ] || { echo "FAIL: acq=$ACQ -- the lock is never taken"; rc=1; }
[ "${VIOL:-1}" -eq 0 ] || { echo "FAIL: violations=$VIOL -- a net path ran without net_lock"; rc=1; }
[ "${BUGS:-1}" -eq 0 ] || { echo "FAIL: $BUGS [netlock] BUG line(s) in the log"; grep '^\[netlock\] BUG' "$LOG"; rc=1; }
[ "${DEPTH:-0}" -ge 2 ] || { echo "FAIL: maxdepth=$DEPTH -- the lock never nested, so the recursion is untested"; rc=1; }

[ $rc -eq 0 ] && echo "PASS: net_lock held on every asserted path (acq=$ACQ maxdepth=$DEPTH violations=0)"
exit $rc
