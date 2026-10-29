#!/usr/bin/env bash
# tcc ON THE MACHINE, and a bare ELF through the kernel's loader.
#
# Four things, each with its own line on the serial log:
#   1. /bin/tcc -vv     tcc.aex starts under mini-libc (crt0, argv, stdio,
#                       exit -- the whole startup path) and prints its version
#                       and the search paths baked into it.
#   2. /bin/hello-elf   a BARE ELF -- linked by tcc against our libc, never
#                       wrapped by mkaex -- executes and prints. The loader's
#                       once-per-boot "is a bare ELF" line must appear, and
#                       its [aex] load line is the measurement: container
#                       cycles vs elf cycles, for the .aex and for the bare
#                       file side by side (/bin/tcc vs /bin/tcc-elf).
#   3. /bin/hello-bad   THE NEGATIVE CONTROL: the same program linked at
#                       0x400000, a stock toolchain's default and shared
#                       kernel low memory. The kernel must refuse it BY NAME
#                       -- `[aex] refused (-12)` with the entry address --
#                       and the shell must get -1 back (its "command not
#                       found" line), with NO `[fault]` line anywhere in the
#                       boot. Refused-by-fault would pass a weaker test.
#   4. tcc compiles and links tests/unit/tcc_bare.c ON THE DEVICE and the
#                       result runs: the loop closed with no host involved.
#
# ONE COMMAND AT A TIME, WAITING FOR THE PROMPT, not burst-fed: tcc under TCG
# takes seconds, and bytes typed while the guest is not reading are dropped
# (tests/boot/run-bigexec.sh measured it). The prompt count is the signal.

set -u
. "$(dirname "$0")/bootwait.sh"

ISO="${1:?usage: run-tcc-test.sh <iso> <disk.img>}"
DISK="${2:?usage: run-tcc-test.sh <iso> <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
LOG="${TCC_LOG:-build/tcc/run.log}"
mkdir -p "$(dirname "$LOG")"
rm -f "$LOG"; touch "$LOG"

count_of() { grep -ac -- "$1" "$LOG" 2>/dev/null || echo 0; }
LASTP=0
prompts() { grep -aoF -- '/ $ ' "$LOG" 2>/dev/null | wc -l; }
send() {  # send <cmd> [wait-for-ready-seconds]
    local i n=$(( ${2:-60} * 10 ))
    for ((i = 0; i < n; i++)); do [ "$(prompts)" -gt "$LASTP" ] && break; sleep 0.1; done
    LASTP=$(prompts)
    sleep 1
    printf '%s\n' "$1"
}
wait_count() {  # wait_count <pattern> <n> <timeout-s>
    local i n=$(( $3 * 10 ))
    for ((i = 0; i < n; i++)); do
        [ "$(count_of "$1")" -ge "$2" ] && return 0
        sleep 0.1
    done
    return 1
}

drive() {
    logit_wait_for_shell "$LOG" 240
    send 'echo TCC-START' 60
    send '/bin/tcc -vv' 120
    send '/bin/hello-elf alpha beta' 120
    send '/bin/hello-bad' 120
    send '/bin/tcc-elf -v' 120
    send '/bin/tcc -nostdlib -nostdinc -o /bare /src/bare.c' 300
    send '/bare' 120
    send 'echo TCC-END' 120
    wait_count 'TCC-END' 2 120
    send 'exit' 60
    sleep 2
}

NET="-netdev user,id=n0 -device e1000,netdev=n0"
drive | \
  "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
    -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
    -m 512M -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
    $NET -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!
cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; return 0; }
trap cleanup EXIT
for _ in $(seq 1 "${WAIT:-9000}"); do
    [ "$(count_of 'TCC-END')" -ge 2 ] && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null

fail=0
need() {
    if grep -aq -- "$1" "$LOG"; then echo "  ok: $2"; else echo "  FAIL: $2   (no match for /$1/)"; fail=1; fi
}
forbid() {
    if grep -aq -- "$1" "$LOG"; then echo "  FAIL: $2:"; grep -a -- "$1" "$LOG" | head -5 | sed 's/^/    /'; fail=1
    else echo "  ok: $2"; fi
}

echo "tcc on the device: $LOG"
need "tcc version 0.9.27 (x86_64 LogitOS)"  "/bin/tcc -v printed its version: crt0, argv, stdio and exit all work under mini-libc"
need "install: /usr/lib/tcc"                 "... and the baked CONFIG_TCCDIR is /usr/lib/tcc"
need "/usr/include"                          "... and /usr/include is on its include path"
need "HELLO-ELF ok argc=3 argv1=alpha"       "/bin/hello-elf, a bare ELF tcc linked, ran and saw its argv"
need "is a bare ELF: no container, no stack hint and no integrity record. Accepted" \
                                             "the loader said, once, that it accepted a bare ELF"
need "tcc version 0.9.27 (x86_64 LogitOS)"   "/bin/tcc-elf (the same tcc as a bare ELF) also ran"

# The control. Three properties, all required: the refusal names itself, the
# caller got an error back, and nothing faulted.
need "\[aex\] refused (-12): a bare ELF this machine cannot run" \
                                             "CONTROL: hello linked at 0x400000 was refused BY NAME by the container check"
need "\[elf\] refused (-[0-9]*): entry point outside the user region" \
                                             "CONTROL: ... and the loader's line names the field (entry) and the fix (0x50000000)"
need "command not found: /bin/hello-bad"     "CONTROL: ... and the shell got -1 back -- a refusal, not a dead child"
forbid "\[fault\]"                           "no ring-3 fault anywhere in the boot (a fault-refusal would be the weak outcome)"
forbid "aex_load failed"                     "no load failed AFTER the address space was torn down (the refusal came first)"

# The measurement: what a program costs to start, container vs elf.
echo "  --- [aex] load lines (container+crc Mcyc vs elf Mcyc) ---"
grep -a "^\[aex\] /bin/" "$LOG" | sed 's/^/    /'
grep -aq "^\[aex\] /bin/tcc:" "$LOG" && echo "  ok: the /bin/tcc load line is on the log" || { echo "  FAIL: no [aex] load line for /bin/tcc"; fail=1; }

# The loop closed on the device. Reported either way; asserted, because it
# worked when this harness was written and a silent regression of "tcc can
# compile on the machine" is the worst kind.
need "BARE-ELF ok: compiled, linked and run on the device" \
                                             "tcc compiled, linked and chmod'd tests/unit/tcc_bare.c ON THE DEVICE, and the kernel ran it"

if [ "$fail" -ne 0 ]; then
    echo "FAIL: tcc/bare-ELF on-machine test"
    echo "----- serial output (tail) -----"
    tail -n 80 "$LOG"
    echo "--------------------------------"
    exit 1
fi
echo "PASS: tcc runs on the device, a bare ELF it linked executes, and the 0x400000 control is refused by name"
exit 0
