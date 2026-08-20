#!/usr/bin/env bash
# Headless end-to-end LOGITLM test: boot LogitOS, run /bin/lm on the model
# packed into the disk image, and require the bytes it generates to equal the
# bytes the IDENTICAL SOURCE generates on the host from the IDENTICAL file.
#
# Same argument as run-video-test.sh makes for the H.264 decoder, and it is
# the only reason this test is worth its minutes: c/apps/lm/lm.c and c/lib/nn
# compile twice from one source, but the two builds share almost nothing else.
# The host build is clang + glibc + glibc's libm on Linux; the target build is
# clang -ffreestanding against mini-libc's arena malloc, third_party/libm's
# double-only subset, SSE enabled by boot code rather than by the ABI, and a
# 3.2 MB weight file read through virtio-blk and LogitFS instead of read().
# Greedy decoding is argmax, which is DISCONTINUOUS in the logits -- so a
# single differing bit anywhere in 74 forward passes shows up here as a
# different byte, and the two sides agreeing over 64 bytes is a much stronger
# statement than a tolerance ever is.
#
# THE EXPECTATION IS COMPUTED, NOT PINNED. build/model.lm is not in the build
# graph and not committed (Makefile:1144 says why: it comes from running the
# host trainer by hand), so a fixture file of expected bytes would be a fixture
# for one particular training run and would go stale silently the first time
# anybody retrains. This harness runs the host binary itself, here, against the
# same file it just packed, so the comparison cannot drift from the weights.
#
# ---------------------------------------------------------------------------
# WHY IT RUNS /bin/lm THREE TIMES AND JUDGES ONLY THE UNCONTAMINATED RUNS.
#
# Measured, on this disk image, 2026-08-20 -- not anticipated:
#
#   the\r\n [time] fallback pit ran 500ms (want ~500), switch back rc=0 ...\nthe same\r\n
#
# The kernel's kprintf writes COM1 through serial_putc() directly
# (c/drivers/char/serial.c:23) and a ring-3 write reaches the SAME
# serial_putc() through tty_write (c/kernel/exec/file.c:122). Nothing
# serialises the two, and serial_putc's wait for the transmitter is bounded
# and DROPS the byte on timeout. In the transcript above the program had
# written "the\r\n  the same" -- two spaces -- and exactly ONE of them reached
# the wire, at precisely the point where a kernel line interleaved. That run
# came back 74 bytes against the host's 75.
#
# That is a property of the console, not of the arithmetic, and it must not be
# smoothed over: a byte-comparison that silently accepts a missing byte is a
# byte-comparison that would also accept a wrong one. So:
#
#   - kernel output is REMOVED structurally, not by guessing at content (see
#     the filter's own comment below), and how many bytes it removed FROM
#     INSIDE each generated region is recorded;
#   - a region with zero removed bytes is CLEAN and is held to byte equality
#     with the host. At least one clean region is required, so the test can
#     never pass by having nothing to judge;
#   - a contaminated region is still checked, against a weaker but decisive
#     property: it must be a SUBSEQUENCE of the host's bytes, i.e. explicable
#     by deletions alone. Greedy decoding is argmax, so an arithmetic
#     divergence substitutes a byte and then the two continuations separate
#     completely -- which is not a subsequence, and would fail here.
#
# Portable: no `timeout` dependency, polls the log like the other boot tests.

set -u

ISO="${1:?usage: run-lm-test.sh <iso> <disk.img> <host-lm-binary> <model.lm>}"
DISK="${2:?usage: run-lm-test.sh <iso> <disk.img> <host-lm-binary> <model.lm>}"
HOSTLM="${3:?usage: run-lm-test.sh <iso> <disk.img> <host-lm-binary> <model.lm>}"
MODEL="${4:?usage: run-lm-test.sh <iso> <disk.img> <host-lm-binary> <model.lm>}"
QEMU="${QEMU:-qemu-system-x86_64}"

# One prompt and one length, read by BOTH sides below. Two literals would be
# two things to keep in sync and the sync is what the test is checking.
PROMPT="${LM_PROMPT:-The kernel}"
N="${LM_N:-64}"
RUNS="${LM_RUNS:-3}"

case "$PROMPT" in
  *\'*) echo "FAIL: LM_PROMPT may not contain a single quote (it is passed"
        echo "      through /bin/sh's single-quoted word)"; exit 1;;
esac

[ -x "$HOSTLM" ] || { echo "FAIL: no host lm binary at $HOSTLM"; exit 1; }
[ -f "$MODEL" ]  || { echo "FAIL: no model at $MODEL"; exit 1; }

WORK="$(mktemp -d)"
LOG="$WORK/serial.log"
cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

# ------------------------------------------------------------------- host ----
"$HOSTLM" -m "$MODEL" --greedy -n "$N" -p "$PROMPT" > "$WORK/host.raw" 2>&1 || {
    echo "FAIL: the host lm binary exited non-zero"; cat "$WORK/host.raw"; exit 1; }

