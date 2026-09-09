#!/usr/bin/env bash
# run-shapes-guest.sh -- the tests/jssem/shapes cases ON THE MACHINE.
#
# The host run (run-shapes.sh) is the iteration loop; this is the evidence. The
# apparatus trap named at the top of this workflow is that a host-built qjs and
# the browser's engine are DIFFERENT BINARIES, and reporting the first as the
# second is the likeliest way to produce a wrong answer -- so the claim
# "modern-framework mechanisms work" is not made until it has been watched here.
#
# /bin/jsmicro is the runner: it is linked from $(ENGINE_OBJ), the literal
# object files build/browser.elf links, and it installs the browser's own
# queueMicrotask prelude and drains the job queue the way js_dom.c does.
#
# It reuses the disk image built by tests/jsmicro/build-guest.sh and appends the
# shapes cases to the same mkfs invocation, joining the Makefile's line
# continuations FIRST (CLAUDE.md rule 2) before reading anything out of it.
set -euo pipefail
cd "$(dirname "$0")/../.."
B="${BUILD:-build-jssem}"
ISO="${ISO:-$B/logit.iso}"
OUT="${OUT:-$B/rerun/shapes-guest}"
NODE="${NODE:-node}"
mkdir -p "$OUT"

[ -f "$B/jsmicro.aex" ] || { echo "FAIL: $B/jsmicro.aex absent -- run tests/jsmicro/build-guest.sh first"; exit 2; }

# --- pack the disk ----------------------------------------------------------
make -n build/disk.img 2>/dev/null | python3 -c '
import re, sys
t = re.sub(r"\\\r?\n[ \t]*", " ", sys.stdin.read())
for line in t.split("\n"):
    if "tools/mkfs.py" in line:
        sys.stdout.write(line.strip()); break
else:
    sys.exit("no mkfs.py invocation in make -n output")
' > "$B/mkfs_shapes.txt"
[ -s "$B/mkfs_shapes.txt" ] || { echo "could not read the disk recipe" >&2; exit 1; }

CASES=()
for f in tests/jssem/shapes/*.js; do CASES+=("$f:/shapes/$(basename "$f")"); done

# Drop pack pairs whose host file is absent and SAY SO -- five lines share this
# tree and a silent drop would be somebody else's program quietly missing.
BASE=()
for tok in $(sed "s|tools/mkfs.py build/disk.img|tools/mkfs.py $B/disk.img|" "$B/mkfs_shapes.txt"); do
    host="${tok%%:*}"
    if [ "${#BASE[@]}" -gt 2 ] && [ ! -e "$host" ]; then
        echo "  dropping (host file absent, not mine to build): $tok"
        continue
    fi
    BASE+=("$tok")
done
"${BASE[@]}" "$B/jsmicro.aex:/bin/jsmicro" "${CASES[@]}" >/dev/null

# --- boot once and run every case, fenced by the SHELL ----------------------
# The fence is echoed by the shell rather than by the runner, so a case that
# kills the runner still has its opening fence and shows up as truncated rather
# than silently merging into its neighbour.
LOG="$OUT/serial.log"
FIFO="$(mktemp -u)"
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; [ -n "${FEED:-}" ] && kill "$FEED" 2>/dev/null; rm -f "$FIFO"; }
trap cleanup EXIT
mkfifo "$FIFO"
{ sleep 6
  for f in tests/jssem/shapes/*.js; do
      c="$(basename "$f")"
      printf 'echo ===BEGIN %s\n' "$c"
      printf '/bin/jsmicro /shapes/%s\n' "$c"
      printf 'echo ===END %s\n' "$c"
  done
  printf 'echo ===ALLDONE\n'; sleep 600; } > "$FIFO" &
FEED=$!
qemu-system-x86_64 -cpu max -cdrom "$ISO" \
    -drive file="$B/disk.img",format=raw,if=none,id=hd0,file.locking=off \
    -device virtio-blk-pci,drive=hd0 -boot d -snapshot -m 512M -smp 4 \
    -accel tcg,thread=multi -vga none -device virtio-gpu-pci -serial stdio \
    -display none -no-reboot <"$FIFO" >"$LOG" 2>/dev/null &
QPID=$!
for i in $(seq 1 900); do
    grep -q '===ALLDONE' "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 1
done
grep -q '===ALLDONE' "$LOG" || { echo "FAIL: guest never printed ===ALLDONE"; tail -20 "$LOG"; exit 1; }

# --- slice and diff against node --------------------------------------------
python3 - "$LOG" "$OUT" <<'PY'
import re, subprocess, sys, os, glob
log, out = sys.argv[1], sys.argv[2]
text = open(log, errors="replace").read()
# Kernel diagnostics share the serial line and would SHIFT every following line,
# turning an identical case into a whole-case difference. Removed, and counted.
noise = re.compile(r"^\[[a-z]+\] ")
cur, buf, cases, dropped = None, [], {}, 0
for raw in text.split("\n"):
    l = raw.replace("\r", "")
    if noise.match(l):
        dropped += 1; continue
    m = re.match(r"^===BEGIN (\S+)", l)
    if m: cur, buf = m.group(1), []; continue
    m = re.match(r"^===END (\S+)", l)
    if m:
        if cur: cases[cur] = buf
        cur, buf = None, []; continue
    if cur is not None and not l.startswith("/bin/jsmicro") and not l.startswith("echo "):
        buf.append(l)
print("kernel serial lines removed: %d" % dropped)
bad = 0; ctl = False
for f in sorted(glob.glob("tests/jssem/shapes/*.js")):
    name = os.path.basename(f)
    want = subprocess.run(["node", "tests/jsmicro/node_run.js", f],
                          capture_output=True, text=True).stdout.split("\n")
    while want and want[-1] == "": want.pop()
    got = [x for x in cases.get(name, []) if x.strip() != "" or True]
    while got and got[-1] == "": got.pop()
    # the shell prompt trails the last line of a case
    got = [re.sub(r"^/ \$ ?", "", x) for x in got if x.strip() not in ("", "/ $")]
    if name.endswith("_CONTROL.js"):
        ctl = got != want
        print("%-28s %s" % (name, "control fired" if ctl else "CONTROL DID NOT FIRE"))
        continue
    if got == want:
        print("%-28s SAME (%d lines)" % (name, len(got)))
    else:
        bad += 1
        print("%-28s DIFF" % name)
        for a, b in zip(want, got):
            if a != b: print("    node : %s\n    guest: %s" % (a, b))
        for e in want[len(got):]: print("    node-only : %s" % e)
        for e in got[len(want):]: print("    guest-only: %s" % e)
print("\n%d cases differ" % bad)
if not ctl: print("HARNESS BROKEN: control did not fire"); sys.exit(2)
sys.exit(1 if bad else 0)
PY
