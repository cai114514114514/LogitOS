#!/usr/bin/env bash
# THE WHOLE PRODUCT IN ONE ASSERTION: a user types n i h a o and a space into
# TextEdit, presses Ctrl+S, and the file on the disk contains
#
#     E4 BD A0 E5 A5 BD      (U+4F60 U+597D -- the two characters)
#
# Nothing on this machine could produce those bytes from a keyboard yesterday.
#
# WHY THE BYTES AND NOT A SCREENSHOT. A screenshot of CJK text proves the FONT
# works, which it already did -- zh.wikipedia.org has rendered in the browser
# since M14. The question this gate exists for is whether a codepoint survives
# the whole path (PS/2 scancode -> keyboard.c -> wm_key -> the input ring ->
# ime_ui.c -> EV_KEY -> the app's UTF-8 encoder -> SYS_WRITE_FILE -> LogitFS ->
# media), and only the bytes answer that. CLAUDE.md's scoreboard section makes
# the same point one level up: `changed px` cannot tell a rendered page from a
# flat dark block.
#
# NO -snapshot, on a COPY of the disk. Same trade as tests/boot/run-core-test.sh
# and for the same reason: the write has to reach the host, and it must not leak
# into the image every other harness shares.
#
# THE NEGATIVE CONTROL (--negctl) IS THE SAME RUN WITHOUT Ctrl+Space, and it is
# the only thing that makes the positive mean anything. `nihao ` in the file
# proves the keys arrived, the editor received them, the save worked and the
# extractor can read what was written -- so when the positive run produces two
# Han characters instead, the TOGGLE is the only thing that differs and
# therefore the only thing that can have caused it. Without this control the
# positive is equally consistent with "TextEdit has a Chinese mode".
#
#   usage: run-ime-test.sh <iso> <disk.img> [--negctl]
set -u

ISO="${1:?usage: run-ime-test.sh <iso> <disk.img> [--negctl]}"
DISK="${2:?usage: run-ime-test.sh <iso> <disk.img> [--negctl]}"
MODE="ime"
WANT_HEX="e4bda0e5a5bd"
WANT_DESC="the two Han characters U+4F60 U+597D"
if [ "${3:-}" = "--negctl" ]; then
    MODE="ascii"
    WANT_HEX="6e6968616f20"
    WANT_DESC="the ASCII letters 'nihao '"
fi

WORKDISK="$(mktemp -u).img"
SHOT="$(mktemp -u).ppm"
OUT="$(mktemp -u).txt"
cleanup() { rm -f "$WORKDISK" "$SHOT" "$SHOT.log" "$OUT"; }
trap cleanup EXIT

[ -f "$ISO" ]  || { echo "FAIL: $ISO is not built"; exit 1; }
[ -f "$DISK" ] || { echo "FAIL: $DISK is not built"; exit 1; }
cp "$DISK" "$WORKDISK"

echo "--- driving the machine (mode=$MODE) ---"
python3 tests/boot/ime_type.py "$ISO" "$WORKDISK" "$MODE" "$SHOT" || {
    echo "FAIL: the machine never got as far as saving"
    [ -f "$SHOT.log" ] && tail -40 "$SHOT.log"
    exit 1
}

# READ THE FILE OFF THE IMAGE, not off the serial console. The guest printed it
# too (ime_type.py cats it) and that print is worth having in the log, but a
# guest reading back its own write is one filesystem agreeing with itself; this
# is the host parsing LogitFS v4 with an independent implementation.
echo "--- reading /untitled.txt out of the image ---"
python3 tests/boot/lfs_extract.py "$WORKDISK" /untitled.txt "$OUT" || {
    echo "FAIL: /untitled.txt is not on the image -- the save did not land"
    echo "      (image root follows)"
    python3 tests/boot/lfs_extract.py "$WORKDISK" --ls
    exit 1
}

GOT="$(od -An -tx1 -v "$OUT" | tr -d ' \n')"
echo "  wanted $WANT_HEX  ($WANT_DESC)"
echo "  got    $GOT"

if [ "$GOT" = "$WANT_HEX" ]; then
    echo "PASS: $WANT_DESC reached the disk"
    exit 0
fi

# A TRAILING NEWLINE OR A SECOND SAVE IS STILL A FAIL, deliberately. This gate
# is about exact bytes; softening it to a substring match is how a test starts
# passing on a file that also contains the romanisation.
echo "FAIL: the file does not hold $WANT_DESC"
if [ "$MODE" = "ime" ] && [ "$GOT" = "6e6968616f20" ]; then
    echo "      it holds the ASCII 'nihao ' instead -- the keys arrived and the"
    echo "      IME did not consume them. Look for '[ime] window' on the serial log:"
    [ -f "$SHOT.log" ] && grep -a '\[ime\]' "$SHOT.log" | head -20
fi
exit 1
