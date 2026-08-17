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
mkca() {     # mkca <name> <cn> [curve]
    local nm="$1" cn="$2" cv="${3:-prime256v1}"
    "$OPENSSL" ecparam -name "$cv" -genkey -noout -out "$TMP/$nm.key" 2>/dev/null
    "$OPENSSL" req -x509 -new -key "$TMP/$nm.key" -sha256 -days 3 -subj "/CN=$cn" \
        -addext "basicConstraints=critical,CA:TRUE" \
        -addext "keyUsage=critical,keyCertSign,cRLSign" \
        -out "$TMP/$nm.pem" 2>/dev/null
}

mkleaf() {   # mkleaf <name> <keytype: ec|rsa|ec521> <cn> <ca> [extra-SAN-dns]
    local nm="$1" kt="$2" cn="$3" ca="$4" alt="${5:-}"
    local md="-sha256"
    case "$kt" in
      ec)    "$OPENSSL" ecparam -name prime256v1 -genkey -noout -out "$TMP/$nm.key" 2>/dev/null ;;
      # A P-521 leaf, signed with SHA-512 by a P-521 CA. This is the certificate
      # that made a server unreachable: we advertise ecdsa_secp521r1_sha512, the
      # server obliges, and before this work nothing could check the result.
      ec521) "$OPENSSL" ecparam -name secp521r1 -genkey -noout -out "$TMP/$nm.key" 2>/dev/null
             md="-sha512" ;;
      *)     "$OPENSSL" genrsa -out "$TMP/$nm.key" 2048 2>/dev/null ;;
    esac
    "$OPENSSL" req -new -key "$TMP/$nm.key" -subj "/CN=$cn" -out "$TMP/$nm.csr" 2>/dev/null
    local san="DNS:$cn"
    [ -n "$alt" ] && san="$san,DNS:$alt"
    printf 'subjectAltName=%s\nbasicConstraints=CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=serverAuth\n' "$san" > "$TMP/$nm.ext"
    "$OPENSSL" x509 -req -in "$TMP/$nm.csr" -CA "$TMP/$ca.pem" -CAkey "$TMP/$ca.key" \
        -CAcreateserial -days 3 $md -extfile "$TMP/$nm.ext" -out "$TMP/$nm.pem" 2>/dev/null
}

# An Ed25519 root. This is how Ed25519 realistically enters a PKI -- as the
# ANCHOR over conventional leaves -- and it is exactly the path the x509.c work
# added: the chain signature is Ed25519, the CertificateVerify is not. We do
# not advertise ed25519 (0x0807) in signature_algorithms, so no server can
# choose it for CertificateVerify and the "advertised but unimplemented" trap
# the P-521 comment warns about is not reopened here.
mked25519ca() {  # mked25519ca <name> <cn>
    "$OPENSSL" genpkey -algorithm ED25519 -out "$TMP/$1.key" 2>/dev/null
    "$OPENSSL" req -x509 -new -key "$TMP/$1.key" -days 3 -subj "/CN=$2" \
        -addext "basicConstraints=critical,CA:TRUE" \
        -addext "keyUsage=critical,keyCertSign,cRLSign" \
        -out "$TMP/$1.pem" 2>/dev/null
}

mkca ca    "LogitOS Interop Test CA"
mkca rogue "LogitOS Rogue CA (never trusted)"
cp "$TMP/ca.pem" "$TMP/roots/interop_ca.pem"      # only this one becomes an anchor
mkleaf ec    ec  localhost              ca
mkleaf rsa   rsa localhost              ca
mkleaf bad   ec  not-localhost.example  ca
mkleaf rogue_leaf ec localhost          rogue
# Valid for BOTH names, so the ticket-scoping case can change SNI between the
# two connections without the change being masked by a certificate rejection.
mkleaf multi ec  localhost              ca   alt.localhost
# A whole P-521 PKI: P-521 root, P-521 leaf, SHA-512 signatures throughout. The
# CA is a separate anchor so BOTH the chain signature (x509.c) and the
# CertificateVerify (tls.c / tls12.c) run over P-521, not just one of them.
mkca   ca521 "LogitOS Interop P-521 CA" secp521r1
cp "$TMP/ca521.pem" "$TMP/roots/interop_ca521.pem"
mkleaf p521 ec521 localhost             ca521

# Ed25519 anchors: one trusted, one deliberately not. The second is what turns
# "the signature verified" into "the signature verified AND we hold the key" --
# without it, a chain that merely parses would look like a pass.
mked25519ca ed25519ca  "LogitOS Interop Ed25519 CA"
mked25519ca ed25519bad "LogitOS Rogue Ed25519 CA (never trusted)"
cp "$TMP/ed25519ca.pem" "$TMP/roots/interop_ed25519_ca.pem"
mkleaf edleaf    ec localhost ed25519ca
mkleaf edleafbad ec localhost ed25519bad

