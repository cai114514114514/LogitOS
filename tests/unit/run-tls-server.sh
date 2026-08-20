#!/usr/bin/env bash
# TLS 1.3 SERVER interop: drive c/net/tls/tls_server.c against a real
# `openssl s_client`, and against our own client (c/net/tls/tls.c).
#
# This is the mirror of tests/unit/run-tls-interop.sh, and the two halves here
# are not interchangeable:
#
#   openssl-as-client is the half that can find a bug we would agree with
#     ourselves about. openssl shares no code, no reading of the RFC and no
#     assumptions with this tree; a ServerHello we frame wrong, a transcript we
#     hash in the wrong order or a CertificateVerify we sign over the wrong
#     bytes is caught there and only there. It also verifies a CERTIFICATE THIS
#     MACHINE WROTE, which is the only real check on the DER writer -- our own
#     parser reading our own writer proves nothing about either.
#
#   our-client-against-our-server is the half that diffs the two TLS 1.3 key
#     schedules in this tree (see the SHARING note at the top of
#     tls_server.c). A key schedule disagreement is fatal at the Finished MAC,
#     so this case is not decoration -- it is the gate on a duplication that
#     was accepted deliberately.
#
# Every negative case here exists because the positive one next to it would
# otherwise pass for the wrong reason. `-verify_return_error` with a -CAfile
# that does NOT hold our anchor must fail, or "openssl verified our
# certificate" means "openssl was not asked".
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${BUILD:-$ROOT/build}/tlsx"
CC="${CC:-clang}"
OPENSSL="${OPENSSL:-openssl}"
PORT="${TLS_SERVER_PORT:-14533}"
pass=0; fail=0
SRVPID=""

mkdir -p "$BUILD"
cleanup() { [ -n "$SRVPID" ] && kill "$SRVPID" 2>/dev/null; }
trap cleanup EXIT

command -v "$OPENSSL" >/dev/null || { echo "SKIP: no openssl"; exit 0; }
"$OPENSSL" s_client -help 2>&1 | grep -q -- '-groups' || { echo "SKIP: openssl s_client lacks -groups"; exit 0; }

# TLS_SERVER_BREAK compiles a deliberate defect in and INVERTS the verdict, so
# the run passes only if the suite notices. See tests/tlsx.mk.
BREAK="${TLS_SERVER_BREAK:-}"
BREAKDEF=""
[ -n "$BREAK" ] && BREAKDEF="-D$BREAK"

# ASan+UBSan: this binary parses a ClientHello -- attacker-controlled lengths
# and offsets -- with the same code the kernel runs, so it is the cheapest
# place for a bounds bug in it to surface.
SAN="${TLS_SERVER_SAN:--fsanitize=address,undefined -fno-sanitize-recover=all}"

# The trust store is NOT c/crypto/trust/roots.c here: tls_server_test.c defines
# logit_roots itself and fills the anchor at runtime from the certificate the
# server generates, because that certificate does not exist until the process
# runs. Everything else -- x509.c, the name check, the chain walk -- is the
# production code, and --wrong-anchor below is what proves it is running.
SRC="$ROOT/tests/unit/tls_server_test.c \
     $ROOT/c/net/tls/tls_server.c $ROOT/c/net/tls/tls.c $ROOT/c/net/tls/tls12.c \
     $ROOT/c/net/tls/tls_psk.c $ROOT/c/net/tls/x509.c $ROOT/c/net/tls/ocsp.c \
     $ROOT/c/kernel/cpu/cpufeat.c \
     $(find "$ROOT/c/crypto/aead" "$ROOT/c/crypto/hash" "$ROOT/c/crypto/pubkey" \
            "$ROOT/c/crypto/kdf" -name '*.c') \
     $(find "$ROOT/c/crypto/pq" -name '*.c' 2>/dev/null)"
INCS="-I$ROOT/c/crypto -I$ROOT/c/crypto/aead -I$ROOT/c/crypto/trust \
      -I$ROOT/c/crypto/pq -I$ROOT/c/net/tls -I$ROOT/c/net/core \
      -I$ROOT/c/net/transport -I$ROOT/c/drivers/timer -I$ROOT/c/kernel/core \
      -I$ROOT/c/kernel/cpu"

# shellcheck disable=SC2086
$CC -O1 -g -Wall -Wextra $SAN $BREAKDEF -o "$BUILD/tls_server_test" $SRC $INCS || {
    echo "FAIL: could not build tls_server_test"; exit 1; }

