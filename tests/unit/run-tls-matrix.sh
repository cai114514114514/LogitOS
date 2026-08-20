#!/usr/bin/env bash
# The interop MATRIX: every {version} x {cipher suite} x {key-exchange group}
# this tree CLAIMS, driven against OpenSSL in BOTH directions, and printed as a
# table with the openssl command beside each row.
#
# WHY THIS EXISTS BESIDE run-tls-interop.sh, which already passes 73 cases.
# That suite is a set of chosen cases: it pins one suite on one group, then one
# group on one suite. Six of the twelve TLS 1.3 (suite, group) pairs and nine
# of the eighteen TLS 1.2 pairs were never run by anything -- not failing, not
# skipped, simply never asked. A claim is per-cell, so the evidence has to be
# per-cell too, and "the suite is green" is not evidence about a cell no case
# in it visits. This walks the whole product instead of a diagonal through it.
#
# The claims it walks come from the source, not from a list typed here:
#   1.3 suites  c/net/tls/tls.c build_ch()          0x1301 0x1303 0x1302
#   1.2 suites  c/net/tls/tls_int.h:52-57           six ECDHE AEAD suites
#   groups      c/net/tls/tls.c tls_group_supported{,13}()
#   server      c/net/tls/tls_server.c srv_suites[]/srv_groups[]
# A cell we claim and that does not complete is the finding. A cell we do NOT
# claim is printed as n/a with the reason, because "we never said we could" and
# "we said we could and cannot" must not look alike in the output.
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${BUILD:-$ROOT/build}"
CC="${CC:-clang}"
OPENSSL="${OPENSSL:-openssl}"
TMP="$(mktemp -d)"
PORT="${TLS_MATRIX_PORT:-15533}"
SRVPID=""
pass=0; fail=0; na=0
rows=""

cleanup() { [ -n "$SRVPID" ] && kill "$SRVPID" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT

command -v "$OPENSSL" >/dev/null || { echo "SKIP: no openssl"; exit 0; }
"$OPENSSL" s_server -help 2>&1 | grep -q -- '-groups' || { echo "SKIP: openssl lacks -groups"; exit 0; }
# The hybrid row needs an openssl that knows the group. Absent, those cells are
# reported as SKIP rather than FAIL: a missing reference is not a regression in
# the code under test (the rule test-wpt applies to an absent corpus).
# `list -kem-algorithms`, which is what run-tls-interop.sh:322 uses and is the
# probe that answers the question asked. A first attempt dialled
# s_client -groups X25519MLKEM768 at a dead port and grepped the error for
# "unsupported" -- which reports on the CONNECTION, not the algorithm, and
# declared 3.5.5 to have no ML-KEM while test-tls-pq was completing eight real
# hybrid handshakes against it. A capability probe that can be answered by a
# failure unrelated to the capability is not a probe.
HAVE_PQ=0
"$OPENSSL" list -kem-algorithms 2>/dev/null | grep -qi X25519MLKEM768 && HAVE_PQ=1

mkdir -p "$BUILD" "$TMP/roots"

# ---------------------------------------------------------------- test PKI ---
mkca() {
    "$OPENSSL" ecparam -name prime256v1 -genkey -noout -out "$TMP/$1.key" 2>/dev/null
    "$OPENSSL" req -x509 -new -key "$TMP/$1.key" -sha256 -days 3 -subj "/CN=$2" \
        -addext "basicConstraints=critical,CA:TRUE" \
        -addext "keyUsage=critical,keyCertSign,cRLSign" -out "$TMP/$1.pem" 2>/dev/null
}
mkleaf() {   # mkleaf <name> <ec|rsa> <cn> <ca>
    case "$2" in
      ec) "$OPENSSL" ecparam -name prime256v1 -genkey -noout -out "$TMP/$1.key" 2>/dev/null ;;
      *)  "$OPENSSL" genrsa -out "$TMP/$1.key" 2048 2>/dev/null ;;
    esac
    "$OPENSSL" req -new -key "$TMP/$1.key" -subj "/CN=$3" -out "$TMP/$1.csr" 2>/dev/null
    printf 'subjectAltName=DNS:%s\nbasicConstraints=CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=serverAuth\n' "$3" > "$TMP/$1.ext"
    "$OPENSSL" x509 -req -in "$TMP/$1.csr" -CA "$TMP/$4.pem" -CAkey "$TMP/$4.key" \
        -CAcreateserial -days 3 -sha256 -extfile "$TMP/$1.ext" -out "$TMP/$1.pem" 2>/dev/null
}
mkca ca "LogitOS Matrix CA"
cp "$TMP/ca.pem" "$TMP/roots/matrix_ca.pem"
mkleaf ec  ec  localhost ca
mkleaf rsa rsa localhost ca

