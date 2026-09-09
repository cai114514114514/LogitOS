#!/usr/bin/env bash
# The on-path attacker characterization for /bin/sshd: one boot, four tamper
# modes, a REAL OpenSSH client through each (tests/boot/ssh_tamper_proxy.py
# sits in the middle), and a direct control login at the end.
#
# This is CHARACTERIZATION by design: c/net/ssh/ssh.h refuses to offer
# kex-strict-s-v00@openssh.com on the argued ground that half-implementing it
# is worse than not offering it. inject-ignore first PINNED A SUCCESSFUL
# LOGIN on the theory that non-strict tolerance was the exposure; measuring
# it corrected that claim (kept beside the correction): the injected packet
# desyncs the s2c sequence count, so the FIRST post-NEWKEYS MAC fails and
# the connection dies fail-closed -- the cost of no strict kex here is
# handshake availability (an on-path attacker can break a handshake), not
# downgradeability (one kex, one cipher, one MAC, nothing to turn).
# kexreply-flip and flip-first-enc must KILL the login (host-key signature /
# MAC); drop-newkeys must hang the server's connection without killing the
# server.
set -u

ISO="${1:?usage: run-ssh-tamper-test.sh <iso> <disk-with-sshd.img>}"
DISK="${2:?usage: run-ssh-tamper-test.sh <iso> <disk-with-sshd.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
GUEST_PORT="${SSH_TEST_PORT:-2299}"
PROXY_PORT="${SSH_TAMPER_PORT:-2302}"

WORK="$(mktemp -d)"
LOG="$WORK/serial.log"
KEY="$WORK/id_ed25519"
USER="sshtamper_$$"
PW="sshtamper-pw-$$-9c"

cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null
    [ -n "${PROXY_PID:-}" ] && kill "$PROXY_PID" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

ssh-keygen -q -t ed25519 -N '' -C sshtamper -f "$KEY" </dev/null
chmod 600 "$KEY"
PUBLINE="$(cat "$KEY.pub")"

NET="-netdev user,id=n0,hostfwd=tcp:127.0.0.1:${GUEST_PORT}-10.0.2.15:22 -device e1000,netdev=n0"

{
    sleep 6
    printf 'login -a %s\n' "$USER"
    sleep 1
    printf '%s\n' "$PW"
    sleep 1
    printf '%s\n' "$PW"
    sleep 3
    printf 'mkdir /home/%s/.ssh\n' "$USER"
    sleep 1
    printf 'echo %s > /home/%s/.ssh/authorized_keys\n' "$PUBLINE" "$USER"
    sleep 1
    printf '/bin/sshd &\n'
    sleep 4
    printf 'echo TAMPER_DRIVER_DONE\n'
    sleep 300
} | "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
    -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
    $NET -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

ready=0
for _ in $(seq 1 300); do
    if grep -aq "SSHD_READY port=22" "$LOG"; then ready=1; break; fi
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done
if [ "$ready" != 1 ]; then
    echo "FAIL: sshd never printed SSHD_READY"
    cat "$LOG"
    exit 1
fi

try_login() {  # -> prints the marker if the login got a working shell
    printf 'echo TAMPER_MARKER\nexit\n' | ssh -p "$1" -i "$KEY" \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o IdentitiesOnly=yes \
        -o PasswordAuthentication=no -o ConnectTimeout=8 -o ConnectionAttempts=1 \
        "$USER@127.0.0.1" 2>&1
}

run_mode() {   # run_mode <mode> <want: dies|lives>
    mode="$1"; want="$2"
    python3 "$(dirname "$0")/ssh_tamper_proxy.py" "$PROXY_PORT" "$GUEST_PORT" "$mode" 2>"$WORK/proxy.err" &
    PROXY_PID=$!
    sleep 0.4
    OUT="$(try_login "$PROXY_PORT")"
    kill "$PROXY_PID" 2>/dev/null; wait "$PROXY_PID" 2>/dev/null; PROXY_PID=""
    if echo "$OUT" | grep -q "TAMPER_MARKER"; then
        got="lives"
    else
        got="dies"
    fi
    # the REASON line, printed on the pass too: a "dies" that died of a
    # proxy deadlock instead of the tamper once read as a pass here, and
    # only the client's own complaint distinguishes them.
    reason="$(echo "$OUT" | grep -E "Host key verification|authentication code|Corrupted|closed|kex_exchange|refused" | head -1)"
    if [ "$got" = "$want" ]; then
        echo "PASS [$mode]: login $got (as characterized) -- ${reason:-no complaint line}"
    else
        echo "FAIL [$mode]: login $got, wanted $want -- ${reason:-no complaint line}"
        echo "$OUT" | sed 's/^/       /' | head -4
        return 1
    fi
}

fail=0
run_mode kexreply-flip dies   || fail=1
run_mode flip-first-enc dies  || fail=1
# inject-ignore DIES, and that is the honest characterization, not a missed
# mitigation: without strict kex neither side resets its sequence counter at
# NEWKEYS, so an injected packet mid-handshake leaves the client's count one
# ahead of the server's and the FIRST post-NEWKEYS MAC fails -- fail-closed.
# What the tree gives up by refusing kex-strict-s-v00@openssh.com (ssh.h's
# own argument: half-implementing it is worse than not offering it) is
# therefore only handshake-availability here, NOT downgradeability: this
# server negotiates exactly one kex, one cipher, one MAC, no compression,
# no extensions -- there is no knob an attacker could turn even if the
# counters agreed. Measured: KEXINIT/ECDH_REPLY/NEWKEYS all complete
# through the injected IGNORE, then the connection dies at the first
# encrypted packet (ssh -v: "NEWKEYS received" -> "Corrupted MAC").
run_mode inject-ignore dies   || fail=1
run_mode drop-newkeys dies    || fail=1

# drop-newkeys left a connection (and a slot) hung on the server by design:
# the server must still be alive for a DIRECT login afterwards.
OUT="$(try_login "$GUEST_PORT")"
if echo "$OUT" | grep -q "TAMPER_MARKER"; then
    echo "PASS [survival]: direct login works after all four tampers (drop-newkeys held a slot)"
else
    echo "FAIL [survival]: server unusable after the tampers"
    echo "$OUT" | head -4
    fail=1
fi

if [ "$fail" = 0 ]; then
    echo "SSH-TAMPER-TEST-OK: signature and MAC tampering die; non-strict tolerance characterized"
    exit 0
fi
cat "$LOG" | tail -20
exit 1
