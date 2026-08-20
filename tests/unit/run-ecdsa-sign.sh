#!/usr/bin/env bash
# ecdsa_sign gate: RFC 6979 agreement (an independent Python implementation)
# plus an openssl differential (a program that has never seen our source).
#
# The two halves fail on disjoint bugs, which is why both are here:
#   - a wrong k (biased, repeated, or a mis-seeded DRBG) passes openssl and
#     fails the RFC 6979 half;
#   - a wrong s computed in a way our own ecdsa_verify shares passes the
#     RFC 6979 half only if the k is right, and fails openssl otherwise.
# See the header of tests/unit/ecdsa_sign_test.c.
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${BUILD:-$ROOT/build}/tlsx"
CC="${CC:-clang}"
OPENSSL="${OPENSSL:-openssl}"
mkdir -p "$BUILD"

command -v "$OPENSSL" >/dev/null || { echo "SKIP: no openssl"; exit 0; }

# LOGIT_ECDSA_SIGN_BREAK is the negative control: it compiles a deliberate
# defect in and the verdict below is INVERTED, so the run passes only if the
# gate notices. See test-ecdsa-sign-negctl in tests/tlsx.mk.
BREAK="${LOGIT_ECDSA_SIGN_BREAK:-}"
BREAKDEF=""
[ -n "$BREAK" ] && BREAKDEF="-D$BREAK"

