#!/usr/bin/env bash
# On-device proof for the profiler. Four questions, and none of them is
# "did something print".
#
#  1. DOES IT GET A KNOWN ANSWER RIGHT? A workload is constructed whose split
#     across three functions is 70/20/10 by construction and is measured
#     independently by the span clock. The sampler must reproduce that split
#     within a stated tolerance. A profiler that has never been checked against
#     an answer known in advance is a random number generator with good
#     formatting.
#
#  2. WHAT DOES IT COST? The same integer benchmark with sampling off and on, on
#     the machine, printed whether the number is flattering or not. That figure
#     bounds every result the profiler will ever produce.
#
#  3. DOES IT SURVIVE FOUR CORES? Samples arrive in interrupt context on every
#     core at once. The per-CPU counters the handler keeps for itself are the
#     independent witness: one writer each, so they cannot be wrong, and they
#     must equal what comes back out of the shared histogram. The profile must
#     also have touched more than one core, or it is not evidence about an SMP
#     machine at all.
#
#  4. DO THE ADDRESSES MEAN ANYTHING? Every hot address is resolved against
#     build/kernel.map -- the linker's own record -- and the hottest ones are
#     required to land inside real functions. A histogram of plausible-looking
#     hex is not a profile.
#
# WHY THE FIRST TWO RUN AT -smp 1 AND THE THIRD AT -smp 4. A write to
# /dev/kprof is a syscall and holds the big kernel lock. Held for seconds on an
# application processor it starves every other core of interrupts -- they spin
# for the BKL inside spin_lock_irqsave, i.e. with IF=0 -- so NOTHING can be
# sampled anywhere. Measured directly: a four-second profile with zero samples,
# on the runs where the shell happened to land off the BSP. At -smp 1 the shell
# is the BSP by construction and the known-answer test is deterministic. The
# multi-core claim is then made where it belongs: with the system running
# normally, which is the shape of the workloads this profiler exists for, and
# where all four cores really are taking interrupts.
#
# WHY THE COMMANDS ARE MARKER-DRIVEN AND NOT ON A TIMER. The first version fed
# the guest on fixed sleeps and passed on one machine and failed on another --
# the self-test simply never ran, because the preceding measurement took longer
# than the sleep allowed. A harness whose result depends on how loaded the host
# is measures the host. Each command here waits for the previous one's verdict
# to appear on the serial line before the next is sent.
#
# The negative controls are `make test-prof-negctl` (the accumulator without its
# atomics, required to fail host-side) and `make test-prof-control` (the kernel
# built -DKPROF_DISABLE, required to fail here).

set -u

ISO="${1:?usage: run-prof-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-prof-test.sh <iso> <disk.img>}"
MAP="${MAP:-build/kernel.map}"
QEMU="${QEMU:-qemu-system-x86_64}"

fails=0
ok()  { echo "  ok    $1"; }
bad() { echo "  FAIL  $1"; fails=$((fails+1)); }

QPID=""
FIFO=""
LOG=""
cleanup() {
    [ -n "$QPID" ] && kill "$QPID" 2>/dev/null
    [ -n "$QPID" ] && wait "$QPID" 2>/dev/null
    [ -n "$FIFO" ] && rm -f "$FIFO"
}
trap cleanup EXIT

start_vm() {          # start_vm <smp> <logfile>
    LOG="$2"
    FIFO="$(mktemp -u)"
    mkfifo "$FIFO"
    "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
        -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
        -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
        -m 512M -smp "$1" -accel tcg,thread=multi \
        -vga none -device virtio-gpu-pci \
        -netdev user,id=n0 -device e1000,netdev=n0 \
        -serial stdio -display none -no-reboot <"$FIFO" >"$LOG" 2>/dev/null &
    QPID=$!
    exec 3>"$FIFO"        # hold the write end open so the guest sees no EOF
}

stop_vm() {
    exec 3>&-
    kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
    QPID=""
    rm -f "$FIFO"; FIFO=""
}

say() { printf '%s\n' "$1" >&3; }

waitfor() {           # waitfor <grep-pattern> <seconds>
    local i n=$(( ${2} * 10 ))
    for ((i = 0; i < n; i++)); do
        grep -aq -- "$1" "$LOG" && return 0
        kill -0 "$QPID" 2>/dev/null || return 1
        sleep 0.1
    done
    return 1
}