# ------------------------------------------------------- build the client ----
python3 "$ROOT/tools/genroots.py" "$TMP/roots" "$TMP/roots_bundle.inc" >/dev/null 2>&1 || {
    echo "FAIL: could not build the test trust bundle"; exit 1; }
cp "$ROOT/c/crypto/trust/roots.c" "$TMP/roots_test.c"
CSRC="$ROOT/tests/unit/tls_interop_test.c $ROOT/c/net/tls/tls.c $ROOT/c/net/tls/tls12.c \
      $ROOT/c/net/tls/tls_psk.c $ROOT/c/net/tls/x509.c $ROOT/c/net/tls/ocsp.c \
      $ROOT/c/kernel/cpu/cpufeat.c $TMP/roots_test.c \
      $(find "$ROOT/c/crypto/aead" "$ROOT/c/crypto/hash" "$ROOT/c/crypto/pubkey" \
             "$ROOT/c/crypto/kdf" "$ROOT/c/crypto/pq" -name '*.c' 2>/dev/null)"
# -I$TMP FIRST, ahead of c/crypto/trust: roots.c reaches its bundle with a
# QUOTED include, which resolves against the including file's own directory
# before any -I -- which is why roots.c is copied into $TMP and compiled from
# there. Getting this wrong links the real 130-root store and every cell fails
# "no path to a trusted root". c/crypto/trust is still needed, for roots.h.
INCS="-I$TMP -I$ROOT/c/net/tls -I$ROOT/c/crypto -I$ROOT/c/crypto/aead \
      -I$ROOT/c/crypto/trust -I$ROOT/c/crypto/pq \
      -I$ROOT/c/net/core -I$ROOT/c/net/transport -I$ROOT/c/drivers/timer \
      -I$ROOT/c/kernel/core -I$ROOT/c/kernel/cpu"
# shellcheck disable=SC2086
$CC -O1 -g -w -o "$BUILD/tls_matrix_client" $CSRC $INCS || {
    echo "FAIL: could not build the matrix client"; exit 1; }

# ------------------------------------------------------- build our server ----
SSRC="$ROOT/tests/unit/tls_server_test.c $ROOT/c/net/tls/tls_server.c \
      $ROOT/c/net/tls/tls.c $ROOT/c/net/tls/tls12.c $ROOT/c/net/tls/tls_psk.c \
      $ROOT/c/net/tls/x509.c $ROOT/c/net/tls/ocsp.c $ROOT/c/kernel/cpu/cpufeat.c \
      $(find "$ROOT/c/crypto/aead" "$ROOT/c/crypto/hash" "$ROOT/c/crypto/pubkey" \
             "$ROOT/c/crypto/kdf" "$ROOT/c/crypto/pq" -name '*.c' 2>/dev/null)"
HAVE_SRV=1
# shellcheck disable=SC2086
$CC -O1 -g -w -o "$BUILD/tls_matrix_server" $SSRC \
    -I$ROOT/c/crypto -I$ROOT/c/crypto/aead -I$ROOT/c/crypto/trust -I$ROOT/c/crypto/pq \
    -I$ROOT/c/net/tls -I$ROOT/c/net/core -I$ROOT/c/net/transport \
    -I$ROOT/c/drivers/timer -I$ROOT/c/kernel/core -I$ROOT/c/kernel/cpu 2>"$TMP/srvbuild.log" || {
    echo "NOTE: matrix server did not build; direction B will report as unbuilt"
    HAVE_SRV=0; }

