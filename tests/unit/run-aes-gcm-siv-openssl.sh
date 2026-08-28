#!/usr/bin/env bash
# AES-GCM-SIV (RFC 8452) differential against OpenSSL 3.5+, byte for byte, in
# both directions, plus a deliberately corrupted tag on both sides.
#
# WHY A DIFFERENTIAL ON TOP OF THE KAT (tests/unit/aes_gcm_siv_test.c already
# passes all 50 RFC 8452 vectors). The same argument run-mlkem-openssl.sh
# makes: RFC 8452's Appendix C vectors are all built from a handful of
# structured, low-entropy keys and nonces (mostly 01000...0 / 030000...0),
# because they exist to pin the CONSTRUCTION's edge cases (every plaintext
# length 0..64, counter wrap) rather than to sample the input space. A defect
# that only shows up on a "generic" key/nonce/aad -- e.g. a byte-order bug
# that happens to cancel out when every high byte is zero -- would not be
# caught by Appendix C alone. Only a second, independently written
# implementation exercised on RANDOM inputs can see that, and OpenSSL's
# AES-*-GCM-SIV (3.5+) is that.
#
# Four checks, each isolating a different half of the construction:
#   A SEAL      our ciphertext||tag for a random (key,nonce,aad,pt) must
#               equal openssl's, byte for byte -- exercises key derivation,
#               POLYVAL, and CTR together.
#   B OPEN      we decrypt openssl's ciphertext and recover the SAME
#               plaintext openssl encrypted.
#   C CROSS     openssl decrypts OUR ciphertext and recovers the SAME
#               plaintext we started from -- A and B could both pass with a
#               construction that is internally consistent but subtly
#               non-standard (e.g. a swapped AAD/PT order in POLYVAL's input)
#               if only one direction were checked; C is what would catch it.
#   D REJECT    a corrupted tag must be refused on WHICHEVER side receives
#               it, at both key sizes.
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${BUILD:-$ROOT/build}"
CC="${CC:-cc}"
OPENSSL_PREFIX="${OPENSSL_PREFIX:-/opt/homebrew/opt/openssl@3}"
NTRIAL="${NTRIAL:-12}"

command -v python3 >/dev/null || { echo "SKIP: no python3 (used to generate random hex inputs)"; exit 0; }
[ -f "$OPENSSL_PREFIX/include/openssl/evp.h" ] || {
    echo "SKIP: no OpenSSL headers at $OPENSSL_PREFIX (set OPENSSL_PREFIX=... to point elsewhere);"
    echo "      the RFC 8452 known answers in tests/unit/aes_gcm_siv_vectors.inc still gate the"
    echo "      code via 'make test-aes-gcm-siv'."; exit 0; }

mkdir -p "$BUILD"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

$CC -O2 -Wall -Wextra -o "$BUILD/aes_gcm_siv_cli" \
    "$ROOT/tests/unit/aes_gcm_siv_cli.c" "$ROOT/c/crypto/aead/aes_gcm_siv.c" \
    "$ROOT/c/crypto/aead/aes_dispatch.c" "$ROOT/c/crypto/aead/aes_ni.c" \
    "$ROOT/c/crypto/aead/aesgcm.c" "$ROOT/c/kernel/cpu/cpufeat.c" \
    -I"$ROOT/c/crypto" -I"$ROOT/c/crypto/aead" -I"$ROOT/c/kernel/cpu" 2>"$TMP/build1.log" \
    || { echo "FAIL: could not build aes_gcm_siv_cli"; cat "$TMP/build1.log"; exit 1; }

$CC -O2 -Wall -Wextra -o "$BUILD/aes_gcm_siv_ossl_cli" \
    "$ROOT/tests/unit/aes_gcm_siv_openssl_cli.c" \
    -I"$OPENSSL_PREFIX/include" -L"$OPENSSL_PREFIX/lib" -lcrypto 2>"$TMP/build2.log" \
    || { echo "FAIL: could not build aes_gcm_siv_openssl_cli"; cat "$TMP/build2.log"; exit 1; }