# --- OCSP staples about the `ec` leaf, from a real `openssl ocsp` responder --
# Two of them: one saying good, one saying revoked. The revoked one is the
# point of the whole exercise -- before stapling, a revoked certificate
# verified exactly as well as a good one, and there was no test that could tell
# the difference because there was no difference.
#
# index.txt is written by hand rather than grown with `openssl ca`, because the
# leaves here are made with `x509 -req` and never touch a CA database. The
# responder only looks up the SERIAL; the expiry column is a far-future
# constant and the DN column is not consulted at all.
EC_SERIAL="$("$OPENSSL" x509 -in "$TMP/ec.pem" -noout -serial | cut -d= -f2)"
printf 'V	351231235959Z		%s	unknown	/CN=localhost
' "$EC_SERIAL" > "$TMP/ocsp_index_good.txt"
printf 'R	351231235959Z	250101000000Z	%s	unknown	/CN=localhost
' "$EC_SERIAL" > "$TMP/ocsp_index_rev.txt"
"$OPENSSL" ocsp -issuer "$TMP/ca.pem" -cert "$TMP/ec.pem" -reqout "$TMP/ocsp_req.der" -no_nonce >/dev/null 2>&1
for v in good rev; do
    "$OPENSSL" ocsp -index "$TMP/ocsp_index_$v.txt" -CA "$TMP/ca.pem"         -rsigner "$TMP/ca.pem" -rkey "$TMP/ca.key" -reqin "$TMP/ocsp_req.der"         -respout "$TMP/staple_$v.der" -no_nonce -ndays 2 >/dev/null 2>&1
done
HAVE_STAPLE=0
[ -s "$TMP/staple_good.der" ] && [ -s "$TMP/staple_rev.der" ] && HAVE_STAPLE=1

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

# c/kernel/cpu/cpufeat.c comes along because the AES-GCM backend is chosen at
# runtime from CPUID (c/crypto/aead/aes_dispatch.c). Including it here is not a
# detail: on a host with AES-NI this suite then exercises the SAME accelerated
# path the kernel will use against a real openssl s_server, which is the only
# place the two implementations meet a third-party peer.
INCS="-I$TMP -I$ROOT/c/crypto -I$ROOT/c/crypto/aead -I$ROOT/c/crypto/trust \
      -I$ROOT/c/net/tls -I$ROOT/c/net/core \
      -I$ROOT/c/net/transport -I$ROOT/c/drivers/timer -I$ROOT/c/kernel/core \
      -I$ROOT/c/kernel/cpu"
# Everything EXCEPT the trust-store TU. The fail-closed block at the bottom
# builds a second client from exactly this list plus a roots.c compiled against
# an EMPTY bundle -- so "identical in every respect except which anchors are
# compiled in" is a property of the build line, not a claim in a comment.
SRC_NOTRUST="$ROOT/tests/unit/tls_interop_test.c $ROOT/c/net/tls/tls.c $ROOT/c/net/tls/tls12.c $ROOT/c/net/tls/tls_psk.c \
     $ROOT/c/net/tls/x509.c $ROOT/c/net/tls/ocsp.c $ROOT/c/kernel/cpu/cpufeat.c \
     $(find "$ROOT/c/crypto/aead" "$ROOT/c/crypto/hash" "$ROOT/c/crypto/pubkey" -name '*.c')"
SRC="$SRC_NOTRUST $TMP/roots_test.c"
# ASan+UBSan: this binary parses adversarial-shaped input (certificates, records)
# with the same code the kernel runs, so it is the cheapest place to catch a
# bounds bug in it.
SAN="${TLS_INTEROP_SAN:--fsanitize=address,undefined -fno-sanitize-recover=all}"
# TLS_INTEROP_BREAK is the negative control: it compiles a deliberate defect
# into the client and the verdict at the bottom is INVERTED, so the run passes
# only if the suite notices. See test-tls-resume-control in the Makefile.
BREAK="${TLS_INTEROP_BREAK:-}"
BREAKDEF=""
[ -n "$BREAK" ] && BREAKDEF="-D$BREAK"
# TLS_INTEROP_ONLY=resume restricts the run to the resumption block, which is
# all the control needs and keeps it to a few seconds.
ONLY="${TLS_INTEROP_ONLY:-}"
# TLS_INTEROP_NOROOTS_CONTROL=1 is the fail-closed block's negative control (see
# the block near the bottom). It is read HERE, not there, because it pins ONLY
# to that block: the verdict it computes is "all four pairs failed", and a
# failure anywhere else in the suite would otherwise be counted towards it.
NRCTL="${TLS_INTEROP_NOROOTS_CONTROL:-}"
NRBIN=""                          # set only if the substitute client builds
[ -n "$NRCTL" ] && ONLY="noroots"
# shellcheck disable=SC2086
$CC -O1 -g -Wall -Wextra $SAN $BREAKDEF -o "$BUILD/tls_interop_test" $SRC $INCS || {
    echo "FAIL: could not build tls_interop_test"; exit 1; }

# ------------------------------------------------------------------ harness --
SRV_VER="-tls1_3"        # which version the server is pinned to, per block
CLI_PORT="$PORT"         # what the client dials (the proxy cases redirect this)

