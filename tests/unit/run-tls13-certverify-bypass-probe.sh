#!/usr/bin/env bash
# probe-tls13-certverify-bypass: does our TLS 1.3 client require the server's
# CertificateVerify signature, or only a valid certificate CHAIN?
#
# RFC 8446 4.4.3 makes CertificateVerify mandatory whenever the server
# authenticates with a certificate, specifically because a certificate proves
# only that a CA vouched for a public key once -- it says nothing about
# whether the peer on the wire right now holds the matching private key. The
# signature is the only message in the flight that an attacker who has merely
# captured the target's (public, freely available) certificate cannot forge.
#
# tls13_certverify_omit_server.py plays that attacker: a genuine, chain-
# verifiable certificate, no private key, and a flight with the
# CertificateVerify message either omitted (the attack) or present but
# garbage (the control -- proves the flight is otherwise well-formed and it
# is specifically the omission that matters).
#
# NAMED probe- ON PURPOSE, NOT test-. See tests/tlsx.mk: NOT_CI's
# `^probe-` clause is what keeps tools/ci.sh's classify() from silently
# sweeping this into `make ci`. As of this writing the assertion below FAILS
# -- c/net/tls/tls.c's verify_flight() never checks whether a
# CertificateVerify was seen at all when the message is simply absent, only
# whether one that IS present carries a valid signature (tls.c:1102-1151).
# Wiring a target that fails by design into shared CI would redden
# `make ci` for every other workflow in this tree over a fix outside this
# probe's remit; running it by name is how you find that out on purpose
# instead. The day c/net/tls/tls.c requires CertificateVerify for a full
# (non-PSK) handshake, this script starts passing with no change of its own,
# and it should be renamed test-tls13-certverify-required and wired into
# ci-host at that point (see tests/tlsx.mk).
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${BUILD:-$ROOT/build}"
CC="${CC:-clang}"
OPENSSL="${OPENSSL:-openssl}"
TMP="$(mktemp -d)"
SRVPID=""
cleanup() { [ -n "$SRVPID" ] && kill "$SRVPID" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT

command -v "$OPENSSL" >/dev/null || { echo "SKIP: no openssl"; exit 0; }
python3 -c "import cryptography" 2>/dev/null || {
    echo "SKIP: python3 'cryptography' package not importable."
    echo "      openssl s_server always sends a real CertificateVerify, so it"
    echo "      cannot play this attacker; the mock server needs its own"
    echo "      x25519 + AES-GCM, which is what that package provides."
    exit 0
}

mkdir -p "$BUILD" "$TMP/roots"

# One CA, one leaf for "localhost" -- a real, chain-verifiable certificate,
# exactly what an attacker who captured the target's cert (sent in the clear
# on every real connection) would also have.
"$OPENSSL" ecparam -name prime256v1 -genkey -noout -out "$TMP/ca.key" 2>/dev/null
"$OPENSSL" req -x509 -new -key "$TMP/ca.key" -sha256 -days 3 -subj "/CN=Bypass Probe CA" \
    -addext "basicConstraints=critical,CA:TRUE" \
    -addext "keyUsage=critical,keyCertSign,cRLSign" \
    -out "$TMP/ca.pem" 2>/dev/null
cp "$TMP/ca.pem" "$TMP/roots/ca.pem"
"$OPENSSL" ecparam -name prime256v1 -genkey -noout -out "$TMP/leaf.key" 2>/dev/null
"$OPENSSL" req -new -key "$TMP/leaf.key" -subj "/CN=localhost" -out "$TMP/leaf.csr" 2>/dev/null
printf 'subjectAltName=DNS:localhost\nbasicConstraints=CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=serverAuth\n' \
    > "$TMP/leaf.ext"
"$OPENSSL" x509 -req -in "$TMP/leaf.csr" -CA "$TMP/ca.pem" -CAkey "$TMP/ca.key" -CAcreateserial \
    -days 3 -sha256 -extfile "$TMP/leaf.ext" -out "$TMP/leaf.pem" 2>/dev/null
cat "$TMP/leaf.pem" "$TMP/ca.pem" > "$TMP/chain.pem"
# The mock server holds no private key for this leaf beyond what it already
# used to sign the ClientHello... it holds NO private key for it at all. That
# omission is the entire experiment; leaf.key is generated above only because
# openssl req needs a key to build the CSR, and it is never read again.

# Trust: the SAME production chain-verification code as the interop suite,
# pointed at a bundle holding only this probe's own throwaway CA (see
# run-tls-interop.sh for why a quoted #include means roots.c itself, not just
# -I, has to be compiled from $TMP).
python3 "$ROOT/tools/genroots.py" "$TMP/roots" "$TMP/roots_bundle.inc" >/dev/null 2>&1 || {
    echo "FAIL: could not build the probe's trust bundle"; exit 1; }
cp "$ROOT/c/crypto/trust/roots.c" "$TMP/roots_test.c"

# -Ic/crypto/pq and the mlkem/keccak sources: tls.c #includes "mlkem.h"
# unconditionally as of the X25519MLKEM768 work (see tests/tlsx.mk and
# run-tls-interop.sh for the same addition, same reason) -- this list has to
# track that or the build fails on a missing header before this probe gets
# anywhere near the flight it exists to test.
INCS="-I$TMP -I$ROOT/c/crypto -I$ROOT/c/crypto/aead -I$ROOT/c/crypto/trust \
      -I$ROOT/c/crypto/pq \
      -I$ROOT/c/net/tls -I$ROOT/c/net/core -I$ROOT/c/net/transport \
      -I$ROOT/c/drivers/timer -I$ROOT/c/kernel/core -I$ROOT/c/kernel/cpu"
SRC="$ROOT/tests/unit/tls_interop_test.c $ROOT/c/net/tls/tls.c $ROOT/c/net/tls/tls12.c \
     $ROOT/c/net/tls/tls_psk.c $ROOT/c/net/tls/x509.c $ROOT/c/net/tls/ocsp.c \
     $ROOT/c/kernel/cpu/cpufeat.c \
     $ROOT/c/crypto/pq/mlkem.c $ROOT/c/crypto/pq/mlkem_rand.c $ROOT/c/crypto/pq/keccak.c \
     $(find "$ROOT/c/crypto/aead" "$ROOT/c/crypto/hash" "$ROOT/c/crypto/pubkey" -name '*.c') \
     $TMP/roots_test.c"
# shellcheck disable=SC2086
"$CC" -O1 -g -w -o "$BUILD/tls13_cvbypass_cli" $SRC $INCS || {
    echo "FAIL: could not build the probe's TLS client"; exit 1; }

bad=0

# run_case <label> <server-mode> <wanted-verdict>
# wanted-verdict is what the SERVER (the attacker) prints: ACCEPTED means the
# client trusted the flight enough to send its own encrypted record back;
# REJECTED means it did not. See tls13_certverify_omit_server.py's own header
# for exactly what that record can and cannot be.
run_case() {
    local label="$1" mode="$2" want="$3"
    local port=$((16400 + RANDOM % 5000))
    python3 "$ROOT/tests/unit/tls13_certverify_omit_server.py" "$port" "$TMP/chain.pem" "$mode" \
        >"$TMP/srv_$mode.out" 2>"$TMP/srv_$mode.err" &
    SRVPID=$!
    # NOT a connect-and-close readiness probe: this server does ONE `accept()`
    # ever (deliberately -- it plays a single malicious peer, not a real
    # server), so a probe connection would itself consume that slot and the
    # real client below would then find the port refusing -- "RESULT: FAIL
    # (connect)", which looks exactly like a rejection but is the harness
    # racing itself. (This is the same shape CLAUDE.md's own war stories
    # warn about: the first version of this script did exactly that and every
    # case "passed" by both sides failing to connect.) A fixed sleep is what
    # tests/unit's own tls13_certverify_omit_server.py reference run used.
    sleep 0.8
    "$BUILD/tls13_cvbypass_cli" 127.0.0.1 "$port" localhost >"$TMP/cli_$mode.log" 2>&1
    wait "$SRVPID" 2>/dev/null
    SRVPID=""
    local verdict
    verdict="$(cat "$TMP/srv_$mode.out" 2>/dev/null)"
    echo "-- $label --"
    sed 's/^/    srv| /' "$TMP/srv_$mode.err"
    sed -n '1,6p' "$TMP/cli_$mode.log" | sed 's/^/    cli| /'
    if [ "$verdict" = "$want" ]; then
        echo "    ok: server saw $verdict, as expected"
    else
        echo "    NOT AS EXPECTED: server saw '$verdict', wanted $want"
        bad=$((bad + 1))
    fi
    echo
}

echo "== probe: is CertificateVerify actually required? (chain: throwaway CA -> localhost) =="
run_case "control: CertificateVerify present but garbage" honest  REJECTED
run_case "ATTACK:  CertificateVerify omitted entirely"    omit-cv REJECTED

if [ "$bad" -eq 0 ]; then
    echo "PASS: CertificateVerify is required for a full TLS 1.3 handshake."
    exit 0
fi

echo "FAIL: $bad of 2 case(s) did not behave as RFC 8446 4.4.3 requires."
echo
echo "If the ATTACK case is the one that failed (and the control passed), this"
echo "is not a probe bug: it is c/net/tls/tls.c accepting a server identity it"
echo "never authenticated. verify_flight() (tls.c:1010) only checks a"
echo "CertificateVerify's signature INSIDE the 'if (mt == HS_CERT_VERIFY)'"
echo "branch (tls.c:1102) -- nothing records whether that branch ever ran, and"
echo "the post-loop gate (tls_check_chain, tls.c:1196) proves the certificate"
echo "is authentic without proving the peer holds its private key. An on-path"
echo "attacker holding only the target's PUBLIC certificate (sent in the clear"
echo "on every real connection) can therefore impersonate it to this client."
echo
echo "Minimal fix shape (not applied by this probe): track whether a"
echo "CertificateVerify with a verified signature was seen in the loop, and"
echo "after it, for a non-resumed handshake, refuse if it was not."
exit 1
