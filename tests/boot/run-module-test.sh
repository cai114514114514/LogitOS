#!/usr/bin/env bash
# THE CLAIM: a driver built as a loadable MODULE behaves identically to the
# same driver linked into the kernel.
#
# It is proved with c/drivers/core/qemu_edu.c because that file already exists
# to be machine-checkable: `edu` is the one QEMU device whose whole purpose is
# to be poked, so writing its "raise" register produces an interrupt with a
# payload we chose, and the driver prints one line per interrupt path. Those
# exact lines are what tests/boot/run-devmodel-test.sh already requires of the
# STATIC build:
#
#     LOGIT_IRQ_OK msi    mode=msi  vec=.. count=.. payload=2a
#     LOGIT_IRQ_OK legacy mode=intx vec=.. count=.. payload=2a
#
# so "identically" is not a judgement call here -- it is the same two strings,
# from the same source file, compiled with the same flags, differing only in
# whether it went to the linker or to the loader.
#
# THE KERNEL UNDER TEST HAS NO edu DRIVER IN IT. That is the point and it is
# what makes this a test of the loader rather than of a duplicate registration:
# the kernel is relinked with qemu_edu.c removed from C_SRC, so before the
# module is loaded nothing in the machine can drive the device. LOAD=0 boots
# that same kernel and never loads the module -- the negative control, and not
# a synthetic one: it is the machine the feature exists to fix.
#
# ------------------------------------------------------------------ staging --
# TWO THINGS THIS SCRIPT DOES ON THE make COMMAND LINE RATHER THAN IN A FILE,
# because the Makefile is contended (several agents edit it at once) and
# tests/module.mk records the permanent form:
#
#   1. C_SRC without qemu_edu.c. The value is ASKED OF make (`make -pRrq`) and
#      then filtered, never reconstructed by re-implementing the Makefile's own
#      find/filter-out expression. CLAUDE.md documents four tools in this tree
#      that parsed the Makefile by hand and all four were wrong; the one that
#      was fine read make's own output. A command-line assignment overrides the
#      `:=` in the Makefile outright, and OBJ derives from C_SRC in the same
#      pass, so this removes the object from the link with no file edited.
#
#   2. The module file itself, staged as fsroot/edu.ko. $(DISK)'s file list is
#      $(wildcard fsroot/*), evaluated when make parses -- so a file placed
#      there before make runs is packed at /edu.ko with no Makefile change.
#      It is removed again on exit (trap), so the source tree is unchanged
#      whether this passes, fails or is interrupted. The permanent form is the
#      two lines named at the top of tests/module.mk.
#
# Both are honest workarounds and both are visible in the output below, which
# is the condition for using them at all.
#
#   run-module-test.sh          # positive
#   LOAD=0 run-module-test.sh   # negative control
set -u

QEMU="${QEMU:-qemu-system-x86_64}"
LOAD="${LOAD:-1}"
BUILD="${BUILD:-build}"
ISO="$BUILD/logit.iso"
DISK="$BUILD/disk.img"
KO="$BUILD/edu.ko"
STAGED="fsroot/edu.ko"
LOG="$(mktemp)"

cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null
    rm -f "$STAGED" "$LOG"
}
# NOTE FOR ANYONE RUNNING THIS SCRIPT DIRECTLY: it leaves build/kernel.elf and
# build/logit.iso holding a kernel that DELIBERATELY has no edu driver in it,
# and make cannot tell that by timestamp -- the next ordinary `make run` would
# reuse it and quietly be missing a driver. `make test-module` deletes both
# after the pair (see tests/module.mk); a direct run should too:
#     rm -f build/kernel.elf build/logit.iso
trap cleanup EXIT

say()  { echo "$1"; }
fail=0
need() { if [ "$2" = 0 ]; then say "ok   $1"; else say "FAIL $1"; fail=1; fi; }