# ---------------------------------------------------------------------------
# Pass 1 (-smp 1): the known answer, and what the profiler costs.
# ---------------------------------------------------------------------------
LOG1="$(mktemp)"
start_vm 1 "$LOG1"

echo "--- the machine still boots ---"
if waitfor "LOGIT_BOOT_OK" 90; then ok "LOGIT_BOOT_OK"
else bad "the kernel did not reach LOGIT_BOOT_OK"; fi
sleep 2

# Establish up front that the profiler is even in this kernel. Without this the
# -DKPROF_DISABLE control run spends ten minutes waiting for markers that can
# never appear, and a genuinely misconfigured build looks like a timeout rather
# than like the one-line answer it is.
say 'cat /dev/kprof'
built=1
if waitfor "^built  *yes" 30; then
    ok "the profiler is compiled into this kernel"
else
    built=0
    bad "/dev/kprof reports the profiler is not built in (-DKPROF_DISABLE?)"
fi

if [ "$built" = 1 ]; then
    say 'echo overhead 400 > /dev/kprof'
    waitfor "KPROF_SPANCOST" 120 || bad "the overhead measurement never finished"
    say 'echo selftest 4000 > /dev/kprof'
    waitfor "KPROF_NOMINAL" 120 || bad "the self-test never finished"
    say 'cat /dev/kprof'
    waitfor "sample_hz_measured" 30
    sleep 2
fi
stop_vm

echo "--- a workload whose answer is known in advance ---"
sel=$(grep -a "KPROF_SELFTEST " "$LOG1" | tail -1)
seln=$(grep -a "KPROF_SELFTEST_N" "$LOG1" | tail -1)
nom=$(grep -a "KPROF_NOMINAL" "$LOG1" | tail -1)
[ -n "$sel" ]  && echo "      $sel"
[ -n "$seln" ] && echo "      $seln"
[ -n "$nom" ]  && echo "      $nom"

case "$sel" in
  *"KPROF_SELFTEST ok"*)
      ok "the sampled 70/20/10 split matches the clock within tolerance" ;;
  "")  bad "the self-test did not run at all (is the profiler compiled in?)" ;;
  *)   bad "the sampler disagreed with the clock: $sel" ;;
esac
case "$nom" in
  *"KPROF_NOMINAL ok"*)
      ok "and the workload really was 70/20/10 on the clock" ;;
  *)   bad "the constructed workload was not 70/20/10: ${nom:-<missing>}" ;;
esac

# The tolerance is only as good as the number of samples behind it. A run that
# passes on 40 samples is not the same result as one that passes on 4000, and
# saying so is the difference between a measurement and a green tick.
inwork=$(printf '%s\n' "$seln" | sed -n 's/.*inwork=\([0-9]*\).*/\1/p')
inwork=${inwork:-0}
if [ "$inwork" -ge 500 ]; then
    ok "the split was estimated from $inwork samples inside the workload"
else
    bad "only $inwork samples landed in the workload -- too few for the tolerance to mean anything"
fi

echo "--- what the profiler costs, measured on the machine ---"
ovh=$(grep -a "KPROF_OVERHEAD" "$LOG1" | tail -1)
spc=$(grep -a "KPROF_SPANCOST" "$LOG1" | tail -1)
[ -n "$ovh" ] && echo "      $ovh"
[ -n "$spc" ] && echo "      $spc"
if [ -n "$ovh" ]; then ok "the on/off overhead was measured, not asserted"
else bad "no overhead measurement"; fi

# The claim that has to hold is about the DISABLED path, because that is the one
# that will be compiled into the tree permanently.
dis=$(printf '%s\n' "$spc" | sed -n 's/.*disabled=+\([0-9]*\)ns.*/\1/p')
dis=${dis:-999}
if [ "$dis" -le 5 ]; then
    ok "a KPROF_BEGIN/END pair with spans off costs ${dis}ns (a load and a branch)"
else
    bad "spans cost ${dis}ns per pair even when disabled"
