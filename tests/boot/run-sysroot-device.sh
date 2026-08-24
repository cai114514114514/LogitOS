#!/usr/bin/env bash
# run-sysroot-device.sh -- prove the sysroot ON THE DEVICE is the sysroot the
# host packed, from inside the machine, and measure what the compiler's -I
# probing costs on this VFS.
#
# usage: run-sysroot-device.sh <iso> <base.img> <outdir> <tcg|kvm> [full|readback]
#
# WHAT RUNS ON THE DEVICE (one command per prompt -- the serial console drops
# bytes typed while nothing is reading, tests/boot/run-bigexec.sh measured it,
# so every command waits for the `/ $ ` that follows the previous one):
#
#   ls of every directory under /usr     the COUNT, against what mksysroot.py
#                                        installed (tests/unit/sysroot_dev_verify.py
#                                        walks build/sysroot for the expected set)
#   cat stdio.h | head -n 4              the first 64 bytes through the shell,
#                                        against the host file; a packer that
#                                        gets dirents right and data blocks
#                                        wrong lists right and reads wrong
#   tcc -o /crcwalk /src/crcwalk.c       COMPILED ON THE DEVICE against the
#   /crcwalk /usr/include /usr/lib         sysroot, then a CRC-32 of EVERY file
#                                        -- every byte on the disk, not one file
#   timeit N tcc -E ...                  the preprocessor over the headers tcc's
#                                        own source includes, timed with the TSC
#                                        (tests/unit/sysroot_dev_timeit.c), and
#                                        the output kept on the disk for a
#                                        byte-for-byte comparison with the host
#                                        cross-tcc's -E over build/sysroot
#   /openbench                           what one failed / successful open()
#                                        costs on this VFS, self-scaled to 1 s
#   poweroff                             so the filesystem is synced and the
#                                        host can read tcc's outputs back
#
# NO -snapshot, on a COPY of the image: tcc writes /out*.i to the disk and
# tests/boot/lfs_extract.py reads them off the image afterwards, which is the
# only channel that carries 11-17 KB of preprocessor output back to the host
# without a serial console in between it and the bytes.
#
# `readback` mode is the NEGATIVE CONTROL's driver: the listing, the head,
# and the crcwalk only, against an image with one byte of one header
# corrupted. The verifier then REQUIRES the device to read the corruption.
#
# Timing: every command's send time and the time its prompt returned are
# written to times.txt (host clock, 0.1 s polling), beside timeit's TSC
# numbers from inside -- two clocks that must agree to within the polling.

set -u
. "$(dirname "$0")/bootwait.sh"

ISO="${1:?usage: run-sysroot-device.sh <iso> <base.img> <outdir> <tcg|kvm> [full|readback]}"
BASE="${2:?}"
OUT="${3:?}"
ACCEL="${4:?tcg|kvm}"
MODE="${5:-full}"
QEMU="${QEMU:-qemu-system-x86_64}"
mkdir -p "$OUT"
LOG="$OUT/serial.log"
TIMES="$OUT/times.txt"
IMG="$OUT/run.img"
rm -f "$LOG" "$TIMES"; touch "$LOG" "$TIMES"
cp "$BASE" "$IMG"

case "$ACCEL" in
    kvm) ACCEL_ARGS="-accel kvm"; CPU="${QEMU_CPU:-host}" ;;
    tcg) ACCEL_ARGS="-accel tcg,thread=multi"; CPU="${QEMU_CPU:-max}" ;;
    *) echo "accel must be tcg or kvm"; exit 2 ;;
esac

now_ns() { date +%s.%N; }
prompts() { grep -aoF -- '/ $ ' "$LOG" 2>/dev/null | wc -l; }
LASTP=0
send() {  # send <cmd> [wait-for-prompt-seconds]
    local i n=$(( ${2:-120} * 10 ))
    for ((i = 0; i < n; i++)); do [ "$(prompts)" -gt "$LASTP" ] && break; sleep 0.1; done
    echo "PROMPT $(now_ns)" >> "$TIMES"
    LASTP=$(prompts)
    sleep 0.5
    echo "START $(now_ns) $1" >> "$TIMES"
    printf '%s\n' "$1"
}

drive() {
    logit_wait_for_shell "$LOG" 300
    send 'echo SYS-START' 120
    for d in /usr/include /usr/include/arpa /usr/include/netinet /usr/include/sys \
             /usr/lib /usr/lib/tcc /usr/lib/tcc/include; do
        send "ls $d" 60
    done
    send 'cat /usr/include/stdio.h | head -n 4' 60
    send 'wc /usr/include/stdio.h' 60
    send '/bin/tcc -o /crcwalk /src/crcwalk.c' 600
    send '/crcwalk /usr/include /usr/lib' 600
    if [ "$MODE" = full ]; then
        send '/bin/tcc -o /timeit /src/timeit.c' 600
        send '/timeit 5 /bin/true' 600
        send '/timeit 5 /bin/tcc -E -o /out1.i /src/hdrs.c' 900
        send '/timeit 3 /bin/tcc -E -o /out3.i /src/hdrs_all.c' 900
        send '/bin/tcc -E -P -o /out1p.i /src/hdrs.c' 600
        send '/bin/tcc -E -P -o /out3p.i /src/hdrs_all.c' 600
        send '/bin/tcc -E -nostdinc -I/usr/include -I/usr/lib/tcc/include -o /out2.i /src/hdrs.c' 600
        send 'wc /out1.i /out1p.i /out2.i /out3.i /out3p.i' 120
        send '/bin/tcc -o /openbench /src/openbench.c' 600
        send '/openbench' 600
    fi
    send 'echo SYS-END' 600
    send 'poweroff' 120
    # poweroff does not return; give the machine time to sync and go down.
    sleep 20
}

NET="-netdev user,id=n0 -device e1000,netdev=n0"
T_BOOT=$(now_ns)
drive | \
  "$QEMU" -cpu "$CPU" $ACCEL_ARGS -cdrom "$ISO" \
    -drive file="$IMG",format=raw,if=none,id=hd0,file.locking=off \
    -device virtio-blk-pci,drive=hd0 -boot d \
    -m 512M -smp 4 -vga none -device virtio-gpu-pci \
    $NET -serial stdio -display none -no-reboot >"$LOG" 2>"$OUT/qemu.err" &
QPID=$!
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; return 0; }
trap cleanup EXIT
for _ in $(seq 1 "${WAIT:-18000}"); do
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done
if kill -0 "$QPID" 2>/dev/null; then
    echo "NOTE: QEMU still running after the drive finished (poweroff did not take?) -- killing it" | tee -a "$OUT/notes.txt"
    kill "$QPID" 2>/dev/null
fi
wait "$QPID" 2>/dev/null
echo "SHELL $(grep -aF 'LogitOS shell' "$LOG" | head -1) boot-sent-at $T_BOOT" >> "$TIMES"
echo "run-sysroot-device: $ACCEL $MODE -> $OUT (serial.log $(wc -c < "$LOG") B, $(prompts) prompts)"
[ -s "$OUT/qemu.err" ] && { echo "  qemu stderr:"; head -5 "$OUT/qemu.err" | sed 's/^/    /'; }
grep -aq 'SYS-END' "$LOG" || { echo "FAIL: SYS-END never printed -- the drive did not complete"; exit 1; }
exit 0
