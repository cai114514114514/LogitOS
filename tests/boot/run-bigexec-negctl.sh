#!/usr/bin/env bash
# THE NEGATIVE CONTROL for test-bigexec: the whole-file materialisation, back.
#
# c/kernel/exec/exec.c carries the pre-change proc_execve() under
# -DEXEC_NEGCTL_SLURP -- kmalloc the whole file, one vfs_read, load from memory.
# That is not "the feature switched off". It is the implementation that shipped
# and that loads every ordinary program on this disk perfectly well, which is
# exactly what makes it the right control: what it cannot do is the one thing
# this work exists to make possible, and it fails at a SIZE rather than at
# everything.
#
# So the gate has TWO halves and both must hold:
#     64 MiB  must FAIL against the control   (else the control is not the bug)
#     16 MiB  must PASS against the control   (else the control merely broke the
#                                              loader and measures nothing)
#
# WHY THE FAILURE LANDS WHERE IT DOES, and it is not "out of memory": kmalloc
# falls through to kheap's grow(), which DOUBLES an arena until it covers the
# request and then asks pmm_alloc_contig() -- a linear first-fit with no
# fallback -- for that many CONTIGUOUS frames. 64 MiB of file therefore wants a
# 128 MiB run, and 16 MiB wants 32. RAM is 192M, which is not a number picked to
# make this work: it is what tests/boot/run-swap-test.sh has used as "a
# deliberately small machine" since that harness was written, so it is already
# known to boot a desktop here.
#
# AND BOTH KERNELS ARE RUN AT THAT SAME 192M. That is the part that makes this a
# control rather than two unrelated runs: if the shipped loader were measured at
# 512M and the crippled one at 192M, the honest reading of the difference would
# be "you gave one of them less memory". Same machine, same disk, same script,
# one object file different.
#
# THE CRIPPLED KERNEL IS NEVER WRITTEN INTO THE TREE'S OWN BUILD OUTPUTS.
# exec.o goes to build/bigexec/, the link and the ISO are done out of make's own
# echoed recipes with that one object substituted, and build/logit.iso is not
# touched -- so a concurrent build in this workspace cannot pick up a loader
# that has been deliberately broken. (Rebuilding in place and restoring in a
# trap was the first draft; a trap that does not run leaves a poisoned tree.)

set -u
. "$(dirname "$0")/bootwait.sh"

DISK="${1:?usage: run-bigexec-negctl.sh <disk.img>}"
QEMU="${QEMU:-qemu-system-x86_64}"
D=build/bigexec/negctl
mkdir -p "$D"

# --- build the crippled kernel --------------------------------------------
# Recipes come from make's OWN expansion, never from parsing the Makefile text,
# and the continuations are joined first -- CLAUDE.md records four tools in this
# tree that read one physical line and were all wrong.
join_recipe() { sed -e ':a' -e '/\\$/{N;s/\\\n[ \t]*/ /;ba' -e '}'; }

# `-W FILE` makes make PRETEND that file is newer, so both recipes are echoed
# without touching anything. `make -n` alone prints "is up to date" whenever the
# tree is built, which is exactly when this harness runs -- and `touch` + `-B`,
# the first two things tried, either dirty a tracked file in a contended
# workspace or echo the entire build.
make -n -W c/kernel/exec/exec.c build/kernel.elf > "$D/mk.txt" 2>/dev/null || true
CC_LINE=$(join_recipe < "$D/mk.txt" | grep -E 'exec\.c' | grep -E -- '-o build/c/kernel/exec/exec\.o' | tail -1)
LD_LINE=$(join_recipe < "$D/mk.txt" | grep -E -- '-o build/kernel\.elf' | tail -1)
[ -n "$CC_LINE" ] || { echo "FAIL: no compile line for exec.c in make -n output"; exit 1; }
[ -n "$LD_LINE" ] || { echo "FAIL: no link line for kernel.elf in make -n output"; exit 1; }

OBJ="$D/exec_slurp.o"
eval "$(printf '%s' "$CC_LINE" | sed -e "s#-o build/c/kernel/exec/exec.o#-DEXEC_NEGCTL_SLURP -o $OBJ#")"
[ -s "$OBJ" ] || { echo "FAIL: the control object did not compile"; exit 1; }

# The tree's own kernel must exist and be CURRENT, because the second boot below
# runs it as the reference.
make build/logit.iso >/dev/null 2>&1 || { echo "FAIL: the shipped ISO would not build"; exit 1; }
# The substitution must actually happen. A link line that still names the real
# object would produce a control identical to the shipped kernel, which would
# fail this gate by PASSING -- the worst outcome available.
case "$LD_LINE" in
    *build/c/kernel/exec/exec.o*) ;;
    *) echo "FAIL: the link line does not name build/c/kernel/exec/exec.o"; exit 1 ;;
esac
LD_LINE=$(printf '%s' "$LD_LINE" | sed -e "s#build/c/kernel/exec/exec\.o#$OBJ#" \
                                       -e "s#-o build/kernel.elf#-o $D/kernel.elf#")
eval "$LD_LINE"
[ -s "$D/kernel.elf" ] || { echo "FAIL: the control kernel did not link"; exit 1; }

