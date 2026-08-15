#!/usr/bin/env bash
# c/boot/efi/build.sh -- build BOOTX64.EFI, LogitOS's own UEFI loader.
#
# STANDALONE ON PURPOSE. `bash c/boot/efi/build.sh` from a bare tree produces
# build/BOOTX64.EFI and touches nothing else. The Makefile is contended by
# another line of work right now, so the wiring it will eventually need is
# REPORTED at the bottom of this file rather than applied -- and until then this
# script is the whole build, which also means the UEFI loader cannot break the
# kernel build by existing.
#
# WHY clang + lld-link AND NOT gnu-efi OR edk2. The point of this milestone is
# that LogitOS's bottom layer is its own: the loader was the one third-party
# component left down there, and starting from a 200-file SDK to remove a
# dependency would be a joke. Neither is installed on this machine either.
# clang cross-compiles to PE/COFF out of the box (it is a Windows target, and
# UEFI's x86_64 ABI *is* Microsoft x64 -- UEFI 2.10 sec 2.3.4), and lld-link
# emits an EFI application directly. Two tools the tree already has.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
SRC="$ROOT/c/boot/efi"
OUT="$ROOT/build"

CC="${CC:-clang}"
LINK="${LINK:-lld-link}"

# Two knobs, both used by tests/boot/run-uefi-test.sh's negative control:
#   EFI_OUT       where to write the image (default build/BOOTX64.EFI)
#   EFI_CPPFLAGS  extra defines, notably -DEFI_BAD_MAGIC, which makes the
#                 trampoline hand the kernel one wrong nibble of Multiboot2
#                 magic so that boot.asm's check_multiboot can be proved to
#                 run on this path (see the comment on MB2_MAGIC in
#                 trampoline.S -- it is where the reasoning lives).
# The object directory is derived from the output name so the two builds
# cannot overwrite each other's .obj files, which would make the control pass
# or fail depending on build order -- the worst kind of flake.
EFI_OUT="${EFI_OUT:-$OUT/BOOTX64.EFI}"
EFI_CPPFLAGS="${EFI_CPPFLAGS:-}"
OBJ="$OUT/efi/$(basename "$EFI_OUT" .EFI)"

mkdir -p "$OBJ" "$(dirname "$EFI_OUT")"

# --target=x86_64-unknown-windows          PE/COFF object, Microsoft x64 ABI,
#                                          and a 16-bit wchar_t so L"..." is the
#                                          UCS-2 the firmware protocols want
#                                          (loader.c asserts that).
# -ffreestanding -fno-builtin              there is no libc, and no libc call
#                                          may be SYNTHESISED either -- see the
#                                          volatile pointer in mzero().
# -fno-stack-protector -fno-stack-check    both want runtime support symbols
#                                          (__security_cookie, __chkstk) that do
#                                          not exist in a 30 KiB freestanding
#                                          binary.
# -mno-red-zone                            the firmware takes timer interrupts
#                                          while this code runs; a red zone
#                                          would be clobbered under them.
# -mno-sse ... -mgeneral-regs-only         nothing here needs floating point,
#                                          and the SSE state is the firmware's
#                                          until we own the machine. Keeping the
#                                          compiler out of XMM means the loader
#                                          cannot disturb it.
# -Wall -Wextra -Werror                    this code has one chance to be right.
CFLAGS=(
  --target=x86_64-unknown-windows
  -ffreestanding -fno-builtin
  -fno-stack-protector -fno-stack-check
  -mno-red-zone -mgeneral-regs-only
  -std=c11 -O2
  -Wall -Wextra -Werror
  -I"$SRC"
)

echo "[efi-build] cc   loader.c"
"$CC" "${CFLAGS[@]}" $EFI_CPPFLAGS -c "$SRC/loader.c" -o "$OBJ/loader.obj"

# .S and not .s: the trampoline is preprocessed, which is what lets EFI_BAD_MAGIC
# and the selector/COM1 names be spelled once instead of as bare numbers.
echo "[efi-build] as   trampoline.S"
"$CC" --target=x86_64-unknown-windows $EFI_CPPFLAGS \
      -c "$SRC/trampoline.S" -o "$OBJ/trampoline.obj"

