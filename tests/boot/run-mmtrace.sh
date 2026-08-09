#!/usr/bin/env bash
# Boot LogitOS under the mmtrace QEMU plugin and record one workload's page
# reference string.
#
#   tests/boot/run-mmtrace.sh <iso> <disk.img> <workload> <out.mmt>
#
# The workloads are named here rather than passed as a command string because
# each one needs a different amount of time and a different completion marker,
# and a harness that gives up early produces a TRUNCATED trace -- which looks
# exactly like a workload with a small footprint and is the one failure mode
# that would silently corrupt every number downstream. So each workload names
# the line it must see before the machine is allowed to shut down, and the
# script says out loud whether it saw it.
#
#   mempress   /usr/as/examples/mempress.as -- the swap test's own pressure
#              program. Four linear passes over two mmap'd regions.
#   video      /bin/vidcheck on the packed H.264 stream: a real decoder with
#              megabytes of live reference frames, decoded 30x a second and
#              discarded. The closest thing in this tree to a streaming
#              working set.
#   churn      a shell loop of fork+exec'd coreutils: many short-lived address
#              spaces, which is the pattern nothing else here produces.
#   as         the AetherScript examples: an interpreter over its own bytecode.
#
# THE MACHINE IS DELIBERATELY BIG (512 MiB by default, no swap device). The
# trace must be of the WORKLOAD, not of the workload plus this kernel's reclaim
# reacting to it: if pages are being evicted and faulted back during the
# recording, the reference string contains the current policy's own footprints
# and a simulation of a different policy on it is measuring something circular.
# Recorded with memory to spare, the string is policy-independent, and the
# simulator can then be told to pretend memory is any size at all.
set -u

ISO="${1:?usage: run-mmtrace.sh <iso> <disk.img> <workload> <out.mmt>}"
DISK="${2:?usage: run-mmtrace.sh <iso> <disk.img> <workload> <out.mmt>}"
WHAT="${3:?workload: mempress|video|churn|as}"
OUT="${4:?usage: run-mmtrace.sh <iso> <disk.img> <workload> <out.mmt>}"

. "$(dirname "$0")/bootwait.sh"

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
RAM="${MMTRACE_RAM:-512}"
LOG="${MMTRACE_LOG:-$(mktemp)}"
export MMTRACE_OUT="$OUT"
QEMU="${QEMU:-$ROOT/tools/mmtrace/qemu-mmtrace}"

case "$WHAT" in
mempress)
    SIZE="${MMTRACE_SIZE:-small}"
    CMDS=$(printf 'as /usr/as/examples/mempress.as %s\nexit\n' "$SIZE")
    MARK="MEMPRESS-DONE"; WAIT="${MMTRACE_WAIT:-900}" ;;
video)
    CMDS=$(printf '/bin/vidcheck /media/sample.h264\nexit\n')
    MARK="CRC"; WAIT="${MMTRACE_WAIT:-900}" ;;
churn)
    # Deliberately many processes rather than much memory: each iteration is a
    # fork, an execve, an ELF image faulted in and an address space torn down.
    CMDS=$(printf 'echo CHURN-START\n')
    for _ in 1 2 3 4 5 6 7 8; do
        CMDS="$CMDS$(printf 'ls /bin | wc\ncat /etc/motd | head\nuname\npwd\n')"
    done
    CMDS="$CMDS$(printf 'echo CHURN-DONE\nexit\n')"
    MARK="CHURN-DONE"; WAIT="${MMTRACE_WAIT:-300}" ;;
as)
    CMDS=$(printf 'as /usr/as/examples/fib.as\nas /usr/as/examples/durcheck.as\necho AS-DONE\nexit\n')
    MARK="AS-DONE"; WAIT="${MMTRACE_WAIT:-300}" ;;
*)  echo "unknown workload '$WHAT'" >&2; exit 2 ;;
esac

mkdir -p "$(dirname "$OUT")"
echo "=== mmtrace: $WHAT, ${RAM} MiB, trace -> $OUT ==="

cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; }
trap cleanup EXIT

{
  logit_wait_for_shell "$LOG" 240
  printf '%s' "$CMDS"
  sleep "$WAIT"
} | "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
      -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
      -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
      -m "${RAM}M" -smp 1 -accel tcg \
      -vga none -device virtio-gpu-pci \
      -netdev user,id=n0 -device e1000,netdev=n0 \
      -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

# Poll for the marker rather than waiting out the budget: a workload that
# finishes in 40 s should not cost 900.
END=$(( $(date +%s) + WAIT + 240 ))
while [ "$(date +%s)" -lt "$END" ]; do
    grep -aq "$MARK" "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 1
done
# The guest is told to exit, and the plugin's atexit callback is what writes the
# trace header. SIGTERM reaches it too (QEMU runs plugin exit callbacks on a
# clean shutdown signal), but a graceful stop is still preferred.
sleep 3
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null

if grep -aq "$MARK" "$LOG"; then
    echo "PASS: the workload reached '$MARK'"
else
    echo "WARNING: '$MARK' never appeared -- the trace may be of a partial run"
    tail -20 "$LOG"
fi
ls -la "$OUT" 2>/dev/null || { echo "FAIL: no trace was written"; exit 1; }
grep -a "mmtrace:" "$LOG" | tail -2
[ -n "${MMTRACE_LOG:-}" ] || rm -f "$LOG"
