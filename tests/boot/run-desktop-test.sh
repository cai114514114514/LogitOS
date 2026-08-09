#!/usr/bin/env bash
# DOES THE DESKTOP KNOW WHO IS USING IT, ON THE REAL MACHINE, ACROSS REBOOTS?
#
# Two claims, and neither can be made anywhere but here.
#
#   1. THE DESKTOP DOES NOT COME UP UNTIL SOMEBODY AUTHENTICATES. Until this
#      landed, /bin/login authenticated the SERIAL CONSOLE while the window
#      manager launched the file manager before init ran at all -- so the
#      previous user's home was on screen 2.7 seconds after power-on with
#      nothing having asked anybody anything. The assertion is an ORDERING on
#      one boot's serial log: "[wm] launched Finder" must not appear before a
#      login succeeded. A host test cannot hold an ordering between a kernel
#      thread and a ring-3 authentication.
#
#   2. A USER'S SETTINGS ARE THEIRS AND THEY SURVIVE A REBOOT. settings_commit()
#      writes through the CALLING process's credential, so while the store was
#      root:root 0600 /etc/settings.conf a logged-in user flipping dark mode
#      was refused and the choice was gone at the next boot. The fix is a
#      per-user store over read-only system defaults, and "it survives" is a
#      claim about a POWER CYCLE. One boot proves nothing: a value can live in
#      a buffer nobody flushed.
#
# NO -snapshot. FOUR real boots against ONE image (a copy, so the build
# artifact is never touched), and the fourth is a SECOND USER -- "per-user" is
# a claim about two people and cannot be tested with one.
#
#   boot 1  no accounts: the machine must behave EXACTLY as before -- no lock,
#           the Finder launches, a root shell. Set a SYSTEM default as root,
#           then enrol alice and bob by TYPING passwords.
#   boot 2  locked. The greeter runs and nothing else does. A wrong password on
#           the console is refused; the right one unlocks the desktop, and only
#           then does the Finder start. alice sees the system default, sets her
#           own value, and it lands in a file that is hers and 0600.
#   boot 3  THE ONE THAT MATTERS: alice's value is still there, it is still in
#           HER file, and it never went into root's.
#   boot 4  bob. He sees the system default and NOT alice's, and he cannot read
#           her store at all.
#
# THE PASSWORDS IN THIS FILE ARE TEST PASSWORDS FOR A DISPOSABLE IMAGE, typed
# at a prompt on a scratch copy of the disk that is deleted when the harness
# exits. Nothing is written into build/disk.img and nothing is shipped: the
# image this boots from has no /etc/passwd until boot 1 makes one.

set -u

ISO="${1:?usage: run-desktop-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-desktop-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"

APW='alice-correct-horse-battery'
BPW='bob-a-different-one-entirely'
BADPW='alice-correct-horse-batter'
ACCENT='0x112233'
SC=/usr/as/examples/setcheck.as

