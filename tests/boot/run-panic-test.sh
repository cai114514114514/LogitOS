#!/usr/bin/env bash
# On-device proof for the panic path: a deliberate panic must produce a
# message, a register dump, a STACK BACKTRACE, a dump of the log ring, and a
# halt that does not turn into a reboot loop.
#
# The backtrace is the part that is easy to fake and easy to get quietly wrong,
# so it is not checked by eyeballing that hex numbers appeared. Every frame the
# kernel printed is resolved against build/kernel.map -- the linker's own
# record of where each function ended up -- and the test requires that the
# innermost frames land on the functions that actually called panic(). A
# backtrace of plausible-looking garbage fails here.
#
# The negative control is `make FPO=1` (drop -fno-omit-frame-pointer and
# rebuild): rbp stops being a frame pointer, the chain assertions below fail,
# and the run degrades to the conservative stack scan.

set -u

ISO="${1:?usage: run-panic-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-panic-test.sh <iso> <disk.img>}"
MAP="${MAP:-build/kernel.map}"
QEMU="${QEMU:-qemu-system-x86_64}"
LOG="$(mktemp)"
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null; }
trap cleanup EXIT

NET="-netdev user,id=n0 -device e1000,netdev=n0"

{
  sleep 5
  # Something the KERNEL printed, shortly before the panic, so the ring dump
  # has run-up to show. (A shell `echo` would not do: that is the process's own
  # stdout going to the tty, and it never passes through kprintf.)
  printf 'echo bt > /dev/ktrigger\n';      sleep 3
  printf 'echo panic deliberate-panic-probe > /dev/ktrigger\n'
  sleep 20
} | "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
      -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
      -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
      -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
      $NET -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

for _ in $(seq 1 400); do
    grep -aq "LOGIT_PANIC_END" "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done
sleep 3
kill "$QPID" 2>/dev/null

fails=0
ok()   { echo "  ok    $1"; }
bad()  { echo "  FAIL  $1"; fails=$((fails+1)); }
want() { if grep -aqE "$1" "$LOG"; then ok "$2"; else bad "$2"; fi; }

echo "--- the report ---"
want '\*\*\* LOGIT_PANIC on cpu[0-9] \*\*\*'   "a panic banner naming the core"
want 'reason: deliberate panic from /dev/ktrigger: deliberate-panic-probe' \
                                               "the reason, with the caller's message"
want '^RIP 0x[0-9a-f]+  RSP 0x[0-9a-f]+  RBP 0x[0-9a-f]+  RFLAGS 0x[0-9a-f]+' \
                                               "instruction/stack/frame pointers and flags"
want '^CR0 0x[0-9a-f]+  CR2 0x[0-9a-f]+  CR3 0x[0-9a-f]+  CR4 0x[0-9a-f]+' \
                                               "control registers"
# The honesty line: a deliberate panic has no trap frame, and the report says
# so rather than printing the panic handler's own registers as if they were the
# caller's.
want 'GPRs: not captured -- deliberate panic'  "it declines to invent a register frame"
want '^backtrace:'                             "a backtrace section"

echo "--- the log ring is dumped with the panic ---"
want '^--- log ring \(last 40 of [0-9]+ records\) ---' "the ring is dumped"
want '\[ *[0-9]+\.[0-9]{3}\] [PEWID] cpu[0-9] KDIAG_BT end' \
                                               "the dump contains kernel output from BEFORE the panic"
dumped=$(sed -n '/--- log ring (last 40/,/LOGIT_PANIC_END/p' "$LOG" |
         grep -acE '^\[ *[0-9]+\.[0-9]{3}\] [PEWID] cpu[0-9]' || true)
if [ "$dumped" -ge 5 ]; then
    ok "the dump carried $dumped retained records, not just a header"
else
    bad "the dump carried only $dumped records"
fi

echo "--- it halts, it does not reboot ---"
want 'LOGIT_PANIC_END -- halted, not rebooting' "the halt marker"
# Count only LIVE boot markers: the ring dump re-prints old lines with a
# "[    2.500] I cpu0 " prefix, and counting those as boots would read the
# panic report itself as evidence of a reboot.
boots=$(grep -acE "^LOGIT_BOOT_OK" "$LOG" || true)
if [ "$boots" = "1" ]; then
    ok "the machine booted exactly once (no reboot loop)"
else
    bad "saw $boots boot markers -- the panic rebooted"
fi
tail_after=$(sed -n '/LOGIT_PANIC_END/,$p' "$LOG" | grep -ac "long mode, C kernel running" || true)
if [ "$tail_after" = "0" ]; then
    ok "nothing was printed after the halt"
else
    bad "the kernel restarted after the panic"
