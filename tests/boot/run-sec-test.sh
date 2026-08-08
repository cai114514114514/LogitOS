#!/usr/bin/env bash
# Ring-3 memory-protection gate.
#
# Boots LogitOS, and from the serial shell runs /bin/secprobe once per attack --
# one process each, because a blocked attack is usually blocked BY A FAULT, and a
# fault kills the whole process. See tests/boot/secprobe.c for what each attack
# does and why.
#
# HOW A VERDICT IS READ
# ---------------------
# Each run prints "SEC-begin <name>" before attacking, so a probe that never
# launched is distinguishable from one that was blocked. Then exactly one of:
#
#   SEC-<name>-PWNED...     the attack succeeded. The boundary is open.
#   SEC-<name>-BLOCKED      the kernel refused it by RETURN VALUE and the
#                           process survived to say so.
#   (neither, plus a kernel "[fault] app exception" line)
#                           the kernel refused it by FAULTING the process. This
#                           is what W^X and NX look like -- there is no return
#                           value to check, the store or the jump simply traps.
#
# So "blocked" is: SEC-begin present AND no PWNED marker. That is asserted
# rather than the presence of a fault line, because a fault line cannot be
# attributed to a particular probe on a shared serial console, while the absence
# of that probe's own PWNED marker can.
#
# NX USED TO BE EXPECTED TO BE PWNED. IT IS NOT ANY MORE.
# -------------------------------------------------------
# For a long time this file asserted that nx and nxstack SUCCEEDED, because
# they did: EFER.NXE was on, but c/kernel/mm masked page-table entries back to
# frames with ~0xFFF, which keeps bit 63, so setting NX handed the frame
# allocator an address 8 EiB up and the machine died in memcpy on the first
# fork+exec. That expectation was not laziness -- a suite in which every case
# passes cannot tell you which protections you have, so the hole was named and
# the suite failed if it silently changed shape in EITHER direction.
#
# The mm masks are MM_PTE_ADDR now and cpu_prot_nx_usable() returns
# cpu_prot_nx(), so both cases have moved to EXPECT_BLOCKED and EXPECT_PWNED is
# empty. Keeping the variable, empty, on purpose: it is the place a future
# known-open boundary gets named, and an empty list is a stronger statement
# than no list.
#
# Note what "blocked" looks like for these two: there is no return value to
# check -- the jump into the data page simply traps -- so the verdict is
# BLOCKED(faulted), and the kernel's own "[fault] app exception" line is the
# corroboration. The [prot] line printed above the table is the other half:
# it must read `nx sup/ON`, not `efer-only`.

set -u

# Wait for /bin/sh to exist before typing at it, rather than sleeping a
# guessed number of seconds. See tests/boot/bootwait.sh for why a longer
# sleep is the same bug with a bigger number.
. "$(dirname "$0")/bootwait.sh"

ISO="${1:?usage: run-sec-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-sec-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
REPORT_ONLY="${SEC_REPORT_ONLY:-0}"
LOG="$(mktemp)"
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null; rm -f "$LOG" "$LOG.txt"; }
trap cleanup EXIT

# The attacks that MUST be refused, and the ones that are known-open today.
EXPECT_BLOCKED="wx rodata nx nxstack kptr kptrrw kread"
EXPECT_PWNED=""
ALL="$EXPECT_BLOCKED $EXPECT_PWNED"

# One `secprobe <name>` per line. The shell fork+execs each, so a probe that
# faults takes only its own process down and the shell reads the next line.
CMDS=""
for a in $ALL; do CMDS="${CMDS}secprobe $a\n"; done
CMDS="${CMDS}echo SEC-ALL-DONE\nexit\n"

NET="-netdev user,id=n0 -device e1000,netdev=n0"
# -snapshot: ephemeral disk writes, so repeated runs are deterministic.
{ logit_wait_for_shell "$LOG" 120; printf "$CMDS"; sleep 8; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
    -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
    -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
    $NET -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

for _ in $(seq 1 400); do
    grep -aq "SEC-ALL-DONE" "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done

# The guest's serial console ends lines with CR LF, so every "$"-anchored match
# below would be looking past a carriage return and would score a probe that ran
# perfectly as DIDNOTRUN -- which is how this harness first "passed" nothing at
# all. Strip the CRs once, here, rather than making each pattern tolerate them.
tr -d '\r' < "$LOG" > "$LOG.txt"
LOG="$LOG.txt"

echo "--- protection bits as the kernel reported them ---"
grep -a "^\[prot\]" "$LOG" | sort -u || true
echo

fail=0
verdict() {                                   # $1 = attack name -> echoes verdict
    if ! grep -aq "^SEC-begin $1\$" "$LOG"; then echo "DIDNOTRUN"; return; fi
    if grep -aq "SEC-$1-PWNED" "$LOG"; then echo "PWNED"; return; fi
    if grep -aq "SEC-$1-BLOCKED" "$LOG"; then echo "BLOCKED(refused)"; return; fi
    echo "BLOCKED(faulted)"
}

printf '%-10s %-20s %s\n' ATTACK VERDICT EXPECTED
for a in $ALL; do
    v="$(verdict "$a")"
    case " $EXPECT_PWNED " in *" $a "*) want=PWNED ;; *) want=BLOCKED ;; esac
    ok="ok"
    case "$v" in
        DIDNOTRUN)  ok="FAIL" ;;
        PWNED)      [ "$want" = PWNED ]   || ok="FAIL" ;;
        BLOCKED*)   [ "$want" = BLOCKED ] || ok="FAIL" ;;
    esac
    [ "$ok" = FAIL ] && fail=1
    printf '%-10s %-20s %-8s %s\n' "$a" "$v" "$want" "$ok"
done

if [ "$REPORT_ONLY" = 1 ]; then
    echo; echo "(report only -- assertions not enforced)"
    exit 0
fi

if ! grep -aq "SEC-ALL-DONE" "$LOG"; then
    echo "FAIL: the shell did not survive the probes (no SEC-ALL-DONE)."
    echo "      A protection that kills the SHELL rather than the probe is not shipped."
    fail=1
fi

if [ "$fail" = 0 ]; then
    echo
    echo "PASS: every attack landed on the expected side of the boundary"
    exit 0
fi

echo
echo "FAIL: at least one attack did not land where expected"
echo "----- serial output -----"
cat "$LOG"
echo "-------------------------"
exit 1
