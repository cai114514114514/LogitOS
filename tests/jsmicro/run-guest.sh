#!/usr/bin/env bash
# run-guest.sh -- run the SAME cases on the machine, over serial, and diff each
# one against node.  This is the run that counts.  The host binary in
# tests/jsmicro/run.sh is the fast iteration loop and no finding is reported
# from it alone: it is arm64/darwin against a system libc, and the guest is
# x86_64 freestanding against a mini-libc arena allocator, with -DLOGIT_OS,
# -DCONFIG_STACK_CHECK and -DNDEBUG all live.
#
# Every case is run in ONE boot, one process per case, with a fence line
# printed around each so the transcript can be cut back into per-case stdout.
# The fence is echoed by the shell rather than by the runner, so a case that
# crashes the runner still has its opening fence and shows up as a truncated
# case rather than silently merging into its neighbour.
set -u
ISO="${ISO:-build/logit.iso}"
DISK="${DISK:-build/disk.img}"
QEMU="${QEMU:-qemu-system-x86_64}"
OUT="${OUT:-build/jsmicro-guest}"
mkdir -p "$OUT"
LOG="$OUT/serial.log"
FIFO="$(mktemp -u)"
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; [ -n "${FEED:-}" ] && kill "$FEED" 2>/dev/null; rm -f "$FIFO"; }
trap cleanup EXIT

CASES=()
for f in tests/jsmicro/cases/*.js; do CASES+=("$(basename "$f")"); done
CHARS=()
for f in tests/jsmicro/chars/*.js; do CHARS+=("$(basename "$f")"); done
echo "run-guest: ${#CASES[@]} cases, one boot"

if [ "${REPARSE:-0}" = 1 ] && [ -s "$LOG" ]; then
    echo "REPARSE=1: re-cutting the saved transcript, not booting."
else
mkfifo "$FIFO"
{
  sleep 6
  for c in "${CASES[@]}" "${CHARS[@]}"; do
      printf 'echo ===BEGIN %s\n' "$c"
      printf '/bin/jsmicro /jsmicro/%s\n' "$c"
      printf 'echo ===END %s\n' "$c"
  done
  printf 'echo ===ALLDONE\n'
  sleep 600
} > "$FIFO" &
FEED=$!

"$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off -device virtio-blk-pci,drive=hd0 \
    -boot d -snapshot -m 512M -smp 4 -accel tcg,thread=multi \
    -vga none -device virtio-gpu-pci -serial stdio -display none -no-reboot \
    <"$FIFO" >"$LOG" 2>/dev/null &
QPID=$!

for _ in $(seq 1 "${TIMEOUT:-900}"); do
    grep -q '===ALLDONE' "$LOG" 2>/dev/null && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 1
done
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
fi

if ! grep -q '===ALLDONE' "$LOG"; then
    echo "GUEST DID NOT FINISH -- last 20 lines of serial:"; tail -20 "$LOG"; exit 1
fi

fail=0; pass=0; ctl_fired=0; ctl_seen=0
for c in "${CASES[@]}" "${CHARS[@]}"; do
    b="${c%.js}"
    # Cut the transcript between the fences.  The `echo` command itself is
    # echoed back by the tty, so the marker appears twice; take the LAST
    # occurrence of BEGIN and the FIRST END after it.
    python3 - "$LOG" "$c" > "$OUT/$b.guest" <<'PY'
import sys, re
log, case = sys.argv[1], sys.argv[2]
txt = open(log, "r", errors="replace").read().replace("\r", "")
lines = txt.split("\n")
bi = [i for i, l in enumerate(lines) if l.strip() == "===BEGIN " + case]
ei = [i for i, l in enumerate(lines) if l.strip() == "===END " + case]
if not bi or not ei:
    sys.stdout.write("<<FENCE MISSING>>\n"); raise SystemExit
b = bi[-1]
e = [i for i in ei if i > b]
if not e:
    sys.stdout.write("<<END FENCE MISSING>>\n"); raise SystemExit
body = lines[b+1:e[0]]
# The serial console carries three things that are not the case's stdout: the
# shell's prompt-plus-echo of what we typed, and the kernel's own periodic
# reports ([mm], [wm], [exec], [execve], ...).  Both are filtered by an
# ANCHORED prefix match and nothing else -- no case in tests/jsmicro/cases
# prints a line beginning with "/ $ " or "[", which is a property of the corpus
# that `grep -c` can check and a reader can too.  A substring filter here would
# be the apparatus editing the measurement.
BANNERS = ("KBENCH_DONE", "WAITQ_SELFTEST_OK")
def noise(l):
    s = l.rstrip()
    if s == "":
        return True
    if s.startswith("/ $ ") or s.startswith("$ "):
        return True
    if re.match(r"^\[[a-z0-9_]+\]", s):          # every kernel report is [tag]
        return True
    if s.split(" ")[0] in BANNERS or s.startswith("WAITQ_SELFTEST_OK"):
        return True
    return False
body = [l for l in body if not noise(l)]
sys.stdout.write("\n".join(body).rstrip("\n") + ("\n" if body else ""))
PY
    case "$c" in c0*) continue ;; esac
    node tests/jsmicro/node_run.js "tests/jsmicro/cases/$c" > "$OUT/$b.node" 2>/dev/null
    case "$b" in *_CONTROL) ctl_seen=1 ;; esac
    if cmp -s "$OUT/$b.node" "$OUT/$b.guest"; then
        pass=$((pass+1)); printf 'ok    %s\n' "$b"
    else
        fail=$((fail+1)); printf 'DIFF  %s\n' "$b"
        case "$b" in *_CONTROL) ctl_fired=1 ;; esac
        diff -u "$OUT/$b.node" "$OUT/$b.guest" | sed 's/^/      /'
    fi
done
for c in "${CHARS[@]}"; do
    b="${c%.js}"
    printf '\n--- characterization %s (node is NOT a valid oracle here; guest output only) ---\n' "$b"
    sed 's/^/      /' "$OUT/$b.guest" 2>/dev/null || echo "      <no output>"
done
printf '\nGUEST: %d ok, %d differ\n' "$pass" "$fail"
if [ "$ctl_seen" = 1 ] && [ "$ctl_fired" = 0 ]; then
    printf 'HARNESS BROKEN: the control did not report a difference in the guest.\n'; exit 2
fi
[ "$ctl_seen" = 1 ] && printf 'control: fired in the guest\n'
exit 0