fi

echo "--- the backtrace, checked against the linker map ---"
if [ ! -f "$MAP" ]; then
    bad "no linker map at $MAP (build it: make)"
else
    python3 - "$LOG" "$MAP" <<'PY'
import re, sys, bisect

log, mapfile = sys.argv[1], sys.argv[2]
text = open(log, 'rb').read().decode('utf-8', 'replace')

# Only the PANIC's backtrace. The run also takes a live one through
# `echo bt > /dev/ktrigger`, and mixing the two would let a good live
# backtrace cover for a bad panic backtrace.
cut = text.find('*** LOGIT_PANIC on cpu')
if cut < 0:
    print("  FAIL  no panic banner in the log"); sys.exit(1)
text = text[cut:]

# Frames the kernel printed: "  #0 0x0000000000104b2c" (a trailing '?' means the
# unwinder could not find a call in front of the address and said so).
frames = re.findall(r'^\s*#(\d+) (0x[0-9a-f]+)(\s+\?)?', text, re.M)
if not frames:
    print("  FAIL  the panic printed no backtrace frames at all"); sys.exit(1)

# ld.lld's map: VMA LMA Size Align  Symbol, with symbol lines indented deepest.
syms = []
for line in open(mapfile, encoding='utf-8', errors='replace'):
    m = re.match(r'^\s*([0-9a-f]{6,16})\s+[0-9a-f]{6,16}\s+[0-9a-f]+\s+\d+\s{4,}(\S+)\s*$', line)
    if m:
        name = m.group(2)
        if name.startswith('.') or '/' in name:
            continue
        syms.append((int(m.group(1), 16), name))
if not syms:
    print("  FAIL  parsed no symbols out of %s" % mapfile); sys.exit(1)
syms.sort()
addrs = [a for a, _ in syms]

def resolve(a):
    i = bisect.bisect_right(addrs, a) - 1
    if i < 0:
        return None, 0
    return syms[i][1], a - syms[i][0]

print("  map: %d symbols from %s" % (len(syms), mapfile))
resolved, named = [], []
for idx, addr, uncertain in frames:
    a = int(addr, 16)
    name, off = resolve(a)
    tag = "  (unverified call site)" if uncertain.strip() else ""
    print("    #%-2s %s  ->  %s+0x%x%s" % (idx, addr, name, off, tag))
    resolved.append((int(idx), a, name, off, bool(uncertain.strip())))
    if name:
        named.append(name)

fails = 0
def check(cond, what):
    global fails
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond: fails += 1

# 1. Enough frames that this is a chain and not one lucky value.
check(len(resolved) >= 3, "the chain has at least 3 frames (got %d)" % len(resolved))

# 2. Every frame must resolve to a symbol at a SMALL offset. A frame that lands
#    64 KiB past the nearest symbol is not inside a function; it is noise that
#    happened to fall in the address range.
big = [(hex(a), n, o) for _, a, n, o, _ in resolved if n is None or o > 4096]
check(not big, "every frame is within 4 KiB of a mapped symbol (%r)" % (big[:3],))

# 3. The innermost frame must be the function that CALLED panic. Anything else
#    means the unwinder is reading the wrong slot.
check(resolved[0][2] == 'kdiag_write',
      "frame #0 is kdiag_write, the caller of panic() (got %s+0x%x)"
      % (resolved[0][2], resolved[0][3]))

# 4. And the frames above it must be the real path a write to /dev/ktrigger
#    takes: the VFS, the file layer, the syscall dispatcher. This is the check
#    that a fabricated backtrace cannot pass.
expected_any = ['vfs_write', 'file_close', 'file_release', 'sys_close',
                'syscall_dispatch', 'interrupt_handler', 'file_write',
                'proc_close', 'do_close']
hit = [n for n in named[1:] if n in expected_any]
check(bool(hit), "an outer frame is on the write(/dev/ktrigger) path (%r of %r)"
      % (hit, named[1:5]))

# 5. Every frame the unwinder presented as certain must really be preceded by a
#    call instruction -- i.e. it did not mark garbage as fact.
unc = [hex(a) for _, a, _, _, u in resolved if u]
check(not unc, "no frame was presented without a verified call site (%r)" % (unc[:3],))

sys.exit(1 if fails else 0)
PY
    if [ $? -ne 0 ]; then fails=$((fails+1)); fi
fi

echo
if [ "$fails" -eq 0 ]; then
    echo "PASS: panic produces a message, registers, a map-verified backtrace, the log, and halts"
    exit 0
fi
echo "FAIL: $fails assertion(s)"
echo "----- serial output -----"
cat "$LOG"
echo "-------------------------"
exit 1
