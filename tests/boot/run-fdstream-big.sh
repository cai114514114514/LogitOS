#!/usr/bin/env bash
# CAN THIS MACHINE OPEN A FILE LARGER THAN ITS KERNEL HEAP? With the control
# that makes the answer readable.
#
# Until this change, opening a file meant kmalloc'ing all of it
# (c/kernel/exec/file.c). "The largest file that can be opened" was therefore
# "the largest kmalloc that succeeds", which is a question about kheap's grow()
# -- it DOUBLES an arena until it covers the request and then asks
# pmm_alloc_contig(), a linear first-fit with no fallback -- and not a question
# about the filesystem at all. A 355.5 MiB file wanted a 512 MiB contiguous run
# on a 512 MiB machine, and open() refused.
#
# THE GATE IS TWO KERNELS ON ONE MACHINE, and that shape is load-bearing here
# more than anywhere else in this tree, because the shipped kernel's evidence is
# an ABSENCE: no "[oom]" line and no "sh: cannot open input". An absence is also
# what a DROPPED COMMAND looks like -- there is no flow control on this serial
# line. The control is what tells those apart: it runs the identical script on
# the identical image at the identical RAM with ONE OBJECT FILE different, and
# it must print BOTH lines. If it does not, the command never arrived and the
# gate says so instead of passing.
#
# -DFILE_SLURP is that object: file.c's read-only branch reverted to the kmalloc
# it shipped with. It is the PLAUSIBLE wrong implementation and not a broken
# one -- it opens every other file on this disk perfectly well, which is exactly
# what makes it the right control. It deliberately does NOT disable the `live`
# (/proc) streaming path, or it would be measuring "did anything break".
#
# THE CRIPPLED KERNEL IS NEVER WRITTEN INTO THE TREE'S OWN BUILD OUTPUTS --
# file.o goes to build/fdstream/, the link and the ISO are done out of make's
# own echoed recipes with that one object substituted, and build/logit.iso is
# not touched. A concurrent build in this workspace must not be able to pick up
# a kernel that has been deliberately broken. (Rebuild-in-place with a restoring
# trap was the first draft; a trap that does not run leaves a poisoned tree.)
#
#   usage: run-fdstream-big.sh <disk.img> <path-on-disk> [mem-mib]
set -u
. "$(dirname "$0")/bootwait.sh"

DISK="${1:?usage: run-fdstream-big.sh <disk.img> <path> [mem]}"
BIGF="${2:?usage: run-fdstream-big.sh <disk.img> <path> [mem]}"
MEM="${3:-512}"
QEMU="${QEMU:-qemu-system-x86_64}"
D=build/fdstream
mkdir -p "$D"

[ -f "$DISK" ] || { echo "SKIP: $DISK is not here -- this gate needs an image carrying a file"; \
                    echo "      larger than the kernel heap can hold, and it is not built by make."; exit 0; }

# --- build the crippled kernel ---------------------------------------------
# Recipes come from make's OWN expansion, never from parsing the Makefile text,
# and the continuations are joined first -- CLAUDE.md records four tools in this
# tree that read one physical line and were all wrong.
join_recipe() { sed -e ':a' -e '/\\$/{N;s/\\\n[ \t]*/ /;ba' -e '}'; }

# `-W FILE` makes make PRETEND that file is newer, so both recipes are echoed
# without touching anything -- `make -n` alone prints "is up to date" whenever
# the tree is built, which is exactly when this harness runs.
make -n -W c/kernel/exec/file.c build/kernel.elf > "$D/mk.txt" 2>/dev/null || true
CC_LINE=$(join_recipe < "$D/mk.txt" | grep -E 'exec/file\.c' | grep -E -- '-o build/c/kernel/exec/file\.o' | tail -1)
LD_LINE=$(join_recipe < "$D/mk.txt" | grep -E -- '-o build/kernel\.elf' | tail -1)
[ -n "$CC_LINE" ] || { echo "FAIL: no compile line for file.c in make -n output"; exit 1; }
[ -n "$LD_LINE" ] || { echo "FAIL: no link line for kernel.elf in make -n output"; exit 1; }

OBJ="$D/file_slurp.o"
eval "$(printf '%s' "$CC_LINE" | sed -e "s#-o build/c/kernel/exec/file.o#-DFILE_SLURP -o $OBJ#")"
[ -s "$OBJ" ] || { echo "FAIL: the control object did not compile"; exit 1; }

