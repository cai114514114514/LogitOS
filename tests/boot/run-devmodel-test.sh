#!/usr/bin/env bash
# Device-model / PCI enumeration boot test.
#
# The point of this test is that it runs the SAME kernel over TWO different
# machine configurations and asserts, by CLASS CODE, that enumeration followed.
# "Works on the one machine we always boot" is the exact failure the device
# model exists to prevent, so a single device set would not be evidence.
#
#   SET=a  i440fx, no MCFG (legacy 0xCF8 config ports)
#          virtio-blk + virtio-gpu + e1000 + QEMU's `edu` device
#          -> asserts ethernet (02.00), display (03.xx), and that an MSI and a
#             legacy INTx interrupt each ACTUALLY REACHED a handler
#
#   SET=b  q35, MCFG present (ECAM), a completely different device set:
#          rtl8139 instead of e1000, plus ich9-ahci, plus an xHCI controller
#          BEHIND A PCIe ROOT PORT (so it is not on bus 0 at all)
#          -> asserts ethernet (02.00), SATA/AHCI (01.06), USB/xHCI (0c.03),
#             that ECAM is in use, and that the xHCI was found on a NON-ZERO bus
#
# Set B is the negative-control shape: every one of its assertions fails against
# a kernel that can only look up an exact vendor:device on bus 0.
set -u

ISO="${1:?usage: run-devmodel-test.sh <iso> [disk.img]}"
DISK="${2:-}"
SET="${SET:-a}"
QEMU="${QEMU:-qemu-system-x86_64}"
LOG="$(mktemp)"

cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${QPID:-}" ] && wait "$QPID" 2>/dev/null
    rm -f "$LOG"
}
trap cleanup EXIT

DISK_ARGS=""
[ -n "$DISK" ] && DISK_ARGS="-drive file=$DISK,format=raw,if=none,id=hd0,file.locking=off -device virtio-blk-pci,drive=hd0 -boot d -snapshot"

case "$SET" in
a)
    MACHINE="-M pc"
    DEVS="-netdev user,id=n0 -device e1000,netdev=n0 -vga none -device virtio-gpu-pci -device edu"
    WANT=(
        "[pci] no MCFG"                     # legacy config-port path in use
        "class=02.00"                       # ethernet, by class not by ID
        "class=03."                         # a display controller
        "LOGIT_IRQ_OK msi mode=msi"         # an MSI reached a handler
        "LOGIT_IRQ_OK legacy mode=intx"     # ... and so did a legacy INTx
    )
    ;;
b)
    MACHINE="-M q35"
    DEVS="-netdev user,id=n0 -device rtl8139,netdev=n0 -vga none -device virtio-gpu-pci \
          -device ich9-ahci,id=ahci1 \
          -device pcie-root-port,id=rp0,chassis=1,slot=1 \
          -device qemu-xhci,bus=rp0,id=xhci0"
    WANT=(
        "[pci] ECAM seg 0"                  # MCFG found, ECAM in use
        "class=02.00"                       # rtl8139 -- a NIC we have no driver for
        "class=01.06"                       # AHCI, by class
        "class=0c.03"                       # xHCI, by class
    )
    ;;
*)  echo "unknown SET='$SET' (want a|b)" >&2; exit 2 ;;
esac

# shellcheck disable=SC2086
"$QEMU" -cpu "${QEMU_CPU:-max}" $MACHINE -cdrom "$ISO" $DISK_ARGS $DEVS \
        -m 512M -smp 4 -accel tcg,thread=multi \
        -serial "file:$LOG" -display none -no-reboot >/dev/null 2>&1 &
QPID=$!

for _ in $(seq 1 400); do
    grep -aq "LOGIT_BOOT_OK" "$LOG" 2>/dev/null && break
    kill -0 "$QPID" 2>/dev/null || break
    sleep 0.1
done

fail=0
if ! grep -aq "LOGIT_BOOT_OK" "$LOG" 2>/dev/null; then
    echo "FAIL[$SET]: kernel did not reach LOGIT_BOOT_OK"
    fail=1
fi
for w in "${WANT[@]}"; do
    if grep -aqF -- "$w" "$LOG"; then
        echo "ok  [$SET] $w"
    else
        echo "FAIL[$SET]: expected '$w' in the boot log"
        fail=1
    fi
done

# Set B additionally requires that the xHCI was found on a bus OTHER than 0 --
# it sits behind a PCIe root port. A bus-0-only scan cannot see it at all, which
# is what makes this assertion a real negative control rather than a restatement.
if [ "$SET" = b ]; then
    if grep -aE '^\[dev\] 0000:(0[1-9a-f]|[1-9a-f][0-9a-f]):[0-9a-f]{2}\.[0-7] .* class=0c\.03' "$LOG" >/dev/null; then
        echo "ok  [b] xHCI enumerated behind a bridge (non-zero bus)"
    else
        echo "FAIL[b]: no 0c.03 device on a non-zero bus (bridge recursion did not run)"
        fail=1
    fi
fi

if [ "$fail" != 0 ]; then
    echo "----- serial output -----"
    grep -aE '^\[(pci|dev|irq)\]|LOGIT_' "$LOG" || cat "$LOG"
    echo "-------------------------"
    exit 1
fi
echo "PASS: device set '$SET' enumerated as expected"
exit 0