# ------------------------------------------------------------------ guest ----
# -snapshot: ephemeral disk writes, so repeated runs are deterministic.
#
# THE FEEDER WAITS FOR WHAT IT CAN SEE, AND RETRIES. The neighbouring boot
# harnesses type after a fixed `sleep`, and that is the single flakiest thing
# in this file: measured 2026-08-20 on a machine that was also compiling the
# kernel, a `sleep 6` put the whole command line into a UART the shell was not
# yet reading, and the transcript came back with the boot log, the prompt, and
# no echo of the command at all -- indistinguishable, from the outside, from a
# model that generated nothing. So the feeder polls the transcript for the
# shell's own banner before typing, then waits for THIS run's closing line
# before typing the next, and retypes a command whose answer never came.
#
# The feeder reads $LOG while QEMU writes it; the file is created below by the
# redirection, and truncated here first so a stale one can never satisfy a
# wait.
: > "$LOG"
CMD="/bin/lm --greedy -n $N -p '$PROMPT'"
{
  shell=0
  for _ in $(seq 1 2400); do             # 240 s; TCG boots slowly under load
      if grep -aq "LogitOS shell" "$LOG" 2>/dev/null; then shell=1; break; fi
      sleep 0.1
  done
  # TYPING ONLY IF THERE IS SOMETHING TO TYPE AT. Seen once, 2026-08-20, with
  # four QEMUs sharing the host: `[fault] app exception: page fault (vector 14)
  # rip=0x50000000 ... -- terminating app` for tid 6 (login), so /bin/sh never
  # started and no banner ever came. Retrying a command into that costs
  # RUNS x 4 x 40 s of nothing; skipping straight to the sentinel bounds a
  # no-shell boot at the 240 s above, and the caller below reports it as what
  # it is rather than as a model that generated nothing.
  if [ "$shell" -eq 1 ]; then
      sleep 1                            # the banner precedes the first read
      i=0
      while [ "$i" -lt "$RUNS" ]; do
          want=$((i + 1)); got=0; tries=0
          while [ "$tries" -lt 4 ] && [ "$got" -eq 0 ]; do
              printf '%s\n' "$CMD"
              tries=$((tries + 1))
              for _ in $(seq 1 400); do  # 40 s: load 3.2 MB + decode under TCG
                  c=$(grep -ac "forward steps" "$LOG" 2>/dev/null); c=${c:-0}
                  [ "$c" -ge "$want" ] && { got=1; break; }
                  sleep 0.1
              done
          done
          i=$((i + 1))
      done
      printf 'exit\n'; sleep 2
  fi
  : > "$WORK/fed"                        # the poll loop below waits on this
} | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off -device virtio-blk-pci,drive=hd0 \
    -boot d -snapshot -m 512M -smp 4 -accel tcg,thread=multi \
    -vga none -device virtio-gpu-pci -serial stdio -display none -no-reboot \
    >"$LOG" 2>/dev/null &
QPID=$!

# Wait for every run to print its closing line, or for the feeder above to give
# up, or for the guest to die. THE SENTINEL IS WHY THIS IS NOT A SECOND TIMER:
# an independent deadline here raced the feeder's -- measured on a machine that
# was also compiling the kernel, this loop expired while the feeder was still
# waiting for the shell banner, and the harness reported "no generated region"
# about a guest it had killed mid-boot. Two clocks for one wait is one clock
# too many.
for _ in $(seq 1 9000); do
    n=$(grep -ac "forward steps" "$LOG" 2>/dev/null); n=${n:-0}
    [ "$n" -ge "$RUNS" ] && break
    [ -f "$WORK/fed" ] && break
    if grep -aqE "^lm: (cannot|refusing|out of memory|short read)" "$LOG"; then break; fi
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done
sleep 1                                  # let the last line finish draining

# Keep the transcript when something is wrong with it: $WORK is a mktemp -d
# that the EXIT trap removes, and "no generated region" is unarguable from the
# 25 tail lines alone.
save_log() { cp "$LOG" "${LM_SAVE_LOG:-build/lm-serial-fail.log}" 2>/dev/null &&
             echo "      (full transcript: ${LM_SAVE_LOG:-build/lm-serial-fail.log})"; }

if ! grep -aq "LogitOS shell" "$LOG"; then
    echo "FAIL: the guest never reached a shell prompt, so /bin/lm never ran."
    echo "  This is a BOOT finding, not a model one -- nothing here measured"
    echo "  the arithmetic. Any ring-3 fault the boot reported:"
    grep -a "app exception" "$LOG" | head -5 | sed 's/^/    /'
    save_log
    exit 1
fi

if grep -aqE "^lm: (cannot|refusing|out of memory|short read)" "$LOG"; then
    echo "FAIL: /bin/lm refused to run on the device:"
    grep -aE "^lm: " "$LOG" | tail -5
    save_log
    exit 1
fi

# ---------------------------------------------------------------- compare ----
cat > "$WORK/compare.py" <<'PY'
import re, sys

CR, LF = 13, 10
host_raw = open(sys.argv[1], "rb").read()
wire = open(sys.argv[2], "rb").read()

