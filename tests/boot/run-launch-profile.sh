#!/usr/bin/env bash
# What a whole app launch spends its time on -- including the phases the storage
# line does not own.
#
# /dev/fsbench times the three phases inside the filesystem exactly (path
# resolve, allocate, read). It cannot time elf_load or the window manager's
# first paint, because those live in c/kernel and instrumenting them is not this
# line's to do. kprof's SAMPLING profiler needs no source change at all, so it is
# the right instrument for exactly that gap: arm it, click the Dock icon, stop it
# the moment the window is up, and the kernel-RIP histogram says which functions
# the machine was actually in.
#
# The two channels have to be interleaved, which is why this is a script and not
# a python driver: the profiler is controlled over the SERIAL console (a FIFO
# into QEMU's stdin) and the click is delivered over QMP.
#
# Usage: run-launch-profile.sh <iso> <disk.img> [app-slot] [settle-seconds]
# Output: the top kernel symbols by share, with names resolved from
# build/kernel.map -- there is no host symbolizer in this tree yet, so this
# carries a small one.

set -u

ISO="${1:?usage: run-launch-profile.sh <iso> <disk.img> [slot] [settle]}"
DISK="${2:?usage: run-launch-profile.sh <iso> <disk.img> [slot] [settle]}"
SLOT="${3:-browser}"
SETTLE="${4:-6}"
QEMU="${QEMU:-qemu-system-x86_64}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

WORK="$(mktemp -d)"
LOG="$WORK/prof.log"
FIFO="$WORK/in"
SOCK="$WORK/qmp.sock"
DISKC="$WORK/disk.img"
cp "$DISK" "$DISKC"
mkfifo "$FIFO"
cleanup() { [ -n "${QPID:-}" ] && kill -9 "$QPID" 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

NET="-netdev user,id=n0 -device e1000,netdev=n0"

sleep 100000 > "$FIFO" &          # hold the write end open so the shell's reads
HOLD=$!                           # do not see EOF between our echoes

"$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISKC",format=raw,if=none,id=hd0 -device virtio-blk-pci,drive=hd0 \
    -boot d -snapshot -m 512M -smp 4 -accel tcg,thread=multi \
    -vga none -device virtio-gpu-pci,xres=1920,yres=1200 \
    $NET -serial stdio -display none -no-reboot \
    -qmp "unix:$SOCK,server,nowait" <"$FIFO" >"$LOG" 2>/dev/null &
QPID=$!

# Wait for the shell prompt on serial (the kernel prints it once init runs).
waited=0
while kill -0 "$QPID" 2>/dev/null && [ "$waited" -lt 120 ]; do
    grep -aq "logit" "$LOG" && grep -aq "\[wm\]" "$LOG" && break
    sleep 1; waited=$((waited + 1))
done
sleep 4                            # let the desktop settle: Finder + Clock are
                                   # launched by wm_run and must not be in frame

printf 'echo reset > /dev/kprof\n' > "$FIFO"
printf 'echo start > /dev/kprof\n' > "$FIFO"
sleep 1

python3 "$ROOT/tests/qmp/qmp_launch_click.py" "$SOCK" "$SLOT" "$SETTLE" || true

printf 'echo stop > /dev/kprof\n' > "$FIFO"
sleep 1
printf 'cat /dev/kprof\n' > "$FIFO"
sleep 6

kill -9 "$HOLD" 2>/dev/null
kill -9 "$QPID" 2>/dev/null
for _ in $(seq 1 100); do kill -0 "$QPID" 2>/dev/null || break; sleep 0.1; done
tr -d '\r' <"$LOG" >"$LOG.n" && mv "$LOG.n" "$LOG"

echo "===== launch profile: $SLOT ====="
python3 - "$LOG" "$ROOT/build/kernel.map" <<'PY'
import re, sys
log, mapf = sys.argv[1], sys.argv[2]

# --- the symbol table, from the linker map -------------------------------
syms = []
for line in open(mapf, errors="ignore"):
    m = re.match(r"\s+([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)\s+\d+\s+(\S+)\s*$", line)
    if not m:
        continue
    name = m.group(3)
    if name.startswith(".") or "(" in name or "=" in name or "/" in name:
        continue
    syms.append((int(m.group(1), 16), int(m.group(2), 16), name))
syms.sort()

def resolve(a):
    lo, hi = 0, len(syms) - 1
    best = None
    while lo <= hi:
        mid = (lo + hi) // 2
        if syms[mid][0] <= a:
            best = syms[mid]; lo = mid + 1
        else:
            hi = mid - 1
    if not best:
        return "?"
    return best[2] + ("+0x%x" % (a - best[0]))

text = open(log, errors="ignore").read()
i = text.rfind("kprof v1")
if i < 0:
    print("no kprof report on the serial log")
    sys.exit(0)
rep = text[i:]

for key in ("run_ns", "samples ", "samples_kernel", "samples_user",
            "sample_hz_measured", "KPROF_INTEGRITY"):
    for line in rep.splitlines():
        if line.startswith(key):
            print(line.rstrip()); break

agg = {}
tot = 0
for line in rep.splitlines():
    m = re.match(r"\s*([0-9a-f]{6,})\s+(\d+)\s+([\d.]+)%\s+([ku])\s*$", line)
    if not m:
        continue
    rip, hits, share, ring = int(m.group(1), 16), int(m.group(2)), m.group(3), m.group(4)
    name = resolve(rip) if ring == "k" else "ring3:0x%x" % rip
    base = name.split("+")[0]
    agg[base] = agg.get(base, 0) + hits
    tot += hits

print()
print("%-34s %8s %7s" % ("symbol", "hits", "share"))
for name, hits in sorted(agg.items(), key=lambda kv: -kv[1])[:25]:
    print("%-34s %8d %6.1f%%" % (name, hits, 100.0 * hits / tot if tot else 0))
PY
echo "================================="
