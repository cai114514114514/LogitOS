#!/usr/bin/env bash
# What survives pulling the plug -- with a journal?
#
# run-crash-test.sh asserts the floor (still mounts, bystanders intact, no block
# handed out twice) and accepts a lost or truncated victim, because LogitFS v3
# had no journal. LogitFS v4 has one, so this harness demands the stronger
# guarantee a write-ahead log exists to give:
#
#   the victim file is NEVER half-written. After a SIGKILL mid-write it is
#   either absent (the transaction never sealed) or byte-for-byte complete
#   (it did). "DURCHECK-FAIL length" / "first bad byte" = the log lied.
#
# plus the floor, unchanged: mounts, bystanders intact, no double allocation.
#
# Four crash rounds, because the kill can land in different phases of the write
# (data blocks in flight vs log install vs header clear), and each phase has to
# recover correctly. A random sub-second delay after CRASH-WRITE-ARMED varies
# which phase each round actually hits.

set -u

ISO="${1:?usage: run-fscrash-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-fscrash-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
DC=/usr/as/examples/durcheck.as
ROUNDS=4

WORK="$(mktemp -d)"
DISKC="$WORK/disk.img"
cp "$DISK" "$DISKC"
cleanup() { [ -n "${QPID:-}" ] && kill -9 "$QPID" 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

NET="-netdev user,id=n0 -device e1000,netdev=n0"

start_qemu() {   # $1 = what to type, $2 = log
    { sleep 5; printf '%s' "$1"; sleep 600; } | \
      "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
        -drive file="$DISKC",format=raw,if=none,id=hd0 -device virtio-blk-pci,drive=hd0 \
        -boot d -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
        $NET -serial stdio -display none -no-reboot >"$2" 2>/dev/null &
    QPID=$!
}

normlog() { tr -d '\r' <"$1" >"$1.n" && mv "$1.n" "$1"; }

# Never `wait` for a SIGKILLed background-pipeline member here: under WSL the
# SIGCHLD is never delivered and bash's do_wait wedges until some OTHER child
# dies (reproduced: kill -9 $! ; wait $! hangs past 8s). Kill, then poll for
# the process to vanish. kill -0 also succeeds on a zombie bash never reaped,
# so the poll is bounded and a stale zombie is tolerated -- it holds no open
# files and cannot touch the disk image the next boot is about to open.
reap() {
    [ -n "${QPID:-}" ] || return 0
    kill -9 "$QPID" 2>/dev/null
    for _ in $(seq 1 100); do
        kill -0 "$QPID" 2>/dev/null || break
        sleep 0.1
    done
    QPID=""
}

boot() {         # normal boot: run, settle, stop
    local cmds="$1" log="$2" settle="$3"
    start_qemu "$cmds" "$log"
    local waited=0
    while kill -0 "$QPID" 2>/dev/null && [ "$waited" -lt 150 ]; do
        grep -aq "BOOTMARK-DONE" "$log" 2>/dev/null && break
        sleep 1; waited=$((waited + 1))
    done
    sleep "$settle"
    reap
    normlog "$log"
}

fail() {
    echo "FAIL: $1"
    for f in "$WORK"/b*.log; do
        [ -f "$f" ] || continue
        echo "----- $(basename "$f") -----"
        grep -aE "DURCHECK|CRASH-WRITE|BOOTMARK|\[fs\]|panic|fault|corrupt|cannot" "$f" | tail -20
    done
    exit 1
}

verify_bystanders() {   # $1 = log, $2 = context
    grep -aq "DURCHECK-OK /dur/tiny.bin"  "$1" || fail "$2: /dur/tiny.bin no longer verifies"
    grep -aq "DURCHECK-OK /dur/small.bin" "$1" || fail "$2: /dur/small.bin no longer verifies"
    grep -aq "DURCHECK-OK /dur/mid.bin"   "$1" || fail "$2: /dur/mid.bin no longer verifies"
}

# The WAL contract for the victim: complete, or never happened. Anything else
# (a length mismatch, a bad byte) means a torn write reached disk.
verify_victim() {       # $1 = log, $2 = context
    if grep -aq "DURCHECK-FAIL /dur/victim.bin length" "$1"; then
        fail "$2: victim is TORN (wrong length) -- the journal allowed a half-written file"
    fi
    if grep -aq "DURCHECK-FAIL /dur/victim.bin first bad byte" "$1"; then
        fail "$2: victim is TORN (bad bytes) -- the journal allowed a half-written file"
    fi
    if grep -aq "DURCHECK-OK /dur/victim.bin" "$1"; then
        echo "  victim: complete (transaction sealed before the plug)"
    elif grep -aq "DURCHECK-FAIL /dur/victim.bin unreadable" "$1"; then
        echo "  victim: absent (transaction discarded whole)"
    else
        fail "$2: victim verify produced no verdict at all"
    fi
}

VERIFY="as $DC verify /dur/tiny.bin tiny
as $DC verify /dur/small.bin small
as $DC verify /dur/mid.bin mid
"

# ---- boot 1: lay down the bystanders, cleanly ---------------------------------
boot "mkdir /dur
as $DC write /dur/tiny.bin tiny
as $DC write /dur/small.bin small
as $DC write /dur/mid.bin mid
${VERIFY}echo BOOTMARK-DONE
" "$WORK/b1.log" 8
grep -aq "BOOTMARK-DONE" "$WORK/b1.log" || fail "boot 1 never finished"
grep -aq "\[fs\] mounted" "$WORK/b1.log" || fail "boot 1: filesystem did not mount"
verify_bystanders "$WORK/b1.log" "boot 1"

replays=0
for round in $(seq 1 "$ROUNDS"); do
    echo "== crash round $round/$ROUNDS =="
    cb="$WORK/bc$round.log"
    vb="$WORK/bv$round.log"

    # ---- pull the plug mid-write ---------------------------------------------
    start_qemu "as $DC crashwrite /dur/victim.bin
" "$cb"
    armed=0
    for _ in $(seq 1 900); do
        if grep -aq "CRASH-WRITE-ARMED" "$cb" 2>/dev/null; then armed=1; break; fi
        kill -0 "$QPID" 2>/dev/null || break
        sleep 0.1
    done
    [ "$armed" = 1 ] || { kill -9 "$QPID" 2>/dev/null; fail "round $round: the victim write never started"; }
    sleep "0.$((RANDOM % 6))"            # vary which phase of the write dies
    reap                                 # SIGKILL, no flush: this is the power cut
    normlog "$cb"

    # ---- reboot: mount, replay if needed, assert the contract -----------------
    boot "${VERIFY}as $DC verify /dur/victim.bin big
as $DC write /dur/new$round.bin small
${VERIFY}echo BOOTMARK-DONE
" "$vb" 10
    grep -aq "BOOTMARK-DONE" "$vb" || fail "round $round: never finished -- the filesystem may not have mounted after the crash"
    grep -aq "\[fs\] mounted" "$vb" || fail "round $round: filesystem did not mount after the crash"
    if grep -aq "\[fs\] log: replayed" "$vb"; then
        replays=$((replays + 1))
        echo "  log replay: yes (an interrupted transaction was finished at mount)"
    fi
    verify_bystanders "$vb" "round $round"
    verify_victim "$vb" "round $round"
    # The verify block runs twice; the second pass is after new$round.bin forced
    # allocation -- a block handed to two files shows up on the OLD file.
    [ "$(grep -ac 'DURCHECK-OK /dur/mid.bin' "$vb")" -ge 2 ] \
        || fail "round $round: /dur/mid.bin stopped verifying once new files were allocated -- a block was handed out twice"
done

echo "PASS: $ROUNDS SIGKILLs mid-write -- mounted every time, every completed file"
echo "      byte-for-byte intact, victim always whole-or-absent, no double allocation"
echo "      (log replays witnessed: $replays/$ROUNDS rounds)"
