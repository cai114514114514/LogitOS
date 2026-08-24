#!/usr/bin/env bash
# THE NEGATIVE CONTROL for test-fdstream: the whole-file kmalloc, back.
#
# c/kernel/exec/file.c carries the pre-change read-only branch under
# -DFILE_SLURP -- kmalloc the file's length, one vfs_read, serve reads out of
# the buffer. That is not "the feature switched off". It is the implementation
# that shipped, and it opens every file on this disk perfectly well; what it
# cannot do is open one larger than the kernel heap can hold in one piece. That
# is what makes it the right control: it fails at a SIZE, not at everything.
#
# WHAT IT MUST DO, and it is an EQUALITY rather than a difference: the fd must
# cost EXACTLY the file -- 9,636 B and 2,222,276 B -- because "the control
# reddened" is satisfied by any breakage at all, while "the fd cost exactly the
# file" is satisfied only by the code that shipped.
#
# AND ONE THING MUST NOT MOVE, or the control is measuring "did I break the
# kernel" instead of "what does the buffer cost": both checksums must still
# MATCH. The control reads the file correctly.
#
# THIS PARAGRAPH USED TO NAME A SECOND ONE -- "/dev/vfsmounts must still cost
# 80 B" -- and it was wrong, which is the one thing in this line of work a
# control caught that nothing else did. FILE_SLURP guards the REGULAR-file
# branch only, so the node really is buffered in both kernels; but its cost
# measures 80 B in the shipped one and 0 B here, because at 23 bytes the
# allocation is magazine-sized and a kfree into a magazine does not move
# st_live. The number was an allocator state, not a property of the node, and
# asserting it made this control fail on a row it does not change. See the block
# above the assertion in run-fdstream.sh for the measurement.
#
# THE CRIPPLED KERNEL IS NEVER WRITTEN INTO THE TREE'S OWN BUILD OUTPUTS.
# file.o goes to build/fdstream/, the link and the ISO are done out of make's
# own echoed recipes with that one object substituted, and build/logit.iso is
# not touched -- a concurrent build in this workspace must not be able to pick
# up a kernel that has been deliberately broken. (Rebuild-in-place with a
# restoring trap was the first draft; a trap that does not run poisons the tree.)
#
#   usage: run-fdstream-negctl.sh <disk.img> [mem]
set -u

DISK="${1:?usage: run-fdstream-negctl.sh <disk.img> [mem]}"
MEM="${2:-512}"
HERE="$(dirname "$0")"
D=build/fdstream
mkdir -p "$D"

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
# The substitution must actually happen. A link line that still named the real
# object would produce a control IDENTICAL to the shipped kernel, which would
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

# ONE COPY OF THE BOOT AND THE PARSING. run-fdstream.sh does both; EXPECT=slurp
# is the only thing that differs, so a change to how the numbers are collected
# cannot make the gate and its control disagree about what they collected.
echo "--- the control: file.c -DFILE_SLURP, same disk, same script, ${MEM}M ---"
EXPECT=slurp LOG="$D/negctl.log" bash "$HERE/run-fdstream.sh" \
    "$D/logit.iso" "$DISK" "$MEM"
rc=$?
[ "$rc" -ne 0 ] && { echo "FAIL: test-fdstream-negctl -- the control did not behave like the"; \
                     echo "      code that shipped, so the gate above is not measuring the buffer"; exit 1; }
echo "negative control ok: with the kmalloc restored an fd costs the whole file"
echo "      at both sizes and reads identically"
exit 0
