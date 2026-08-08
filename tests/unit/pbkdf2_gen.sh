#!/usr/bin/env bash
# openssl-generated PBKDF2 cases for tests/unit/pwhash_test.c.
#
# RFC 7914 s11 publishes two PBKDF2-HMAC-SHA256 vectors and nothing at all for
# SHA-384, and both of its vectors use dkLen=64 -- an exact multiple of neither
# 32 nor 48, but also never a value that lands ON the hash length or one byte
# either side of it, which is where a block-counting bug lives. These fill both
# gaps against an implementation that has never seen ours.
#
#   pbkdf2_gen.sh <outfile> [count]
#
# One case per line:  hlen  pw_hex  salt_hex  iters  dklen  dk_hex
set -eu

OUT="${1:?usage: pbkdf2_gen.sh <outfile> [count]}"
N="${2:-12}"
: > "$OUT"

emit() {
    local digest="$1" hlen="$2" pwhex="$3" salthex="$4" iters="$5" dklen="$6"
    local dk
    dk=$(openssl kdf -keylen "$dklen" -kdfopt "digest:$digest" \
            -kdfopt "hexpass:$pwhex" -kdfopt "hexsalt:$salthex" \
            -kdfopt "iter:$iters" -binary PBKDF2 2>/dev/null | xxd -p | tr -d '\n')
    if [ -z "$dk" ]; then
        echo "pbkdf2_gen: openssl kdf PBKDF2 produced nothing -- is this openssl 3.x?" >&2
        exit 1
    fi
    echo "$hlen $pwhex $salthex $iters $dklen $dk" >> "$OUT"
}

# The dkLen boundaries, both PRFs: one short of the hash, exactly it, one past.
emit SHA256 32 "$(printf 'password' | xxd -p)" "$(printf 'saltsalt' | xxd -p)" 1000 31
emit SHA256 32 "$(printf 'password' | xxd -p)" "$(printf 'saltsalt' | xxd -p)" 1000 32
emit SHA256 32 "$(printf 'password' | xxd -p)" "$(printf 'saltsalt' | xxd -p)" 1000 33
emit SHA384 48 "$(printf 'password' | xxd -p)" "$(printf 'saltsalt' | xxd -p)" 1000 47
emit SHA384 48 "$(printf 'password' | xxd -p)" "$(printf 'saltsalt' | xxd -p)" 1000 48
emit SHA384 48 "$(printf 'password' | xxd -p)" "$(printf 'saltsalt' | xxd -p)" 1000 49
# c=1 is the degenerate case where the XOR loop must not run at all.
emit SHA256 32 "$(printf 'p' | xxd -p)" "$(printf 's' | xxd -p)" 1 20
emit SHA384 48 "$(printf 'p' | xxd -p)" "$(printf 's' | xxd -p)" 1 20

i=8
while [ "$i" -lt "$N" ]; do
    PW=$(head -c $(( (RANDOM % 40) + 1 )) /dev/urandom | xxd -p | tr -d '\n')
    SALT=$(head -c $(( (RANDOM % 40) + 1 )) /dev/urandom | xxd -p | tr -d '\n')
    IT=$(( (RANDOM % 500) + 1 ))
    DK=$(( (RANDOM % 100) + 1 ))
    if [ $(( i % 2 )) -eq 0 ]; then emit SHA256 32 "$PW" "$SALT" "$IT" "$DK"
    else                            emit SHA384 48 "$PW" "$SALT" "$IT" "$DK"; fi
    i=$(( i + 1 ))
done

echo "pbkdf2_gen: $N openssl cases -> $OUT"