# shellcheck disable=SC2086
$CC -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all \
    $BREAKDEF -o "$BUILD/ecdsa_sign_test" \
    "$ROOT/tests/unit/ecdsa_sign_test.c" "$ROOT/c/crypto/pubkey/ecdsa.c" \
    "$ROOT"/c/crypto/hash/*.c "$ROOT"/c/crypto/kdf/*.c \
    -I"$ROOT/c/crypto" -I"$ROOT/c/crypto/aead" -I"$ROOT/c/crypto/hash" || {
        echo "FAIL: could not build ecdsa_sign_test"; exit 1; }

python3 "$ROOT/tests/unit/ecdsa6979_ref.py" > "$BUILD/ecdsa_ref.txt" || {
    echo "FAIL: the RFC 6979 reference did not run"; exit 1; }
NVEC=$(wc -l < "$BUILD/ecdsa_ref.txt")

echo "== ecdsa_sign vs RFC 6979 ($NVEC reference vectors) =="
kat_rc=0
"$BUILD/ecdsa_sign_test" "$BUILD" < "$BUILD/ecdsa_ref.txt" || kat_rc=1

# ---------------------------------------------------- openssl differential --
# Wrap our raw private scalar as a SEC1 ECPrivateKey (RFC 5915) so openssl can
# load it, take ITS idea of the public key, and ask it to verify OUR signature.
# Taking the public key from openssl rather than from our own ecdh_keygen is
# deliberate: if our keygen and our signer shared a wrong base point they would
# agree with each other, and a public key we also produced could not tell.
echo "== ecdsa_sign vs openssl ($($OPENSSL version)) =="
oss_pass=0; oss_fail=0
for c in 256 384 521; do
    case $c in
        256) curve=prime256v1; md=sha256 ;;
        384) curve=secp384r1;  md=sha384 ;;
        521) curve=secp521r1;  md=sha512 ;;
    esac
    [ -s "$BUILD/c$c.sig" ] || { echo "FAIL P-$c: the test emitted no signature"; oss_fail=$((oss_fail+1)); continue; }

    python3 - "$BUILD" "$c" "$curve" <<'PY'
import sys, subprocess, os
build, cid, curve = sys.argv[1], sys.argv[2], sys.argv[3]
def der(tag, body):
    if len(body) < 0x80: return bytes([tag, len(body)]) + body
    n = len(body).to_bytes((len(body).bit_length()+7)//8, 'big')
    return bytes([tag, 0x80 | len(n)]) + n + body
def integer(v):
    b = v.to_bytes((v.bit_length()+8)//8 or 1, 'big')
    return der(0x02, b)
OID = {'prime256v1': '2a8648ce3d030107', 'secp384r1': '2b81040022', 'secp521r1': '2b81040023'}
priv = open(os.path.join(build, 'c%s.priv' % cid), 'rb').read()
# RFC 5915 ECPrivateKey { version 1, privateKey OCTET STRING, [0] namedCurve }
body = der(0x02, b'\x01') + der(0x04, priv) + der(0xa0, der(0x06, bytes.fromhex(OID[curve])))
open(os.path.join(build, 'c%s.key.der' % cid), 'wb').write(der(0x30, body))
# our r||s -> DER SEQUENCE { INTEGER r, INTEGER s }
raw = open(os.path.join(build, 'c%s.sig' % cid), 'rb').read()
f = len(raw)//2
r = int.from_bytes(raw[:f], 'big'); s = int.from_bytes(raw[f:], 'big')
open(os.path.join(build, 'c%s.sig.der' % cid), 'wb').write(der(0x30, integer(r) + integer(s)))
PY
    [ -s "$BUILD/c$c.key.der" ] || { echo "FAIL P-$c: could not build the key DER"; oss_fail=$((oss_fail+1)); continue; }

    "$OPENSSL" pkey -inform DER -in "$BUILD/c$c.key.der" -pubout \
        -out "$BUILD/c$c.pub.pem" 2>/dev/null || {
            echo "FAIL P-$c: openssl would not load the key"; oss_fail=$((oss_fail+1)); continue; }

    # The public key openssl derived from the scalar must equal the one OUR
    # ecdh_keygen produced. This is what makes the verify below evidence rather
    # than circular: if our keygen and our signer shared a wrong base point they
    # would agree with each other, and a public key we also produced could not
    # tell. The point is read out of the DER SubjectPublicKeyInfo -- a BIT
    # STRING whose contents are the uncompressed point -- rather than scraped
    # out of `-text`, whose layout is a display format and not an interface.
    "$OPENSSL" pkey -pubin -in "$BUILD/c$c.pub.pem" -outform DER \
        -out "$BUILD/c$c.pub.der" 2>/dev/null
    if ! python3 "$ROOT/tests/unit/ecdsa_pubcmp.py" "$BUILD" "$c"; then
        echo "FAIL P-$c: our public key != openssl's for the same scalar"
        oss_fail=$((oss_fail+1)); continue
    fi

    if "$OPENSSL" dgst -"$md" -verify "$BUILD/c$c.pub.pem" \
            -signature "$BUILD/c$c.sig.der" "$BUILD/c$c.msg" >/dev/null 2>&1; then
        echo "ok   P-$c/$md  openssl verified our signature"
        oss_pass=$((oss_pass+1))
    else
        echo "FAIL P-$c/$md  openssl REJECTED our signature"
        oss_fail=$((oss_fail+1))
    fi

    # Control: the same signature over a different message must be REJECTED.
    # Without it, "openssl verified" could mean "openssl verifies anything",
    # which is what a mis-built command line looks like.
    printf 'not the signed message' > "$BUILD/c$c.wrong"
    if "$OPENSSL" dgst -"$md" -verify "$BUILD/c$c.pub.pem" \
            -signature "$BUILD/c$c.sig.der" "$BUILD/c$c.wrong" >/dev/null 2>&1; then
        echo "FAIL P-$c/$md  openssl accepted the signature over the WRONG message"
        oss_fail=$((oss_fail+1))
    else
        oss_pass=$((oss_pass+1))
    fi
done

echo
echo "openssl differential: $oss_pass passed, $oss_fail failed"
rc=0
[ "$kat_rc" -ne 0 ] && rc=1
[ "$oss_fail" -ne 0 ] && rc=1
[ "$oss_pass" -lt 6 ] && { echo "FAIL: expected 6 openssl checks, ran $oss_pass"; rc=1; }

if [ -n "$BREAK" ]; then
    # Inverted verdict: the control passes only when the gate reddens.
    if [ "$rc" -ne 0 ]; then echo "NEGCTL OK: $BREAK was caught"; exit 0; fi
    echo "NEGCTL FAILED: $BREAK changed nothing the gate could see"; exit 1
fi
[ "$rc" -eq 0 ] && echo "ecdsa-sign: OK"
exit $rc