make build/logit.iso >/dev/null 2>&1 || { echo "FAIL: the shipped ISO would not build"; exit 1; }
# The substitution must actually happen. A link line that still names the real
# object would produce a control identical to the shipped kernel, which would
# fail this gate by PASSING -- the worst outcome available.
case "$LD_LINE" in
    *build/c/kernel/exec/file.o*) ;;
    *) echo "FAIL: the link line does not name build/c/kernel/exec/file.o"; exit 1 ;;
esac
LD_LINE=$(printf '%s' "$LD_LINE" | sed -e "s#build/c/kernel/exec/file\.o#$OBJ#" \
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

# --- run both kernels, same machine, same image, same script ----------------
run_one() {   # run_one <iso> <log>
    local iso="$1" log="$2" qpid i
    : > "$log"
    {
        logit_wait_for_shell "$log" 300
        sleep 2
        printf 'true < %s\n' "$BIGF"
        sleep 8
        printf 'echo FDBIG-END\n'
        sleep 4
    } | \
      "$QEMU" -cpu "${QEMU_CPU:-max}" -cdrom "$iso" \
        -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
        -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
        -m "${MEM}M" -smp 4 -accel "${QEMU_ACCEL:-tcg,thread=multi}" \
        -vga none -device virtio-gpu-pci \
        -serial stdio -display none -no-reboot >"$log" 2>/dev/null &
    qpid=$!
    for ((i = 0; i < ${WAIT:-9000}; i++)); do
        grep -aq 'FDBIG-END' "$log" && break
        kill -0 "$qpid" 2>/dev/null || break
        sleep 0.1
    done
    sleep 2
    kill "$qpid" 2>/dev/null; wait "$qpid" 2>/dev/null
}

CLOG="$D/control.log"
SLOG="$D/shipped.log"
run_one "$D/logit.iso"  "$CLOG"
run_one build/logit.iso "$SLOG"

booted() { grep -aq 'LogitOS shell' "$1"; }
oom()    { grep -aq '\[oom\] kmalloc'  "$1"; }
refused(){ grep -aq 'sh: cannot open input' "$1"; }
ran()    { grep -aq 'FDBIG-END' "$1"; }

fail=0
for l in "$CLOG" "$SLOG"; do
    booted "$l" || { echo "  FAIL: $l never reached a shell -- a BOOT finding, not an fd one"; fail=1; }
    ran    "$l" || { echo "  FAIL: $l never reached the end marker -- the script did not run"; fail=1; }
done

# THE CONTROL FIRST. It is what makes the shipped run's silence mean anything.
if oom "$CLOG" && refused "$CLOG"; then
    echo "  ok: the control refuses $BIGF at ${MEM}M, out loud, on both channels:"
    grep -a '\[oom\] kmalloc\|sh: cannot open input' "$CLOG" | head -2 | sed 's/^/        /'
else
    echo "  FAIL: the control did NOT refuse -- so a silent shipped run proves nothing."
    echo "        Either the command never arrived or the control is not the old code."
    grep -a '\[oom\]\|cannot open' "$CLOG" | head -4 | sed 's/^/        /'
    fail=1
fi

if oom "$SLOG" || refused "$SLOG"; then
    echo "  FAIL: the shipped kernel also refuses $BIGF -- open() still slurps"
    grep -a '\[oom\]\|cannot open' "$SLOG" | head -4 | sed 's/^/        /'
    fail=1
else
    echo "  ok: the shipped kernel opens $BIGF at ${MEM}M with no [oom] and no refusal"
fi

# What each kernel charged its heap, side by side. Not asserted on -- it is the
# number a reader wants and grow() is shared with everything else on the boot.
echo "  --- kheap growth on each boot ---"
printf '      control: %s\n' "$(grep -a '\[kheap\] grow' "$CLOG" | tail -1)"
printf '      shipped: %s\n' "$(grep -a '\[kheap\] grow' "$SLOG" | tail -1)"

if [ "$fail" -ne 0 ]; then
    echo "FAIL: test-fdstream-big (transcripts: $CLOG $SLOG)"
    exit 1
fi
echo "PASS: same machine, same image, one object file different -- the control"
echo "      cannot open $BIGF at ${MEM}M and the shipped kernel can"
exit 0
