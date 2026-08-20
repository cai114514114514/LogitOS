#!/usr/bin/env bash
# run-e1000-link-test.sh -- unplug the cable while the machine is running.
#
# WHAT WAS NOT ANSWERABLE BEFORE THIS
# ===================================
# "Is the cable in?" A LogitOS machine could not tell a down link from a network
# with nothing on it: the driver never read STATUS after bring-up, never
# unmasked the link-status-change cause, and printed nothing either way. Both
# states look identical from userland -- packets go out, nothing comes back.
#
# So this drives the one thing a guest cannot fake: QMP `set_link`, which makes
# QEMU's e1000 model clear STATUS.LU and raise ICR.LSC exactly as silicon does
# when somebody pulls the connector.
#
# WHAT THIS GATE DOES AND DOES NOT SEPARATE
# =========================================
# It asserts a COUNT, not a grep:
#
#     [e1000] link: UP 1000 Mb/s full duplex     <- bring-up
#     [e1000] link: DOWN                         <- set_link up=false
#     [e1000] link: UP 1000 Mb/s full duplex     <- set_link up=true
#
# `grep -q 'link: DOWN'` would pass on a driver that printed the link state on
# every poll, and the driver polls STATUS once a second, so a per-poll reporter
# would emit tens of lines over a boot this long. The count catches that.
#
# WHAT IT DOES NOT CATCH, MEASURED RATHER THAN ASSUMED: comparing the WHOLE
# STATUS register instead of the LU/FD/SPEED mask. That is the obvious wrong
# implementation -- STATUS also carries GIO_MASTER_ENABLE, TXOFF and the
# auto-speed-detect value -- and on 2026-08-20 it was built (`return a != b` in
# e1000_link_changed) and run through this harness, which PASSED, with the same
# three lines. Under QEMU's e1000 nothing but LU moves during a boot, so the
# emulator cannot exercise the difference.
#
# That is recorded here rather than quietly dropped, because a gate believed to
# cover a property it does not cover is worse than one nobody claims covers it.
# The property IS covered, on the host, where the switch can be watched
# reddening: `make test-e1000-linkmask-negctl` builds -DE1000_LINK_NEGCTL and
# requires exactly the mask checks to fail.
#
# Usage: run-e1000-link-test.sh <iso> <disk.img>
set -u

ISO="${1:?usage: run-e1000-link-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-e1000-link-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"