fi
# 150ppt is not a target, it is a ceiling: measured runs land at 12-51ppt on an
# unloaded host, and the spread is the host's, not the profiler's. What the
# bound excludes is a regression that makes sampling cost more than a seventh of
# the workload, at which point the profile stops describing the unprofiled run.
ppt=$(printf '%s\n' "$ovh" | sed -n 's/.*(\([0-9]*\)ppt).*/\1/p')
ppt=${ppt:-9999}
if [ "$ppt" -le 150 ]; then
    ok "sampling at the default rate costs ${ppt} parts per thousand on one core"
else
    bad "sampling costs ${ppt}ppt on one core -- too much to leave on"
fi

# ---------------------------------------------------------------------------
# Pass 2 (-smp 4): four cores really do arrive in the accumulator, and nothing
# is lost. The load here is the SYSTEM running, not a busy loop in a syscall --
# see the note at the top of this file for why that distinction is the point.
# ---------------------------------------------------------------------------
LOG4="$(mktemp)"
start_vm 4 "$LOG4"
waitfor "LOGIT_BOOT_OK" 120 || bad "the -smp 4 boot did not reach LOGIT_BOOT_OK"
sleep 2

if [ "$built" = 1 ]; then
    say 'echo start > /dev/kprof'
    waitfor "KPROF_START" 45 || bad "the profiler never started"
    sleep 6
    say 'echo stop > /dev/kprof'
    waitfor "KPROF_STOP" 45 || bad "the profiler never stopped"
    say 'echo smpcheck > /dev/kprof'
    waitfor "KPROF_SMP" 45 || bad "the SMP verdict never printed"
fi
say 'cat /dev/kprof'
waitfor "sample_hz_measured" 30
sleep 2
stop_vm

echo "--- four cores, one accumulator ---"
smp=$(grep -a "KPROF_SMP" "$LOG4" | tail -1)
[ -n "$smp" ] && echo "      $smp"
case "$smp" in
  *"KPROF_SMP ok"*) ok "every sample the handlers counted came back out of the histogram" ;;
  "") bad "the SMP check did not run" ;;
  *)  bad "the accumulator lost or tore samples: $smp" ;;
esac

ncores=$(printf '%s\n' "$smp" | sed -n 's/.*cores=\([0-9]*\).*/\1/p')
ncores=${ncores:-0}
if [ "$ncores" -ge 2 ]; then
    ok "samples arrived from $ncores cores, so this is an SMP result"
else
    bad "only $ncores core(s) ever produced a sample -- the fan-out did not reach the others"
fi

taken=$(printf '%s\n' "$smp" | sed -n 's/.*taken=\([0-9]*\).*/\1/p')
taken=${taken:-0}
if [ "$taken" -ge 2000 ]; then
    ok "$taken samples were taken across the run"
else
    bad "only $taken samples -- not enough to have stressed anything"
fi

if grep -aq "KPROF_INTEGRITY ok" "$LOG4"; then
    ok "the report's own integrity line agrees"
else
    bad "the report did not carry an ok integrity line"
    grep -a "KPROF_INTEGRITY" "$LOG4" | tail -2 | sed 's/^/      /'
fi

# ---------------------------------------------------------------------------
# The addresses have to mean something.
# ---------------------------------------------------------------------------
echo "--- the hot addresses, resolved against the linker map ---"
if [ ! -f "$MAP" ]; then
    bad "no linker map at $MAP (build it: make)"
else
    python3 - "$LOG4" "$MAP" <<'PY'
import re, sys, bisect

log, mapfile = sys.argv[1], sys.argv[2]
text = open(log, 'rb').read().decode('utf-8', 'replace')

cut = text.rfind('kprof v1')
if cut < 0:
    print("  FAIL  the report was never rendered"); sys.exit(1)
text = text[cut:]

rows = re.findall(r'^([0-9a-f]{16})\s+(\d+)\s+\S+\s+([ku])\s*$', text, re.M)
if not rows:
    print("  FAIL  the report carried no sample rows at all"); sys.exit(1)

syms = []
for line in open(mapfile, encoding='utf-8', errors='replace'):
    m = re.match(r'^\s*([0-9a-f]{6,16})\s+[0-9a-f]{6,16}\s+([0-9a-f]+)\s+\d+\s{4,}(\S+)\s*$', line)
    if m:
        name = m.group(3)
        if name.startswith('.') or '/' in name:
            continue
        syms.append((int(m.group(1), 16), int(m.group(2), 16), name))
