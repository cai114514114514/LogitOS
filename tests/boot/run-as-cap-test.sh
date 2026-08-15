#!/usr/bin/env bash
# M28's headline gate, on the real machine: a script that was NOT granted CAP_FS
# provably cannot read /etc, and one that was, can.
#
# =============================================================================
# HISTORY. Written 2026-08-14 while the tree could not build an ISO (the
# page-cache line's fault.c was mid-flight), committed then as a deliverable
# ahead of its own runnability. First actually RUN 2026-08-15 -- and its first
# run earned its keep twice over: (1) it caught its own spawn line spelling
# `as script.as --scope P` (flag AFTER the script), which as.c parsed as
# script args and ran UNNARROWED -- run 2 read /etc identically to run 1;
# (2) chasing that turned the silence into a hard refusal in as.c (a trailing
# --scope anywhere in argv now refuses to run). The reason this file exists at
# all stands: the 4152 host checks in tests/unit/as_cap_test.c all run against
# a held set the test itself installed through as_caps_set(); only this boot
# proves the set a script runs under is the one the KERNEL granted
# (SYS_CAP_QUERY -> install_kernel_grant() in c/apps/as/as.c).
# =============================================================================
#
# WHY TWO RUNS OF THE SAME SCRIPT. A single run showing "denied" is equally
# consistent with the capability check working and with /etc/logit.conf simply
# not existing, or with open() being broken, or with the script never having
# run at all. The evidence is the DIFFERENCE: identical source, identical
# machine, two grants, two answers. The unscoped run must read /etc; the scoped
# run must be refused there and still succeed inside its own subtree -- which
# also rules out "the scoped process just can't open anything".
#
# The scoped child is spawned through ash.as, which is the point of doing it
# this way rather than with a kernel-side fixture: the narrowing travels the
# real path a program would use (SYS_CAP_SPAWN, ceiling-checked by
# proc_cap_subset), not a back door built for the test.

set -u

ISO="${1:?usage: run-as-cap-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-as-cap-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
LOG="$(mktemp)"
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null; rm -f "$LOG"; }
trap cleanup EXIT

# Run 1: the console shell holds CAP_ALL (proc_spawn grants it -- that is the
# root of the chain, the one place "granted by the kernel" is a sentence with a
# referent). Run 2: the same script under a capability narrowed to /usr/as.
{ sleep 4
  printf 'as /usr/as/examples/capcheck.as\n'
  # FLAG BEFORE SCRIPT -- as.c consumes --scope only at argv[1] (everything
  # after the script path belongs to the script's own args()). The first
  # version of this line put the flag AFTER the script and the narrowing
  # silently never happened: run 2 printed the same "read-etc ok" as run 1
  # and this gate caught it. The trailing-flag spelling is now a REFUSAL in
  # as.c, not a silence -- see the guard beside the --scope parse.
  printf 'as --scope /usr/as /usr/as/examples/capcheck.as\n'
  printf 'exit\n'
  sleep 12
} | "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
      -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
      -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
      -m 512M -smp 1 -accel tcg -vga none -device virtio-gpu-pci \
      -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

for _ in $(seq 1 300); do
    if [ "$(grep -ac 'capcheck: done' "$LOG")" -ge 2 ]; then break; fi
    sleep 1
done

fail=0
say() { echo "  $*"; }

# Both runs completed at all.
if [ "$(grep -ac 'capcheck: done' "$LOG")" -lt 2 ]; then
    say "FAIL: capcheck did not complete twice"; fail=1
fi

# Run 1 (unscoped, CAP_ALL): /etc is readable. If this fails, the harness is
# measuring a broken open() rather than a capability, and every refusal below
# would be worthless.
if ! grep -aq 'read-etc ok' "$LOG"; then
    say "FAIL: the UNSCOPED run could not read /etc -- the control for the test itself"
    fail=1
fi

# Run 2 (scoped to /usr/as): /etc refused, /usr readable. The pair is the
# assertion; either half alone proves nothing.
if ! grep -aq 'read-etc denied' "$LOG"; then
    say "FAIL: the SCOPED run read /etc -- the capability did not travel across exec"
    fail=1
fi
if ! grep -aq 'read-usr ok' "$LOG"; then
    say "FAIL: the SCOPED run could not read inside its own scope -- it was denied everything,"
    say "      which is not evidence that the PREFIX is being enforced"
    fail=1
fi

# Attenuation cannot be undone, on the device as on the host.
if grep -aq 'REGAINED-ROOT' "$LOG"; then
    say "FAIL: a narrowed capability widened itself back to /"; fail=1
fi
if ! grep -aq 'no-regain ok' "$LOG"; then
    say "FAIL: the no-regain check never ran"; fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "FAIL: M28 on-device capability gate"
    grep -a 'capcheck\|read-etc\|read-usr\|raw-peek\|narrowed\|no-regain' "$LOG" | head -20
    exit 1
fi
echo "PASS: a script without CAP_FS cannot read /etc; the same script with it can"
