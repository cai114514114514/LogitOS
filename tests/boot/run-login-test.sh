#!/usr/bin/env bash
# DOES THIS MACHINE HAVE A CONCEPT OF A PERSON, ON THE REAL MACHINE, ACROSS
# REBOOTS?
#
# The host suite (make test-login) proves the store parses, that a wrong
# password is refused, and that vmeta_permission() refuses a non-root uid the
# things it should. Every bit of that runs in one process against RAM. It
# cannot see the three things that actually matter here:
#
#   - that /etc/passwd is STILL root:root 0600 after a power cut, which is the
#     entire reason a file is allowed to be the account store at all (see
#     c/apps/coreutils/accounts.h; before logitfs put modes on the medium, a
#     0600 file was 0644 again on the next boot);
#   - that the shell the KERNEL spawns is not root -- the host test can assert
#     what a credential of 1000 is refused, not that anything ever has one;
#   - that a refusal reaches a user AS A REFUSAL rather than as a truncated
#     file or a silent success.
#
# NO -snapshot. That is the point, and CLAUDE.md warns specifically against
# adding it to a harness to work around a write. FOUR real boots against ONE
# image (a copy, so the build artifact is never touched):
#
#   boot 1  no accounts. The machine must behave EXACTLY as it did before this
#           work: /bin/login says there is nobody to authenticate and hands
#           over a root shell. Then enrol `alice`, by TYPING a password -- no
#           credential is ever shipped in the image, and this harness typing
#           one is the human doing it.
#   boot 2  the store exists, so init's login authenticates. A wrong password
#           first (it must be REFUSED), then the right one, then the refusals:
#           alice cannot read /etc/passwd, cannot read a root-owned file,
#           cannot write to /, cannot enrol a second account -- and CAN write
#           in her own home.
#   boot 3  churn, then log in again. A store that survives a quiet reboot but
#           not an allocating one is a real failure mode.
#   boot 4  once more, and check the mode and owner one final time.
#
# THE PASSWORD IN THIS FILE IS A TEST PASSWORD FOR A DISPOSABLE IMAGE. It is
# typed at a prompt on a scratch copy of the disk that is deleted when the
# harness exits. Nothing here is written into build/disk.img and nothing is
# shipped: the image this boots from has no /etc/passwd until boot 1 makes one.

set -u

ISO="${1:?usage: run-login-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-login-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"

PW='hunter2-correct-horse'
BADPW='hunter2-correct-hors'

