#!/usr/bin/env bash
# THE NEGATIVE CONTROL for test-closefull: file_close() built
# -DFILE_CLOSE_ALWAYS_OK, i.e. SYS_CLOSE back to returning 0 unconditionally
# -- the behaviour this whole item replaced. Same technique as
# run-fdstream-negctl.sh: pull file.c's own compile/link lines out of make's
# expansion (continuations already joined by `make -n` itself), recompile
# just that one object with the extra -D, relink a kernel.elf and an ISO in
# their own build directory, and hand it to run-closefull-test.sh with
# EXPECT=broken. build/kernel.elf and build/logit.iso are never touched --
# a concurrent build in this workspace must not be able to pick up a
# deliberately crippled kernel.
#
#   usage: run-closefull-negctl.sh <disk.img> [mem] [keep_free_blocks]
set -u

DISK="${1:?usage: run-closefull-negctl.sh <disk.img> [mem] [keep_free_blocks]}"
MEM="${2:-512}"
KEEP_FREE="${3:-2}"
HERE="$(dirname "$0")"
D=build/closefull-negctl
mkdir -p "$D"

join_recipe() { sed -e ':a' -e '/\\$/{N;s/\\\n[ \t]*/ /;ba' -e '}'; }

make -n -W c/kernel/exec/fd/file.c build/kernel.elf > "$D/mk.txt" 2>/dev/null || true
CC_LINE=$(join_recipe < "$D/mk.txt" | grep -E 'exec/file\.c' | grep -E -- '-o build/c/kernel/exec/file\.o' | tail -1)
LD_LINE=$(join_recipe < "$D/mk.txt" | grep -E -- '-o build/kernel\.elf' | tail -1)
[ -n "$CC_LINE" ] || { echo "FAIL: no compile line for file.c in make -n output"; exit 1; }
[ -n "$LD_LINE" ] || { echo "FAIL: no link line for kernel.elf in make -n output"; exit 1; }

OBJ="$D/file_closealwaysok.o"
eval "$(printf '%s' "$CC_LINE" | sed -e "s#-o build/c/kernel/exec/fd/file.o#-DFILE_CLOSE_ALWAYS_OK -o $OBJ#")"
[ -s "$OBJ" ] || { echo "FAIL: the control object did not compile"; exit 1; }

make build/logit.iso >/dev/null 2>&1 || { echo "FAIL: the shipped ISO would not build"; exit 1; }
# The substitution must actually happen, or this control is identical to the
# shipped kernel -- which would fail this gate by PASSING.
case "$LD_LINE" in
    *build/c/kernel/exec/fd/file.o*) ;;
    *) echo "FAIL: the link line does not name build/c/kernel/exec/fd/file.o"; exit 1 ;;
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

echo "--- the control: file.c -DFILE_CLOSE_ALWAYS_OK, same near-full disk, ${MEM}M ---"
EXPECT=broken LOG="$D/negctl.log" DISKFULL="$D/diskfull.img" bash "$HERE/run-closefull-test.sh" \
    "$D/logit.iso" "$DISK" "$MEM" "$KEEP_FREE"
rc=$?
[ "$rc" -ne 0 ] && { echo "FAIL: test-closefull-negctl -- the control did not behave like the"; \
                     echo "      code file_close() replaced, so the gate above is not measuring it"; exit 1; }
echo "negative control confirmed"
exit 0
