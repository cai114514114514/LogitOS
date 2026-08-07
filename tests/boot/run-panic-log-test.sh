#!/usr/bin/env bash
# On-device proof for the kernel log ring: that it SURVIVES the events that
# wrote it, that it is readable from userland after the fact, and that logging
# from a real interrupt handler neither corrupts it nor deadlocks.
#
# Everything here is driven through the serial shell, because that is how a
# person would do it on a machine with no debugger:
#
#   cat /dev/kmsg              the log, rendered
#   cat /dev/kstat             what the system thinks its state is
#   echo CMD > /dev/ktrigger   this kernel's /proc/sysrq-trigger
#
# THE DECISIVE ASSERTIONS are the ones that distinguish a line printed live on
# the serial port from the same line READ BACK OUT OF THE RING afterwards: a
# ring record renders with a "[    3.210] W cpu0 " prefix that live console
# output does not have. Grepping for the prefixed form is the difference
# between "the kernel printed something" and "the kernel remembered it".

set -u

ISO="${1:?usage: run-panic-log-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-panic-log-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
LOG="$(mktemp)"
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null; }
trap cleanup EXIT

NET="-netdev user,id=n0 -device e1000,netdev=n0"

# Order matters. The first `cat /dev/kmsg` happens BEFORE the storms, so the
# boot lines it must contain have not yet been aged out; the storms then
# deliberately overrun the ring to show that a full ring overwrites instead of
# blocking, and the last readback shows the log still coherent afterwards.
{
  # Boot now runs several subsystems' self-tests on the console before init
  # spawns the shell; typing into the serial port before the prompt exists
  # loses the command (the PS/2-era 1-byte buffer problem, on the tty).
  sleep 12
  printf 'cat /dev/kmsg\n';                      sleep 4
  printf 'echo warn > /dev/ktrigger\n';          sleep 2
  printf 'echo bt > /dev/ktrigger\n';            sleep 2
  # Read the whole ring again: the WARN and the backtrace must be IN it, not
  # merely to have been printed while they happened.
  printf 'cat /dev/kmsg\n';                      sleep 5
  printf 'cat /dev/kstat\n';                     sleep 3
  printf 'echo logstorm 4000 > /dev/ktrigger\n'; sleep 8
  printf 'echo irqstorm 200 > /dev/ktrigger\n';  sleep 20
  printf 'cat /dev/kmsg | wc\n';                 sleep 4
  printf 'ls /dev\n';                            sleep 2
  printf 'exit\n'
  sleep 4
} | "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
      -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
      -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
      -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
      $NET -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

for _ in $(seq 1 700); do
    grep -aq "KDIAG_IRQSTORM" "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done
sleep 3
kill "$QPID" 2>/dev/null

fails=0
ok()   { echo "  ok    $1"; }
bad()  { echo "  FAIL  $1"; fails=$((fails+1)); }
want() { if grep -aqE "$1" "$LOG"; then ok "$2"; else bad "$2"; fi; }

RINGPFX='\[ *[0-9]+\.[0-9]{3}\] [PEWID] cpu[0-9]'

echo "--- the log exists and is fed by the ordinary kprintf path ---"
want "\[klog\] ring 512 lines x 112 bytes"      "klog announced its ring at boot"

echo "--- it SURVIVES: boot-time output read back out of the ring later ---"
# The un-prefixed line is printed at boot; only the ring can produce the
# prefixed one, and only long after the line was written.
want "$RINGPFX \[logitos\] interrupts \+ memory" "an early boot line is still readable via /dev/kmsg"
want "$RINGPFX \[klog\] ring 512 lines"          "the ring contains its own first line"

echo "--- WARN reports and CONTINUES ---"
want "\[warn\] .*deliberate WARN from /dev/ktrigger" "WARN printed live"
want "$RINGPFX \[warn\] .*deliberate WARN"           "...and was retained in the ring"
want "KDIAG_WARN done warns=1"                       "the machine kept running after the WARN"