syms.sort()
addrs = [a for a, _, _ in syms]

def resolve(a):
    i = bisect.bisect_right(addrs, a) - 1
    if i < 0: return None, 0
    base, size, name = syms[i]
    lim = size if size else (addrs[i+1] - base if i+1 < len(addrs) else 1 << 20)
    if a - base >= lim: return None, 0
    return name, a - base

fails = 0
def check(cond, what):
    global fails
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond: fails += 1

kern = [(int(a, 16), int(h), r) for a, h, r in rows if r == 'k']
user = [(int(a, 16), int(h), r) for a, h, r in rows if r == 'u']
check(len(kern) >= 5, "the report lists at least 5 kernel addresses (got %d)" % len(kern))

print("  map: %d symbols from %s" % (len(syms), mapfile))
named, unnamed = [], []
for a, h, _ in sorted(kern, key=lambda t: -t[1])[:12]:
    n, off = resolve(a)
    print("    %10d  %016x  ->  %s" % (h, a, ("%s+0x%x" % (n, off)) if n else "<unmapped>"))
    (named if n else unnamed).append(a)

# Every hot ring-0 address must be INSIDE a function the linker placed. One that
# resolves to nothing is either a wrong map or a fabricated sample; either way
# the profile is not usable.
check(not unnamed, "every hot kernel address is inside a mapped function (%r)"
      % [hex(a) for a in unnamed[:3]])

# And a profile of an idle-ish machine must be dominated by the idle, scheduler,
# compositor and console paths. This is the check a histogram of random numbers
# cannot pass: not "the addresses resolve" but "they resolve to the functions
# that have to be hot".
top = sorted(kern, key=lambda t: -t[1])[:6]
names = [resolve(a)[0] or '' for a, _, _ in top]
expect = ('idle', 'sched', 'schedule', 'spin', 'lock', 'hlt', 'wm_', 'fb_',
          'thread', 'context_switch', 'kprof', 'timer', 'file_', 'tty',
          'serial', 'read', 'poll')
hit = [n for n in names if any(e in n for e in expect)]
check(bool(hit), "the hottest kernel functions are on the idle/scheduler/"
      "compositor/console path, which is where an idle machine must be (%r)" % (names,))

if user:
    print("  %d ring-3 address(es) were sampled separately from the kernel ones"
          % len(user))
    umax = max(int(a) for a, _, _ in user)
    check(umax >= 0x40000000,
          "ring-3 samples carry user addresses (>= 1 GiB), not kernel ones (max %x)" % umax)

sys.exit(1 if fails else 0)
PY
    [ $? -ne 0 ] && fails=$((fails+1))
fi

echo "--- the host symbolisation tool runs on this report ---"
sed -n '/kprof v1/,$p' "$LOG4" | tr -d '\r' > "$LOG4.report"
if python3 tools/ksymbolize.py "$LOG4.report" --map "$MAP" > "$LOG4.sym" 2>"$LOG4.err"; then
    if grep -qE '^ *[0-9]+\.[0-9]+% ' "$LOG4.sym"; then
        ok "tools/ksymbolize.py folded the report into named functions"
        head -14 "$LOG4.sym" | sed 's/^/      /'
    else
        bad "ksymbolize.py produced no function rows"
        head -20 "$LOG4.sym" "$LOG4.err" | sed 's/^/      /'
    fi
else
    bad "ksymbolize.py failed"
    head -20 "$LOG4.err" | sed 's/^/      /'
fi

echo
if [ "$fails" -eq 0 ]; then
    echo "PASS: the profiler reproduces a known distribution, states its own cost,"
    echo "      survives four cores without losing a sample, and its addresses resolve"
    rm -f "$LOG1" "$LOG4" "$LOG4.report" "$LOG4.sym" "$LOG4.err"
    exit 0
fi
echo "FAIL: $fails assertion(s)"
echo "----- -smp 1 serial (kprof lines) -----"
grep -a "KPROF" "$LOG1" | sed 's/^/  /'
echo "----- -smp 4 serial (kprof lines) -----"
grep -a "KPROF" "$LOG4" | sed 's/^/  /'
echo "---------------------------------------"
exit 1
