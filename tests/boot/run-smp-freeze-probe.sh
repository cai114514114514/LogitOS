#!/usr/bin/env bash
# Where is each core when the machine wedges?
#
# run-smp-fork-storm.sh says THAT it wedges and at which rep; this says WHERE.
# It drives the same fork/exec storm, watches the serial log go quiet, and then
# asks QEMU -- through QMP, while the guest is still frozen -- for every core's
# RIP, which it maps to a kernel symbol. That pairing is the whole point: a
# stalled machine's registers are the only evidence that survives, and a symbol
# per core is what distinguishes "all four spinning in the same lock" (a
# deadlock) from "one core wedged and three idle" (something else entirely).
#
# The technique is the one docs/superpowers/specs/2026-06-08-smp-bkl-deadlock.md
# used to catch the previous deadlock of this class, and is written down here so
# it does not have to be reinvented a third time.
#
#   run-smp-freeze-probe.sh <iso> <disk.img> [reps] [smp]
set -u

ISO="${1:?usage: run-smp-freeze-probe.sh <iso> <disk.img> [reps] [smp]}"
DISK="${2:?usage: run-smp-freeze-probe.sh <iso> <disk.img> [reps] [smp]}"
REPS="${3:-60}"
SMP="${4:-4}"
QEMU="${QEMU:-qemu-system-x86_64}"
PROG="${PROG:-/bin/libctest}"
ELF="${ELF:-build/kernel.elf}"
LOG="$(mktemp)"
SOCK="$(mktemp -u)"
QPID=""
cleanup() {
    [ -n "$QPID" ] && kill "$QPID" 2>/dev/null
    [ -n "$QPID" ] && wait "$QPID" 2>/dev/null
    rm -f "$LOG" "$SOCK"
}
trap cleanup EXIT

cmds=""
for i in $(seq 1 "$REPS"); do
    cmds="${cmds}${PROG}
echo STORM-$i-OK
"
done

ACCEL="-accel tcg"
[ "$SMP" != "1" ] && ACCEL="-accel tcg,thread=multi"

{ sleep 4; printf '%s' "$cmds"; sleep 600; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
    -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
    -m 512M -smp "$SMP" $ACCEL -vga none -device virtio-gpu-pci \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -qmp "unix:$SOCK,server,nowait" \
    -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

# Quiet for QUIET_S seconds with the run unfinished == wedged. The storm prints
# on every rep, so silence is unambiguous.
QUIET_S=25
last_size=0
quiet=0
wedged=0
for _ in $(seq 1 150); do
    grep -aq "STORM-$REPS-OK" "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sz=$(wc -c < "$LOG")
    if [ "$sz" = "$last_size" ]; then
        quiet=$((quiet + 2))
        [ "$quiet" -ge "$QUIET_S" ] && { wedged=1; break; }
    else
        quiet=0
        last_size=$sz
    fi
    sleep 2
done

reached=0
for i in $(seq 1 "$REPS"); do grep -aq "STORM-$i-OK" "$LOG" && reached=$i; done

if [ "$wedged" != "1" ]; then
    echo "no freeze: reached $reached/$REPS (nothing to probe)"
    exit 0
fi
echo "FROZEN after $reached/$REPS reps -- probing $SMP core(s)"

regs=$(python3 - "$SOCK" <<'PY'
import json, socket, sys
s = socket.socket(socket.AF_UNIX)
s.connect(sys.argv[1])
f = s.makefile("rw")
f.readline()                                   # greeting
f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n"); f.flush(); f.readline()
f.write(json.dumps({"execute": "human-monitor-command",
                    "arguments": {"command-line": "info registers -a"}}) + "\n")
f.flush()
print(json.loads(f.readline()).get("return", ""))
PY
)

echo "$regs" | grep -aE "^CPU#|^RIP" | sed 's/^/    /'

# WHICH LOCK. The lock pointer is the first argument, so it is in RDI at the
# call and stays there across the spin (spin_lock_irqsave does not clobber it).
# Four cores in the same lock is a deadlock on that lock; four cores in
# DIFFERENT locks is a cycle, and the cycle is the bug.
echo "--- the lock each core is waiting for (RDI) ---"
echo "$regs" | grep -aoE "RDI=[0-9a-f]+" | sed 's/RDI=//' | while read -r a; do
    sym=$(python3 - "$ELF" "$a" <<'PY'
import subprocess, sys
elf, want = sys.argv[1], int(sys.argv[2], 16)
best, bestn = None, 0
for ln in subprocess.run(["nm", "-n", elf], capture_output=True, text=True).stdout.splitlines():
    p = ln.split()
    if len(p) < 3 or p[1].upper() not in "BDGRSV":
        continue
    try:
        a = int(p[0], 16)
    except ValueError:
        continue
    if a <= want and a > bestn:
        bestn, best = a, p[2]
print("%s+0x%x" % (best, want - bestn) if best else "?")
PY
)
    echo "    $a  $sym"
done

# THE LOCK WORDS THEMSELVES, plus who holds each one -- spinlock_t records the
# return address of the acquiring caller precisely so a freeze has an answer to
# "which code took this and never gave it back".
# TWICE, three seconds apart. A single snapshot cannot tell a core that is
# STUCK at an address from one that merely happened to be there -- and the
# difference decides whether the address means anything at all.
echo "--- second look, 3s later (identical == truly stuck) ---"
sleep 3
python3 - "$SOCK" <<'PY' > /tmp/regs2.txt
import json, socket, sys
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1])
f = s.makefile("rw"); f.readline()
f.write(json.dumps({"execute": "qmp_capabilities"}) + chr(10)); f.flush(); f.readline()
f.write(json.dumps({"execute": "human-monitor-command",
                    "arguments": {"command-line": "info registers -a"}}) + chr(10)); f.flush()
print(json.loads(f.readline()).get("return", ""))
PY
grep -aE "^RIP" /tmp/regs2.txt | sed "s/^/    /"

# THE MEASUREMENT THAT DECIDES IT. A core spinning in spin_lock has the same
# RIP forever -- the loop is two instructions -- so identical registers prove
# nothing about whether the lock is moving. `serving` does: it advances once
# per release. Sampled three times, two seconds apart. Frozen serving with
# cores queued is a stuck lock; advancing serving with cores still queued is
# STARVATION, and they are different bugs with different fixes.
echo "--- is the lock moving? (g_bkl ticket/serving, 3 samples 2s apart) ---"
for i in 1 2 3; do
    python3 tests/boot/qmp_lockdump.py "$SOCK" "$ELF" g_bkl g_sched_lock | sed "s/^/  t$i/"
    sleep 2
done

echo "--- lock state ---"
python3 tests/boot/qmp_lockdump.py "$SOCK" "$ELF"

echo "--- symbols ---"
echo "$regs" | grep -aoE "RIP=[0-9a-f]+" | sed 's/RIP=//' | while read -r rip; do
    sym=$(python3 - "$ELF" "$rip" <<'PY'
import subprocess, sys
elf, rip = sys.argv[1], int(sys.argv[2], 16)
best, bestn = None, 0
out = subprocess.run(["nm", "-n", elf], capture_output=True, text=True).stdout
for ln in out.splitlines():
    p = ln.split()
    if len(p) < 3:
        continue
    try:
        a = int(p[0], 16)
    except ValueError:
        continue
    if a <= rip and a > bestn:
        bestn, best = a, p[2]
print("%s+0x%x" % (best, rip - bestn) if best else "?")
PY
)
    echo "    $rip  $sym"
done
exit 1