# THE FILTER. A kernel diagnostic on this wire is identifiable STRUCTURALLY,
# not by a list of prefixes that would rot: kprintf goes to serial_putc with no
# newline translation, so it ends in a BARE LF, while every byte a program
# writes passes through tty_write, which emits CR before every LF
# (c/kernel/exec/file.c:122). So program output contains no bare LF at all, and
# any match below necessarily ends inside a kernel message. Starting the match
# at a '[' that has no '[' after it before the LF picks the INNERMOST opening
# bracket, so a '[' the model itself generated (CLAUDE.md is full of "[mm]",
# "[dl]" and friends, and the model learned them) cannot be swallowed as the
# start of a kernel line that happened to interleave just after it.
BS = bytes([92])                       # a literal backslash, built rather
                                       # than typed: this file is read back
                                       # through a shell heredoc.
KERN = re.compile(BS + b"[[^" + BS + b"[" + bytes([CR, LF]) + b"]*" + bytes([LF]))

START = re.compile(b"KiB total[" + bytes([CR]) + b"]?" + bytes([LF]))
END = re.compile(b"lm: [0-9]+ forward steps")

def regions(data, filt):
    """The bytes lm.c writes between its 'KiB total' line and its closing
    'forward steps' line -- fputs(prompt) plus one putchar per token."""
    out = []
    starts = [m.end() for m in START.finditer(data)]
    ends = [m.start() for m in END.finditer(data)]
    for s in starts:
        nxt = [e for e in ends if e > s]
        if not nxt:
            continue
        raw = data[s:nxt[0]]
        removed = sum(len(m.group(0)) for m in KERN.finditer(raw)) if filt else 0
        clean = KERN.sub(b"", raw) if filt else raw
        if filt:
            bare = [i for i, b in enumerate(clean)
                    if b == LF and (i == 0 or clean[i - 1] != CR)]
            if bare:
                # Something reached the console that is neither this program's
                # CR-expanded output nor a '['-prefixed kernel line. Naming it
                # is the point: silently keeping it would be reported later as
                # a model divergence, which it is not.
                sys.stderr.write("unrecognised console output inside a "
                                 "generated region: " + repr(clean) + "\n")
                sys.exit(3)
            clean = clean.replace(bytes([CR, LF]), bytes([LF]))
        out.append((clean, removed))
    return out

def is_subseq(small, big):
    it = iter(big)
    return all(c in it for c in small)

host = regions(host_raw, False)
if len(host) != 1:
    sys.stderr.write("host transcript did not contain exactly one region\n")
    sys.exit(3)
host = host[0][0]

guest = regions(wire, True)
if not guest:
    sys.stderr.write("no generated region in the serial transcript\n")
    sys.exit(3)

fail = 0
clean_seen = 0
for i, (g, removed) in enumerate(guest):
    if removed == 0:
        clean_seen += 1
        verdict = "IDENTICAL" if g == host else "DIFFERENT"
        if g != host:
            fail = 1
        print("  run %d: clean (%d bytes) -- %s to the host" % (i + 1, len(g), verdict))
        if g != host:
            print("    host  : " + repr(host))
            print("    device: " + repr(g))
    else:
        ok = is_subseq(g, host)
        if g == host:
            why = "identical to the host anyway"
        elif ok:
            why = ("a subsequence of the host's -- %d byte(s) lost to the "
                   "console race, no substitution" % (len(host) - len(g)))
        else:
            why = "NOT a subsequence of the host's"
        print("  run %d: %d byte(s) of kernel log interleaved; %d bytes vs the "
              "host's %d -- %s" % (i + 1, removed, len(g), len(host), why))
        if not ok:
            fail = 1
            print("    host  : " + repr(host))
            print("    device: " + repr(g))

if clean_seen == 0:
    print("  no run was free of interleaved kernel output, so nothing was held")
    print("  to byte equality -- raise LM_RUNS or quiet the console.")
    fail = 1
print("  %d of %d run(s) clean" % (clean_seen, len(guest)))
print("HOSTBYTES " + str(len(host)))
sys.exit(1 if fail else 0)
PY

echo "test-lm-os: comparing $RUNS device run(s) against the host"
if ! python3 "$WORK/compare.py" "$WORK/host.raw" "$LOG"; then
    echo "FAIL: the device did not reproduce the host's greedy bytes."
    echo "  Same weights, same source, same f32 arithmetic -- a substitution"
    echo "  here is a real divergence (mini-libc's math, the arena allocator,"
    echo "  or SSE state), not a tolerance to widen."
    echo "----- serial output (tail) -----"
    tail -25 "$LOG"
    echo "--------------------------------"
    save_log
    exit 1
fi

echo "PASS: LogitOS generated the host's bytes, greedily."
grep -a "forward steps" "$LOG" | tr -d '\r' | sed 's/^/      /'
echo "      $(grep -a '^LOGITLM ' "$LOG" | tail -1 | tr -d '\r')"
exit 0