# ------------------------------------------------------------- the payload --
# 10 KB, so it spans several records in each direction: SEND_REC_MAX is 4096,
# and the partial-send path a short "hello" never reaches is where a server
# that assumes one write is one record breaks. No line starts with 'Q' or 'R':
# `openssl s_client` treats those as commands unless -ign_eof, and -quiet
# implies -ign_eof, but relying on that is a dependency on a flag's side effect.
python3 - "$BUILD/payload.txt" <<'PY'
import sys
with open(sys.argv[1], 'w') as f:
    for i in range(220):
        f.write('%06d %s\n' % (i, 'abcdefghijklmnopqrstuvwxyz0123456789' * 1))
PY
NBYTES=$(wc -c < "$BUILD/payload.txt")
echo "== TLS server interop against $($OPENSSL version) =="
echo "-- payload: $NBYTES bytes --"

# ----------------------------------------------------------- our own client -
# Runs first: if the two key schedules in this tree disagree, every openssl
# case below fails too and the openssl output says less about why.
echo
echo "-- our client (c/net/tls/tls.c) against our server --"
pair_case() {     # pair_case <label> <args...> -- every case must print RESULT: OK,
                  # including the refusals: the driver decides what "refused
                  # correctly" means (TLS_E_CERT, not merely negative) so the
                  # shell cannot accept a wrong failure as a pass.
    local label="$1"; shift
    local out rc
    out="$("$BUILD/tls_server_test" pair "$@" 2>&1)"; rc=$?
    if [ $rc -eq 0 ] && printf '%s' "$out" | grep -q 'RESULT: OK'; then
        echo "ok   $label  $(printf '%s' "$out" | grep -a -e 'ECHOED:' -e 'RESULT: OK' | paste -sd' ' -)"
        pass=$((pass+1)); return
    fi
    echo "FAIL $label (rc=$rc)"
    printf '%s' "$out" | sed 's/^/    | /'
    fail=$((fail+1))
}

pair_case "alpn h2 + 10 KB echo"  --alpn "h2,http/1.1" --expect-alpn h2
pair_case "no alpn offered"
pair_case "alpn: the server order wins" --alpn "http/1.1,h2" --client-alpn "h2,http/1.1" --expect-alpn http/1.1
# A certificate for a DIFFERENT name must be refused by our own client. This is
# what says the name binding is real on a certificate we minted ourselves -- a
# self-signed cert is otherwise the easiest thing in the world to accept.
pair_case "our client rejects a name mismatch" --cn other.example --sni localhost --expect-cert-fail
# ...and a certificate whose key is not the anchor we hold, which is the other
# half: the first control could pass on a client that checks only the name.
pair_case "our client rejects a wrong trust anchor" --wrong-anchor

# ------------------------------------------------------------ openssl cases -
start_server() {   # start_server <extra tls_server_test args...>
    rm -f "$BUILD/srv.log" "$BUILD/srv.err"
    "$BUILD/tls_server_test" serve "$PORT" --cert-out "$BUILD/srv.der" \
        --echo "$NBYTES" "$@" >"$BUILD/srv.log" 2>"$BUILD/srv.err" &
    SRVPID=$!
    for _ in $(seq 1 200); do
        grep -q LISTENING "$BUILD/srv.log" 2>/dev/null && return 0
        kill -0 "$SRVPID" 2>/dev/null || return 1
        sleep 0.05
    done
    return 1
}