rm -rf "$D/iso"
cp -r build/iso "$D/iso"
cp "$D/kernel.elf" "$D/iso/boot/kernel.elf"
GRUB=$(command -v i686-elf-grub-mkrescue || command -v grub-mkrescue)
[ -n "$GRUB" ] || { echo "FAIL: no grub-mkrescue"; exit 1; }
"$GRUB" -o "$D/logit.iso" "$D/iso" >/dev/null 2>&1
[ -s "$D/logit.iso" ] || { echo "FAIL: the control ISO was not produced"; exit 1; }

# --- run both kernels, same machine ----------------------------------------
RAM="${RAM:-192M}"

# ONE COMMAND AT A TIME, for the reason run-bigexec.sh documents at length: a
# 64 MiB load is minutes under TCG and there is no flow control on this serial
# line, so a burst-fed script loses the lines typed while the guest is busy.
# Here it would be worse than a lost line -- a dropped `/bin/pad64` and a
# refused `/bin/pad64` look identical in this log, and one of them is the
# control passing when it should fail.
run_one() {   # run_one <iso> <log>
    local iso="$1" log="$2" qpid i
    rm -f "$log"; touch "$log"
    cnt() { grep -ac -- "$1" "$log" 2>/dev/null || echo 0; }
    waitfor() {  # waitfor <pattern> <n> <timeout-s>
        local k n=$(( $3 * 10 ))
        for ((k = 0; k < n; k++)); do
            [ "$(cnt "$1")" -ge "$2" ] && return 0
            sleep 0.1
        done
        return 1
    }
    # READY IS COUNTED -- run-bigexec.sh gives the full argument, and THIS
    # harness is where the bug it describes was found: the control's refusal
    # makes the kernel print AFTER sh's prompt ("[proc] kill: pid 1 exiting"),
    # so a test for "the log ends in the prompt" waits forever on a machine that
    # is ready. Counting prompts is immune to anything printed after one.
    #
    # Waiting for the previous command's OUTPUT is not enough either: a program
    # prints and then exits, and a line typed during the teardown is dropped.
    # Here that would be worse than a lost line -- a dropped `/bin/pad64` and a
    # refused `/bin/pad64` look identical in this log, and one of them is the
    # control passing when it should fail.
    lastp=0
    prompts() { grep -aoF -- '/ $ ' "$log" 2>/dev/null | wc -l; }
    send() {  # send <cmd> <wait-for-ready-seconds>
        local j m=$(( ${2:-60} * 10 ))
        for ((j = 0; j < m; j++)); do [ "$(prompts)" -gt "$lastp" ] && break; sleep 0.1; done
        lastp=$(prompts)
        sleep 1
        printf '%s\n' "$1"
    }
    drive() {
        logit_wait_for_shell "$log" 240
        send '/bin/pad16' 60
        send '/bin/pad64' "${PADWAIT:-1200}"
        send 'echo NEGCTL-END' "${PADWAIT:-1200}"
        waitfor 'NEGCTL-END' 2 120
        send 'exit' 60; sleep 3
    }
    drive | \
      "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$iso" \
        -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
        -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
        -m "$RAM" -smp 4 -accel tcg,thread=multi -vga none -device virtio-gpu-pci \
        -netdev user,id=n0 -device e1000,netdev=n0 \
        -serial stdio -display none -no-reboot >"$log" 2>/dev/null &
    qpid=$!
    for ((i = 0; i < ${WAIT:-40000}; i++)); do
        [ "$(cnt 'NEGCTL-END')" -ge 2 ] && break
        kill -0 "$qpid" 2>/dev/null || break
        sleep 0.1
    done
    kill "$qpid" 2>/dev/null; wait "$qpid" 2>/dev/null
    return 0
}

CLOG="$D/control.log"
SLOG="$D/shipped.log"
run_one "$D/logit.iso"   "$CLOG"
run_one build/logit.iso  "$SLOG"

loaded() { grep -aq "BIGEXEC ok bytes=$1 sum=315" "$2"; }
fail=0

# The shipped loader first, because it is the reference the control is compared
# to. If it cannot do both at 192M then the RAM is simply too small and nothing
# below this line means anything.
if loaded 16777216 "$SLOG" && loaded 67108864 "$SLOG"; then
    echo "  ok: the shipped loader does 16 MiB AND 64 MiB at $RAM"
else
    echo "  FAIL: the shipped loader could not do both at $RAM -- the machine is too small"
    echo "        and the comparison below would measure the RAM, not the loader"
    fail=1
fi

if loaded 16777216 "$CLOG"; then
    echo "  ok: the control still loads 16 MiB -- it is the ceiling that broke, not the loader"
else
    echo "  FAIL: the control cannot load 16 MiB either, so it measures nothing about size"
    fail=1
fi
if loaded 67108864 "$CLOG"; then
    echo "  FAIL: the control LOADED 64 MiB -- test-bigexec does not measure the whole-file kmalloc"
    fail=1
else
    echo "  ok: the control cannot load 64 MiB, on the same machine that just did it"
    grep -a "\[oom\]\|kmalloc .* failed\|aex_load failed" "$CLOG" | head -4 | sed 's/^/    /' || true
fi

if [ "$fail" -ne 0 ]; then
    echo "FAIL: test-bigexec-negctl"
    echo "--- control (tail) ---";  tail -40 "$CLOG"
    echo "--- shipped (tail) ---";  tail -40 "$SLOG"
    exit 1
fi
echo "PASS: same machine, same disk, one object file different --"
echo "      the control fails at 64 MiB and passes at 16, so the gate measures the ceiling"
exit 0