WORK="$(mktemp -d)"
DISKC="$WORK/disk.img"
cp "$DISK" "$DISKC"
cleanup() { [ -n "${QPID:-}" ] && kill -9 "$QPID" 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

NET="-netdev user,id=n0 -device e1000,netdev=n0"

# Never `wait` for a SIGKILLed background-pipeline member: under WSL the
# SIGCHLD is never delivered and bash's do_wait wedges until some other child
# dies. Kill, then poll for the process to vanish. (From run-login-test.sh --
# same environment, same trap.)
reap() {
    [ -n "${QPID:-}" ] || return 0
    kill -9 "$QPID" 2>/dev/null
    for _ in $(seq 1 100); do kill -0 "$QPID" 2>/dev/null || break; sleep 0.1; done
    QPID=""
}

# WAIT FOR THE THING, never sleep at it -- tests/boot/bootwait.sh's rule, and
# eleven harnesses were converted to it after one reported "boot 1 never
# finished its commands" when nobody had typed anything. The COUNT matters:
# "LOGIN:" is printed once per attempt, so waiting for its mere presence
# returns instantly on the second attempt and types into the first prompt.
LOG=""
waitn() {
    local pat="$1" want="$2" secs="${3:-120}" i=0 n
    while [ "$i" -lt $((secs * 10)) ]; do
        # `grep -c` prints its count AND exits 1 when the count is zero, so a
        # `|| echo 0` fallback yields "0\n0" and `[` refuses it as a non-integer.
        n=$(grep -ac -- "$pat" "$LOG" 2>/dev/null)
        [ -n "$n" ] || n=0
        if [ "$n" -ge "$want" ]; then sleep 0.4; return 0; fi
        sleep 0.1; i=$((i + 1))
    done
    echo "WARNING: '$pat' did not reach $want occurrences in ${secs}s -- anything" >&2
    echo "         typed after this went into nothing, so a later failure is a" >&2
    echo "         BOOT or LOGIN failure and not a failure of what is asserted." >&2
    return 1
}
say() { printf '%s\n' "$1"; sleep 0.35; }

fail() {
    echo "FAIL: $1"
    for f in "$WORK"/b*.log; do
        [ -f "$f" ] || continue
        echo "----- $(basename "$f") -----"
        # `Password:` is excluded for the reason login.c states: the kernel tty
        # echoes each byte before login can overwrite it with '*', so a
        # RECORDING of the wire holds the typed characters. That is a real
        # property of a console with no termios and there is no reason to
        # reprint it into CI output on every failure.
        grep -aE "LOGIN|GREETER|ENROLLED|SETCHECK|SETTINGS_USER|STAT |\[wm\] |BOOT[0-9]-DONE|panic|fault|refus" "$f" \
            | grep -av "Password:" | tail -40
    done
    echo "  (full logs were in $WORK, now removed; re-run to inspect)"
    exit 1
}

# One boot against the persistent copy. The MARKER is why this is not a timer:
# this machine never powers itself off, so QEMU runs until it is killed, and a
# fixed deadline costs every boot the full deadline whether it finished in 40
# seconds or not -- and runs the slowest boot one command short of its marker.
BOOT_MAX="${BOOT_MAX:-420}"
# Cores for this boot. Four everywhere except the ENROLMENT boot -- see the
# comment above drive1 for the deadlock that forces it and for why it is
# scaffolding rather than a weakened assertion.
SMP=4
boot() {
    local driver="$1" done_marker="$3" settle="$4"
    LOG="$2"
    : > "$LOG"
    { "$driver"; sleep "$settle"; } | \
      "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
        -drive file="$DISKC",format=raw,if=none,id=hd0 -device virtio-blk-pci,drive=hd0 \
        -boot d -m 512M -smp "$SMP" -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
        $NET -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
    QPID=$!
    local waited=0
    while kill -0 "$QPID" 2>/dev/null && [ "$waited" -lt "$BOOT_MAX" ]; do
        if grep -aq "$done_marker" "$LOG" 2>/dev/null; then sleep "$settle"; break; fi
        sleep 1; waited=$((waited + 1))
    done
    [ "$waited" -ge "$BOOT_MAX" ] &&
        echo "WARNING: no '$done_marker' after ${BOOT_MAX}s -- the boot itself is the suspect" >&2
    reap
    tr -d '\r' <"$LOG" >"$LOG.n" && mv "$LOG.n" "$LOG"     # serial is CRLF
}

# The line number of the FIRST match, or 0. Used for the ordering assertion,
# which is the only form in which "the desktop waited" can be stated.
firstline() { grep -an -- "$2" "$1" 2>/dev/null | head -1 | cut -d: -f1; }

# ---------------------------------------------------------------- boot 1 ----
# THE ONE BOOT THAT DOES NOT RUN ON FOUR CORES, and the reason is a bug that
# was here before any of this work -- stated in full because a harness that
# quietly lowers -smp is a harness nobody can trust.
#
# A SECOND `login -a` IN ONE BOOT FREEZES THE WHOLE MACHINE UNDER -smp 4.
# Not the program: the machine. The serial log stops mid-line and nothing --
# no kernel timer line, no compositor report, no echo of a typed character --
# ever comes out again. It is not deterministic in WHERE it stops (once at the
# second enrolment's "New password: " prompt, once before the prompt was
# printed, once after "hashing (120000"), which is the signature of a global
# stall rather than of a program going wrong.
#
# MEASURED, and measured against a kernel WITHOUT this line's changes, which
# is the part that matters: 0 of 5 runs completed at -smp 4 (and 0 of 2 on
# c/kernel at 378b46692, before the greeter or the per-user store existed);
# 3 of 3 completed at -smp 1. So it is an SMP deadlock in the existing kernel,
# reproduced by two enrolments, and it is being REPORTED, not worked around
# silently and not fixed here -- it is somewhere in the BKL/exec/tty
# interaction and belongs to whoever owns that, with this as the reproducer.
#
# Enrolling two accounts is FIXTURE, not assertion. Boots 2, 3 and 4 -- which
# carry every claim this harness makes -- all run on four cores.
drive1() {
    waitn "LogitOS shell" 1 150
    say "login -i"
    # A SYSTEM default, written as root into /etc/settings.conf. Everything
    # later that says "the defaults still reach this user" is about this value.
    say "as $SC set ui.accent $ACCENT"
    say "as $SC check ui.accent $ACCENT"
    say "login -a alice"
    waitn "New password:" 1 30
    say "$APW"
    waitn "Retype password:" 1 30
    say "$APW"
    waitn "ENROLLED alice" 1 90
    # WAIT OUT THE KERNEL'S BENCHMARK DUMP before the second enrolment. It is
    # forty lines in one burst, and the kernel's kprintf and a ring-3 write()
    # share the serial port with no lock -- so a program prompting in the middle
    # of it has its prompt shredded, and this harness's `waitn` would then be
    # waiting for a string that was printed and destroyed. Not flakiness
    # management: it is the same unsynchronised-device property login.c
    # documents, handled by not sampling the device while it is busy.
    waitn "KBENCH_DONE" 1 200 || true
    say "login -a bob"
    waitn "New password:" 2 30
    say "$BPW"
    waitn "Retype password:" 2 30
    say "$BPW"
    waitn "ENROLLED bob" 1 90
    say "echo BOOT1-DONE"
}
SMP=1
boot drive1 "$WORK/b1.log" BOOT1-DONE 8
SMP=4

grep -aq "BOOT1-DONE" "$WORK/b1.log" || fail "boot 1 never finished its commands"
# THE UNCHANGED-MACHINE PROMISE. A machine with no accounts has nobody to
# authenticate, so it must behave exactly as it did before any of this -- which
# is what keeps the sixty other boot harnesses in this tree working.
grep -aq "\[wm\] no accounts on this machine" "$WORK/b1.log" ||
    fail "boot 1: a machine with NO ACCOUNTS did not come up unauthenticated"
grep -aq "\[wm\] launched Finder" "$WORK/b1.log" ||
    fail "boot 1: the desktop did not start on a machine with no accounts. Every
      other harness in this tree expects it to"
grep -aq "\[wm\] LOCKED" "$WORK/b1.log" &&
    fail "boot 1: the machine locked itself with no accounts to unlock it with"
grep -aq "ID uid=0 gid=0 session=0 store=absent" "$WORK/b1.log" ||
    fail "boot 1: the freshly built image already has an /etc/passwd. Something
      shipped a credential"
grep -aq "SETCHECK-OK ui.accent = $ACCENT" "$WORK/b1.log" ||
    fail "boot 1: root could not write a system default"
grep -aq "ENROLLED alice uid=1000 gid=1000 home=/home/alice" "$WORK/b1.log" ||
    fail "boot 1: alice was not enrolled"
grep -aq "ENROLLED bob uid=1001 gid=1001 home=/home/bob" "$WORK/b1.log" ||
    fail "boot 1: bob was not enrolled"

# ---------------------------------------------------------------- boot 2 ----
drive2() {
    waitn "LOGIN:" 1 150
    say "alice"
    waitn "Password:" 1 30
    say "$BADPW"
    waitn "LOGIN-DENIED" 1 90 || true
    waitn "LOGIN:" 2 60
    say "alice"
    waitn "Password:" 2 30
    say "$APW"
    waitn "LogitOS shell" 1 120
    say "as $SC check ui.accent $ACCENT"
    say "as $SC set ui.dark 1"
    say "as $SC check ui.dark 1"
    say "cat /home/alice/.config/settings.conf"
    say "stat /home/alice/.config/settings.conf"
    say "stat /home/alice/.config/settings.conf"
    say "login -i"
    say "echo BOOT2-DONE"
}
boot drive2 "$WORK/b2.log" BOOT2-DONE 8

grep -aq "BOOT2-DONE" "$WORK/b2.log" || fail "boot 2 never finished its commands"

# --- CLAIM 1: THE DESKTOP WAITED ------------------------------------------
grep -aq "\[wm\] LOCKED: /etc/passwd exists" "$WORK/b2.log" ||
    fail "boot 2: the machine has accounts and did NOT come up locked"
grep -aq "GREETER-READY accounts=2" "$WORK/b2.log" ||
    fail "boot 2: the greeter did not start, or could not read the account store"
grep -aq "LOGIN-DENIED" "$WORK/b2.log" ||
    fail "boot 2: A WRONG PASSWORD WAS NOT REFUSED. That is the negative
      control's behaviour appearing in the real build"
grep -aq "LOGIN-OK alice uid=1000 gid=1000" "$WORK/b2.log" ||
    fail "boot 2: the right password did not produce a session as uid 1000"
grep -aq "\[wm\] UNLOCKED by session uid=1000" "$WORK/b2.log" ||
    fail "boot 2: authenticating did not unlock the desktop"

# THE ORDERING, which is the whole of claim 1. Asserted on line numbers rather
# than on presence: both lines are in the log either way, and the only thing
# that distinguishes a desktop that waited from one that did not is WHICH CAME
# FIRST. Also assert the launcher refused, which is the mechanism rather than
# the appearance -- if the refusal is what stopped it, the log says so.
FIN=$(firstline "$WORK/b2.log" "\[wm\] launched Finder"); FIN=${FIN:-0}
UNL=$(firstline "$WORK/b2.log" "\[wm\] UNLOCKED"); UNL=${UNL:-0}
[ "$FIN" -gt 0 ] || fail "boot 2: the Finder never started, even after the login"
[ "$UNL" -gt 0 ] && [ "$FIN" -gt "$UNL" ] ||
    fail "boot 2: THE FINDER STARTED BEFORE ANYBODY AUTHENTICATED (line $FIN,
      unlock at line $UNL). This is the state this whole change exists to end"

