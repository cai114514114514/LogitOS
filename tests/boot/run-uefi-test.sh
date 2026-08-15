#!/usr/bin/env bash
# tests/boot/run-uefi-test.sh -- boot LogitOS through the SELF-BUILT UEFI
# loader under OVMF, and prove the whole hand-off contract rather than just
# "the kernel eventually printed its marker".
#
# THE CONTRACT BEING CHECKED, IN ORDER (this is the point of the milestone:
# the loader IMPERSONATES GRUB, and every step below is a step GRUB also does,
# just sourced from UEFI instead of BIOS):
#   1. "[efi] gop"    -- the loader found a Graphics Output Protocol instance
#                         and can build the Multiboot2 framebuffer tag from it
#   2. "[efi] mmap"   -- it captured the UEFI memory map (the mmap tag)
#   3. "[efi] ebs ok" -- ExitBootServices succeeded (the point of no return:
#                         ConOut is a dangling pointer from here on, so every
#                         breadcrumb after this one can ONLY be on serial)
#   4. "[efi] jump"   -- about to enter the 64->32 bit descent and jump to the
#                         kernel's Multiboot2 entry, eax/ebx already loaded
#   5. "[smp] 2 CPU(s) detected" -- the KERNEL's own line: acpi.c consumed
#                         the RSDP the loader forwarded as Multiboot2 tag 15
#                         (acpi-mb2-tag.patch, applied at integration) and the
#                         MADT behind it named both vCPUs. Before that patch
#                         this slot asserted "[acpi] no RSDP" PRESENT, on
#                         purpose -- see the ASSERT #5 note below.
#   6. "LOGIT_BOOT_OK"  -- the kernel reached 64-bit C and ran its self-test
#   7. "[wm] desktop live" -- the window manager's own marker (c/kernel/gui/
#                         wm.c) that the compositor loop is running, checked
#                         before the screendump so a slow host does not get a
#                         screendump of a black boot screen mistaken for "the
#                         desktop never painted".
#
# Steps 1-4 are the loader's (c/boot/efi/*, built by build.sh -- see the
# BOOTX64.EFI contract in tests/uefi.mk); 5-7 are the kernel's, UNCHANGED by
# this milestone, and are exactly what tests/boot/run-test.sh already checks
# for the BIOS/GRUB path -- the point of "one kernel, two loaders" is that
# nothing below the jump needs a UEFI-specific assertion at all.
#
# WHY AHCI FOR THE ESP AND VIRTIO-BLK FOR THE DATA DISK, NOT BOTH ON ONE BUS.
# c/drivers/virtio/virtio_blk.c is a SINGLETON (one global "present" flag, no
# device table) -- attaching two virtio-blk-pci drives would leave the second
# one invisible to the kernel with no error, which is a much worse failure
# mode than a wrong test. AHCI has no such limit (c/drivers/block/blkdev.c's
# ahci_init() registers every port it finds), and OVMF's own BdsDxe log,
# captured while building tools/mkesp.py, shows it boots the removable-media
# path off an AHCI-attached superfloppy exactly as it does off virtio-blk --
# so putting the ESP there costs nothing. blkdev.c's root selection then picks
# whichever registered device actually carries a LogitFS superblock
# (has_logitfs(), blkdev.c:450) regardless of bus or attach order, so the ESP
# (FAT, no LogitFS signature) can never be mistaken for the data disk.
#
# WHY -device virtio-gpu-pci AND NOT -vga std, EVEN THOUGH THIS IS THE LOADER'S
# FIRST TIME TALKING TO GOP. It was not obvious in advance that this build of
# OVMF (/usr/share/OVMF/OVMF_CODE_4M.fd, no gnu-efi/edk2 source tree to read
# here) ships VirtioGpuDxe -- `strings` on the .fd finds nothing because OVMF's
# firmware volumes are LZMA-compressed, so silence proves nothing either way.
# Decided by experiment instead (a throwaway EFI app that calls
# LocateProtocol(&gEfiGraphicsOutputProtocolGuid) and reports over serial,
# c/boot/efi's real toolchain, run against every candidate device this test
# could plausibly use):
#     -vga std                    -> GOP FOUND, mode 1280x800
#     -vga none -device virtio-gpu-pci -> GOP FOUND, mode 1280x800
#     -vga none -device bochs-display  -> GOP FOUND, mode 1280x800
#     -vga qxl                    -> GOP FOUND, mode 1280x800
# All four work, so the tiebreaker is what happens AFTER ExitBootServices:
# c/kernel/gui/fb.c's fb_init() tries virtio_gpu_init() FIRST and only reads
# the Multiboot2 framebuffer tag the loader built as a fallback (see the M19
# note in CLAUDE.md) -- so virtio-gpu-pci is the one choice that exercises the
# SAME driver + DMA present path (TRANSFER_TO_HOST_2D + RESOURCE_FLUSH) that
# every other boot test in this tree already runs, rather than quietly falling
# back to the legacy multiboot-LFB path on the one boot mode nobody else uses.
# It also matches tests/boot/run-test.sh's GPU_ARGS verbatim, so a screen that
# looks wrong here and not there is a loader/GOP bug, not a display-device
# choice this script made differently.
#
# WHY -smp 2. This assertion used to be its own opposite: the milestone
# shipped with acpi.c unable to read the loader's forwarded RSDP (the loader
# ALWAYS emitted mb2 tag 15 -- an earlier revision of this comment claimed it
# did not, which the adversarial verifier caught as stale), the harness ran
# one vCPU, and "[acpi] no RSDP" was asserted PRESENT so that landing
# acpi-mb2-tag.patch would break the gate on purpose. That patch landed at
# integration (2026-08-15): acpi.c now takes the RSDP from the Multiboot2
# info when one was forwarded, before falling back to the BIOS-area scan. So
# this harness now runs TWO vCPUs and asserts SMP comes up -- the regained
# capability is checked, not quietly assumed.
#
# Usage:
#   run-uefi-test.sh <esp.img> <disk.img> [negctl]
#
# Positional mode (default) asserts the full chain above, then screendumps the
# guest's own scanout over QMP (never the host window -- tools/shot.sh's
# header is the canonical statement of why) and requires more than a
# four-colour panic screen, exactly shot.sh's idiom, inlined here because
# shot.sh hardcodes `-cdrom build/logit.iso` and this boot has no cdrom at all.
#
# "negctl" mode is THE CONTROL: <esp.img> is expected to be built from a
# loader whose trampoline hands the kernel eax != 0x36D76289 on purpose (see
# tests/uefi.mk's BOOTX64-badmagic.EFI rule and its EFI_BAD_MAGIC contract).
# Steps 1-4 must still happen -- the LOADER did nothing wrong, it did exactly
# what it was built to do -- and then boot.asm's check_multiboot .fail path
# (c/boot/boot.asm:38-44) must swallow the jump: no LOGIT_BOOT_OK, ever. This
# is the only way to know the magic check in the kernel's OWN entry point,
# unchanged by this milestone, actually runs on this path and is not just
# assumed to because it runs on the BIOS one.
set -u

