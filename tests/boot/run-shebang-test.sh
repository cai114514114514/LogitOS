#!/usr/bin/env bash
# `#!` on the machine: a script the shell writes, made executable by
# /bin/chmod, run BY PATH with arguments -- and run by the interpreter its
# first line names (c/kernel/exec/load/shebang.h, proc_execve).
#
#   1. before chmod the script is refused: the execute bit still decides
#   2. after chmod a `#!/bin/sh` script runs with $0/$1/$2/$# as passed
#   3. the kernel log names the rewrite: "[execve] #! /sb.sh -> /bin/sh"
#   4. a `#!/bin/as` script runs too: /bin/as skips the #! line and finds
#      '# aether: 3.0' on line 2
#
# With a control kernel (-DEXEC_NEGCTL_NO_SHEBANG) pass --expect-refused: the
# same script must then NOT run, and the log must show the old refusal.

set -u
. "$(dirname "$0")/bootwait.sh"

ISO="${1:?usage: run-shebang-test.sh <iso> <disk.img> [--expect-refused]}"
DISK="${2:?usage: run-shebang-test.sh <iso> <disk.img> [--expect-refused]}"
MODE="${3:-}"
QEMU="${QEMU:-qemu-system-x86_64}"
LOG="$(mktemp)"
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null; rm -f "$LOG"; }
trap cleanup EXIT

SCRIPT="echo warmup
echo SB-START
echo '#!/bin/sh' > /sb.sh
echo 'echo SB-ARGC \$#' >> /sb.sh
echo 'echo SB-ARGS \$1 \$2' >> /sb.sh
echo 'echo SB-ZERO \$0' >> /sb.sh
/sb.sh one two
echo SB-NOX \$?
/bin/chmod 755 /sb.sh
/sb.sh one two
echo SB-STATUS \$?
echo '#!/bin/as' > /sb.as
echo '# aether: 3.0' >> /sb.as
echo 'def main() -> i64:' >> /sb.as
echo '    print(\"SB-AS-RAN\")' >> /sb.as
echo '    return 0' >> /sb.as
/bin/chmod +x /sb.as
/sb.as
echo SB-AS-STATUS \$?
echo SB-END
exit"

# The first line typed after the prompt appears is occasionally lost (seen once
# in four runs); `echo warmup` absorbs that, so SB-START is never the casualty.
NET="-netdev user,id=n0 -device e1000,netdev=n0"
{ logit_wait_for_shell "$LOG" 120; printf '%s' "$SCRIPT"; sleep 30; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
    -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
    -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
    $NET -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

for _ in $(seq 1 900); do
    grep -aq "^SB-END" "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done

fail=0
need()  { if grep -aqE "$1" "$LOG"; then echo "  ok: $2"; else echo "  FAIL: $2   (no match for /$1/)"; fail=1; fi; }
never() { if grep -aqE "$1" "$LOG"; then echo "  FAIL: $2   (matched /$1/)"; fail=1; else echo "  ok: $2"; fi; }

need "^SB-START" "the commands reached the shell"
need "^SB-NOX [1-9]" "without the execute bit the script is refused (non-zero status)"
if [ "$MODE" = "--expect-refused" ]; then
    never "^SB-ARGC" "control kernel: the script never runs"
    never "^SB-AS-RAN" "control kernel: the AetherScript script never runs either"
    # A 61-byte script is shorter than an AEX header, so the old loader says
    # "missing/too small"; a longer one reads as a bad header. Either is the
    # pre-#! refusal.
    need  "\[execve\] /sb\.sh: (bad aex header|missing/too small)" "control kernel: refused as not-a-binary, as before #! existed"
else
    need "\[execve\] #! /sb.sh -> /bin/sh" "the kernel names the rewrite to the interpreter"
    need "^SB-ARGC 2" "\$# is 2"
    need "^SB-ARGS one two" "the arguments arrive in order"
    need "^SB-ZERO /sb.sh" "\$0 is the script's path as the caller spelled it"
    need "^SB-STATUS 0" "the script exits 0"
    need "\[execve\] #! /sb.as -> /bin/as" "an AetherScript script is rewritten to /bin/as"
    need "^SB-AS-RAN" "...and /bin/as reads '# aether: 3.0' from line 2, after the #! line"
    need "^SB-AS-STATUS 0" "...and exits 0"
fi
need "^SB-END" "the run reached the end"

if [ "$fail" -eq 0 ]; then echo "PASS"; exit 0; fi
echo "FAIL: see above"
echo "----- serial output (tail) -----"
tail -60 "$LOG" | tr -d '\r'
exit 1