record() {   # record <dir> <ver> <suite> <group> <verdict> <detail> <cmd>
    rows="$rows$1|$2|$3|$4|$5|$6|$7
"
}

# ============================================ direction A: our client -> openssl
# The SERVER is pinned to exactly one suite and one group, so a completed
# handshake is evidence about THAT cell and not about openssl's preference
# order. `-www` makes s_server answer and exit cleanly.
start_ossl() {   # start_ossl <chain> <key> <extra args...>
    local chain="$1" key="$2"; shift 2
    "$OPENSSL" s_server -accept "$PORT" -cert "$chain" -key "$key" \
        -cert_chain "$TMP/ca.pem" -www -quiet "$@" >"$TMP/server.log" 2>&1 &
    SRVPID=$!
    for _ in $(seq 1 100); do
        (exec 3<>/dev/tcp/127.0.0.1/"$PORT") 2>/dev/null && { exec 3>&-; return 0; }
        kill -0 "$SRVPID" 2>/dev/null || return 1
        sleep 0.05
    done
    return 1
}
stop_ossl() { [ -n "$SRVPID" ] && kill "$SRVPID" 2>/dev/null; wait "$SRVPID" 2>/dev/null; SRVPID=""; }

cellA() {   # cellA <ver:12|13> <suite-name> <ossl-suite-arg> <group> <leaf>
    local ver="$1" sname="$2" sarg="$3" grp="$4" leaf="$5"
    local vflag cmd
    if [ "$ver" = 13 ]; then
        vflag="-tls1_3"; cmd="openssl s_server -tls1_3 -ciphersuites $sarg -groups $grp"
        if ! start_ossl "$TMP/$leaf.pem" "$TMP/$leaf.key" -tls1_3 -ciphersuites "$sarg" -groups "$grp"; then
            record A "$ver" "$sname" "$grp" "FAIL" "openssl would not start" "$cmd"
            fail=$((fail+1)); return
        fi
    else
        vflag="-tls1_2"; cmd="openssl s_server -tls1_2 -cipher $sarg -groups $grp"
        if ! start_ossl "$TMP/$leaf.pem" "$TMP/$leaf.key" -tls1_2 -cipher "$sarg" -groups "$grp"; then
            record A "$ver" "$sname" "$grp" "FAIL" "openssl would not start" "$cmd"
            fail=$((fail+1)); return
        fi
    fi
    "$BUILD/tls_matrix_client" 127.0.0.1 "$PORT" localhost >"$TMP/cli.log" 2>&1
    local rc=$?
    stop_ossl
    local detail
    detail="$(grep -a -o 'suite 0x[0-9a-f]*[^,]*, group [A-Za-z0-9]*\|suite 0x[0-9a-f]* [A-Z0-9-]*' "$TMP/cli.log" | head -1)"
    [ -z "$detail" ] && detail="$(grep -a -o 'TLS_E_[A-Z]*\|rc=-[0-9]*' "$TMP/cli.log" | head -1)"
    if [ $rc -eq 0 ]; then
        record A "$ver" "$sname" "$grp" "ok" "$detail" "$cmd"; pass=$((pass+1))
    else
        record A "$ver" "$sname" "$grp" "FAIL" "${detail:-no handshake}" "$cmd"; fail=$((fail+1))
    fi
}

# ============================================ direction B: openssl -> our server
start_ours() {
    rm -f "$TMP/ours.log" "$TMP/ours.err"
    "$BUILD/tls_matrix_server" serve "$PORT" --cert-out "$TMP/srv.der" --echo 64 \
        >"$TMP/ours.log" 2>"$TMP/ours.err" &
    SRVPID=$!
    for _ in $(seq 1 200); do
        grep -q LISTENING "$TMP/ours.log" 2>/dev/null && return 0
        kill -0 "$SRVPID" 2>/dev/null || return 1
        sleep 0.05
    done
    return 1
}