ESP="${1:?usage: run-uefi-test.sh <esp.img> <disk.img> [negctl]}"
DISK="${2:?usage: run-uefi-test.sh <esp.img> <disk.img> [negctl]}"
MODE="${3:-positive}"

OVMF_CODE="${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}"
OVMF_VARS_TEMPLATE="${OVMF_VARS_TEMPLATE:-/usr/share/OVMF/OVMF_VARS_4M.fd}"
QEMU="${QEMU:-qemu-system-x86_64}"

for f in "$ESP" "$DISK" "$OVMF_CODE" "$OVMF_VARS_TEMPLATE"; do
    [ -f "$f" ] || { echo "FAIL: missing input file: $f"; exit 2; }
done

LOG="$(mktemp)"
# OVMF's VARS region is writable NVRAM -- boot variables, the console
# preference, etc. get written back to it during a real boot. Handing QEMU the
# REPO'S own copy would make every run of this test mutate a file every other
# test (and a human's `make run`) also reads; a per-run copy is the only way
# two invocations, concurrent or sequential, cannot see each other's NVRAM.
VARS="$(mktemp)"
cp "$OVMF_VARS_TEMPLATE" "$VARS"
SOCK="$(mktemp -u /tmp/logit-uefi-XXXXXX.sock)"
PPM="$(mktemp -u /tmp/logit-uefi-XXXXXX.ppm)"
PNG="${UEFI_SHOT:-build/uefi-desktop.png}"

cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null
    rm -f "$LOG" "$VARS" "$SOCK" "$PPM"
}
trap cleanup EXIT

