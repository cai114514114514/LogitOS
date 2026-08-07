#!/usr/bin/env bash
# Deliberately broken builds, for showing a test go RED.
#
#   tools/break-build.sh <mode> [args]        -> prints the path of a disk image
#
# A green test that has never been shown to fail is not evidence. Every claim
# tests/boot/run-fullsystem-test.sh makes has to be demonstrated against a
# machine that really is broken in that exact way, and doing that by hand -- and
# then describing it in a commit message -- is how "I checked" becomes something
# nobody can repeat. This makes the broken machine a command.
#
# Modes (each writes a NEW image under build/ and leaves build/disk.img alone):
#
#   drop-app <name.aex>    the app is missing from the disk entirely. The dock
#                          has one fewer icon; the root inventory assertion and
#                          anything that needed that app must fail.
#   corrupt-app <name.aex> the .aex header survives (so it still registers in
#                          the Dock) but the ELF inside is shredded. The icon is
#                          there and clicking it must produce "launch: load
#                          failed", not a launched app.
#   wipe-superblock        the first 4 KiB of the image is zeroed: nothing
#                          mounts. Everything downstream of the filesystem must
#                          fail, and the mount assertion must be the one that
#                          says why.
#
# The network fault needs no image at all: FS_NIC=off boots with no card.
#
# drop-app rebuilds the image with tools/mkfs.py from the same argument list the
# Makefile uses, minus one entry, so the result is a REAL filesystem rather than
# a damaged one -- "this app was never shipped" and "this app's bytes are
# corrupt" are different failures and the test should be able to tell them apart.

set -u

MODE="${1:?usage: break-build.sh drop-app <a.aex> | corrupt-app <a.aex> | wipe-superblock}"
shift || true

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1
BUILD=build
# SRC lets the caller point at a snapshot of the image rather than the live
# build/disk.img -- which matters on a tree several sessions are building in,
# where the file under you can be rewritten mid-test.
SRC="${SRC:-$BUILD/disk.img}"
[ -f "$SRC" ] || { echo "break-build: $SRC missing -- run make first" >&2; exit 1; }

case "$MODE" in
drop-app)
    APP="${1:?drop-app needs an .aex name, e.g. textedit.aex}"
    OUT="$BUILD/disk-drop-${APP%.aex}.img"
    # Ask make what the disk rule would do, then run it again without one file.
    # Re-deriving the argument list here would be a second copy of a list that
    # has gone stale before.
    ARGS="$(make -n "$BUILD/disk.img" 2>/dev/null | tr '\n' ' ' | tr -d '\\' |
            sed -n 's/.*mkfs\.py [^ ]*\(.*\)/\1/p')"
    if [ -z "$ARGS" ]; then
        # disk.img was up to date, so make printed nothing: force it.
        touch tools/mkfs.py
        ARGS="$(make -n "$BUILD/disk.img" 2>/dev/null | tr '\n' ' ' |
                sed -n 's/.*mkfs\.py [^ ]*\(.*\)/\1/p')"
    fi
    [ -n "$ARGS" ] || { echo "break-build: could not read the mkfs argument list" >&2; exit 1; }
    NEW=""
    for a in $ARGS; do
        case "$a" in
            *":$APP"|*"/$APP:"*) continue ;;      # the root copy of this app
        esac
        NEW="$NEW $a"
    done
    # shellcheck disable=SC2086
    python3 tools/mkfs.py "$OUT" $NEW >/dev/null || exit 1
    echo "$OUT"
    ;;

corrupt-app)
    APP="${1:?corrupt-app needs an .aex name}"
    OUT="$BUILD/disk-corrupt-${APP%.aex}.img"
    cp "$SRC" "$OUT" || exit 1
    APP_SRC="$BUILD/$APP"
    [ -f "$APP_SRC" ] || { echo "break-build: $APP_SRC not found" >&2; exit 1; }
    python3 - "$OUT" "$APP_SRC" <<'PY' || exit 1
import sys
img, src = sys.argv[1], sys.argv[2]
head = open(src, "rb").read(4096)
data = bytearray(open(img, "rb").read())
at = data.find(head)
if at < 0:
    sys.exit("break-build: could not find the app's first block in the image")
# Keep the 64-byte AEX header intact -- the Dock reads only that, so the icon
# still appears and the failure is "it would not load", not "it is not there".
# Everything after it (the ELF header and the first program headers) is filled
# with a byte that is not a valid ELF magic, an opcode, or anything else.
for i in range(64, 4096):
    data[at + i] = 0x5A
open(img, "wb").write(bytes(data))
print("corrupted %s at offset %d (AEX header preserved)" % (src, at), file=sys.stderr)
PY
    echo "$OUT"
    ;;

wipe-superblock)
    OUT="$BUILD/disk-nofs.img"
    cp "$SRC" "$OUT" || exit 1
    dd if=/dev/zero of="$OUT" bs=4096 count=1 conv=notrunc status=none || exit 1
    echo "$OUT"
    ;;

*)
    echo "break-build: unknown mode '$MODE'" >&2
    exit 2
    ;;
esac
