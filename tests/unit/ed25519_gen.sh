#!/usr/bin/env bash
# Generate Ed25519 cases with openssl for tests/unit/ed25519_test.c.
#
# Why openssl and not our own signer: a vector we produced ourselves proves only
# that we are self-consistent. These are keys openssl generated, over messages
# /dev/urandom generated, signed by openssl's Ed25519. Our verifier only gets to
# say yes or no.
#
#   ed25519_gen.sh <outfile> [count]
#
# One case per line:  pub_hex  msg_hex(or "-" for empty)  sig_hex
set -euo pipefail

OUT="${1:?usage: ed25519_gen.sh <outfile> [count]}"
N="${2:-12}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

: > "$OUT"

for i in $(seq 1 "$N"); do
    openssl genpkey -algorithm ED25519 -out "$TMP/k.pem" 2>/dev/null

    # The raw 32-byte public key is the last 32 bytes of the DER SPKI
    # (12-byte prefix: SEQUENCE, SEQUENCE{OID}, BIT STRING). Take it that way
    # rather than by parsing, so this script has no DER logic of its own.
    openssl pkey -in "$TMP/k.pem" -pubout -outform DER -out "$TMP/pub.der" 2>/dev/null
    PUB=$(tail -c 32 "$TMP/pub.der" | xxd -p | tr -d '\n')

    # message length 0 on the first case, then a spread including a length that
    # straddles SHA-512's 128-byte block boundary.
    # No zero-length case: `openssl pkeyutl -sign -rawin` refuses an empty input
    # file (rc=1, "buffer" error) even though Ed25519 is defined for it. The
    # empty message is covered by RFC 8032 TEST 1, which is compiled into the
    # test, so nothing is lost -- it just cannot come from here.
    case "$i" in
        1) LEN=1 ;;
        2) LEN=2 ;;
        3) LEN=127 ;;
        4) LEN=128 ;;
        5) LEN=129 ;;
        *) LEN=$(( (RANDOM % 500) + 1 )) ;;
    esac

    if [ "$LEN" -eq 0 ]; then
        : > "$TMP/m.bin"
        MSG="-"
    else
        head -c "$LEN" /dev/urandom > "$TMP/m.bin"
        MSG=$(xxd -p "$TMP/m.bin" | tr -d '\n')
    fi

    # Ed25519 is a one-shot algorithm: openssl requires -rawin and no -sha*.
    openssl pkeyutl -sign -inkey "$TMP/k.pem" -rawin -in "$TMP/m.bin" -out "$TMP/s.bin" 2>/dev/null
    SIG=$(xxd -p "$TMP/s.bin" | tr -d '\n')

    echo "$PUB $MSG $SIG" >> "$OUT"
done

echo "ed25519_gen: $N openssl cases -> $OUT"