# ------------------------------------------------------------------- build --
# SKIP_BUILD=1 reuses build/{logit.iso,disk.img,edu.ko} exactly as they are.
# It is not a convenience: the positive run and the control are only a
# controlled pair if they boot the SAME BYTES, and this tree has several agents
# rebuilding build/ concurrently -- between two ordinary invocations, an
# unrelated .o can change underneath and the two runs then differ in more than
# the one variable. Run the pair as:
#     bash tests/boot/run-module-test.sh                    # builds
#     SKIP_BUILD=1 LOAD=0 bash tests/boot/run-module-test.sh # same binary
# It still prints the sha256 of what it booted, so "the same bytes" is a
# recorded fact rather than a claim about make.
SKIP_BUILD="${SKIP_BUILD:-0}"
if [ "$SKIP_BUILD" = 1 ]; then
    echo "=== SKIP_BUILD=1: reusing the existing artifacts ==="
    for f in "$ISO" "$DISK" "$KO"; do
        [ -f "$f" ] || { echo "FAIL: $f missing and SKIP_BUILD=1"; exit 1; }
        echo "    $(sha256sum "$f")"
    done
    if nm "$BUILD/kernel.elf" 2>/dev/null | grep -q 'edu_driver'; then
        echo "FAIL: the kernel in build/ contains edu_driver -- this is not the"
        echo "      module-only kernel. Re-run without SKIP_BUILD."
        exit 1
    fi
    echo "    kernel.elf contains no edu_driver symbol: correct"
    cp "$KO" "$STAGED" 2>/dev/null || true
fi
if [ "$SKIP_BUILD" != 1 ]; then
echo "=== building the module ==="
make "$KO" >/dev/null || { echo "FAIL: could not build $KO"; exit 1; }
ls -l "$KO"
echo "module relocation types (readelf, the object that will be loaded):"
readelf -r "$KO" 2>/dev/null | awk '/R_X86_64/{print $3}' | sort | uniq -c | sed 's/^/    /'

echo
echo "=== relinking the kernel WITHOUT c/drivers/core/qemu_edu.c ==="
# Ask make for its own C_SRC rather than rebuilding the expression here.
BASE_CSRC="$(make -pRrq 2>/dev/null | sed -n 's/^C_SRC := //p' | head -1)"
if [ -z "$BASE_CSRC" ]; then
    echo "FAIL: could not read C_SRC out of make -pRrq"
    exit 1
fi
# The check that the filter did something. Without it, a rename of the driver
# file would leave this script silently testing a kernel that still has the
# static driver in it -- and then EVERY assertion below would pass for the
# wrong reason, which is the worst outcome this harness can have.
case "$BASE_CSRC" in
  *c/drivers/core/qemu_edu.c*) ;;
  *) echo "FAIL: qemu_edu.c is not in C_SRC -- has it moved? Nothing to remove."; exit 1;;
esac
NEW_CSRC="$(printf '%s\n' $BASE_CSRC | grep -v 'c/drivers/core/qemu_edu.c' | tr '\n' ' ')"
echo "    C_SRC: $(printf '%s\n' $BASE_CSRC | wc -l) files -> $(printf '%s\n' $NEW_CSRC | wc -l) files"

# FORCE THE RELINK. make compares TIMESTAMPS, not variables: an ordinary
# `make` elsewhere in this tree leaves build/logit.iso newer than every object,
# and the invocation below then reports "up to date" and leaves the DEFAULT
# kernel -- the one with the static driver in it -- in place. Every assertion
# in this script would then pass while measuring nothing. The nm check a few
# lines down catches it, but catching it is the second-best outcome; deleting
# the two products make cannot reason about is the first.
rm -f "$BUILD/kernel.elf" "$ISO"
make "$ISO" C_SRC="$NEW_CSRC" >/dev/null 2>&1 || {
    echo "FAIL: kernel build without qemu_edu.c failed"
    make "$ISO" C_SRC="$NEW_CSRC" 2>&1 | tail -20
    exit 1
}
# Proof the object really is absent from the kernel that will boot. `edu_probe`
# is static, so the symbol to look for is the driver descriptor.
if nm "$BUILD/kernel.elf" 2>/dev/null | grep -q 'edu_driver'; then
    echo "FAIL: the relinked kernel still contains edu_driver"
    exit 1
