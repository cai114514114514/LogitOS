#!/usr/bin/env bash
# /proc ON THE MACHINE: boot, and drive ORDINARY PROGRAMS that read files.
#
# THE CLAIM, and it is the one thing this gate exists for: /bin/ps lists the
# console shell and itself, and it does so without a single syscall that `cat`
# does not also make. If ps ever needs a syscall of its own, /proc has failed
# at the one thing it is for -- so the gate is not "ps prints something", it is
# "an ordinary file reader can see the process table".
#
# It also checks the LIVENESS property on the real machine, which the host gate
# can only check against a table it controls: two `cat /proc/uptime` a couple
# of seconds apart must print DIFFERENT numbers. A snapshot taken at open()
# would print the same one twice and everything else here would still pass.
#
# BUILD. Three programs have to be on the disk, and the Makefile lists CLI
# programs by name (Makefile:461). The one-word-each addition is written down
# in tests/procfs.mk; until it lands this passes CLI on the command line, which
# is equivalent: CLI is a simply-expanded variable used only by the CLI_RULE
# $(foreach) and by the $(DISK) recipe's $(foreach c,$(CLI),...) pack line, so
# overriding it adds the three programs and changes nothing else.
#
#   SKIP_BUILD=1   use the build/ that is already there. For a tree where the
#                  kernel does not link for an unrelated reason (see the report
#                  that shipped this file), or to re-run without a rebuild.

set -u

ISO="${ISO:-build/logit.iso}"
DISK="${DISK:-build/disk.img}"
QEMU="${QEMU:-qemu-system-x86_64}"
LOG="$(mktemp)"
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null; rm -f "$LOG" "$LOG.txt"; }
trap cleanup EXIT

CLI_BASE="sh echo ls cat pwd wc head true false sleep mkdir rm touch clear uname net cp mv smptest socktest show dir chart prog clip notify execinfo entropy httpd stat poweroff reboot pref ping syslogd"
CLI_ALL="$CLI_BASE ps free uptime"

if [ "${SKIP_BUILD:-0}" != "1" ]; then
    # MAKE is a variable so a tree whose kernel does not link for an unrelated
    # reason can point this at a wrapper that drops the offending object. It
    # was needed on the day this landed -- c/kernel/mm/oom.c was untracked and
    # mid-flight, referencing three symbols nothing defined -- and the wrapper
    # is four lines. Nothing about /proc depends on it.
    "${MAKE:-make}" CLI="$CLI_ALL" "$ISO" "$DISK" || { echo "FAIL: build"; exit 1; }
fi

for f in "$ISO" "$DISK"; do
    [ -f "$f" ] || { echo "FAIL: $f is missing (build it, or drop SKIP_BUILD=1)"; exit 1; }
done

# BOOT A PRIVATE COPY, and this is not caution -- it is a bug that already bit.
# Several lines of work share this build/ and several of them pass their own
# CLI= override, so `make ... build/disk.img` and the QEMU that reads it are
# separated by seconds in which another agent's build can replace the image.
# It did: this gate reported "/bin/ps: permission denied (not executable)" for
# a disk that had been rebuilt, 19 seconds after ours, from a CLI list without
# ps in it -- and that message is what execve prints for a file that IS NOT
# THERE (c/kernel/exec/exec.c:308 calls vfs_access, whose ENOENT and EACCES
# both land on that one line), so the symptom named the wrong problem entirely.
# The copy costs 78 MB and one second and makes the run reproducible.
RUNISO="$(dirname "$ISO")/procfs-run.iso"
RUNDISK="$(dirname "$DISK")/procfs-run.img"
cp "$ISO" "$RUNISO"
cp "$DISK" "$RUNDISK"
echo "iso  $(sha256sum "$RUNISO"  | cut -c1-12)"
echo "disk $(sha256sum "$RUNDISK" | cut -c1-12)"

NET="-netdev user,id=n0 -device e1000,netdev=n0"
# -snapshot: the mkdir of /proc at boot writes to the image, and this gate must
# be identical on its first run and its hundredth.
{ sleep 5; printf 'echo PROCFS-BEGIN\nls /proc\nps\ncat /proc/self/status\ncat /proc/self/maps\nfree\nuptime\ncat /proc/uptime\nsleep 3\ncat /proc/uptime\nuptime -d\ncat /proc/version\necho PROCFS-END\nexit\n'; sleep 14; } | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$RUNISO" \
    -drive file="$RUNDISK",format=raw,if=none,id=hd0,file.locking=off \
    -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
    -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
    $NET -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

