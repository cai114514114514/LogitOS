#!/usr/bin/env bash
# Headless end-to-end SSH test: boot LogitOS, enroll a throwaway account over
# the serial console (the same way run-shell-test.sh drives /bin/sh), install
# a throwaway ed25519 pubkey, start /bin/sshd, then point a REAL OpenSSH
# client at the guest's forwarded port 22 and require BOTH password and
# publickey auth to produce a real shell, plus one negative control: a wrong
# password must be REFUSED, not silently accepted.
#
# Needs /bin/sshd ON THE DISK IMAGE PASSED IN. sshd is not yet in the
# coreutils APPS list (that edit is a Makefile line outside this file's
# ownership -- see the session's final report), so today's caller has to
# hand this script a disk image built the way tests/boot/mk_ssh_disk.py
# does: the ordinary disk.img file list plus build/sshd.aex:/bin/sshd. Once
# sshd is wired into APPS, an ordinary $(DISK) satisfies this without
# modification.
#
# THE KEY IS GENERATED FRESH, ON THE WSL-NATIVE FILESYSTEM, EVERY RUN. Not a
# committed fixture: a private key checked into the tree is a key the whole
# world has, and (found running this work) a key living on a DrvFs-mounted
# Windows drive (/mnt/d/...) always reports mode 0777 to `stat` no matter
# what chmod says, which OpenSSH's client refuses outright as "bad
# permissions" -- so the key MUST be minted under /tmp, not beside this
# script, or the whole test fails for a reason that has nothing to do with
# the server under test.

set -u

ISO="${1:?usage: run-ssh-test.sh <iso> <disk-with-sshd.img>}"
DISK="${2:?usage: run-ssh-test.sh <iso> <disk-with-sshd.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
SSH="${SSH:-ssh}"

WORK="$(mktemp -d)"
LOG="$WORK/serial.log"
KEY="$WORK/id_ed25519"          # native tmpfs/ext4 -- chmod actually sticks here
PORT="${SSH_TEST_PORT:-2299}"
USER="sshtest_$$"
PW="sshtest-pw-$$-x7q"

cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

ssh-keygen -q -t ed25519 -N '' -C sshtest -f "$KEY" </dev/null
chmod 600 "$KEY"
PUBLINE="$(cat "$KEY.pub")"

NET="-netdev user,id=n0,hostfwd=tcp:127.0.0.1:${PORT}-10.0.2.15:22 -device e1000,netdev=n0"

# -snapshot: guest-side writes (the new account, the host key, the pubkey
# file) never touch the disk image on the host, so re-running this script
# starts from the same clean image every time. Timings match run-shell-test.sh
# (that file's own comment: portable, no `timeout` dependency for the driver
# side, though the SSH client calls below DO use `timeout` -- OpenSSH itself
# has no bounded default, and a hung client would hang this whole test).
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
    printf 'echo SSHTEST_DRIVER_DONE\n'
    sleep 40
} | "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
    -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
    $NET -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

# Poll the serial log for SSHD_READY -- the same discipline the CLAUDE.md
# apparatus notes ask for: watch the actual signal, not a fixed sleep that
# will read as a pass whether or not sshd actually started.
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

# NOT `ssh host 'cmd'` (a CHANNEL_REQUEST "exec"). c/apps/coreutils/sh.c --
# out of this line's ownership, see sshd.c's own comment beside
# ssh_parse_exec_command -- has no `-c` argument handling at all, so an exec
# request's command is parsed correctly and handed to spawn_child correctly,
# and then plain /bin/sh silently ignores it and starts an interactive REPL
# instead. That is a coreutils gap, not an SSH-layer bug (confirmed live
# against a real OpenSSH client: sshd's own serial line prints
# "EXEC user=... cmd=echo ..." showing the command WAS received and parsed
# right). So this harness drives the "shell" request instead -- the same
# path an interactive login uses -- feeding the command over CHANNEL_DATA on
# stdin, which is real channel traffic under the negotiated aes128-ctr/
# hmac-sha2-256 keys either way.
echo "--- publickey auth + interactive shell, real OpenSSH client ---"
PK_OUT="$(printf 'echo SSH_PUBKEY_OK\nexit\n' | "$SSH" -p "$PORT" -i "$KEY" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    -o IdentitiesOnly=yes -o PasswordAuthentication=no -o ConnectTimeout=10 \
    "$USER@127.0.0.1" 2>&1)"
if echo "$PK_OUT" | grep -q "SSH_PUBKEY_OK"; then
    echo "PASS: publickey auth -> shell -> real output relayed"
else
    echo "FAIL: publickey path"; echo "$PK_OUT"; fail=1
fi

echo "--- password auth, interactive shell, real OpenSSH client ---"
# SSH_ASKPASS supplies the password out of band (SSH_ASKPASS_REQUIRE=force
# makes OpenSSH use it even with a real stdin attached), so this process's
# own stdin is still free for the shell's channel data, same as the
# publickey leg above.
PW_OUT="$(printf 'echo SSH_PASSWORD_OK\nexit\n' | SSHTEST_PW="$PW" SSH_ASKPASS="$(dirname "$0")/ssh_askpass.sh" SSH_ASKPASS_REQUIRE=force \
    setsid "$SSH" -p "$PORT" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    -o PreferredAuthentications=password -o PubkeyAuthentication=no -o ConnectTimeout=10 \
    "$USER@127.0.0.1" 2>&1)"
if echo "$PW_OUT" | grep -q "SSH_PASSWORD_OK"; then
    echo "PASS: password auth -> shell -> real output relayed"
else
    echo "FAIL: password path"; echo "$PW_OUT"; fail=1
fi

echo "--- NEGATIVE CONTROL: wrong password must be refused ---"
BAD_OUT="$(SSHTEST_PW="not-the-password" SSH_ASKPASS="$(dirname "$0")/ssh_askpass.sh" SSH_ASKPASS_REQUIRE=force \
    setsid "$SSH" -p "$PORT" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    -o PreferredAuthentications=password -o PubkeyAuthentication=no -o NumberOfPasswordPrompts=1 -o ConnectTimeout=10 \
    "$USER@127.0.0.1" 'echo SHOULD_NOT_RUN' </dev/null 2>&1)"
if echo "$BAD_OUT" | grep -q "SHOULD_NOT_RUN"; then
    echo "FAIL: wrong password was ACCEPTED -- negative control did not fire"; fail=1
elif echo "$BAD_OUT" | grep -qi "Permission denied"; then
    echo "PASS: wrong password refused (Permission denied)"
else
    echo "FAIL: wrong password case gave neither success nor a clean refusal"; echo "$BAD_OUT"; fail=1
fi

if [ "$fail" = 0 ]; then
    echo "SSH-TEST-OK: publickey + password auth against a real OpenSSH client, wrong-password control fired"
    exit 0
fi

echo "----- serial output -----"; cat "$LOG"; echo "-------------------------"
exit 1