# See the header note: AHCI carries the ESP (any number of controllers/ports
# work), virtio-blk-pci carries the LogitFS data disk (the driver is a
# singleton, so it is the only virtio-blk device on the machine).
AHCI_ARGS="-device ich9-ahci,id=ahci0 -drive file=$ESP,format=raw,if=none,id=esp0,file.locking=off -device ide-hd,drive=esp0,bus=ahci0.0"
BLK_ARGS="-drive file=$DISK,format=raw,if=none,id=hd0,file.locking=off -device virtio-blk-pci,drive=hd0"
GPU_ARGS="-vga none -device virtio-gpu-pci"
NET_ARGS="-netdev user,id=n0 -device e1000,netdev=n0"

"$QEMU" -machine q35 -cpu "${QEMU_CPU:-max}" -smp 2 \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$VARS" \
    $AHCI_ARGS $BLK_ARGS $GPU_ARGS $NET_ARGS \
    -m 512M -rtc base=localtime \
    -serial "file:$LOG" -no-reboot \
    -display none -qmp "unix:$SOCK,server,nowait" &
QPID=$!

fail() {
    echo "FAIL ($MODE mode, ESP=$ESP): $1"
    echo "----- serial output -----"
    cat "$LOG"
    echo "-------------------------"
    exit 1
}

# Ordered wait: each marker is polled for in turn, so a run that hangs before
# reaching it times out pointing at the RIGHT step instead of a generic
# "LOGIT_BOOT_OK never appeared" that could mean anything from "OVMF never
# found the ESP" to "the kernel triple-faulted".
wait_for() {
    local pattern="$1" what="$2" budget="$3"
    for _ in $(seq 1 "$budget"); do
        grep -aq -- "$pattern" "$LOG" 2>/dev/null && return 0
        kill -0 "$QPID" 2>/dev/null || { fail "qemu exited before '$what' appeared"; }
        sleep 0.1
    done
    fail "'$what' not seen within $(python3 -c "print($budget/10)")s"
}

echo "--- UEFI boot ($MODE mode): ESP=$ESP disk=$DISK"

wait_for '\[efi\] gop'    "[efi] gop"    150
wait_for '\[efi\] mmap'   "[efi] mmap"   100
wait_for '\[efi\] ebs ok' "[efi] ebs ok" 100
wait_for '\[efi\] jump'   "[efi] jump"   50

# Loader done its part; both modes must have gotten this far identically.
line_of() { grep -an -- "$1" "$LOG" | head -1 | cut -d: -f1; }
L1=$(line_of '\[efi\] gop'); L2=$(line_of '\[efi\] mmap')
L3=$(line_of '\[efi\] ebs ok'); L4=$(line_of '\[efi\] jump')
[ "$L1" -lt "$L2" ] && [ "$L2" -lt "$L3" ] && [ "$L3" -lt "$L4" ] \
    || fail "loader breadcrumbs out of order (lines $L1,$L2,$L3,$L4) -- the contract is GOP, then mmap, then ExitBootServices, then jump"

if [ "$MODE" = "negctl" ]; then
    # The control: give the kernel 20s to prove it does NOT come up, then
    # declare victory. A longer wait would just be a slower correct test; a
    # shorter one risks catching a slow-but-honest LOGIT_BOOT_OK and calling
    # it a pass for the wrong reason, so this mirrors the budget the positive
    # path spends between "[efi] jump" and "LOGIT_BOOT_OK" (see below).
    for _ in $(seq 1 200); do
        if grep -aq "LOGIT_BOOT_OK" "$LOG" 2>/dev/null; then
            fail "CONTROL FAILED: LOGIT_BOOT_OK appeared with a deliberately corrupted Multiboot2 magic -- boot.asm's check_multiboot is not being run, or not being reached, on the UEFI path"
        fi
        kill -0 "$QPID" 2>/dev/null || break   # a triple fault ending the VM is also "did not boot" -- fine
        sleep 0.1
    done
    echo "PASS (negctl): loader ran its full hand-off ([efi] gop/mmap/ebs ok/jump all seen, in order) and the kernel refused the corrupted magic -- no LOGIT_BOOT_OK in 20s"
    grep -a '^\[efi\]' "$LOG" | sed 's/^/    /'
    exit 0
fi

# --- positive path only, from here -----------------------------------------