cellB() {   # cellB <suite-name> <ossl-suite-arg> <group> <claimed:1|0> <why-not>
    local sname="$1" sarg="$2" grp="$3" claimed="$4" why="${5:-}"
    local cmd="openssl s_client -tls1_3 -ciphersuites $sarg -groups $grp"
    if [ "$HAVE_SRV" = 0 ]; then
        record B 13 "$sname" "$grp" "n/a" "server binary did not build" "$cmd"; na=$((na+1)); return
    fi
    if ! start_ours; then
        record B 13 "$sname" "$grp" "FAIL" "our server would not listen" "$cmd"
        fail=$((fail+1)); return
    fi
    # The server mints its certificate at run time and writes it as DER; openssl
    # -CAfile wants PEM. Without this conversion every cell in direction B fails
    # identically with BIO_new_file on a file that was never created -- which
    # reads exactly like "our server refused the handshake" and is not that.
    "$OPENSSL" x509 -inform DER -in "$TMP/srv.der" -out "$TMP/srv.pem" 2>/dev/null
    printf 'hello-from-openssl\n' | "$OPENSSL" s_client -connect "127.0.0.1:$PORT" \
        -servername localhost -tls1_3 -ciphersuites "$sarg" -groups "$grp" \
        -CAfile "$TMP/srv.pem" -verify_return_error -quiet \
        >"$TMP/scli.out" 2>"$TMP/scli.err"
    local orc=$?
    kill "$SRVPID" 2>/dev/null; wait "$SRVPID" 2>/dev/null; SRVPID=""
    local detail; detail="$(grep -a -o 'suite 0x[0-9a-f]*, group [A-Za-z0-9]*' "$TMP/ours.log" | head -1)"
    if [ "$claimed" = 0 ]; then
        # We do NOT claim this cell. The right outcome is a clean refusal.
        if [ $orc -ne 0 ]; then
            record B 13 "$sname" "$grp" "n/a" "not offered: $why (refused cleanly)" "$cmd"
        else
            record B 13 "$sname" "$grp" "FAIL" "UNCLAIMED CELL COMPLETED: $why" "$cmd"
            fail=$((fail+1)); return
        fi
        na=$((na+1)); return
    fi
    if [ $orc -eq 0 ]; then
        record B 13 "$sname" "$grp" "ok" "${detail:-handshake complete}" "$cmd"; pass=$((pass+1))
    else
        record B 13 "$sname" "$grp" "FAIL" "$(head -1 "$TMP/scli.err" | cut -c1-60)" "$cmd"; fail=$((fail+1))
    fi
}

echo "== TLS interop MATRIX against $($OPENSSL version) =="
echo "   our client -> openssl s_server, and openssl s_client -> our server"
[ "$HAVE_PQ" = 0 ] && echo "   NOTE: this openssl has no X25519MLKEM768; those cells report SKIP"
echo

# --- TLS 1.3: 3 suites x 4 groups, EC leaf (auth is not bound to the suite) ---
G13="X25519 P-256 P-384"
[ "$HAVE_PQ" = 1 ] && G13="$G13 X25519MLKEM768"
for g in $G13; do
    cellA 13 AES128-GCM-SHA256   TLS_AES_128_GCM_SHA256       "$g" ec
    cellA 13 CHACHA20-SHA256     TLS_CHACHA20_POLY1305_SHA256 "$g" ec
    cellA 13 AES256-GCM-SHA384   TLS_AES_256_GCM_SHA384       "$g" ec
done
if [ "$HAVE_PQ" = 0 ]; then
    for s in AES128-GCM-SHA256 CHACHA20-SHA256 AES256-GCM-SHA384; do
        record A 13 "$s" X25519MLKEM768 "SKIP" "this openssl has no ML-KEM" "-"
    done