echo "--- a backtrace can be taken from a live system, no crash required ---"
want "KDIAG_BT begin"                            "bt trigger ran"
want "^ *#0 0x[0-9a-f]+"                         "backtrace produced a frame"
want "KDIAG_BT end"                              "...and returned to the shell"

echo "--- a FULL ring overwrites, counts the loss, and does not block ---"
want "KDIAG_LOGSTORM ok wrote=4000 .* torn=0"    "4000 records, none torn"
if grep -aoE "KDIAG_LOGSTORM ok wrote=4000 retained=[0-9]+ torn=0 aged_out=[0-9]+" "$LOG" | \
   head -1 | grep -qE "aged_out=[1-9][0-9]*"; then
    ok "the ring genuinely wrapped (aged_out > 0) rather than growing"
else
    bad "the ring did not wrap -- the full-ring path was never exercised"
fi

echo "--- THE INTERRUPT-CONTEXT CLAIM, tested rather than asserted ---"
# A real IDT gate driven by asynchronous LAPIC IPIs, logging from inside the
# handler while the shell thread logs from thread context on four cores.
want "KDIAG_IRQSTORM ok "                        "the storm completed (so: no deadlock)"
want "KDIAG_IRQSTORM ok .* torn=0"               "no record was torn by an interrupt"
if grep -aoE "irq_records=[0-9]+" "$LOG" | head -1 | grep -qE "irq_records=[1-9][0-9]*"; then
    ok "interrupt handlers actually wrote records ($(grep -aoE 'irq_records=[0-9]+' "$LOG" | head -1))"
else
    bad "no interrupt-context records -- the test proved nothing"
fi
if grep -aoE "irq_taken=[0-9]+" "$LOG" | head -1 | grep -qE "irq_taken=[1-9][0-9]*"; then
    ok "the diagnostic IPI was delivered ($(grep -aoE 'irq_taken=[0-9]+' "$LOG" | head -1))"
else
    bad "no IPI was ever taken"
fi
# More than one core logging from interrupt context is what makes this a test
# of the ring LOCK and not only of the per-CPU buffer. irq_hit_cpus is counted
# by the handler itself; irq_cpus is derived from records that survived in the
# ring, which additionally proves those lines are readable afterwards.
if grep -aoE "irq_hit_cpus=[0-9]+" "$LOG" | head -1 | grep -qE "irq_hit_cpus=([2-9]|[1-9][0-9])"; then
    ok "interrupt handlers ran on >1 core ($(grep -aoE 'irq_hit_cpus=[0-9]+' "$LOG" | head -1))"
else
    bad "only one core took the diagnostic IPI -- cross-core contention untested"
fi
if grep -aoE "irq_cpus=[0-9]+" "$LOG" | head -1 | grep -qE "irq_cpus=([2-9]|[1-9][0-9])"; then
    ok "the ring retains interrupt-context records from >1 core ($(grep -aoE 'irq_cpus=[0-9]+' "$LOG" | head -1))"
else
    bad "the ring holds interrupt-context records from only one core"
fi

echo "--- runtime introspection ---"
want "^uptime_ms +[0-9]+"                        "/dev/kstat reports uptime"
want "^mem_free_bytes +[0-9]+"                   "/dev/kstat reports memory"
want "^klog_records +[0-9]+"                     "/dev/kstat reports log statistics"
want "^cpus_online +[1-9]"                       "/dev/kstat reports the live core count"
want "^ctx_switches +[0-9]+"                     "/dev/kstat reports scheduler activity"
want "^warns +1"                                 "/dev/kstat counted the WARN"
want "kmsg"                                      "/dev is enumerable (ls /dev)"

echo
if [ "$fails" -eq 0 ]; then
    echo "PASS: the log ring survives, is readable from userland, and is safe from interrupt context"
    exit 0
fi
echo "FAIL: $fails assertion(s)"
echo "----- serial output -----"
cat "$LOG"
echo "-------------------------"
exit 1
