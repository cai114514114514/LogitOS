#!/usr/bin/env bash
# secp256k1 differential against OpenSSL 3.6.3+ (needs `ecparam -list_curves`
# to list secp256k1 -- verified present), byte for byte, in both directions.
#
# WHY A DIFFERENTIAL ON TOP OF THE WYCHEPROOF KAT (secp256k1_test.c). Wycheproof
# is verify-only: it can never exercise secp256k1_sign or secp256k1_keygen at
# all, so a signer or a keygen that quietly compute the wrong thing (while
# still agreeing with itself) would be invisible to it. This script is the
# other half:
#   A KEYGEN   the SAME raw private scalar, handed to both openssl (wrapped as
#              a SEC1 ECPrivateKey with the secp256k1 OID) and to
#              secp256k1_keygen, must derive the SAME public point.
#   B SIGN     we sign; openssl, given only the public key, must accept it --
#              and must reject the same signature over a different message.
#   C VERIFY   openssl signs (with ITS OWN k, not ours) over the SAME private
#              key; secp256k1_verify_der, given only the public key, must
#              accept it -- and reject it over a tampered message.
# B alone cannot see a signer that is subtly wrong in a way our own verifier
# shares (both files never share code, but nothing stops them making the same
# mistake independently); C alone cannot see a verifier that is too lenient in
# a way that happens not to matter for openssl's own signatures. Running both
# closes that gap the same way ecdsa_sign_test.c's header argues.
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${BUILD:-$ROOT/build}/secp256k1x"
CC="${CC:-clang}"
OPENSSL="${OPENSSL:-openssl}"
NTRIAL="${NTRIAL:-6}"
mkdir -p "$BUILD"

command -v "$OPENSSL" >/dev/null || { echo "SKIP: no openssl"; exit 0; }
"$OPENSSL" ecparam -list_curves 2>/dev/null | grep -qi secp256k1 || {
    echo "SKIP: this openssl build has no secp256k1"; exit 0; }
PYTHON3="${PYTHON3:-python3}"
command -v "$PYTHON3" >/dev/null || { echo "SKIP: no python3"; exit 0; }

# CTL is the negative control (test-secp256k1-negctl): builds the CLI with
# SECP256K1_CTL_NIST_DOUBLE (the a=-3 shortcut wired onto secp256k1's a=0
# curve -- see secp256k1.h) and the verdict below is INVERTED, so the run
# passes only if this gate actually notices.
CTL="${SECP256K1_CTL:-}"
CTLDEF=""
[ -n "$CTL" ] && CTLDEF="-D$CTL"

# shellcheck disable=SC2086
$CC -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all \
    $CTLDEF -o "$BUILD/secp256k1_cli" \
    "$ROOT/tests/unit/secp256k1_cli.c" "$ROOT/c/crypto/pubkey/secp256k1.c" \
    -I"$ROOT/c/crypto" -I"$ROOT/c/crypto/pubkey" || {
        echo "FAIL: could not build secp256k1_cli"; exit 1; }

pass=0; fail=0
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); echo "  FAIL: $*"; }

# ---- shared DER helpers (python, ASN.1 minimal-DER, matches run-ecdsa-sign.sh) --
DERWRAP="$BUILD/derwrap.py"
cat > "$DERWRAP" << 'PY'
import sys, os
def der(tag, body):
    if len(body) < 0x80: return bytes([tag, len(body)]) + body
    nb = (len(body).bit_length()+7)//8
    return bytes([tag, 0x80|nb]) + len(body).to_bytes(nb, 'big') + body