for _ in $(seq 1 400); do
    grep -aq "PROCFS-END" "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done

# THE CARRIAGE RETURNS COME OFF FIRST, and this is not tidiness: tty_write in
# c/kernel/exec/file.c expands LF to CRLF for a serial terminal, so every line
# the guest printed ends "\r\n" and a `$`-anchored pattern matches NOTHING. The
# first run of this gate reported eight failures for output that was, in the
# log printed underneath them, exactly right -- which is precisely the shape
# CLAUDE.md's "suspect the apparatus first" section is about.
OUT="$LOG.txt"
tr -d '\r' < "$LOG" > "$OUT"

fail=0
say() { echo "  $1"; }
need() {   # need <description> <grep-pattern>
    if grep -aqE "$2" "$OUT"; then say "ok   $1"; else say "FAIL $1   (/$2/)"; fail=1; fi
}

echo "--- checks ---"
need "the sequence ran to the end"            'PROCFS-END'
need "the kernel mounted it"                  '\[fs\] /proc mounted'
need "ls /proc shows the machine-wide files"  '^meminfo|meminfo'
need "ls /proc shows self"                    'self'
need "ps printed its header"                  'PID +PPID S FDS CMD'
# The two rows the task asks for by name. `ps` sees the shell that forked it
# and it sees itself, and nothing else on this machine can produce those two
# strings in a ps-shaped row.
need "ps lists the console shell"             '^ *[0-9]+ +[0-9]+ [RZK] +[0-9]+ sh$'
need "ps lists itself"                        '^ *[0-9]+ +[0-9]+ [RZK] +[0-9]+ ps$'
need "/proc/self is the READER"               '^Name:	cat$'
need "status carries the capability bitmap"   '^Caps:	0x[0-9a-f]{16}$'
need "maps shows a mapped range"              '^[0-9a-f]{16}-[0-9a-f]{16} [-r][-w][-x] '
need "free read meminfo"                      '^Mem: +[0-9]+ +[0-9]+ +[0-9]+$'
need "uptime printed a clock"                 '^up [0-9]+:[0-9][0-9]:[0-9][0-9]$'
need "version came from the kernel"           'LogitOS version'

# LIVENESS THROUGH ONE HELD DESCRIPTOR, which is the strong form and the only
# one a ring-3 program can ask. `uptime -d` opens /proc/uptime, waits three
# seconds WITHOUT reading, then reads it beside a fresh open. Two `cat`s cannot
# ask this: they are two OPENS, so they differ even if every /proc file were
# rendered at open() and cached for the life of the fd -- which is exactly the
# implementation c/kernel/exec/file.c had for every other file and had to be
# taught not to use here (`live`, file.h).
need "a HELD descriptor reads live"           '^held=[0-9]+\.[0-9][0-9] fresh=[0-9]+\.[0-9][0-9] delta=[0-9]+\.[0-9][0-9] LIVE$'

# And the weak form, which is still worth having because it exercises the
# ordinary path a person uses: two bare `cat /proc/uptime` three seconds apart.
vals=$(grep -aoE '^[0-9]+\.[0-9][0-9]$' "$OUT")
nval=$(printf '%s\n' "$vals" | grep -c '.' )
nuniq=$(printf '%s\n' "$vals" | sort -u | grep -c '.')
say "uptime samples: $(printf '%s' "$vals" | tr '\n' ' ')"
if [ "$nval" -lt 2 ]; then
    say "FAIL two reads of /proc/uptime did not both arrive ($nval seen)"; fail=1
elif [ "$nuniq" -lt 2 ]; then
    say "FAIL LIVENESS: two reads of /proc/uptime three seconds apart are IDENTICAL"; fail=1
else
    say "ok   LIVENESS: two reads of /proc/uptime differ ($nuniq distinct values)"
fi

if [ "$fail" = 0 ]; then
    echo "PASS: /proc is mounted and ordinary programs read the process table out of it"
    exit 0
fi
echo "----- serial output -----"
cat "$OUT"
echo "-------------------------"
exit 1
