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
# WHY SOME CASES ARE EXPECTED TO BE PWNED
# ---------------------------------------
# nx and nxstack are expected to succeed today, and this script asserts that
# they DO. That is deliberate. NX is not enabled -- c/kernel/cpu/prot.h has the
# account: EFER.NXE is on, but c/kernel/mm masks page-table entries back to
# frames with ~0xFFF, which keeps bit 63, so setting NX hands the frame
# allocator an address 8 EiB up and the machine dies in memcpy on the first
# fork+exec. A suite in which every case passes cannot tell you which
# protections you have; this one names the hole and fails if the hole silently
# changes shape in either direction. When the mm masks are fixed, move nx and
# nxstack from EXPECT_PWNED to EXPECT_BLOCKED and nothing else here changes.

set -u

ISO="${1:?usage: run-sec-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-sec-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
REPORT_ONLY="${SEC_REPORT_ONLY:-0}"
LOG="$(mktemp)"
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null; rm -f "$LOG" "$LOG.txt"; }
trap cleanup EXIT

# The attacks that MUST be refused, and the ones that are known-open today.
EXPECT_BLOCKED="wx rodata kptr kptrrw kread"
EXPECT_PWNED="nx nxstack"
ALL="$EXPECT_BLOCKED $EXPECT_PWNED"

# One `secprobe <name>` per line. The shell fork+execs each, so a probe that
# faults takes only its own process down and the shell reads the next line.
CMDS=""
for a in $ALL; do CMDS="${CMDS}secprobe $a\n"; done
CMDS="${CMDS}echo SEC-ALL-DONE\nexit\n"

NET="-netdev user,id=n0 -device e1000,netdev=n0"
# -snapshot: ephemeral disk writes, so repeated runs are deterministic.
{ sleep 6; printf "$CMDS"; sleep 8; } | \
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