# --- CLAIM 2: THE SETTINGS ARE HERS ---------------------------------------
grep -aq "SETTINGS_USER uid=1000 store=/home/alice/.config/settings.conf" "$WORK/b2.log" ||
    fail "boot 2: the settings store was not switched to alice's home"
grep -aq "SETCHECK-OK ui.accent = $ACCENT" "$WORK/b2.log" ||
    fail "boot 2: the SYSTEM default did not reach a logged-in user. The system
      store is supposed to supply defaults under the user's own file"
grep -aq "SETCHECK-SET ui.dark = 1" "$WORK/b2.log" ||
    fail "boot 2: A USER COULD NOT SAVE A SETTING. This is the original
      regression: settings_commit() refused because the file was root's"
grep -aq "^ui.dark = 1" "$WORK/b2.log" ||
    fail "boot 2: her value is not in /home/alice/.config/settings.conf"
grep -aq "^ui.accent" "$WORK/b2.log" &&
    fail "boot 2: her file froze a system DEFAULT into her account. A later
      change to /etc/settings.conf would then never reach her"
# The mode and owner are the reason she can write it a SECOND time -- a file
# created by a non-root process lands root:root on this filesystem, and that
# store would take exactly one save. Read twice: the kernel and a ring-3
# write() share the serial port with no lock, so one clean line is all it takes
# and repeating a read-only command is the only way to sample the device.
grep -aq "STAT /home/alice/.config/settings.conf mode=600 type=reg uid=1000 gid=1000 " "$WORK/b2.log" ||
    fail "boot 2: her settings file is not hers and 0600 -- so the NEXT save
      would be refused, and a store that can be written once is not a store"