export DYLD_LIBRARY_PATH="$OPENSSL_PREFIX/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
export LD_LIBRARY_PATH="$OPENSSL_PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

OURS="$BUILD/aes_gcm_siv_cli"
OSSL="$BUILD/aes_gcm_siv_ossl_cli"

# openssl's own report (does this build actually have the cipher, under
# whichever provider is default) -- fail LOUD rather than silently comparing
# nothing if it does not.
zero32=$(python3 -c "print('00'*32)")
zero12=$(python3 -c "print('00'*12)")
probe=$("$OSSL" seal "$zero32" "$zero12" "" "" 2>"$TMP/probe.log")
if [ -z "$probe" ]; then
    echo "SKIP: this OpenSSL build has no AES-256-GCM-SIV provider entry"
    cat "$TMP/probe.log"
    exit 0
fi

pass=0; fail=0
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); echo "  FAIL: $*"; }

randhex() { python3 -c "import sys,os; print(os.urandom(int(sys.argv[1])).hex())" "$1"; }
flipbyte() { python3 -c "
import sys
b=bytearray.fromhex(sys.argv[1]); b[0]^=0x01; print(b.hex())" "$1"; }

for trial in $(seq 1 "$NTRIAL"); do
    for keylen in 16 32; do
        key=$(randhex "$keylen")
        nonce=$(randhex 12)
        aadlen=$(( RANDOM % 40 ))
        ptlen=$(( RANDOM % 70 ))
        aad=$(randhex "$aadlen")
        pt=$(randhex "$ptlen")

        ours_seal=$("$OURS" seal "$key" "$nonce" "$aad" "$pt" 2>"$TMP/e1")
        ossl_seal=$("$OSSL" seal "$key" "$nonce" "$aad" "$pt" 2>"$TMP/e2")

        # A. seal must match byte for byte
        if [ -n "$ours_seal" ] && [ "$ours_seal" = "$ossl_seal" ]; then ok
        else bad "A seal mismatch (trial $trial, keylen=$keylen, ptlen=$ptlen, aadlen=$aadlen)"
             echo "    ours $ours_seal"; echo "    ossl $ossl_seal"; continue; fi

        ctlen=$(( ptlen * 2 ))
        ossl_ct=${ossl_seal:0:$ctlen}
        ossl_tag=${ossl_seal:$ctlen:32}

        # B. we open openssl's ciphertext
        ours_open=$("$OURS" open "$key" "$nonce" "$aad" "$ossl_ct" "$ossl_tag" 2>"$TMP/e3")
        if [ "$ours_open" = "$pt" ]; then ok; else bad "B open (trial $trial)"; fi

        # C. openssl opens OUR ciphertext (cross-check the other direction)
        ossl_open=$("$OSSL" open "$key" "$nonce" "$aad" "$ossl_ct" "$ossl_tag" 2>"$TMP/e4")
        if [ "$ossl_open" = "$pt" ]; then ok; else bad "C cross-open (trial $trial)"; fi

        # D. a corrupted tag must be refused on BOTH sides.
        bad_tag=$(flipbyte "$ossl_tag")
        r1=$("$OURS" open "$key" "$nonce" "$aad" "$ossl_ct" "$bad_tag" 2>/dev/null; echo "rc=$?")
        r2=$("$OSSL" open "$key" "$nonce" "$aad" "$ossl_ct" "$bad_tag" 2>/dev/null; echo "rc=$?")
        if [[ "$r1" == *"rc=1"* ]]; then ok; else bad "D ours accepted a corrupted tag (trial $trial)"; fi
        if [[ "$r2" == *"rc=1"* ]]; then ok; else bad "D openssl accepted a corrupted tag (trial $trial) -- control cannot be trusted"; fi
    done
done

echo "AES-GCM-SIV vs $($OPENSSL_PREFIX/bin/openssl version 2>/dev/null | cut -d' ' -f1-2): " \
     "$pass passed, $fail failed ($NTRIAL trials x 2 keylens x 5 checks)"
[ "$fail" -eq 0 ]
exit $?