fi
echo "    kernel.elf contains no edu_driver symbol: correct"

echo
echo "=== staging the module onto the disk image ==="
cp "$KO" "$STAGED"
make "$DISK" >/dev/null 2>&1 || { echo "FAIL: disk build failed"; exit 1; }
echo "    packed fsroot/edu.ko -> /edu.ko"
fi   # SKIP_BUILD

# -------------------------------------------------------------------- boot --
# The loader script is written on the guest by the shell, the same way
# run-swap-test.sh writes /mmaudit.as. Deliberately free of quotes, `>` and
# glob characters: sh.c handles all three, but a quoting failure in a harness
# is indistinguishable from the feature not working, and the marker is not the
# measurement -- the numbers are.
#
#   7001  load /edu.ko as root            -> expect a module id >= 1
#   7002  load a MISSING path as root     -> expect MOD_E_NOFILE (-2)
#   7003  setuid(1000)                    -> expect 0
#   7004  load the same missing path      -> expect MOD_E_PERM (-1)
#
# 7002 and 7004 together are the whole privilege argument: the same call with
# the same argument answers "no such file" to root and "not permitted" to
# everyone else, which is what says the credential is checked BEFORE the path
# is ever looked at. Either line alone proves nothing -- a loader that refused
# everything would satisfy 7004, and one that checked nothing would satisfy
# 7002.
echo
echo "=== booting (LOAD=$LOAD) ==="
NET="-netdev user,id=n0 -device e1000,netdev=n0"

{
  # Wait for the serial shell. 150 tries at 0.4s, the same budget the other
  # harnesses use; bootwait.sh is not sourced because this script needs no
  # other part of it.
  for _ in $(seq 1 400); do
      grep -aq 'LOGIT_BOOT_OK' "$LOG" 2>/dev/null && break
      sleep 0.25
  done
  sleep 3
  printf 'echo MOD_MARK_BEGIN\n';                                       sleep 2
  if [ "$LOAD" = 1 ]; then
      printf 'echo a = args() > /insmod.as\n';                          sleep 1
      printf 'echo p = a[1] >> /insmod.as\n';                           sleep 1
      printf 'echo q = a[2] >> /insmod.as\n';                           sleep 1
      printf 'echo print(7001, syscall(182, addr(p), 0, 0)) >> /insmod.as\n'; sleep 1
      printf 'echo print(7002, syscall(182, addr(q), 0, 0)) >> /insmod.as\n'; sleep 1
      printf 'echo print(7003, syscall(154, 1000, 0, 0)) >> /insmod.as\n';    sleep 1
      printf 'echo print(7004, syscall(182, addr(q), 0, 0)) >> /insmod.as\n'; sleep 1
      printf 'cat /insmod.as\n';                                        sleep 2
      printf 'as /insmod.as /edu.ko /no-such-module.ko\n';              sleep 8
  fi
  printf 'echo MOD_MARK_END\n';                                         sleep 3
  printf 'exit\n';                                                      sleep 2
} | $QEMU -cpu max -cdrom "$ISO" \
      -drive file="$DISK",format=raw,if=none,id=hd0,file.locking=off \
      -device virtio-blk-pci,drive=hd0 -boot d -snapshot \
      -m 512M -smp 4 -accel tcg,thread=multi \
      -vga none -device virtio-gpu-pci -device edu \
      $NET -serial stdio -display none -no-reboot >"$LOG" 2>/dev/null &
QPID=$!

for _ in $(seq 1 1600); do
    grep -aq 'MOD_MARK_END' "$LOG" && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.25
done
sleep 1
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null

echo "----- module + driver lines -----"
grep -a -e '^\[mod\]' -e '^\[edu\]' -e 'LOGIT_IRQ' -e '^700[0-9]' "$LOG" | head -20
echo "---------------------------------"

booted=1; grep -aq 'LOGIT_BOOT_OK' "$LOG" || booted=0
need "the kernel booted" "$([ $booted = 1 ] && echo 0 || echo 1)"

