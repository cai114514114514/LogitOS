#!/usr/bin/env bash
# TLS 1.3 interop: drive c/net/tls/tls.c against a real `openssl s_server`.
#
# This is the test that turns "we implemented HelloRetryRequest" into evidence.
# Each case pins openssl to a specific configuration -- a single key-exchange
# group, a single cipher suite, a specific certificate key type, a specific
# ALPN -- and asserts our client either completes the handshake or refuses for
# the right reason. `-groups P-256` in particular is a server that CANNOT be
# reached without HRR: it will not accept the x25519 share we lead with.
#
# Trust: we mint a throwaway CA, generate a bundle from it with
# tools/genroots.py, and link that in place of the real 130-root store. The
# chain verification under test is therefore the production one -- there is no
# "trust anything" switch anywhere in this file or in tls.c.
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${BUILD:-$ROOT/build}"
CC="${CC:-clang}"
OPENSSL="${OPENSSL:-openssl}"
TMP="$(mktemp -d)"
PORT="${TLS_INTEROP_PORT:-14433}"
SRVPID=""
pass=0; fail=0

cleanup() {
    [ -n "$SRVPID" ] && kill "$SRVPID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

command -v "$OPENSSL" >/dev/null || { echo "SKIP: no openssl"; exit 0; }
"$OPENSSL" s_server -help 2>&1 | grep -q -- '-groups' || { echo "SKIP: openssl s_server lacks -groups"; exit 0; }

mkdir -p "$BUILD" "$TMP/roots"

# ---------------------------------------------------------------- test PKI ---
# One CA, two leaves (EC and RSA) so both CertificateVerify paths are covered,
# plus a leaf for the wrong name so the host-name check is exercised for real.
mkca() {     # mkca <name> <cn>
    local nm="$1" cn="$2"
    "$OPENSSL" ecparam -name prime256v1 -genkey -noout -out "$TMP/$nm.key" 2>/dev/null
    "$OPENSSL" req -x509 -new -key "$TMP/$nm.key" -sha256 -days 3 -subj "/CN=$cn" \
        -addext "basicConstraints=critical,CA:TRUE" \
        -addext "keyUsage=critical,keyCertSign,cRLSign" \
        -out "$TMP/$nm.pem" 2>/dev/null
}

mkleaf() {   # mkleaf <name> <keytype> <cn> <ca>
    local nm="$1" kt="$2" cn="$3" ca="$4"
    if [ "$kt" = ec ]; then
        "$OPENSSL" ecparam -name prime256v1 -genkey -noout -out "$TMP/$nm.key" 2>/dev/null
    else
        "$OPENSSL" genrsa -out "$TMP/$nm.key" 2048 2>/dev/null
    fi
    "$OPENSSL" req -new -key "$TMP/$nm.key" -subj "/CN=$cn" -out "$TMP/$nm.csr" 2>/dev/null
    printf 'subjectAltName=DNS:%s\nbasicConstraints=CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=serverAuth\n' "$cn" > "$TMP/$nm.ext"
    "$OPENSSL" x509 -req -in "$TMP/$nm.csr" -CA "$TMP/$ca.pem" -CAkey "$TMP/$ca.key" \
        -CAcreateserial -days 3 -sha256 -extfile "$TMP/$nm.ext" -out "$TMP/$nm.pem" 2>/dev/null
}

mkca ca    "LogitOS Interop Test CA"
mkca rogue "LogitOS Rogue CA (never trusted)"
cp "$TMP/ca.pem" "$TMP/roots/interop_ca.pem"      # only this one becomes an anchor
mkleaf ec    ec  localhost              ca
mkleaf rsa   rsa localhost              ca
mkleaf bad   ec  not-localhost.example  ca
mkleaf rogue_leaf ec localhost          rogue

# ------------------------------------------------------ build the test client
# Trust: a bundle holding exactly the throwaway CA. roots.c reaches its bundle
# with a quoted #include, and a quoted include resolves against the INCLUDING
# FILE's own directory before any -I -- so pointing -I at the temp dir is not
# enough, roots.c itself has to be compiled from there. (Getting this wrong
# links the real 130-root store and every case fails "no path to a trusted
# root", which is exactly how it was found.)
python3 "$ROOT/tools/genroots.py" "$TMP/roots" "$TMP/roots_bundle.inc" >/dev/null 2>&1 || {
    echo "FAIL: could not build the test trust bundle"; exit 1; }
cp "$ROOT/c/crypto/trust/roots.c" "$TMP/roots_test.c"

INCS="-I$TMP -I$ROOT/c/crypto -I$ROOT/c/crypto/trust -I$ROOT/c/net/tls -I$ROOT/c/net/core \
      -I$ROOT/c/net/transport -I$ROOT/c/drivers/timer -I$ROOT/c/kernel/core"
SRC="$ROOT/tests/unit/tls_interop_test.c $ROOT/c/net/tls/tls.c $ROOT/c/net/tls/x509.c \
     $TMP/roots_test.c \
     $(find "$ROOT/c/crypto/aead" "$ROOT/c/crypto/hash" "$ROOT/c/crypto/pubkey" -name '*.c')"
# ASan+UBSan: this binary parses adversarial-shaped input (certificates, records)
# with the same code the kernel runs, so it is the cheapest place to catch a
# bounds bug in it.
SAN="${TLS_INTEROP_SAN:--fsanitize=address,undefined -fno-sanitize-recover=all}"
# shellcheck disable=SC2086
$CC -O1 -g -Wall -Wextra $SAN -o "$BUILD/tls_interop_test" $SRC $INCS || {
    echo "FAIL: could not build tls_interop_test"; exit 1; }

# ------------------------------------------------------------------ harness --
start_server() {   # start_server <chain.pem> <key> [extra openssl args...]
    local chain="$1" key="$2"; shift 2
    "$OPENSSL" s_server -accept "$PORT" -cert "$chain" -key "$key" \
        -tls1_3 -www -quiet "$@" >"$TMP/server.log" 2>&1 &
    SRVPID=$!
    for _ in $(seq 1 100); do
        if (exec 3<>/dev/tcp/127.0.0.1/"$PORT") 2>/dev/null; then exec 3>&-; return 0; fi
        kill -0 "$SRVPID" 2>/dev/null || return 1
        sleep 0.05
    done
    return 1
}
stop_server() { [ -n "$SRVPID" ] && kill "$SRVPID" 2>/dev/null; wait "$SRVPID" 2>/dev/null; SRVPID=""; }

# case <label> <expect-hrr:0|1> <chain> <key> <server-args...> -- <client-args...>
case_run() {
    local label="$1" want_hrr="$2" chain="$3" key="$4"; shift 4
    local sargs=() cargs=()
    while [ $# -gt 0 ] && [ "$1" != "--" ]; do sargs+=("$1"); shift; done
    [ $# -gt 0 ] && shift
    while [ $# -gt 0 ]; do cargs+=("$1"); shift; done

    if ! start_server "$chain" "$key" "${sargs[@]}"; then
        echo "FAIL $label (server did not start)"; cat "$TMP/server.log"; fail=$((fail+1)); return
    fi
    "$BUILD/tls_interop_test" 127.0.0.1 "$PORT" localhost "${cargs[@]}" >"$TMP/client.log" 2>&1
    local rc=$?
    stop_server

    local hrr=0
    grep -q "HelloRetryRequest:" "$TMP/client.log" && hrr=1
    if [ $rc -ne 0 ]; then
        echo "FAIL $label"; sed 's/^/    | /' "$TMP/client.log"; fail=$((fail+1)); return
    fi
    if [ "$hrr" != "$want_hrr" ]; then
        echo "FAIL $label (HelloRetryRequest=$hrr, expected $want_hrr)"
        sed 's/^/    | /' "$TMP/client.log"; fail=$((fail+1)); return
    fi
    local detail
    detail="$(grep -a 'ServerHello:\|HelloRetryRequest:\|ALPN:' "$TMP/client.log" | tr '\n' ' ')"
    echo "ok   $label  ${detail}"
    pass=$((pass+1))
}

echo "== TLS 1.3 interop against $($OPENSSL version) =="
CHAIN="-cert_chain $TMP/ca.pem"        # server sends leaf + CA (in-band anchor)

# --- key exchange. The group cases are the reachability proof: with only
#     x25519 offered and no HRR, everything below the first line was
#     unreachable, not merely slow. ---
# shellcheck disable=SC2086
case_run "x25519 (no retry)"        0 "$TMP/ec.pem"  "$TMP/ec.key"  $CHAIN -groups X25519 --
# shellcheck disable=SC2086
case_run "secp256r1 via HRR"        1 "$TMP/ec.pem"  "$TMP/ec.key"  $CHAIN -groups P-256  --
# shellcheck disable=SC2086
case_run "secp384r1 via HRR"        1 "$TMP/ec.pem"  "$TMP/ec.key"  $CHAIN -groups P-384  --
# A server offering several groups but not x25519 must still land on one of ours.
# shellcheck disable=SC2086
case_run "P-384+P-256 via HRR"      1 "$TMP/ec.pem"  "$TMP/ec.key"  $CHAIN -groups P-384:P-256 --

# --- cipher suites, both AEADs, on both the direct and the retry path ---
# shellcheck disable=SC2086
case_run "chacha20-poly1305"        0 "$TMP/ec.pem"  "$TMP/ec.key" \
    $CHAIN -groups X25519 -ciphersuites TLS_CHACHA20_POLY1305_SHA256 --
# shellcheck disable=SC2086
case_run "aes128-gcm"               0 "$TMP/ec.pem"  "$TMP/ec.key" \
    $CHAIN -groups X25519 -ciphersuites TLS_AES_128_GCM_SHA256 --
# shellcheck disable=SC2086
case_run "chacha20 + P-256 retry"   1 "$TMP/ec.pem"  "$TMP/ec.key" \
    $CHAIN -groups P-256 -ciphersuites TLS_CHACHA20_POLY1305_SHA256 --

# --- certificate key types: ECDSA and RSA-PSS CertificateVerify ---
# shellcheck disable=SC2086
case_run "RSA leaf (PSS certverify)" 0 "$TMP/rsa.pem" "$TMP/rsa.key" $CHAIN -groups X25519 --
# shellcheck disable=SC2086
case_run "RSA leaf via HRR"          1 "$TMP/rsa.pem" "$TMP/rsa.key" $CHAIN -groups P-384 --

# --- trust path: leaf only, anchor NOT sent in band. Verified through
#     signed_by_root() instead of is_pinned_root(); both paths must work. ---
case_run "anchor held, not sent"     0 "$TMP/ec.pem" "$TMP/ec.key" -groups X25519 --

# --- ALPN ---
# shellcheck disable=SC2086
case_run "alpn http/1.1"            0 "$TMP/ec.pem" "$TMP/ec.key" \
    $CHAIN -groups X25519 -alpn http/1.1 -- --alpn "h2,http/1.1" --expect-alpn http/1.1
# shellcheck disable=SC2086
case_run "alpn h2 preferred"        0 "$TMP/ec.pem" "$TMP/ec.key" \
    $CHAIN -groups X25519 -alpn h2,http/1.1 -- --alpn "h2,http/1.1" --expect-alpn h2
# shellcheck disable=SC2086
case_run "no alpn offered"          0 "$TMP/ec.pem" "$TMP/ec.key" \
    $CHAIN -groups X25519 -- --expect-alpn ""

# --- the blocking wrapper still works on top of the stepped core ---
# shellcheck disable=SC2086
case_run "blocking tls_connect"     0 "$TMP/ec.pem" "$TMP/ec.key" $CHAIN -groups X25519 -- --blocking
# shellcheck disable=SC2086
case_run "blocking + HRR"           1 "$TMP/ec.pem" "$TMP/ec.key" $CHAIN -groups P-256  -- --blocking

# --- rejections. These matter more than the successes: a client that reaches
#     more servers by checking less is a downgrade, not a feature. ---
# shellcheck disable=SC2086
case_run "rejects wrong host name"  0 "$TMP/bad.pem" "$TMP/bad.key" $CHAIN -groups X25519 -- --expect-fail
# Correct name, valid dates, self-consistent chain -- but rooted in a CA we do
# not hold. This is the check that stands between us and any CA on the internet.
case_run "rejects untrusted anchor" 0 "$TMP/rogue_leaf.pem" "$TMP/rogue_leaf.key" \
    -cert_chain "$TMP/rogue.pem" -groups X25519 -- --expect-fail
# TLS 1.2-only server: we do not implement 1.2, so this must fail cleanly rather
# than misparse a 1.2 ServerHello as a 1.3 one. (When 1.2 lands, flip it.)
"$OPENSSL" s_server -accept "$PORT" -cert "$TMP/ec.pem" -key "$TMP/ec.key" \
    -tls1_2 -www -quiet >"$TMP/server.log" 2>&1 &
SRVPID=$!
sleep 0.4
"$BUILD/tls_interop_test" 127.0.0.1 "$PORT" localhost --expect-fail >"$TMP/client.log" 2>&1
if [ $? -eq 0 ]; then echo "ok   rejects TLS 1.2-only server (not implemented)"; pass=$((pass+1));
else echo "FAIL rejects TLS 1.2-only server"; sed 's/^/    | /' "$TMP/client.log"; fail=$((fail+1)); fi
stop_server

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ] || exit 1
echo "PASS: TLS interop"