TMP="$(mktemp -d)"
LOG="$TMP/serial.log"
SOCK="$TMP/qmp.sock"
QPID=""
cleanup() {
    [ -n "$QPID" ] && kill "$QPID" 2>/dev/null
    sleep 0.3
    [ -n "$QPID" ] && kill -9 "$QPID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

fail() {
    echo "FAIL: $1"
    echo "----- link lines -----"; grep -a '\[e1000\] link:' "$LOG" || echo "(none)"
    echo "----- serial tail -----"; tail -40 "$LOG"
    if [ -s "$TMP/qemu.err" ]; then echo "----- qemu stderr -----"; cat "$TMP/qemu.err"; fi
    exit 1
}

# `id=nic0` on the DEVICE, not just the netdev: qmp_set_link resolves a name to
# a NetClientState and calls that client's link_status_changed. Naming the NIC
# reaches e1000_set_link_status directly; naming the backend reaches it only
# through the peer walk. The driver below tries the NIC first and the backend
# second, so this works on either resolution.
"$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
    -device virtio-blk-pci,drive=hd0 -boot d -snapshot -m 512M -smp 4 \
    -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
    -netdev user,id=n0 -device e1000,netdev=n0,id=nic0 \
    -qmp "unix:$SOCK,server=on,wait=off" \
    -serial file:"$LOG" -display none -no-reboot >"$TMP/qemu.out" 2>"$TMP/qemu.err" &
QPID=$!

wait_for() {   # wait_for <count> <pattern> <seconds>
    local want="$1" pat="$2" secs="$3" i n
    for i in $(seq 1 $((secs * 10))); do
        # grep -c ALREADY prints 0 when it matches nothing, and exits 1 doing
        # it. `|| echo 0` therefore appends a SECOND zero, `[ -ge ]` is handed
        # "0\n0" and rejects it as not-an-integer -- ten lines of shell noise
        # per wait, on a harness whose entire useful output is three lines.
        n=$(grep -ac "$pat" "$LOG" 2>/dev/null) || true
        [ "${n:-0}" -ge "$want" ] && return 0
        kill -0 "$QPID" 2>/dev/null || return 1
        sleep 0.1
    done
    return 1
}

# 1. The machine comes up and says what the link is doing. This is also the
#    control for everything after it: "no line at all" and "DOWN" read the same
#    in a log, so the down assertion means nothing without a prior up.
wait_for 1 '\[e1000\] link: UP' 180 || fail "the driver never reported a link at bring-up"
UP_AT_BOOT="$(grep -ac '\[e1000\] link: UP' "$LOG")"
[ "$UP_AT_BOOT" = "1" ] || fail "expected 1 link-up line at bring-up, got $UP_AT_BOOT"

python3 - "$SOCK" "$LOG" <<'PY' &
import json, socket, sys, time

sock_path, log_path = sys.argv[1], sys.argv[2]

s = socket.socket(socket.AF_UNIX)
deadline = time.time() + 60
while True:
    try:
        s.connect(sock_path); break
    except OSError:
        if time.time() > deadline:
            print("qmp: could not connect", file=sys.stderr); sys.exit(1)
        time.sleep(0.1)
f = s.makefile("rw")
f.readline()                                   # greeting
f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n"); f.flush(); f.readline()

def cmd(execute, arguments=None):
    d = {"execute": execute}
    if arguments is not None:
        d["arguments"] = arguments
    f.write(json.dumps(d) + "\n"); f.flush()
    while True:
        line = f.readline()
        if not line:
            return None
        m = json.loads(line)
        if "return" in m or "error" in m:
            return m

def set_link(up):
    # NIC id first, backend id second. Reported rather than swallowed: a
    # set_link that silently did nothing would make this harness assert that
    # the guest failed to notice an event that never happened.
    for name in ("nic0", "n0"):
        r = cmd("set_link", {"name": name, "up": up})
        if r is not None and "error" not in r:
            print("qmp: set_link %s up=%s" % (name, up), file=sys.stderr)
            return
    print("qmp: set_link FAILED for both nic0 and n0", file=sys.stderr)
    sys.exit(1)

def wait_for(pattern, count, secs):
    end = time.time() + secs
    while time.time() < end:
        try:
            n = open(log_path, "rb").read().decode("utf-8", "replace").count(pattern)
        except FileNotFoundError:
            n = 0
        if n >= count:
            return True
        time.sleep(0.2)
    return False

# Let the desktop settle so the once-a-second STATUS poll has had several
# chances to print a line it must NOT print.
time.sleep(8)
set_link(False)
if not wait_for("[e1000] link: DOWN", 1, 40):
    print("qmp: the guest never reported the link going down", file=sys.stderr)
    sys.exit(2)
time.sleep(6)                                  # more polls that must stay quiet
set_link(True)
if not wait_for("[e1000] link: UP", 2, 40):
    print("qmp: the guest never reported the link coming back", file=sys.stderr)
    sys.exit(3)
time.sleep(6)
sys.exit(0)
PY
PYPID=$!
wait "$PYPID"; PYRC=$?
[ "$PYRC" = "0" ] || fail "the QMP driver failed (rc=$PYRC); see stderr above"

DOWN="$(grep -ac '\[e1000\] link: DOWN' "$LOG")"
UP="$(grep -ac '\[e1000\] link: UP' "$LOG")"
TOTAL="$(grep -ac '\[e1000\] link:' "$LOG")"

[ "$DOWN" = "1" ] || fail "expected exactly 1 'link: DOWN', got $DOWN (per-poll reporting?)"
[ "$UP" = "2" ]   || fail "expected exactly 2 'link: UP', got $UP (per-poll reporting?)"
[ "$TOTAL" = "3" ]|| fail "expected exactly 3 link lines, got $TOTAL"

# Speed and duplex are decoded, not guessed: QEMU's e1000 resets STATUS to
# 0x80080783, which is 1000 Mb/s full duplex. A driver that printed a constant
# would pass every assertion above.
grep -aq '\[e1000\] link: UP 1000 Mb/s full duplex' "$LOG" || \
    fail "the link line does not carry the decoded speed/duplex"

echo "PASS: link UP at boot, DOWN on unplug, UP on replug -- 3 lines, one per transition"
grep -a '\[e1000\] link:' "$LOG" | sed 's/^/    /'
exit 0
