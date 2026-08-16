#!/usr/bin/env bash
# SW-shipped-compiler, the half a symbol table cannot show: THE COMPILER THAT
# RUNS IS THE ONE ON THE DISK.
#
# tests/unit/run-as-shipped.sh proves compiler.c and lexer.c are not in
# /bin/as. That is an absence. This proves the corresponding presence, on the
# real machine, by taking the compiler away mid-boot:
#
#   run 1   as /usr/as/examples/hello.as        -> works
#   rm /usr/as/lib/asc.la
#   run 2   as /usr/as/examples/hello.as        -> must FAIL, naming that path
#
# Identical binary, identical script, one file different: the only thing that
# can explain the difference is that /usr/as/lib/asc.la did the compiling. If
# any C compiler were still reachable in that binary, run 2 would print "Hello
# from AetherScript!" again and this gate would say so.
#
# It is also the NEGATIVE CONTROL the unit asked for: the file-missing case is
# the whole ship. /bin/as is what the GUI Terminal's shell can be, so a missing
# compiler must be a named, one-line refusal -- not a hang, not a fault, not a
# silent fallback. So the assertions are not just "run 2 failed": they are that
# it failed WITH THE PATH IN THE MESSAGE, that hello's output appears exactly
# once across the whole log, and that the shell was still taking commands
# afterwards.
#
# -snapshot, so the rm never reaches the real disk.
set -u

ISO="${1:?usage: run-as-shipped-negctl.sh <iso> <disk.img>}"
DISK="${2:?usage: run-as-shipped-negctl.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
LOG="$(mktemp)"
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null; rm -f "$LOG"; }
trap cleanup EXIT

{ sleep 4
  # Phase 0, the positive half: the shipped `-c` still produces bytecode the VM
  # loads and runs. It is the AetherScript compiler writing the .la now, so this
  # is not a formality -- if dump_module() and as_dump() had drifted, `-run`
  # would reject the file or run the wrong program.
  printf 'as -c /usr/as/examples/fib.as -o /negctl_fib.la\n'
  printf 'as -run /negctl_fib.la\n'
  printf 'as /usr/as/examples/hello.as\n'
  printf 'echo NEGCTL-RUN1-DONE\n'
  printf 'rm /usr/as/lib/asc.la\n'
  printf 'echo NEGCTL-REMOVED\n'
  printf 'as /usr/as/examples/hello.as\n'
  printf 'echo NEGCTL-RUN2-DONE\n'
  printf 'echo NEGCTL-SHELL-ALIVE\n'
  printf 'exit\n'
  sleep 15
} | "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
      -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
      -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
      -m 512M -smp 1 -accel tcg -vga none -device virtio-gpu-pci \
      -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

for _ in $(seq 1 300); do
    grep -aq "NEGCTL-SHELL-ALIVE" "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.5
done

fail=0
say() { echo "  $*"; }

# Control: run 1 must have worked, or the "difference" below is between two
# broken things.
if ! grep -aq "NEGCTL-RUN1-DONE" "$LOG"; then
    say "FAIL(control): the first run never completed -- nothing here is a measurement"; fail=1
fi
if [ "$(grep -ac 'Hello from AetherScript!' "$LOG")" -ne 1 ]; then
    say "FAIL: 'Hello from AetherScript!' appears $(grep -ac 'Hello from AetherScript!' "$LOG") times, want exactly 1."
    say "      0 = the compiler never worked; 2 = /bin/as compiled the script WITHOUT"
    say "      /usr/as/lib/asc.la, i.e. a C compiler is still in there."
    fail=1
fi
if ! grep -aq "fib(20) = 6765" "$LOG"; then
    say "FAIL: 'as -c' + 'as -run' round trip did not produce the right answer --"
    say "      the AetherScript compiler's .la is not what this VM loads"
    fail=1
fi
if ! grep -aq "NEGCTL-REMOVED" "$LOG"; then
    say "FAIL(control): rm of /usr/as/lib/asc.la did not complete"; fail=1
fi

# The refusal itself: by path, in one line, from /bin/as.
if ! grep -aq "as: cannot open /usr/as/lib/asc.la" "$LOG"; then
    say "FAIL: /bin/as did not name /usr/as/lib/asc.la when it could not find its compiler"
    fail=1
fi

# Not a hang and not a fault: the shell kept going.
if ! grep -aq "NEGCTL-RUN2-DONE" "$LOG"; then
    say "FAIL: /bin/as did not return after the missing compiler -- it hung"; fail=1
fi
if ! grep -aq "NEGCTL-SHELL-ALIVE" "$LOG"; then
    say "FAIL: the shell did not survive -- a missing compiler must not take the session down"; fail=1
fi
for bad in "PAGE FAULT" "PANIC" "GENERAL PROTECTION"; do
    if grep -aq "$bad" "$LOG"; then say "FAIL: '$bad' in the log -- it crashed rather than refused"; fail=1; fi
done

if [ "$fail" -ne 0 ]; then
    echo "FAIL: the missing-compiler path is not a loud, named, survivable refusal"
    echo "----- serial output -----"
    grep -a 'NEGCTL\|Hello from\|as: \|count ' "$LOG" | head -40
    echo "-------------------------"
    exit 1
fi
echo "as-shipped-negctl: hello ran once, then /usr/as/lib/asc.la was removed and the same"
echo "as-shipped-negctl: command refused BY PATH; shell survived."
echo "PASS: /bin/as compiles through /usr/as/lib/asc.la, and says so when it is gone"
