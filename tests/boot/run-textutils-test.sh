#!/usr/bin/env bash
# The Fluid text tools ON THE MACHINE, against the same sources built on the
# host. The host gate (make test-textutils-host) proves the logic against the
# host's own tools; this proves the /bin binaries -- linked against MINI-LIBC,
# not the host's libc -- do the same thing on LogitOS: getline, qsort,
# fnmatch, opendir and stdio are this tree's, and any disagreement is theirs.
#
# Each case writes its output to /tu/o/N and appends a "." line (a file of
# zero bytes is indistinguishable from one never written, and lfs_extract.py
# refuses it). The machine powers off; the files are read OFF A COPY of the
# disk image and compared byte for byte with the same command run on the host
# by the natively built tools. Serial output is never parsed for results --
# kernel log lines interleave with it.
set -u
. "$(dirname "$0")/bootwait.sh"

ISO="${1:?usage: run-textutils-test.sh <iso> <disk.img> <host-tools-dir>}"
DISK="${2:?}"
HOSTTOOLS="$(cd "${3:?}" && pwd)"    # ABSOLUTE: the golden run cd's into a copy of
                                      # the fixture, and a relative PATH entry then
                                      # silently falls through to the HOST's tools
QEMU="${QEMU:-qemu-system-x86_64}"
CASES=tests/textutils/guest_cases.txt
WORK="$(mktemp -d)"
LOG="$WORK/serial.log"
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

cp "$DISK" "$WORK/disk.img"

# --- the expected outputs: the same commands, the host-built tools ---------
mkdir -p "$WORK/host"
cp -R tests/textutils/fixture/. "$WORK/host/"
mkdir -p "$WORK/host/o"
n=0
while IFS= read -r c; do
    n=$((n + 1))
    ( cd "$WORK/host" && PATH="$HOSTTOOLS:/usr/bin:/bin" LC_ALL=C /bin/sh -c "$c > o/$n; echo . >> o/$n" ) 2>/dev/null
done < "$CASES"
NCASES=$n

# --- the machine ------------------------------------------------------------
SCRIPT="echo warmup
cd /tu
mkdir o
"
n=0
while IFS= read -r c; do
    n=$((n + 1))
    SCRIPT="$SCRIPT$c > o/$n; echo . >> o/$n
"
done < "$CASES"
SCRIPT="${SCRIPT}echo TU-DONE
/bin/poweroff
"

NET="-netdev user,id=n0 -device e1000,netdev=n0"
{ logit_wait_for_shell "$LOG" 120; printf '%s' "$SCRIPT"; sleep 60; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$WORK/disk.img",format=raw,if=none,id=hd0,file.locking=off \
    -device virtio-blk-pci,drive=hd0 -boot d \
    -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
    $NET -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!
for _ in $(seq 1 1800); do
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=

fail=0
grep -aq "^TU-DONE" "$LOG" || { echo "  FAIL: the commands never finished (no TU-DONE on serial)"; fail=1; }
n=0
while IFS= read -r c; do
    n=$((n + 1))
    if ! python3 tests/boot/lfs_extract.py "$WORK/disk.img" "/tu/o/$n" "$WORK/guest.$n" >/dev/null 2>&1; then
        echo "  FAIL: [$n] $c -- no output file on the disk"; fail=1; continue
    fi
    if cmp -s "$WORK/guest.$n" "$WORK/host/o/$n"; then
        echo "  ok: [$n] $c"
    else
        echo "  FAIL: [$n] $c"
        diff "$WORK/host/o/$n" "$WORK/guest.$n" | head -8 | sed 's/^/        /'
        fail=1
    fi
done < "$CASES"
if [ "$fail" -eq 0 ]; then echo "PASS: $NCASES commands, byte-identical to the host build"; exit 0; fi
cp "$WORK/disk.img" "${TU_KEEP:-/tmp}/textutils-guest-disk.img" 2>/dev/null && \
    echo "  (the machine's disk is kept at ${TU_KEEP:-/tmp}/textutils-guest-disk.img)"
echo "----- serial (tail) -----"; tail -30 "$LOG" | tr -d '\r'
exit 1
