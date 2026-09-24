#!/usr/bin/env bash
# statvfs/df/du on the machine (SYS_STATVFS, c/fs/logitfs/logitfs.c
# logitfs_statfs). Every number is checked against something that is not the
# code under test:
#
#   total      == TOTAL_BLOCKS * 4 KiB from tools/mkfs.py, the image's geometry
#   cp a file  -> used grows by the file's own block count (host byte size,
#                 rounded up to 4 KiB, plus at most a couple of indirect blocks)
#   rm it      -> used returns to exactly where it was
#   df (no arg) lists logitfs mounted at /, from the kernel's mount table
#
# With --expect-enosys (a kernel built -DLOGITFS_NO_STATFS) df must print
# dashes for / -- the capacity is unknown, and says so, rather than invented.

set -u
. "$(dirname "$0")/bootwait.sh"

ISO="${1:?usage: run-statvfs-test.sh <iso> <disk.img> [--expect-enosys]}"
DISK="${2:?usage: run-statvfs-test.sh <iso> <disk.img> [--expect-enosys]}"
MODE="${3:-}"
QEMU="${QEMU:-qemu-system-x86_64}"
LOG="$(mktemp)"
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null; rm -f "$LOG"; }
trap cleanup EXIT

TOTAL_BLOCKS=$(sed -n 's/^TOTAL_BLOCKS *= *\([0-9]*\).*/\1/p' tools/mkfs.py)
FONT_BYTES=$(wc -c < fsroot/fonts/ui.ttf | tr -d ' ')
[ -n "$TOTAL_BLOCKS" ] && [ -n "$FONT_BYTES" ] || { echo "FAIL: could not read the geometry"; exit 1; }

SCRIPT="echo warmup
echo SV-START
/bin/df
/bin/df -k /
/bin/cp /fonts/ui.ttf /svcopy
/bin/df -k /
/bin/rm /svcopy
/bin/df -k /
/bin/df -i /
echo SV-END
exit
"

# The first line typed after the prompt appears is occasionally lost; the
# warmup line absorbs that (same as run-shebang-test.sh).
NET="-netdev user,id=n0 -device e1000,netdev=n0"
{ logit_wait_for_shell "$LOG" 120; printf '%s' "$SCRIPT"; sleep 25; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
    -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
    -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
    $NET -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

for _ in $(seq 1 900); do
    grep -aq "^SV-END" "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done

# The log goes in by PATH: a heredoc already owns python's stdin, so piping the
# log in as well hands the checks an empty string (found by the control).
python3 - "$TOTAL_BLOCKS" "$FONT_BYTES" "$LOG" "$MODE" <<'PY'
import re, sys
total_blocks, font_bytes, mode = int(sys.argv[1]), int(sys.argv[2]), sys.argv[4]
log = open(sys.argv[3], "rb").read().decode("utf-8", "replace").replace("\r", "")
fails = []
def ck(cond, what):
    print("  %s: %s" % ("ok" if cond else "FAIL", what))
    if not cond: fails.append(what)

ck("SV-START" in log and "SV-END" in log, "the commands ran")
rows = re.findall(r"^/\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+/\s*$", log, re.M)
if mode == "--expect-enosys":
    ck(len(rows) >= 1 and all(r == ("-", "-", "-", "-") for r in rows),
       "control kernel: df prints dashes for / (capacity unknown, not invented)")
else:
    num = [tuple(int(x.rstrip("%")) for x in r) for r in rows if r[0] != "-"]
    ck(len(num) >= 3, "three df -k / rows (before, after cp, after rm): got %d" % len(num))
    if len(num) >= 3:
        (t0, u0, a0, _), (t1, u1, _, _), (t2, u2, _, _) = num[:3]
        want_kib = total_blocks * 4
        ck(t0 == want_kib, "total is the image's geometry: %d KiB (mkfs.py %d blocks x 4 KiB = %d)"
           % (t0, total_blocks, want_kib))
        ck(u0 + a0 == t0, "used + available == total (%d + %d = %d)" % (u0, a0, t0))
        fb = -(-font_bytes // 4096)
        grew = (u1 - u0) // 4
        ck(fb <= grew <= fb + 2, "copying a %d-byte file adds its %d blocks (+ indirects): grew %d"
           % (font_bytes, fb, grew))
        ck(u2 == u0, "removing it gives every block back: %d -> %d -> %d KiB" % (u0, u1, u2))
    ck(re.search(r"^logitfs\s+\d+\s+\d+\s+\d+\s+\d+%\s+/\s*$", log, re.M) is not None,
       "df with no argument lists logitfs mounted at /")
    ck(re.search(r"^/\s+\d+\s+\d+\s+\d+\s+\d+%\s+/\s*$", log.split("Inodes", 1)[-1], re.M) is not None,
       "df -i reports inode counts")
if fails:
    print("FAIL: see above"); print("----- serial (tail) -----"); print("\n".join(log.splitlines()[-40:]))
    sys.exit(1)
print("PASS")
PY