def integer(v):
    b = v.to_bytes((v.bit_length()+8)//8 or 1, 'big')
    return der(0x02, b)
OID = bytes.fromhex('2b8104000a')  # 1.3.132.0.10, secp256k1 (SEC 2 / X9.62)

cmd = sys.argv[1]
if cmd == 'privkey':                      # privkey <build> <privhex> -> k.der (RFC 5915)
    build, priv = sys.argv[2], sys.argv[3]
    body = der(0x02, b'\x01') + der(0x04, bytes.fromhex(priv)) + der(0xa0, der(0x06, OID))
    open(os.path.join(build, 'k.der'), 'wb').write(der(0x30, body))
elif cmd == 'sig':                        # sig <build> <r||s hex> -> sig.der
    build, raw = sys.argv[2], bytes.fromhex(sys.argv[3])
    f = len(raw)//2
    r = int.from_bytes(raw[:f], 'big'); s = int.from_bytes(raw[f:], 'big')
    open(os.path.join(build, 'sig.der'), 'wb').write(der(0x30, integer(r) + integer(s)))
PY

run_suite() {
    pass=0; fail=0
    local CLI="$BUILD/secp256k1_cli"
    for i in $(seq 1 "$NTRIAL"); do
        local priv pub ossl_pub hash k sig
        priv=$("$PYTHON3" -c "import secrets; print(secrets.token_hex(32))")

        # A. keygen agreement
        pub=$("$CLI" keygen "$priv")
        "$PYTHON3" "$DERWRAP" privkey "$BUILD" "$priv"
        "$OPENSSL" pkey -inform DER -in "$BUILD/k.der" -pubout -out "$BUILD/pub.pem" 2>/dev/null
        "$OPENSSL" pkey -pubin -in "$BUILD/pub.pem" -outform DER -out "$BUILD/pub.der" 2>/dev/null
        ossl_pub=$(tail -c 65 "$BUILD/pub.der" 2>/dev/null | xxd -p | tr -d '\n')
        if [ "$pub" = "$ossl_pub" ] && [ -n "$pub" ]; then ok; else bad "A keygen (trial $i): ours=$pub openssl=$ossl_pub"; fi

        printf 'trial %d payload' "$i" > "$BUILD/msg.bin"
        hash=$("$OPENSSL" dgst -sha256 -binary "$BUILD/msg.bin" | xxd -p | tr -d '\n')
        k=$("$PYTHON3" -c "import secrets; print(secrets.token_hex(32))")

        # B. we sign; openssl verifies
        sig=$("$CLI" sign "$priv" "$hash" "$k")
        if [ "$sig" = "REJECTED" ] || [ -z "$sig" ]; then bad "B sign (trial $i) produced nothing"; continue; fi
        "$PYTHON3" "$DERWRAP" sig "$BUILD" "$sig"
        if "$OPENSSL" dgst -sha256 -verify "$BUILD/pub.pem" -signature "$BUILD/sig.der" "$BUILD/msg.bin" >/dev/null 2>&1
        then ok; else bad "B openssl rejected our signature (trial $i)"; fi

        printf 'a different message' > "$BUILD/wrong.bin"
        if "$OPENSSL" dgst -sha256 -verify "$BUILD/pub.pem" -signature "$BUILD/sig.der" "$BUILD/wrong.bin" >/dev/null 2>&1
        then bad "B openssl accepted our signature over the WRONG message (trial $i)"; else ok; fi

        # C. openssl signs (its own k); we verify
        "$OPENSSL" dgst -sha256 -sign "$BUILD/k.der" -out "$BUILD/osig.der" "$BUILD/msg.bin" 2>/dev/null
        local osighex verdict
        osighex=$(xxd -p "$BUILD/osig.der" 2>/dev/null | tr -d '\n')
        verdict=$("$CLI" verify "$pub" "$osighex" "$hash")
        if [ "$verdict" = "valid" ]; then ok; else bad "C we rejected openssl's signature (trial $i)"; fi

        verdict=$("$CLI" verify "$pub" "$osighex" "$(printf 'trial %d payload!' "$i" | "$OPENSSL" dgst -sha256 -binary | xxd -p | tr -d '\n')")
        if [ "$verdict" = "invalid" ]; then ok; else bad "C we accepted openssl's signature over the WRONG hash (trial $i)"; fi
    done
}

if [ -z "$CTL" ]; then
    run_suite
    echo "secp256k1 vs $($OPENSSL version | cut -d' ' -f1-2): $pass passed, $fail failed ($NTRIAL trials x 5 checks)"
    [ "$fail" -eq 0 ]
    exit $?
fi

# ------------------------------------------------------------- the control --
echo "== secp256k1 negative control ($CTL): MUST fail =="
run_suite
echo "  $CTL: $pass passed, $fail failed"
if [ "$fail" -gt 0 ]; then
    echo "NEGCTL OK: $CTL was caught ($fail/$((pass+fail)) checks reddened)"
    exit 0
else
    echo "NEGCTL FAILED: $CTL changed nothing this gate could see"
    exit 1
fi
