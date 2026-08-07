#!/usr/bin/env bash
# The full-system boot test: one command that says the SYSTEM works.
#
#   tests/boot/run-fullsystem-test.sh <iso> <disk.img> [root .aex ...]
#
# `make test` asserts LOGIT_BOOT_OK, and asserted it happily on a build where
# no application could load. This boots the same ISO and then makes ~20 named
# assertions about the running machine -- filesystem, shell, every dock app,
# the network, the browser's pixels, and the keyboard after all of it. The
# assertions live in tests/qmp/qmp_fullsystem.py; this script is the part that
# knows how the tree is laid out and how to fail loudly.
#
# The trailing .aex paths are the ones the Makefile packs at the LogitFS root.
# They are how the test knows what SHOULD be in the Dock: it reads each header's
# display name on the host and requires the guest to list, and launch, exactly
# those. Passing them from the Makefile rather than hardcoding a list here is
# deliberate -- a list in a test file goes stale the first time an app is added
# and then quietly stops testing the new one.
#
# Environment (all optional):
#   FS_NIC=off     boot with no network card    (fault injection)
#   FS_MODE=WxH    boot at another display mode (default 1280x800)
#   FS_OUT=dir     keep screendumps + serial log here
#   QEMU, QEMU_CPU as elsewhere in the tree

set -u

ISO="${1:?usage: run-fullsystem-test.sh <iso> <disk.img> [root .aex ...]}"
DISK="${2:?usage: run-fullsystem-test.sh <iso> <disk.img> [root .aex ...]}"
shift 2

HERE="$(cd "$(dirname "$0")" && pwd)"
DRIVER="$HERE/../qmp/qmp_fullsystem.py"

for f in "$ISO" "$DISK" "$DRIVER"; do
    [ -f "$f" ] || { echo "FAIL: missing $f" >&2; exit 1; }
done

QEMU="${QEMU:-qemu-system-x86_64}"
command -v "$QEMU"   >/dev/null 2>&1 || { echo "FAIL: $QEMU not found" >&2; exit 1; }
command -v python3   >/dev/null 2>&1 || { echo "FAIL: python3 not found" >&2; exit 1; }

# The harness talks to the guest over a Unix-domain socket chardev, which needs
# a real POSIX host. Saying so here beats a stack trace three minutes into a
# boot.
case "$(uname -s)" in
    Linux|Darwin) ;;
    *) echo "FAIL: run this under Linux/WSL -- the serial channel is a unix socket" >&2
       exit 1 ;;
esac

exec python3 "$DRIVER" "$ISO" "$DISK" "$@"