grep -aq "ID uid=1000 gid=1000 session=1000 store=present" "$WORK/b2.log" ||
    fail "boot 2: the session was not established as uid 1000"

# ---------------------------------------------------------------- boot 3 ----
# The claim a single boot cannot make.
drive3() {
    waitn "LOGIN:" 1 150
    say "alice"
    waitn "Password:" 1 30
    say "$APW"
    waitn "LogitOS shell" 1 120
    say "as $SC check ui.dark 1"
    say "as $SC check ui.accent $ACCENT"
    say "cat /etc/settings.conf"
    say "stat /home/alice/.config/settings.conf"
    say "stat /home/alice/.config/settings.conf"
    say "echo BOOT3-DONE"
}
boot drive3 "$WORK/b3.log" BOOT3-DONE 8

grep -aq "BOOT3-DONE" "$WORK/b3.log" || fail "boot 3 never finished its commands"
grep -aq "LOGIN-OK alice uid=1000" "$WORK/b3.log" ||
    fail "boot 3: alice no longer authenticates"
grep -aq "SETCHECK-OK ui.dark = 1" "$WORK/b3.log" ||
    fail "boot 3: HER DARK-MODE CHOICE DID NOT SURVIVE THE REBOOT. That is the
      regression, or a build that writes the user's file and reads the system
      one -- which is what the negative control does on purpose"