start_server() {   # start_server <chain.pem> <key> [extra openssl args...]
    local chain="$1" key="$2"; shift 2
    "$OPENSSL" s_server -accept "$PORT" -cert "$chain" -key "$key" \
        $SRV_VER -www -quiet "$@" >"$TMP/server.log" 2>&1 &
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
SECTION="core"                   # which block the following cases belong to
case_run() {
    local label="$1" want_hrr="$2" chain="$3" key="$4"; shift 4
    # TLS_INTEROP_ONLY restricts the run to one block; a skipped case is not
    # counted either way, so it cannot turn a failure into a pass.
    if [ -n "$ONLY" ] && [ "$ONLY" != "$SECTION" ]; then return; fi
    local sargs=() cargs=()
    while [ $# -gt 0 ] && [ "$1" != "--" ]; do sargs+=("$1"); shift; done
    [ $# -gt 0 ] && shift
    while [ $# -gt 0 ]; do cargs+=("$1"); shift; done

    if ! start_server "$chain" "$key" "${sargs[@]}"; then
        echo "FAIL $label (server did not start)"; cat "$TMP/server.log"; fail=$((fail+1)); return
    fi
    "$BUILD/tls_interop_test" 127.0.0.1 "$CLI_PORT" localhost "${cargs[@]}" >"$TMP/client.log" 2>&1
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
    detail="$(grep -a 'ServerHello:\|HelloRetryRequest:\|VERSION:\|ALPN:' "$TMP/client.log" | tr '\n' ' ')"
    echo "ok   $label  ${detail}"
    pass=$((pass+1))
}

echo "== TLS interop against $($OPENSSL version) =="
CHAIN="-cert_chain $TMP/ca.pem"        # server sends leaf + CA (in-band anchor)

echo "-- TLS 1.3 --"

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
# Even with TLS 1.2 implemented, a server pinned to 1.3 must still land on 1.3 --
# the reachability work must not have quietly moved the common path backwards.
# shellcheck disable=SC2086
case_run "1.3 still wins when offered" 0 "$TMP/ec.pem" "$TMP/ec.key" \
    $CHAIN -groups X25519 -- --expect-version 13

# ------------------------------------------------- session resumption (PSK) --
# Every case here runs TWO handshakes inside ONE client process, because the
# ticket cache is process-global static state (as it is in the kernel) and
# resumption is by definition a property of the second connection. The client
# asserts what the second handshake WAS -- resumed or full -- rather than
# printing it, so a case cannot pass by doing the wrong thing quietly.
#
# This is the part where "it should work" is not good enough: a client whose
# binder is wrong, whose obfuscated_ticket_age is wrong, or whose
# resumption_master_secret is keyed on the wrong transcript does not fail. The
# server just ignores the pre_shared_key extension and does a full handshake,
# which looks identical in every log except the RESUMED line.
echo
# --------------------------------------------------------------- P-521 ------
# THE REACHABILITY CASES. Every ClientHello this client has ever sent listed
# ecdsa_secp521r1_sha512 (0x0603) in signature_algorithms, and nothing could
# verify it: ecdsa_verify mapped every curve id that was not 256 to P-384. A
# server holding a P-521 certificate therefore took the offer, signed with it,
# and we failed at CertificateVerify -- so these two cases FAIL on the parent
# commit and pass here. That is the whole claim, and it is checkable by running
# this block against the previous build.
# --- OCSP stapling ---------------------------------------------------------
# The reachability proof for c/net/tls/ocsp.c: the unit test feeds it DER
# directly, and these two prove the TLS wiring around it -- that the
# status_request extension makes openssl staple at all, that the response is
# found in the CertificateEntry extension where TLS 1.3 puts it, and that its
# verdict reaches the handshake result. Without the second case the first one
# passes on a client that ignores the staple completely.
# --- Ed25519 chain signatures ----------------------------------------------
# The reachability proof for the x509.c Ed25519 path. Both cases send the
# Ed25519 CA in-band AND hold it as an anchor, so the chain exercises
# x509_verify_signed_by (leaf signed by the Ed25519 CA) and the root match.
SECTION="ed25519"
echo "-- Ed25519 chain signatures --"
# shellcheck disable=SC2086
case_run "Ed25519 CA over an EC leaf"        0 "$TMP/edleaf.pem" "$TMP/edleaf.key" \
         -cert_chain "$TMP/ed25519ca.pem" -groups X25519 --
# shellcheck disable=SC2086
case_run "Ed25519 CA over an EC leaf, TLS 1.3 (repeat under HRR)" 1 "$TMP/edleaf.pem" "$TMP/edleaf.key" \
         -cert_chain "$TMP/ed25519ca.pem" -groups P-256 --
# The same anchor over TLS 1.2. The chain check is the one thing the two
# protocol versions genuinely share, and it is worth showing that the new key
# type reaches it from both.
SRV_VER="-tls1_2"
# shellcheck disable=SC2086
case_run "Ed25519 CA over an EC leaf, TLS 1.2" 0 "$TMP/edleaf.pem" "$TMP/edleaf.key" \
         -cert_chain "$TMP/ed25519ca.pem" -groups X25519 --
SRV_VER="-tls1_3"
# An Ed25519 signature that verifies perfectly under a key we do not hold.
# shellcheck disable=SC2086
case_run "Ed25519 rejects an untrusted anchor" 0 "$TMP/edleafbad.pem" "$TMP/edleafbad.key" \
         -cert_chain "$TMP/ed25519bad.pem" -groups X25519 -- --expect-fail

# --- TLS_AES_256_GCM_SHA384 -------------------------------------------------
# The whole risk of this suite is a 32 written where a hash length should have
# been derived, and every such mistake surfaces as a Finished MAC failure. So
# the case is simply: pin the server to it and complete a handshake.
SECTION="gcm256"
echo "-- TLS_AES_256_GCM_SHA384 --"
# shellcheck disable=SC2086
case_run "AES-256-GCM-SHA384"               0 "$TMP/ec.pem" "$TMP/ec.key" $CHAIN \
         -groups X25519 -ciphersuites TLS_AES_256_GCM_SHA384 --
# shellcheck disable=SC2086
case_run "AES-256-GCM-SHA384 via HRR"       1 "$TMP/ec.pem" "$TMP/ec.key" $CHAIN \
         -groups P-384 -ciphersuites TLS_AES_256_GCM_SHA384 --
# shellcheck disable=SC2086
case_run "AES-256-GCM-SHA384, RSA leaf"     0 "$TMP/rsa.pem" "$TMP/rsa.key" $CHAIN \
         -groups X25519 -ciphersuites TLS_AES_256_GCM_SHA384 --

SECTION="ocsp"
echo "-- OCSP stapling --"
if [ "$HAVE_STAPLE" = 1 ]; then
    # shellcheck disable=SC2086
    case_run "OCSP staple: good"     0 "$TMP/ec.pem" "$TMP/ec.key" $CHAIN -groups X25519              -status_file "$TMP/staple_good.der" --
    # shellcheck disable=SC2086
    case_run "OCSP staple: REVOKED is refused" 0 "$TMP/ec.pem" "$TMP/ec.key" $CHAIN -groups X25519              -status_file "$TMP/staple_rev.der" -- --expect-fail
    # A staple whose bytes were changed: the signature must notice, and a
    # response we cannot verify must be fatal rather than treated as absent.
    head -c 400 "$TMP/staple_good.der" > "$TMP/staple_trunc.der" 2>/dev/null
    dd if="$TMP/staple_good.der" of="$TMP/staple_bad.der" status=none 2>/dev/null
    printf 'X' | dd of="$TMP/staple_bad.der" bs=1 seek=60 conv=notrunc status=none 2>/dev/null
    # shellcheck disable=SC2086
    case_run "OCSP staple: tampered is refused" 0 "$TMP/ec.pem" "$TMP/ec.key" $CHAIN -groups X25519              -status_file "$TMP/staple_bad.der" -- --expect-fail
    # and no staple at all is still fine -- see the policy in ocsp.h
    # shellcheck disable=SC2086
    case_run "no OCSP staple still connects" 0 "$TMP/ec.pem" "$TMP/ec.key" $CHAIN -groups X25519 --
else
    echo "SKIP: this openssl could not produce an OCSP response"
fi

SECTION="p521"
echo
echo "-- P-521 certificates --"
# shellcheck disable=SC2086
case_run "P-521 leaf + P-521 CA (1.3)"  0 "$TMP/p521.pem" "$TMP/p521.key" \
    -cert_chain "$TMP/ca521.pem" -groups X25519 -- --expect-version 13
# TLS 1.2 + a P-521 certificate is a DOCUMENTED LIMITATION, asserted here so it
# cannot quietly be believed to work. It is not a bug in the verification code
# this commit added -- that code is exercised by the 1.3 cases above.
#
# RFC 4492 5.1.1 makes supported_groups constrain the server's CERTIFICATE curve
# in TLS 1.2, not just the ECDHE group. We advertise x25519/P-256/P-384, so an
# openssl server holding a P-521 certificate refuses with handshake_failure
# before any signature is checked. This was verified to be the server's policy
# and not our parsing: openssl's OWN s_client, given
#     -tls1_2 -groups X25519:P-256:P-384
# against the same server, fails with the same alert 40, and succeeds the moment
# P-521 is added to that list. Our client is behaving exactly as openssl's does.
#
# Making it pass means advertising secp521r1 in supported_groups, which means
# supporting P-521 ECDHE -- struct tls_sess's priv[48]/pub[97]/speer[97] all
# have to grow to 66/133/133, on every one of 16 sessions. That is a separate
# change with its own memory and performance argument, and it is deliberately
# not smuggled in here. TLS 1.3 does not have this coupling (signature_algorithms
# alone decides), which is why the cases above pass and this one does not.
SRV_VER="-tls1_2"
# shellcheck disable=SC2086
case_run "P-521 cert unreachable on 1.2 (RFC 4492)" 0 "$TMP/p521.pem" "$TMP/p521.key" \
    -cert_chain "$TMP/ca521.pem" -groups X25519 -- --expect-fail
SRV_VER="-tls1_3"
# The anchor held but not sent in band, so signed_by_root() does the P-521
# signature check rather than the in-band chain walk.
case_run "P-521 anchor held, not sent"  0 "$TMP/p521.pem" "$TMP/p521.key" \
    -groups X25519 -- --expect-version 13
# A P-521 chain whose leaf name is wrong must still be refused: gaining a curve
# must not gain a way past the host-name check.
mkleaf p521bad ec521 not-localhost.example ca521
# shellcheck disable=SC2086
case_run "P-521 rejects wrong host name" 0 "$TMP/p521bad.pem" "$TMP/p521bad.key" \
    -cert_chain "$TMP/ca521.pem" -groups X25519 -- --expect-fail

SECTION="resume"
echo "-- TLS 1.3 session resumption --"

# shellcheck disable=SC2086
case_run "resumes (aes128-gcm)"     0 "$TMP/ec.pem" "$TMP/ec.key" \
    $CHAIN -groups X25519 -ciphersuites TLS_AES_128_GCM_SHA256 -- --expect-resume
# The PSK binder is keyed on the suite's hash, so the other suite is a
# genuinely different code path through the same schedule.
# shellcheck disable=SC2086
case_run "resumes (chacha20)"       0 "$TMP/ec.pem" "$TMP/ec.key" \
    $CHAIN -groups X25519 -ciphersuites TLS_CHACHA20_POLY1305_SHA256 -- --expect-resume
# An RSA leaf changes nothing about resumption -- and that is the point: the
# resumed handshake has NO certificate at all, so the case proves the chain
# verification really was skipped rather than silently re-run.
# shellcheck disable=SC2086
case_run "resumes (RSA leaf)"       0 "$TMP/rsa.pem" "$TMP/rsa.key" $CHAIN -groups X25519 -- --expect-resume
# Resumption across a HelloRetryRequest. The binder must be recomputed over the
# post-retry transcript (message_hash || HRR || truncated CH2); a client that
# reuses the first binder is rejected and falls back. Both connections here
# take the retry, so this asserts the recomputation, not just that HRR works.
# shellcheck disable=SC2086
case_run "resumes across HRR"       1 "$TMP/ec.pem" "$TMP/ec.key" $CHAIN -groups P-256 -- --expect-resume

# --- the negatives, which are the reason to trust the positives above ---
# A ticket belongs to the host that issued it. Connection 1 is localhost,
# connection 2 is alt.localhost on a certificate valid for both -- so nothing
# but the cache key can decide the outcome, and it must decide "full".
# shellcheck disable=SC2086
case_run "ticket is scoped to host" 0 "$TMP/multi.pem" "$TMP/multi.key" \
    $CHAIN -groups X25519 -- --sni2 alt.localhost --expect-no-resume
# A server that issues no tickets must produce no resumption. Without this the
# suite could not tell "we resume correctly" from "we claim to resume whenever
# we feel like it" -- the client would have nothing to offer and must say so.
# shellcheck disable=SC2086
case_run "no ticket -> no resume"   0 "$TMP/ec.pem" "$TMP/ec.key" \
    $CHAIN -groups X25519 -num_tickets 0 -- --expect-no-resume

# ============================================================== TLS 1.2 ======
# Everything below talks to a server pinned to -tls1_2, which is what
# sectigo.com / www.mas.gov.sg / www.cbuae.gov.ae actually are. Every case
# asserts the negotiated VERSION explicitly: a 1.2 case that silently completed
# over 1.3 would look identical in the pass column and prove nothing.
echo
SECTION="core"
echo "-- TLS 1.2 --"
SRV_VER="-tls1_2"

# --- cipher suites. Both AEADs at both key sizes, on both certificate key
#     types, because the suite decides three things at once: the AEAD, the PRF
#     hash (SHA-384 for the AES-256 suites -- a completely different transcript)
#     and which signature algorithm the ServerKeyExchange is signed with. ---
# shellcheck disable=SC2086
case_run "1.2 ECDHE-ECDSA-AES128-GCM"  0 "$TMP/ec.pem" "$TMP/ec.key" \
    $CHAIN -cipher ECDHE-ECDSA-AES128-GCM-SHA256 -- --expect-version 12
# shellcheck disable=SC2086
case_run "1.2 ECDHE-RSA-AES128-GCM"    0 "$TMP/rsa.pem" "$TMP/rsa.key" \
    $CHAIN -cipher ECDHE-RSA-AES128-GCM-SHA256 -- --expect-version 12
# The SHA-384 suites are the reason the transcript runs two hashes at once.
# shellcheck disable=SC2086
case_run "1.2 ECDHE-ECDSA-AES256-GCM"  0 "$TMP/ec.pem" "$TMP/ec.key" \
    $CHAIN -cipher ECDHE-ECDSA-AES256-GCM-SHA384 -- --expect-version 12
# shellcheck disable=SC2086
case_run "1.2 ECDHE-RSA-AES256-GCM"    0 "$TMP/rsa.pem" "$TMP/rsa.key" \
    $CHAIN -cipher ECDHE-RSA-AES256-GCM-SHA384 -- --expect-version 12
# ChaCha20 in 1.2 uses the 1.3-style implicit nonce, unlike GCM in the same
# version -- so it exercises a different branch of the record layer.
# shellcheck disable=SC2086
case_run "1.2 ECDHE-ECDSA-CHACHA20"    0 "$TMP/ec.pem" "$TMP/ec.key" \
    $CHAIN -cipher ECDHE-ECDSA-CHACHA20-POLY1305 -- --expect-version 12
# shellcheck disable=SC2086
case_run "1.2 ECDHE-RSA-CHACHA20"      0 "$TMP/rsa.pem" "$TMP/rsa.key" \
    $CHAIN -cipher ECDHE-RSA-CHACHA20-POLY1305 -- --expect-version 12

# --- key exchange groups. In 1.2 there is no HelloRetryRequest: the server
#     names its curve in the ServerKeyExchange and we generate a key on it. ---
# shellcheck disable=SC2086
case_run "1.2 x25519"                  0 "$TMP/ec.pem" "$TMP/ec.key" $CHAIN -groups X25519 -- --expect-version 12
# shellcheck disable=SC2086
case_run "1.2 secp256r1"               0 "$TMP/ec.pem" "$TMP/ec.key" $CHAIN -groups P-256  -- --expect-version 12
# shellcheck disable=SC2086
case_run "1.2 secp384r1"               0 "$TMP/ec.pem" "$TMP/ec.key" $CHAIN -groups P-384  -- --expect-version 12

# --- extended master secret, both ways. With the extension the master secret is
#     bound to the handshake hash; without it, to the two randoms. Both have to
#     work, and the pair is the only way to know the EMS branch is not dead code
#     that happens to be skipped. ---
# shellcheck disable=SC2086
case_run "1.2 extended master secret"  0 "$TMP/ec.pem" "$TMP/ec.key" $CHAIN -groups X25519 -- --expect-version 12
# shellcheck disable=SC2086
case_run "1.2 without EMS (-no_ems)"   0 "$TMP/ec.pem" "$TMP/ec.key" $CHAIN -groups X25519 -no_ems -- --expect-version 12

# --- the rest of the surface ---
# shellcheck disable=SC2086
case_run "1.2 alpn h2 preferred"       0 "$TMP/ec.pem" "$TMP/ec.key" \
    $CHAIN -groups X25519 -alpn h2,http/1.1 -- --alpn "h2,http/1.1" --expect-alpn h2 --expect-version 12
# A CertificateRequest we decline with an empty Certificate. `-verify` asks for
# a client certificate but does not require one, which is the configuration that
# distinguishes "declined politely" from "said nothing and got dropped".
# shellcheck disable=SC2086
case_run "1.2 declines client cert"    0 "$TMP/ec.pem" "$TMP/ec.key" \
    $CHAIN -groups X25519 -verify 1 -- --expect-version 12
# The anchor held but not sent in band, through the 1.2 Certificate parser.
case_run "1.2 anchor held, not sent"   0 "$TMP/ec.pem" "$TMP/ec.key" -groups X25519 -- --expect-version 12
# shellcheck disable=SC2086
case_run "1.2 blocking tls_connect"    0 "$TMP/ec.pem" "$TMP/ec.key" $CHAIN -groups X25519 -- --blocking

# --- rejections, which matter more than the successes ---
# shellcheck disable=SC2086
case_run "1.2 rejects wrong host name" 0 "$TMP/bad.pem" "$TMP/bad.key" $CHAIN -groups X25519 -- --expect-fail
case_run "1.2 rejects untrusted anchor" 0 "$TMP/rogue_leaf.pem" "$TMP/rogue_leaf.key" \
    -cert_chain "$TMP/rogue.pem" -groups X25519 -- --expect-fail

# --- the ServerKeyExchange signature. In TLS 1.2 the server's first flight is
#     in the clear, so an on-path attacker can rewrite it; the signature over
#     client_random || server_random || ECDHParams is the ONLY thing that
#     notices. tls12_tamper_proxy.py is that attacker. If either of these two
#     passes the handshake, the 1.2 path is unauthenticated -- which is worse
#     than not having 1.2 at all. ---
PROXY_PORT=$((PORT + 1))
tamper_case() {   # tamper_case <label> <mode> [want-log-line]
    local label="$1" mode="$2" want="${3:-ServerKeyExchange signature rejected}"
    if [ -n "$ONLY" ] && [ "$ONLY" != "$SECTION" ]; then return; fi
    if ! start_server "$TMP/ec.pem" "$TMP/ec.key" -cert_chain "$TMP/ca.pem" -groups X25519; then
        echo "FAIL $label (server did not start)"; fail=$((fail+1)); return
    fi
    python3 "$ROOT/tests/unit/tls12_tamper_proxy.py" "$PROXY_PORT" "$PORT" "$mode" \
        >"$TMP/proxy.log" 2>&1 &
    local ppid=$!
    for _ in $(seq 1 100); do
        (exec 3<>/dev/tcp/127.0.0.1/"$PROXY_PORT") 2>/dev/null && { exec 3>&-; break; }
        sleep 0.05
    done
    "$BUILD/tls_interop_test" 127.0.0.1 "$PROXY_PORT" localhost --expect-fail >"$TMP/client.log" 2>&1
    local rc=$?
    kill "$ppid" 2>/dev/null; wait "$ppid" 2>/dev/null
    stop_server
    # Not just "it failed": it must have failed for the RIGHT reason. A client
    # that rejected the tampered handshake at some earlier parse error would
    # also reject an honest server, and would pass a bare "it failed" check.
    if [ $rc -eq 0 ] && grep -q "$want" "$TMP/client.log"; then
        echo "ok   $label"; pass=$((pass+1))
    else
        echo "FAIL $label (rc=$rc, expected '$want' in the log)"
        sed 's/^/    | /' "$TMP/client.log"; sed 's/^/    p /' "$TMP/proxy.log"
        fail=$((fail+1))
    fi
}
tamper_case "1.2 rejects tampered SKE signature" sig
tamper_case "1.2 rejects substituted ECDHE key"  pubkey
# Not a signature case: a length field claiming more bytes than the message
# holds. The client here is built with ASan, so "rejected cleanly" and "did not
# read past the buffer" are both being asserted.
tamper_case "1.2 rejects over-long ECDHE point"  pointlen "ServerKeyExchange malformed"

# ================================================= fail-closed: ZERO anchors ==
# Everything above proves the trust store lets the right chains through and
# keeps the wrong ones out. This block proves the other end of the scale: with
# NO anchors at all, nothing gets through.
#
# The property is structural -- both trust loops are bounded by logit_nroots
# (c/net/tls/x509.c:422 is_pinned_root, :447 signed_by_root), so at zero they
# both return 0 and x509_verify_chain falls to X509_E_UNTRUSTED (x509.c:534-536)
# -- and a tree-wide grep for skip_verify / insecure / allow_untrusted finds
# nothing. But a property nobody has ever watched fail is not evidence, and the
# day someone adds a "just this once" bypass, every case above still passes.
#
# THREE THINGS MAKE THIS A PROOF RATHER THAN A RUN THAT HAPPENED TO GO RED:
#
#  1. The two binaries differ ONLY in the trust TU. Same sources, same flags,
#     same server, same connection -- see SRC_NOTRUST above. So a refusal cannot
#     be blamed on anything else in the client.
#  2. Every case runs the TRUSTED client against the SAME live server first. If
#     that control does not connect, the case is reported as broken rather than
#     scored -- otherwise a server that failed to start, a bad certificate or a
#     wrong port would all "prove" fail-closed.
#  3. The refusal must carry the RIGHT REASON. `--expect-fail` is satisfied by
#     any failure at all, including a build that produced a client which cannot
#     parse a ServerHello. The log has to say "no path to a trusted root", which
#     is tls.c:954 printing X509_E_UNTRUSTED specifically.
#
# And before any of that, the substitute store is verified to BE empty and to
# LINK -- see tests/unit/roots_count_probe.c for why both halves are load-bearing.
SECTION="noroots"
# TLS_INTEROP_NOROOTS_CONTROL=1 is this block's negative control: the substitute
# client is built against this suite's OWN test bundle (the three throwaway CAs
# minted above) instead of an empty one,
# so every case here MUST fail, and the verdict at the bottom is inverted. It is
# the only way to watch this block fail on purpose -- the honest bypass it is
# meant to catch would live in c/net/tls, which this harness does not edit.
# (NRCTL/NRBIN are declared at the top, next to ONLY, for the reason given there.)

if [ -z "$ONLY" ] || [ "$ONLY" = "$SECTION" ]; then
echo
echo "-- fail-closed: a trust store with ZERO anchors --"

mkdir -p "$TMP/nr" "$TMP/nrroots"
# An empty source directory -- unless this is the control run, in which case the
# substitute store is the suite's own populated test store and the block must go red.
NRSRCDIR="$TMP/nrroots"; NRWANT="zero"
if [ -n "$NRCTL" ]; then NRSRCDIR="$TMP/roots"; NRWANT="nonzero"; fi

nrfail() { echo "FAIL fail-closed: $1"; fail=$((fail+1)); }

if ! python3 "$ROOT/tools/genroots.py" "$NRSRCDIR" "$TMP/nr/roots_bundle.inc" >"$TMP/nr/gen.log" 2>&1; then
    nrfail "tools/genroots.py could not build a bundle from $NRSRCDIR"
    sed 's/^/    | /' "$TMP/nr/gen.log"
else
    cp "$ROOT/c/crypto/trust/roots.c" "$TMP/nr/roots_test.c"
    # -I$TMP/nr FIRST, ahead of the populated test bundle in -I$TMP. roots.c reaches
    # its bundle with a quoted include, which resolves against the including
    # file's own directory before any -I -- so this is belt and braces, and the
    # probe below is what actually settles which bundle got linked.
    NRINCS="-I$TMP/nr $INCS"

    # (0a) The substitute store must LINK and must be EMPTY, asserted before a
    #      single handshake. genroots.py emits `logit_roots[] = {};` for an empty
    #      directory (tools/genroots.py:222-224) -- an empty C initialiser, which
    #      is a C23 feature and a GNU extension. Measured on this toolchain:
    #      clang accepts it at the default -std with no diagnostic and
    #      logit_nroots comes out 0. If that ever stops being true the failure is
    #      a COMPILE error, and a harness that only asked "did the run fail?"
    #      would score it as a pass.
    if ! $CC -O1 -g -Wall -Wextra -o "$BUILD/roots_count_probe_nr" \
            "$ROOT/tests/unit/roots_count_probe.c" "$TMP/nr/roots_test.c" $NRINCS \
            >"$TMP/nr/probe_build.log" 2>&1; then
        nrfail "the substitute trust bundle does not COMPILE. genroots.py emits an
      empty C initialiser for an empty directory; this toolchain rejects it.
      That is a REQUEST for the owner of tools/genroots.py, not a pass."
        sed 's/^/    | /' "$TMP/nr/probe_build.log"
    elif ! "$BUILD/roots_count_probe_nr" "$NRWANT" >"$TMP/nr/probe.log" 2>&1; then
        nrfail "the substitute trust store is not what this block requires"
        sed 's/^/    | /' "$TMP/nr/probe.log"
    else
        echo "ok   fail-closed: substitute store links and is $NRWANT  $(cat "$TMP/nr/probe.log")"
        pass=$((pass+1))

        # (0b) The same probe against the REAL bundle. Without this, a probe that
        #      reported 0 unconditionally -- or a bundle path that silently
        #      resolved to nothing -- would satisfy (0a) and mean nothing.
        if $CC -O1 -g -Wall -Wextra -o "$BUILD/roots_count_probe_real" \
               "$ROOT/tests/unit/roots_count_probe.c" "$ROOT/c/crypto/trust/roots.c" \
               "-I$ROOT/c/crypto/trust" >"$TMP/nr/probe_real_build.log" 2>&1 \
           && "$BUILD/roots_count_probe_real" nonzero >"$TMP/nr/probe_real.log" 2>&1; then
            echo "ok   fail-closed: the same probe reads the real store as non-empty  $(cat "$TMP/nr/probe_real.log")"
            pass=$((pass+1))
        else
            nrfail "the probe cannot see the REAL trust store as non-empty, so its
      'empty' verdict distinguishes nothing"
            sed 's/^/    | /' "$TMP/nr/probe_real_build.log" "$TMP/nr/probe_real.log" 2>/dev/null
        fi

        # (0c) Build the substitute client. A build failure here is a FAILURE,
        #      never a skip: a client that does not exist refuses every server,
        #      which is exactly the observation this block is trying to make.
        # shellcheck disable=SC2086
        if $CC -O1 -g -Wall -Wextra $SAN $BREAKDEF -o "$BUILD/tls_interop_test_noroots" \
               $SRC_NOTRUST "$TMP/nr/roots_test.c" $NRINCS >"$TMP/nr/build.log" 2>&1; then
            NRBIN="$BUILD/tls_interop_test_noroots"
        else
            nrfail "could not build the zero-anchor client"
            sed 's/^/    | /' "$TMP/nr/build.log"
        fi
    fi
fi

# noroots_pair <label> <srv-ver> <chain> <key> [server-args...]
# Runs the trusted client and the zero-anchor client against ONE server process,
# back to back. The trusted run is the control and is checked first.
noroots_pair() {
    local label="$1" ver="$2" chain="$3" key="$4"; shift 4
    [ -z "$NRBIN" ] && return
    SRV_VER="$ver"
    if ! start_server "$chain" "$key" "$@"; then
        nrfail "$label (server did not start)"; cat "$TMP/server.log"; return
    fi
    "$BUILD/tls_interop_test" 127.0.0.1 "$PORT" localhost >"$TMP/nr_ctl.log" 2>&1
    local crc=$?
    "$NRBIN" 127.0.0.1 "$PORT" localhost --expect-fail >"$TMP/nr_case.log" 2>&1
    local nrc=$?
    stop_server

    if [ $crc -ne 0 ]; then
        nrfail "$label -- the TRUSTED control could not connect to this server,
      so a refusal by the zero-anchor client proves nothing about anchors"
        sed 's/^/    | /' "$TMP/nr_ctl.log"
        return
    fi
    if [ $nrc -ne 0 ]; then
        nrfail "$label -- the zero-anchor client did NOT refuse (a chain verified
      against a store holding no anchors)"
        sed 's/^/    | /' "$TMP/nr_case.log"
        return
    fi
    if ! grep -qa "no path to a trusted root" "$TMP/nr_case.log"; then
        nrfail "$label -- refused, but not for want of an anchor. Expected
      x509.c's X509_E_UNTRUSTED (tls.c:954 'no path to a trusted root')"
        sed 's/^/    | /' "$TMP/nr_case.log"
        return
    fi
    # Echo back what the client itself said its store held, so a passing line
    # carries its own evidence instead of only a label. Printed if present and
    # never required: the banner belongs to c/crypto/trust, and a gate that
    # greps for another owner's log wording breaks the day they reword it.
    local banner
    banner="$(grep -a -m1 'trust store:' "$TMP/nr_case.log" || true)"
    echo "ok   $label  (with anchors: connects; zero anchors: no path to a trusted root)${banner:+  [client saw: ${banner#*trust store: }]}"
    pass=$((pass+1))
}

# The in-band anchor path: the server sends leaf + CA, so with the anchor held this
# is decided by is_pinned_root (x509.c:420-435), the loop that byte-compares the
# top certificate's key against each held root.
# shellcheck disable=SC2086
noroots_pair "zero anchors refuse an in-band anchor (1.3)" -tls1_3 \
    "$TMP/ec.pem" "$TMP/ec.key" $CHAIN -groups X25519
# The held-not-sent path: leaf only, so with the anchor held this is decided by
# signed_by_root (x509.c:439-462) -- the OTHER loop, and the common case on the
# real web. Both loops have to be shown closing, not one of them.
noroots_pair "zero anchors refuse a held-not-sent anchor (1.3)" -tls1_3 \
    "$TMP/ec.pem" "$TMP/ec.key" -groups X25519
# An RSA anchor reaches the same loops down a different verify_with_key branch.
# shellcheck disable=SC2086
noroots_pair "zero anchors refuse an RSA leaf (1.3)" -tls1_3 \
    "$TMP/rsa.pem" "$TMP/rsa.key" $CHAIN -groups X25519
# TLS 1.2 arrives at tls_check_chain from tls12.c, not tls.c. The certificate is
# the one thing the two versions share, and sharing it is worth asserting rather
# than assuming.
# shellcheck disable=SC2086
noroots_pair "zero anchors refuse over TLS 1.2" -tls1_2 \
    "$TMP/ec.pem" "$TMP/ec.key" $CHAIN -groups X25519
SRV_VER="-tls1_3"

# Session resumption is not a bypass, and this is why it needs no case of its
# own: a resumed handshake carries no certificate, but the ticket that makes it
# possible is only ever issued on a connection that already verified one. With
# zero anchors the first connection above never completes, so there is nothing
# to resume from -- the cache is populated by success, and success is what is
# gone. (`--expect-resume` cases in the resume block above are what assert the
# cache is only filled that way.)
fi

echo
echo "$pass passed, $fail failed"

# --- the verdict ---
if [ -n "$NRCTL" ]; then
    # The fail-closed block's own negative control. The substitute client was
    # built against a POPULATED bundle, so all FOUR pairs must have
    # noticed. "fail > 0" is not enough here: a build error would also produce
    # it, and would credit the control for an outcome it did not cause.
    if [ -n "$NRBIN" ] && [ "$fail" -ge 4 ]; then
        echo "PASS: fail-closed negative control -- a non-empty substitute store"
        echo "      was caught by all $fail case(s); the block is not vacuous."
        exit 0
    fi
    echo "FAIL: fail-closed negative control -- expected all 4 pairs to fail with a"
    echo "      populated substitute store (got fail=$fail, client='$NRBIN')."
    echo "      Either the block cannot tell the two stores apart, or it never ran."
    exit 1
fi
if [ -n "$BREAK" ]; then
    # Negative control. The client was built with a deliberate defect, so the
    # ONLY acceptable outcome is that this suite caught it. A control that
    # "passes" by going green is not a control, it is a second way to be wrong.
    if [ "$fail" -gt 0 ]; then
        echo "PASS: negative control -- $BREAK was detected by $fail case(s)"
        exit 0
    fi
    echo "FAIL: negative control -- $BREAK changed nothing, so these tests"
    echo "      do not actually test what they claim to."
    exit 1
fi
[ "$fail" -eq 0 ] || exit 1
# A run that executed no cases at all must not report success -- that is how a
# broken filter turns into a green build.
[ "$pass" -gt 0 ] || { echo "FAIL: no cases ran"; exit 1; }
echo "PASS: TLS interop"