wait_for '\[smp\] 2 CPU(s) detected' "[smp] 2 CPU(s) detected" 150
wait_for 'LOGIT_BOOT_OK'             "LOGIT_BOOT_OK"           200

# Re-check ordering now that the kernel's two lines are in the log too.
L5=$(line_of '\[smp\] 2 CPU'); L6=$(line_of 'LOGIT_BOOT_OK')
[ "$L4" -lt "$L5" ] && [ "$L5" -lt "$L6" ] \
    || fail "kernel lines out of order relative to the jump (lines $L4,$L5,$L6)"

# THE REGAINED CAPABILITY, ASSERTED SO IT CANNOT SILENTLY STOP BEING TRUE.
# This is the assertion the milestone DESIGNED to be rewritten: it shipped
# asserting "[acpi] no RSDP" PRESENT (the kernel could not yet read the
# loader's forwarded mb2 tag 15), precisely so that landing
# c/boot/efi/acpi-mb2-tag.patch would break the gate and force this flip.
# The patch landed; now the inverse pair is load-bearing. MADT-from-UEFI is
# proven by the second CPU actually coming online -- a printed RSDP address
# alone would only prove the tag was parsed, not that the tables behind it
# were reachable and sane.
grep -aq '\[acpi\] no RSDP' "$LOG" \
    && fail "'[acpi] no RSDP' printed on the UEFI path -- acpi.c stopped consuming the loader's forwarded RSDP (mb2 tag 15); the acpi-mb2-tag.patch capability regressed"
grep -aq '\[smp\] 2/2 CPUs online' "$LOG" \
    || fail "second CPU never came online under UEFI -- MADT-from-forwarded-RSDP is broken (or SMP regressed on this path)"

wait_for '\[wm\] desktop live' "[wm] desktop live" 300
sleep 2   # let the first frames land after the desktop reports live (shot.sh's idiom)

mkdir -p "$(dirname "$PNG")"
python3 - "$SOCK" "$PPM" <<'PY'
import json, socket, sys
sock, ppm = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX); s.connect(sock)
f = s.makefile('rw'); f.readline()
def cmd(c, **a):
    f.write(json.dumps({'execute': c, 'arguments': a} if a else {'execute': c}) + '\n'); f.flush()
    while True:
        r = json.loads(f.readline())
        if 'return' in r or 'error' in r: return r
cmd('qmp_capabilities')
r = cmd('screendump', filename=ppm)
if 'error' in r:
    print('screendump failed:', r['error']); sys.exit(1)
PY
[ $? -eq 0 ] || fail "QMP screendump failed"
[ -s "$PPM" ] || fail "screendump produced an empty file"

# PPM -> PNG (zlib is stdlib; PNG filter-0 scanlines are a byte away from raw
# RGB) and the colour-count idiom from tools/shot.sh: a real desktop paints
# far more than a handful of colours, a panic or a black screen does not.
COLOURS=$(python3 - "$PPM" "$PNG" <<'PY'
import struct, sys, zlib
ppm, out = sys.argv[1], sys.argv[2]
d = open(ppm, 'rb').read()
i = d.index(b'255\n') + 4
w, h = (int(x) for x in d[:i].split()[1:3])
px = d[i:]
raw = b''.join(b'\x00' + px[y*w*3:(y+1)*w*3] for y in range(h))
def chunk(t, data):
    c = t + data
    return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c))
open(out, 'wb').write(
    b'\x89PNG\r\n\x1a\n'
    + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    + chunk(b'IDAT', zlib.compress(raw, 6))
    + chunk(b'IEND', b''))
step = 7
seen = {px[((y*w + x) * 3):((y*w + x) * 3) + 3] for y in range(0, h, step) for x in range(0, w, step)}
print(len(seen))
PY
)
[ -n "$COLOURS" ] || fail "could not read back the screendump"
[ "$COLOURS" -gt 32 ] || fail "guest scanout has only $COLOURS distinct colours sampled -- that is a panic/black screen, not a desktop ($PNG)"

echo "PASS: UEFI loader hand-off verified in order (gop -> mmap -> ebs ok -> jump -> [smp] 2 CPU(s) via forwarded RSDP -> LOGIT_BOOT_OK -> desktop live), screendump $PNG has $COLOURS distinct colours"
grep -a -e '^\[efi\]' -e '^\[acpi\]' -e '^\[blk\]' -e '^\[wm\] desktop live' "$LOG" | sed 's/^/    /'
exit 0