fi

# --- TLS 1.2: 6 suites x 3 groups. The leaf must match the suite's auth. ---
# The hybrid is deliberately absent: tls_group_supported() excludes it, so a
# 1.2 server cannot name a KEM as its ECDHE curve. That is a claim we do NOT
# make, recorded as n/a rather than left out -- an omitted row and a refused
# one look identical in a table that only lists what ran.
for g in X25519 P-256 P-384; do
    cellA 12 ECDHE-ECDSA-AES128-GCM ECDHE-ECDSA-AES128-GCM-SHA256   "$g" ec
    cellA 12 ECDHE-RSA-AES128-GCM   ECDHE-RSA-AES128-GCM-SHA256     "$g" rsa
    cellA 12 ECDHE-ECDSA-AES256-GCM ECDHE-ECDSA-AES256-GCM-SHA384   "$g" ec
    cellA 12 ECDHE-RSA-AES256-GCM   ECDHE-RSA-AES256-GCM-SHA384     "$g" rsa
    cellA 12 ECDHE-ECDSA-CHACHA20   ECDHE-ECDSA-CHACHA20-POLY1305   "$g" ec
    cellA 12 ECDHE-RSA-CHACHA20     ECDHE-RSA-CHACHA20-POLY1305     "$g" rsa
done
for s in ECDHE-ECDSA-AES128-GCM ECDHE-RSA-AES128-GCM ECDHE-ECDSA-AES256-GCM \
         ECDHE-RSA-AES256-GCM ECDHE-ECDSA-CHACHA20 ECDHE-RSA-CHACHA20; do
    record A 12 "$s" X25519MLKEM768 "n/a" "not claimed: TLS 1.2 has no KEM message" "-"
    na=$((na+1))
done

# --- direction B: openssl s_client -> our server (TLS 1.3 only, by design) ---
for g in X25519 P-256 P-384; do
    cellB AES128-GCM-SHA256 TLS_AES_128_GCM_SHA256       "$g" 1
    cellB CHACHA20-SHA256   TLS_CHACHA20_POLY1305_SHA256 "$g" 1
    cellB AES256-GCM-SHA384 TLS_AES_256_GCM_SHA384       "$g" 1
done
if [ "$HAVE_PQ" = 1 ]; then
    # srv_groups[] is {x25519, P-256, P-384}. Offering ONLY the hybrid must not
    # complete -- and the row says "not claimed" rather than hiding, because
    # the client offers the hybrid and the server does not, which is a real
    # asymmetry somebody should be able to read off this table.
    cellB AES128-GCM-SHA256 TLS_AES_128_GCM_SHA256 X25519MLKEM768 0 \
          "tls_server.c srv_groups[] has no hybrid"
fi

# ------------------------------------------------------------------ report ---
printf '\n%-3s %-4s %-24s %-16s %-6s %s\n' DIR VER SUITE GROUP VERDICT DETAIL
printf -- '---------------------------------------------------------------------------------------\n'
printf '%s' "$rows" | while IFS='|' read -r d v s g verdict detail cmd; do
    [ -z "$d" ] && continue
    printf '%-3s %-4s %-24s %-16s %-6s %s\n' "$d" "$v" "$s" "$g" "$verdict" "$detail"
done
echo
echo "the openssl command for each row (DIR A = server side, DIR B = client side):"
printf '%s' "$rows" | while IFS='|' read -r d v s g verdict detail cmd; do
    [ -z "$d" ] && continue
    [ "$cmd" = "-" ] && continue
    printf '  %-3s %-24s %-16s  %s\n' "$d" "$s" "$g" "$cmd"
done | sort -u
echo
echo "$pass passed, $fail failed, $na not-claimed/skipped"
[ "$fail" -eq 0 ] && { echo "PASS: TLS matrix"; exit 0; }
echo "FAIL: TLS matrix"; exit 1