grep -aq "SETCHECK-OK ui.accent = $ACCENT" "$WORK/b3.log" ||
    fail "boot 3: the system default stopped reaching her after a reboot"
# HER CHOICE IS NOT IN ROOT'S FILE. `cat /etc/settings.conf` is the only thing
# in this boot that can emit a line STARTING with the raw key, so the anchor is
# what makes this assertion mean the file and not the SETCHECK-OK line above.
grep -aq "^ui.dark = 1" "$WORK/b3.log" &&
    fail "boot 3: her setting was written into the SYSTEM store. It would then
      be every user's setting, and root would own the user's preferences"
grep -aq "STAT /home/alice/.config/settings.conf mode=600 type=reg uid=1000 gid=1000 " "$WORK/b3.log" ||
    fail "boot 3: her store lost its mode or owner across the reboot"

# ---------------------------------------------------------------- boot 4 ----
# A SECOND USER, which is the only way "per-user" can be tested at all.
drive4() {
    waitn "LOGIN:" 1 150
    say "bob"
    waitn "Password:" 1 30
    say "$BPW"
    waitn "LogitOS shell" 1 120
    say "login -i"
    say "as $SC check ui.dark 0"
    say "as $SC check ui.accent $ACCENT"
    say "cat /home/alice/.config/settings.conf"
    say "cat /home/alice/.config/settings.conf"
    say "as $SC set ui.dark 1"
    say "echo BOOT4-DONE"
}
boot drive4 "$WORK/b4.log" BOOT4-DONE 8

grep -aq "BOOT4-DONE" "$WORK/b4.log" || fail "boot 4 never finished its commands"
grep -aq "LOGIN-OK bob uid=1001 gid=1001" "$WORK/b4.log" ||
    fail "boot 4: bob does not authenticate"
grep -aq "SETTINGS_USER uid=1001 store=/home/bob/.config/settings.conf" "$WORK/b4.log" ||
    fail "boot 4: bob's settings store is not his own home"
grep -aq "SETCHECK-OK ui.dark = 0" "$WORK/b4.log" ||
    fail "boot 4: BOB INHERITED ALICE'S DARK MODE. The stores are not per-user"
grep -aq "SETCHECK-OK ui.accent = $ACCENT" "$WORK/b4.log" ||
    fail "boot 4: the system default did not reach the second user"
# Asserted by the ABSENCE OF THE EFFECT rather than by an error string: an
# error message is a thing a program prints, and the claim is about what the
# filesystem did.
grep -aq "^ui.dark = 1" "$WORK/b4.log" &&
    fail "REFUSAL BROKEN: bob read the contents of alice's settings store"
grep -aq "SETCHECK-SET ui.dark = 1" "$WORK/b4.log" ||
    fail "boot 4: bob could not save a setting of his own -- the refusal above
      would then prove nothing except that everything is broken"

echo "PASS: with accounts on the disk the desktop came up LOCKED and the Finder"
echo "      started only after a login (boot 2, line $FIN vs unlock $UNL); a wrong"
echo "      password was refused; alice's dark mode was written to her own 0600"
echo "      store, was still there after a reboot, and was never in root's file;"
echo "      the system default reached both users; and bob saw neither alice's"
echo "      setting nor her file. A machine with no accounts (boot 1) behaved"
echo "      exactly as it did before any of this existed."