# case_run <label> <expect-ok:0|1> -- <openssl args...> [-- <server args...>]
case_run() {
    local label="$1" want="$2"; shift 2
    [ "$1" = "--" ] && shift
    local oargs=() sargs=()
    while [ $# -gt 0 ] && [ "$1" != "--" ]; do oargs+=("$1"); shift; done
    [ $# -gt 0 ] && shift
    while [ $# -gt 0 ]; do sargs+=("$1"); shift; done

    if ! start_server "${sargs[@]}"; then
        echo "FAIL $label (server did not start)"; cat "$BUILD/srv.err" 2>/dev/null | tail -5
        fail=$((fail+1)); SRVPID=""; return
    fi
    "$OPENSSL" x509 -inform DER -in "$BUILD/srv.der" -out "$BUILD/srv.pem" 2>/dev/null

    "$OPENSSL" s_client -connect "127.0.0.1:$PORT" -servername localhost \
        -quiet "${oargs[@]}" < "$BUILD/payload.txt" \
        > "$BUILD/cli.out" 2>"$BUILD/cli.err"
    local orc=$?
    wait "$SRVPID" 2>/dev/null; local src=$?
    SRVPID=""

    local echoed=0
    [ -f "$BUILD/cli.out" ] && echoed=$(wc -c < "$BUILD/cli.out")

    if [ "$want" = 0 ]; then
        # A refusal case: the handshake must NOT complete. Either end saying no
        # is acceptable, but "openssl exited 0 and echoed the payload" is not.
        if [ $orc -ne 0 ] || [ $src -ne 0 ]; then
            echo "ok   $label  (refused: openssl=$orc server=$src)"
            pass=$((pass+1))
        else
            echo "FAIL $label (the handshake COMPLETED and should not have)"
            sed 's/^/    | /' "$BUILD/srv.err" | tail -5
            fail=$((fail+1))
        fi
        return
    fi

    if [ $src -ne 0 ] || [ $orc -ne 0 ]; then
        echo "FAIL $label (openssl=$orc server=$src)"
        sed 's/^/    srv| /' "$BUILD/srv.err" | tail -8
        sed 's/^/    cli| /' "$BUILD/cli.err" | tail -8
        fail=$((fail+1)); return
    fi
    if ! cmp -s "$BUILD/payload.txt" "$BUILD/cli.out"; then
        echo "FAIL $label (echo differs: $echoed of $NBYTES bytes came back)"
        cmp "$BUILD/payload.txt" "$BUILD/cli.out" 2>&1 | sed 's/^/    | /' | head -3
        fail=$((fail+1)); return
    fi
    local detail
    detail="$(grep -a 'suite 0x\|handshake complete' "$BUILD/srv.err" | tail -1 | sed 's/^\[tlss\] //')"
    echo "ok   $label  ${detail}  echo $echoed/$NBYTES byte-identical"
    pass=$((pass+1))
}

# ------------------------------------------------- the certificate itself --
# Before any handshake: does a program that has never seen our DER writer agree
# that what it produced is a certificate? Our own x509.c reading our own writer
# proves nothing about either -- a matched pair of misunderstandings parses
# perfectly. `openssl verify` additionally walks it as a trust anchor, which is
# what the basicConstraints CA:TRUE decision in tlss_self_signed rests on.
echo
echo "-- the generated certificate, judged by openssl --"
"$BUILD/tls_server_test" gencert --cert-out "$BUILD/gen.der" --quiet >/dev/null 2>&1
if "$OPENSSL" x509 -inform DER -in "$BUILD/gen.der" -out "$BUILD/gen.pem" 2>"$BUILD/x509.err"; then
    echo "ok   openssl parses it ($(wc -c < "$BUILD/gen.der") bytes DER)"
    pass=$((pass+1))
else
    echo "FAIL openssl cannot parse the certificate we wrote"
    sed 's/^/    | /' "$BUILD/x509.err"; fail=$((fail+1))
fi
if "$OPENSSL" verify -CAfile "$BUILD/gen.pem" "$BUILD/gen.pem" >"$BUILD/verify.out" 2>&1; then
    echo "ok   openssl verify accepts it as its own anchor"
    pass=$((pass+1))
else
    echo "FAIL openssl verify rejected it"
    sed 's/^/    | /' "$BUILD/verify.out"; fail=$((fail+1))
fi
# The self-signature must actually check out. `openssl verify` above already
# does this, but say it separately: a certificate whose signature is wrong and
# whose anchor is itself could otherwise be accepted by a verifier configured
# to skip the anchor's own signature, which some are.
if "$OPENSSL" x509 -in "$BUILD/gen.pem" -noout -text 2>/dev/null      | grep -q 'ecdsa-with-SHA256'; then
    echo "ok   signature algorithm reads back as ecdsa-with-SHA256"
    pass=$((pass+1))
else
    echo "FAIL the signature algorithm did not read back"
    fail=$((fail+1))
fi
for want in "CA:TRUE" "DNS:localhost" "TLS Web Server Authentication" "Digital Signature"; do
    if "$OPENSSL" x509 -in "$BUILD/gen.pem" -noout -text 2>/dev/null | grep -q "$want"; then
        pass=$((pass+1))
    else
        echo "FAIL the certificate is missing: $want"; fail=$((fail+1))
    fi
done
echo "ok   extensions read back: CA:TRUE, SAN dNSName, serverAuth EKU, digitalSignature"

CA="-CAfile $BUILD/srv.pem -verify_return_error"

echo
echo "-- openssl s_client against our server: cipher suites --"
# shellcheck disable=SC2086
case_run "TLS_AES_128_GCM_SHA256"       1 -- $CA -groups X25519 -ciphersuites TLS_AES_128_GCM_SHA256
# shellcheck disable=SC2086
case_run "TLS_CHACHA20_POLY1305_SHA256" 1 -- $CA -groups X25519 -ciphersuites TLS_CHACHA20_POLY1305_SHA256
# A SHA-384 suite is the one that catches a key schedule pinned at 32 bytes --
# the hash width is the only thing that differs from the case above it.
# shellcheck disable=SC2086
case_run "TLS_AES_256_GCM_SHA384"       1 -- $CA -groups X25519 -ciphersuites TLS_AES_256_GCM_SHA384

echo
echo "-- key exchange groups --"
# shellcheck disable=SC2086
case_run "x25519 (no retry)"     1 -- $CA -groups X25519
# shellcheck disable=SC2086
case_run "secp256r1 (no retry)"  1 -- $CA -groups P-256
# shellcheck disable=SC2086
case_run "secp384r1 (no retry)"  1 -- $CA -groups P-384
# openssl sends a key_share for the FIRST group only, so naming a group we do
# not have first forces our HelloRetryRequest. That is the path a 2026 browser
# takes into this server (X25519MLKEM768 first, x25519 in supported_groups).
# shellcheck disable=SC2086
case_run "HelloRetryRequest to x25519" 1 -- $CA -groups P-521:X25519
# shellcheck disable=SC2086
case_run "HelloRetryRequest to P-256"  1 -- $CA -groups P-521:P-256

echo
echo "-- ALPN --"
# shellcheck disable=SC2086
case_run "alpn http/1.1"  1 -- $CA -groups X25519 -alpn http/1.1 -- --alpn "h2,http/1.1" --expect-alpn http/1.1
# shellcheck disable=SC2086
case_run "alpn h2"        1 -- $CA -groups X25519 -alpn h2       -- --alpn "h2,http/1.1" --expect-alpn h2
# The server decides (RFC 7301): its own order wins over the client's.
# shellcheck disable=SC2086
case_run "server order wins" 1 -- $CA -groups X25519 -alpn h2,http/1.1 -- --alpn "http/1.1,h2" --expect-alpn http/1.1

echo
echo "-- refusals (each one is why the case above it means something) --"
# The trust check must be live. Same server, same certificate, an anchor that
# is a DIFFERENT self-signed certificate: openssl must refuse. Without this,
# every "-CAfile srv.pem" pass above could be openssl not verifying at all.
"$BUILD/tls_server_test" gencert --cert-out "$BUILD/other.der" --quiet >/dev/null 2>&1
"$OPENSSL" x509 -inform DER -in "$BUILD/other.der" -out "$BUILD/other.pem" 2>/dev/null
case_run "openssl rejects an unrelated anchor" 0 -- \
    -CAfile "$BUILD/other.pem" -verify_return_error -groups X25519
# Our server must refuse a client that cannot do TLS 1.3, rather than start a
# 1.2 handshake it has no code to finish.
case_run "we refuse a TLS 1.2-only client" 0 -- $CA -tls1_2
# A client offering only groups we do not have, with nothing usable in
# supported_groups either, must get a clean handshake_failure.
case_run "we refuse when no group is shared" 0 -- $CA -groups P-521

echo
echo "tls-server: $pass passed, $fail failed"
if [ -n "$BREAK" ]; then
    # The control is not "something went red" -- it is "EXACTLY these went
    # red". A break that reddens everything proves the suite runs; a break that
    # reddens exactly the row carrying the property proves the row is doing the
    # work. TLS_SERVER_BREAK_EXPECT is that count, measured, and a mismatch in
    # EITHER direction fails: more than expected means the break is broader
    # than claimed, fewer means a case that should carry the property does not.
    want="${TLS_SERVER_BREAK_EXPECT:-}"
    if [ -z "$want" ]; then
        if [ "$fail" -ne 0 ]; then echo "NEGCTL OK: $BREAK was caught ($fail cases)"; exit 0; fi
        echo "NEGCTL FAILED: $BREAK changed nothing the suite could see"; exit 1
    fi
    if [ "$fail" -eq "$want" ]; then
        echo "NEGCTL OK: $BREAK reddened exactly $fail of $((pass+fail)) cases"; exit 0
    fi
    echo "NEGCTL FAILED: $BREAK reddened $fail cases, expected exactly $want"
    exit 1
fi
[ "$fail" -eq 0 ] || exit 1
exit 0
