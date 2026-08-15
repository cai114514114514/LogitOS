#!/usr/bin/env bash
# c/boot/efi/check-acpi-patch.sh -- is the DEFERRED kernel patch still good?
#
# OBSOLETE SINCE 2026-08-15: the patch was applied at integration, so "does it
# still apply" is no longer a meaningful question and the check below would
# fail loudly against an already-patched acpi.c. Detected and short-circuited
# here rather than deleted, because the file documents the deferral pattern.
if grep -q acpi_set_mb2_info "$(dirname "$0")/../../kernel/cpu/acpi.c" 2>/dev/null; then
    echo "[check-acpi-patch] patch is ALREADY APPLIED (acpi.c defines acpi_set_mb2_info); nothing to check"
    exit 0
fi
#
# acpi-mb2-tag.patch is deferred rather than applied because c/kernel/cpu/acpi.c
# belongs to another line of work. A deferred patch rots silently: the file it
# targets keeps moving, and the first anyone finds out is at integration, which
# is the worst moment. This answers the question on demand, in seconds, WITHOUT
# touching the working tree -- it applies the patch into a scratch copy and
# compiles the result with the same target and include list the kernel build
# uses, the scratch dir first on the include path so the patched acpi.h wins.
#
# Green here means: applies, compiles under -Wall -Wextra -Werror, exports the
# setter, and kmain calls it. Red here means go and rebase the patch.
#
# It does NOT prove the RSDP is found at runtime -- that needs the patch landed
# and the UEFI harness's ASSERT #5 flipped from "[acpi] no RSDP" to an SMP
# assertion, which is the job of whoever lands it.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../../.."
S=/tmp/uefi/L/acpichk
rm -rf "$S"; mkdir -p "$S/c/kernel/cpu" "$S/c/kernel/core"
cp c/kernel/cpu/acpi.c c/kernel/cpu/acpi.h "$S/c/kernel/cpu/"
cp c/kernel/core/kmain.c "$S/c/kernel/core/"

( cd "$S" && git apply -p1 "$OLDPWD/c/boot/efi/acpi-mb2-tag.patch" )
echo "[checkpatch] patch applied to the scratch copy"

INCDIRS=$(find c include -type d | grep -v -e '/include/sys$' -e '/include/uonly$' | sort | sed 's/^/-I/' | tr '\n' ' ')
clang --target=x86_64-elf -ffreestanding -mno-red-zone -mcmodel=kernel \
      -std=c11 -Wall -Wextra -Werror -O2 \
      -I"$S/c/kernel/cpu" $INCDIRS \
      -c "$S/c/kernel/cpu/acpi.c" -o "$S/acpi.o"
echo "[checkpatch] patched acpi.c compiles clean (-Wall -Wextra -Werror)"

# The setter must actually be defined and exported, and the tag walk present.
llvm-nm-21 "$S/acpi.o" | grep -q 'T acpi_set_mb2_info' \
  && echo "[checkpatch] acpi_set_mb2_info is defined and global"
grep -q 'acpi_set_mb2_info(mb_info);' "$S/c/kernel/core/kmain.c" \
  && echo "[checkpatch] kmain.c calls it after pmm_init"