WORK="$(mktemp -d)"
DISKC="$WORK/disk.img"
cp "$DISK" "$DISKC"
cleanup() { [ -n "${QPID:-}" ] && kill -9 "$QPID" 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

NET="-netdev user,id=n0 -device e1000,netdev=n0"

# Never `wait` for a SIGKILLed background-pipeline member: under WSL the
# SIGCHLD is never delivered and bash's do_wait wedges until some other child
# dies. Kill, then poll for the process to vanish.
reap() {
    [ -n "${QPID:-}" ] || return 0
    kill -9 "$QPID" 2>/dev/null
    for _ in $(seq 1 100); do kill -0 "$QPID" 2>/dev/null || break; sleep 0.1; done
    QPID=""
}

# WAIT FOR THE THING, never sleep at it. Two forms, and the count matters:
# "LOGIN:" is printed once per attempt, so waiting for its mere presence would
# return instantly on the second attempt and type the password into the first
# prompt. `waitn <pattern> <n>` waits until it has appeared n times.
LOG=""
waitn() {
    local pat="$1" want="$2" secs="${3:-120}" i=0
    while [ "$i" -lt $((secs * 10)) ]; do
        if [ "$(grep -ac -- "$pat" "$LOG" 2>/dev/null || echo 0)" -ge "$want" ]; then
            sleep 0.4     # one scheduling quantum, not a guess at the boot time
            return 0
        fi
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
        grep -aE "LOGIN|ENROLLED|STAT |BOOT[0-9]-DONE|REFUS|panic|fault|denied|login:" "$f" | tail -30
    done
    echo "  (full logs were in $WORK, now removed; re-run to inspect)"
    exit 1
}

# One boot against the persistent copy. $1 = a function that types, $2 = log,
# $3 = settle seconds after the typing finishes.
boot() {
    local driver="$1" settle="$3"
    LOG="$2"
    : > "$LOG"
    { "$driver"; sleep "$settle"; } | \
      "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
        -drive file="$DISKC",format=raw,if=none,id=hd0 -device virtio-blk-pci,drive=hd0 \
        -boot d -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
        $NET -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
    QPID=$!
    local waited=0
    while kill -0 "$QPID" 2>/dev/null && [ "$waited" -lt 260 ]; do sleep 1; waited=$((waited + 1)); done
    reap
    tr -d '\r' <"$LOG" >"$LOG.n" && mv "$LOG.n" "$LOG"     # serial is CRLF
}

# ---------------------------------------------------------------- boot 1 ----
# No accounts: the machine must reach a root shell exactly as it did before
# /bin/login existed. Then enrol, by typing.
drive1() {
    waitn "LogitOS shell" 1 150
    say "echo topsecret > /rootonly.txt"
    say "stat -o 0:0 /rootonly.txt"
    say "stat -c 600 /rootonly.txt"
    say "login -a alice"
    waitn "New password:" 1 30
    say "$PW"
    waitn "Retype password:" 1 30
    say "$PW"
    waitn "ENROLLED" 1 60
    say "stat /etc/passwd /home/alice /rootonly.txt"
    say "echo BOOT1-DONE"
    sleep 3
}
boot drive1 "$WORK/b1.log" 10

grep -aq "BOOT1-DONE" "$WORK/b1.log" || fail "boot 1 never finished its commands"
grep -aq "login: no accounts on this machine" "$WORK/b1.log" ||
    fail "boot 1: an image with no /etc/passwd did NOT say so -- either login is not
      in the boot path, or something shipped a credential in the image"
grep -aq "ENROLLED alice uid=1000 gid=1000 home=/home/alice" "$WORK/b1.log" ||
    fail "boot 1: enrolment did not complete"
# The mode is the whole reason a file may be the account store.
grep -aq "STAT /etc/passwd mode=600 type=reg uid=0 gid=0 " "$WORK/b1.log" ||
    fail "boot 1: /etc/passwd is not root:root 0600 even within the boot"
grep -aq "STAT /home/alice mode=700 type=dir uid=1000 gid=1000 " "$WORK/b1.log" ||
    fail "boot 1: /home/alice is not owned 0700 by the new account"

# ---------------------------------------------------------------- boot 2 ----
# The store exists, so init's /bin/login authenticates. This is the boot that
# carries every claim.
drive2() {
    waitn "LOGIN:" 1 150
    say "alice"
    waitn "Password:" 1 30
    say "$BADPW"
    waitn "LOGIN-DENIED" 1 90 || true
    waitn "LOGIN:" 2 60
    say "alice"
    waitn "Password:" 2 30
    say "$PW"
    waitn "LogitOS shell" 1 90

    # --- what the machine must now REFUSE ---------------------------------
    say "cat /etc/passwd"
    say "cat /rootonly.txt"
    say "echo pwned > /rootonly.txt"
    say "echo pwned > /newfile-in-root.txt"
    say "login -a mallory"
    # --- and what it must still ALLOW --------------------------------------
    say "echo minefile > /home/alice/mine.txt"
    say "cat /home/alice/mine.txt"
    say "login -i"
    say "stat /etc/passwd /rootonly.txt"
    say "echo BOOT2-DONE"
    sleep 4
}
boot drive2 "$WORK/b2.log" 12

grep -aq "BOOT2-DONE" "$WORK/b2.log" || fail "boot 2 never finished its commands"

# The store survived the reboot at all -- the assertion the RAM metadata store
# this replaced could never have passed.
grep -aq "LOGIN:" "$WORK/b2.log" ||
    fail "boot 2: no login prompt. /etc/passwd did not survive the reboot, or init
      is still spawning /bin/sh directly"
grep -aq "LOGIN-DENIED" "$WORK/b2.log" ||
    fail "boot 2: A WRONG PASSWORD WAS NOT REFUSED. This is the negative control's
      behaviour appearing in the real build"
grep -aq "LOGIN-OK alice uid=1000 gid=1000" "$WORK/b2.log" ||
    fail "boot 2: the right password did not produce a session as uid 1000"
grep -aq "LogitOS shell" "$WORK/b2.log" ||
    fail "boot 2: login authenticated but never reached a shell"

# THE REFUSALS. Asserted by the ABSENCE OF THE EFFECT, not by an error string:
# an error message is a thing a program prints, and the claim is about what the
# filesystem did.
grep -aq '\$pbkdf2-sha256\$' "$WORK/b2.log" &&
    fail "REFUSAL BROKEN: alice read the password hash out of /etc/passwd"
grep -aq "topsecret" "$WORK/b2.log" &&
    fail "REFUSAL BROKEN: alice read a root-owned 0600 file"
grep -aq "ENROLLED mallory" "$WORK/b2.log" &&
    fail "REFUSAL BROKEN: a non-root user enrolled an account"
grep -aq "login: only root may add an account" "$WORK/b2.log" ||
    fail 'boot 2: `login -a` as alice did not report that it was refused'
# The root-owned file must be UNCHANGED, not merely unreadable -- and the SIZE
# is what says so. A refused write and a write that silently succeeded leave
# identical mode and owner; only the length distinguishes "topsecret\n" (10)
# from the "pwned\n" (6) alice tried to put there.
grep -aq "STAT /rootonly.txt mode=600 type=reg uid=0 gid=0 .* size=10 " "$WORK/b2.log" ||
    fail "REFUSAL BROKEN: /rootonly.txt is no longer root:root 0600 holding its
      original 10 bytes -- alice's write went through"
grep -aq "STAT /etc/passwd mode=600 type=reg uid=0 gid=0 " "$WORK/b2.log" ||
    fail "boot 2: /etc/passwd lost its mode or owner across the reboot"

# ...and what a user must still be able to do. An identity that refuses
# everything is not a user account, it is a broken machine.
grep -aq "^minefile" "$WORK/b2.log" ||
    fail "boot 2: alice could not write and read back a file in her OWN home --
      the refusals above would then prove nothing but that everything is broken"
grep -aq "ID uid=1000 gid=1000 session=1000" "$WORK/b2.log" ||
    fail "boot 2: the session was not established as uid 1000"

# ---------------------------------------------------------------- boot 3 ----
# Churn between the reboots. A store that survives a quiet reboot but not an
# allocating one is a real failure mode and it does not show on the churned
# files -- it shows on /etc/passwd.
drive3() {
    waitn "LOGIN:" 1 150
    say "alice"
    waitn "Password:" 1 30
    say "$PW"
    waitn "LogitOS shell" 1 90
    say "echo c1 > /home/alice/c1"
    say "echo c2 > /home/alice/c2"
    say "rm /home/alice/c1"
    say "echo c3 > /home/alice/c3"
    say "rm /home/alice/c2"
    say "cat /home/alice/mine.txt"
    say "cat /etc/passwd"
    say "stat /newfile-in-root.txt"
    say "stat /etc/passwd /home/alice"
    say "echo BOOT3-DONE"
    sleep 4
}
boot drive3 "$WORK/b3.log" 12

grep -aq "BOOT3-DONE" "$WORK/b3.log" || fail "boot 3 never finished its commands"
grep -aq "LOGIN-OK alice uid=1000 gid=1000" "$WORK/b3.log" ||
    fail "boot 3: the account no longer authenticates after a round of churn"
grep -aq "^minefile" "$WORK/b3.log" ||
    fail "boot 3: alice's own file did not survive the reboot"
grep -aq '\$pbkdf2-sha256\$' "$WORK/b3.log" &&
    fail "REFUSAL BROKEN (boot 3): the hash became readable after churn"
grep -aq "STAT /etc/passwd mode=600 type=reg uid=0 gid=0 " "$WORK/b3.log" ||
    fail "boot 3: /etc/passwd lost its mode after allocate/free churn"
grep -aq "STAT /home/alice mode=700 type=dir uid=1000 gid=1000 " "$WORK/b3.log" ||
    fail "boot 3: /home/alice lost its owner after churn"
# The file alice tried to create in / on boot 2. Its ABSENCE a reboot later is
# the durable form of the refusal: a create that was refused and a create that
# succeeded into a buffer nobody flushed look the same within one boot.
grep -aq "stat: cannot stat /newfile-in-root.txt" "$WORK/b3.log" ||
    fail "REFUSAL BROKEN: alice's file in / exists a reboot later, so the write
      into a root-owned 0755 directory was not refused"

# ---------------------------------------------------------------- boot 4 ----
drive4() {
    waitn "LOGIN:" 1 150
    say "alice"
    waitn "Password:" 1 30
    say "$PW"
    waitn "LogitOS shell" 1 90
    say "cat /etc/passwd"
    say "stat /etc/passwd /home/alice /rootonly.txt"
    say "login -i"
    say "echo BOOT4-DONE"
    sleep 4
}
boot drive4 "$WORK/b4.log" 12

grep -aq "BOOT4-DONE" "$WORK/b4.log" || fail "boot 4 never finished its commands"
grep -aq "LOGIN-OK alice uid=1000 gid=1000" "$WORK/b4.log" ||
    fail "boot 4: the account stopped authenticating after two more reboots"
grep -aq '\$pbkdf2-sha256\$' "$WORK/b4.log" &&
    fail "REFUSAL BROKEN (boot 4): the hash became readable"
grep -aq "STAT /etc/passwd mode=600 type=reg uid=0 gid=0 " "$WORK/b4.log" ||
    fail "boot 4: /etc/passwd finally lost its mode"
grep -aq "STAT /rootonly.txt mode=600 type=reg uid=0 gid=0 .* size=10 " "$WORK/b4.log" ||
    fail "boot 4: the root-owned file is no longer intact after three reboots"
grep -aq "ID uid=1000 gid=1000 session=1000" "$WORK/b4.log" ||
    fail "boot 4: the session is no longer uid 1000"

echo "PASS: an account enrolled on boot 1 authenticated on boots 2, 3 and 4 across"
echo "      four real reboots with churn between them; /etc/passwd is still"
echo "      root:root 0600 on the medium; and as uid 1000 the machine refused"
echo "      the hash, a root-owned file, a write to a root-owned file and an"
echo "      enrolment -- while still allowing everything inside /home/alice."
