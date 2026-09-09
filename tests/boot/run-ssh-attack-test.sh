#!/usr/bin/env bash
# The in-guest ATTACK battery against /bin/sshd: boots LogitOS the same way
# run-ssh-test.sh does (enroll a throwaway account over serial, install a
# freshly minted pubkey, start sshd), but instead of a well-behaved OpenSSH
# client it points tests/boot/ssh_attack_client.py -- a hostile raw-socket
# SSH-2 client sharing no code with the server -- at the forwarded port.
#
# Every attack ends with a clean control login, so a server that "survives"
# an attack by dying cannot pass. Serial evidence is grepped for the attacks
# whose required outcome lives on the serial line (the six AUTH_FAIL lines
# of MaxAuthTries, the CONN_REFUSED of slot exhaustion) -- the wire alone
# cannot distinguish "auth attempts were counted" from "connection died".
#
# Key minted fresh under mktemp -d every run, for the reason run-ssh-test.sh
# gives: a committed private key is a key the whole world holds.
set -u

ISO="${1:?usage: run-ssh-attack-test.sh <iso> <disk-with-sshd.img>}"
DISK="${2:?usage: run-ssh-attack-test.sh <iso> <disk-with-sshd.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
PORT="${SSH_ATTACK_PORT:-2301}"

WORK="$(mktemp -d)"
LOG="$WORK/serial.log"
KEY="$WORK/id_ed25519"
USER="sshatk_$$"
PW="sshatk-pw-$$-k3y"

cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

ssh-keygen -q -t ed25519 -N '' -C sshattack -f "$KEY" </dev/null
chmod 600 "$KEY"
PUBLINE="$(cat "$KEY.pub")"

NET="-netdev user,id=n0,hostfwd=tcp:127.0.0.1:${PORT}-10.0.2.15:22 -device e1000,netdev=n0"

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
    printf 'echo ATTACK_DRIVER_DONE\n'
    sleep 240
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
    echo "----- serial output -----"; cat "$LOG"; echo "-------------------------"
    exit 1
fi

fail=0

echo "--- hostile raw-socket battery (tests/boot/ssh_attack_client.py) ---"
# No `timeout(1)`: macOS has none (run-ssh-test.sh's own header makes the same
# portability point), and the battery self-bounds -- every socket op carries a
# timeout and every attack's wait is capped -- while the feeder's final sleep
# bounds QEMU's life from underneath.
BAT_OUT="$(python3 "$(dirname "$0")/ssh_attack_client.py" 127.0.0.1 "$PORT" \
    --user "$USER" --pw "$PW" --pubkey "$KEY" 2>&1)"
echo "$BAT_OUT"

if echo "$BAT_OUT" | grep -q "BATTERY: .*/.* passed"; then
    passed="$(echo "$BAT_OUT" | sed -n 's/^BATTERY: \(.*\) passed/\1/p')"
    echo "battery: $passed"
else
    echo "FAIL: the battery did not run to completion"
    fail=1
fi
if echo "$BAT_OUT" | grep -q "ATTACK-RESULT .* FAIL"; then
    echo "FAIL: at least one attack's required server behavior was absent (see ATTACK-RESULT lines)"
    fail=1
fi

echo "--- serial evidence ---"
n_fail="$(grep -c "method=password.*tries=" "$LOG")"
if [ "$n_fail" -ge 6 ]; then
    echo "PASS: $n_fail password AUTH_FAIL lines -- MaxAuthTries counted on the serial line"
else
    echo "FAIL: only $n_fail password AUTH_FAIL lines (want >= 6 counted attempts)"
    fail=1
fi
if grep -q "CONN_REFUSED at capacity" "$LOG"; then
    echo "PASS: slot exhaustion logged CONN_REFUSED at capacity"
else
    echo "FAIL: the 9th connection never produced CONN_REFUSED on the serial line"
    fail=1
fi
if grep -q "AUTH_OK user=$USER method=password" "$LOG"; then
    echo "PASS: a clean password login succeeded after the battery (serial AUTH_OK)"
else
    echo "FAIL: no AUTH_OK after the battery -- the server did not survive"
    fail=1
fi

echo "--- a real OpenSSH client still gets in afterwards ---"
SSH_OUT="$(printf 'echo ATTACK_SSH_OK\nexit\n' | ssh -p "$PORT" -i "$KEY" \
    -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o IdentitiesOnly=yes \
    -o PasswordAuthentication=no -o ConnectTimeout=10 \
    "$USER@127.0.0.1" 2>&1)"
if echo "$SSH_OUT" | grep -q "ATTACK_SSH_OK"; then
    echo "PASS: real OpenSSH publickey login works after the battery"
else
    echo "FAIL: OpenSSH client could not log in after the battery"; echo "$SSH_OUT"; fail=1
fi

if [ "$fail" = 0 ]; then
    echo "SSH-ATTACK-TEST-OK: hostile battery survived, controls green"
    exit 0
fi
echo "----- serial output -----"; cat "$LOG"; echo "-------------------------"
exit 1