# /subsystem:efi_application  PE subsystem 10 (UEFI 2.10 sec 2.1.1). This is
#                             what makes the firmware willing to run the file.
# /entry:efi_main             lld's default entry name for this subsystem is not
#                             ours; say it rather than depend on a default.
# /nodefaultlib               clang emits /DEFAULTLIB directives for the MSVC
#                             runtime into every object; without this the link
#                             fails looking for libcmt on a Linux box.
# /base:0x10000000            256 MiB, and the number MATTERS -- this was
#                             0x100000 for exactly one build, and OVMF honoured
#                             it: the loader landed at 0x100000..0x109000, which
#                             is THE KERNEL'S LINK ADDRESS. The loader's first
#                             act was then to ask the firmware for memory it had
#                             itself been given, and the refusal it printed was
#                             correct and self-inflicted. Any preferred base
#                             clear of the kernel's ~12 MiB image works; 256 MiB
#                             is comfortably clear and comfortably below 4 GiB,
#                             which the descent in trampoline.S requires.
#                             loader.c verifies the ACTUAL placement anyway
#                             (below 4 GiB, and disjoint from the kernel span),
#                             because a preferred base is a request.
#
# NOT passed: /align. lld-link already gives SectionAlignment 4096 and
# FileAlignment 512, which is what a UEFI image wants; forcing them equal with
# /align:4096 earns "warning: /align specified without /driver; image may not
# run" -- and a warning that says the image may not run is not a thing to carry
# into a boot loader for tidiness.
echo "[efi-build] link $(basename "$EFI_OUT")"
"$LINK" \
  /subsystem:efi_application \
  /entry:efi_main \
  /nodefaultlib \
  /base:0x10000000 \
  /out:"$EFI_OUT" \
  "$OBJ/loader.obj" "$OBJ/trampoline.obj"

# ---- the gate's first half, run here so it cannot be forgotten -------------
# A PE that the firmware silently refuses looks exactly like a machine that did
# not boot. Two fields decide it, so both are checked here rather than by eye:
# Machine must be x86-64 (0x8664) and Subsystem must be 10 (EFI application).
# Read straight out of the file so the check does not depend on llvm-readobj
# being installed.
python3 - "$EFI_OUT" <<'PY'
import struct, sys
path = sys.argv[1]
b = open(path, 'rb').read()
assert b[:2] == b'MZ', "not a PE file (no MZ)"
pe = struct.unpack_from('<I', b, 0x3C)[0]
assert b[pe:pe+4] == b'PE\0\0', "no PE signature"
machine, = struct.unpack_from('<H', b, pe + 4)
magic,   = struct.unpack_from('<H', b, pe + 24)          # optional header magic
subsys,  = struct.unpack_from('<H', b, pe + 24 + 68)     # PE32+ Subsystem
entry,   = struct.unpack_from('<I', b, pe + 24 + 16)     # AddressOfEntryPoint
assert machine == 0x8664, f"machine is {machine:#x}, want 0x8664 (x86-64)"
assert magic   == 0x20b,  f"optional header magic {magic:#x}, want 0x20b (PE32+)"
assert subsys  == 10,     f"subsystem is {subsys}, want 10 (EFI application)"
assert entry != 0, "no entry point"
print(f"[efi-build] PE ok: machine=0x{machine:04x} PE32+ subsystem={subsys} "
      f"entry=0x{entry:x} size={len(b)}")
PY

echo "[efi-build] -> $EFI_OUT"

# ---------------------------------------------------------------------------
# MAKEFILE WIRING, REPORTED NOT APPLIED (the Makefile belongs to another line
# of work this week). When it is free, this is the whole change:
#
#   BOOTX64 := $(BUILD)/BOOTX64.EFI
#   EFI_SRC := c/boot/efi/loader.c c/boot/efi/trampoline.S \
#              c/boot/efi/efi.h c/boot/efi/mb2.h
#
#   $(BOOTX64): $(EFI_SRC)
#   	@bash c/boot/efi/build.sh
#
#   .PHONY: uefi
#   uefi: $(BOOTX64)
#
# and nothing else -- note in particular that NO change to C_SRC, ASM_SRC or
# INCDIRS is needed or wanted:
#   * C_SRC (Makefile:253) is `find c/kernel c/drivers c/lib c/fs c/net
#     c/crypto`, which does not include c/boot at all, so loader.c cannot leak
#     into the kernel link.
#   * ASM_SRC (Makefile:254) is `$(wildcard c/boot/*.asm)` -- non-recursive AND
#     .asm only, so trampoline.S is invisible to it twice over.
#   * INCDIRS (Makefile:79) DOES pick up c/boot/efi, which is why efi.h and
#     mb2.h were checked to be globally unique basenames before being created,
#     and why the ELF structs live inside loader.c instead of an elf.h that
#     would shadow c/kernel/exec/elf.h. See CLAUDE.md, "Source layout".