MSI="$(grep -a 'LOGIT_IRQ_OK msi mode=msi' "$LOG" | head -1 | tr -d '\r')"
INTX="$(grep -a 'LOGIT_IRQ_OK legacy mode=intx' "$LOG" | head -1 | tr -d '\r')"

# ------------------------------------------------------- negative control --
if [ "$LOAD" != 1 ]; then
    if [ -z "$MSI" ] && [ -z "$INTX" ] && ! grep -aq '^\[edu\]' "$LOG"; then
        echo
        echo "PASS (control): the edu device is present, no driver is compiled in,"
        echo "                the module was NOT loaded, and nothing claimed it."
        exit 0
    fi
    echo "CONTROL FAILED: the driver ran without the module being loaded:"
    echo "  $MSI"; echo "  $INTX"
    grep -a '^\[edu\]' "$LOG" | head -3
    exit 1
fi

# ------------------------------------------------------------- the claims --
LOADLINE="$(grep -a '^\[mod\] edu: loaded' "$LOG" | head -1 | tr -d '\r')"
need "the loader reported the module loaded" \
     "$([ -n "$LOADLINE" ] && echo 0 || echo 1)"
[ -n "$LOADLINE" ] && echo "     $LOADLINE"

R7001="$(grep -a '^7001 ' "$LOG" | head -1 | awk '{print $2}' | tr -d '\r')"
R7002="$(grep -a '^7002 ' "$LOG" | head -1 | awk '{print $2}' | tr -d '\r')"
R7003="$(grep -a '^7003 ' "$LOG" | head -1 | awk '{print $2}' | tr -d '\r')"
R7004="$(grep -a '^7004 ' "$LOG" | head -1 | awk '{print $2}' | tr -d '\r')"

need "SYS_MODULE_LOAD returned a module id (got '${R7001:-none}')" \
     "$([ -n "$R7001" ] && [ "$R7001" -ge 1 ] 2>/dev/null && echo 0 || echo 1)"
need "root loading a missing path gets MOD_E_NOFILE -2 (got '${R7002:-none}')" \
     "$([ "${R7002:-x}" = "-2" ] && echo 0 || echo 1)"
need "setuid(1000) succeeded (got '${R7003:-none}')" \
     "$([ "${R7003:-x}" = "0" ] && echo 0 || echo 1)"
need "NON-root gets MOD_E_PERM -1 for the SAME path root got -2 for (got '${R7004:-none}')" \
     "$([ "${R7004:-x}" = "-1" ] && echo 0 || echo 1)"

need "the module's probe ran and the device answered its liveness check" \
     "$(grep -aq '^\[edu\] .*live' "$LOG" && echo 0 || echo 1)"

need "an MSI raised by the module-loaded driver reached a handler" \
     "$([ -n "$MSI" ] && echo 0 || echo 1)"
[ -n "$MSI" ] && echo "     $MSI"
need "and so did a legacy INTx" \
     "$([ -n "$INTX" ] && echo 0 || echo 1)"
[ -n "$INTX" ] && echo "     $INTX"

# The payload is the part that says the interrupt carried OUR data rather than
# that something spurious arrived: edu echoes back what was written to its
# raise register, and qemu_edu.c writes 0x2a.
need "the MSI carried the payload the driver wrote (payload=2a)" \
     "$(echo "$MSI" | grep -q 'payload=2a' && echo 0 || echo 1)"
need "the INTx carried it too" \
     "$(echo "$INTX" | grep -q 'payload=2a' && echo 0 || echo 1)"

if [ "$fail" = 0 ]; then
    echo
    echo "PASS: c/drivers/core/qemu_edu.c, compiled with -c instead of linked,"
    echo "      loaded at runtime into a kernel that does not contain it, bound"
    echo "      the device through the same DRIVER_DECLARE section, and produced"
    echo "      the same two LOGIT_IRQ_OK lines the static build produces."
    exit 0
fi
echo
echo "----- serial tail -----"; tail -60 "$LOG"; echo "-----------------------"
exit 1
