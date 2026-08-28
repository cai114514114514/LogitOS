#!/usr/bin/env bash
# scrypt (RFC 7914) differential against OpenSSL 3.6.3's SCRYPT EVP_KDF.
#
# WHY A DIFFERENTIAL ON TOP OF THE RFC KAT (test-scrypt / scrypt_test.c). The
# RFC vectors only exercise r in {1, 8} and p in {1, 16}. This exercises r in
# {1,2,3,4,8} and p in {1,2,3,16} against a second, independently written
# implementation, which is the only way to catch an off-by-one in the
# interleave or the block-size arithmetic that happens to cancel out at
# exactly the r values RFC 7914 chose to publish.
#
# A missing reference is not a regression in the code under test -- same rule
# test-mlkem-openssl and test-wpt use for an absent corpus/tool.
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${BUILD:-$ROOT/build}"
CC="${CC:-cc}"
OPENSSL="${OPENSSL:-/opt/homebrew/opt/openssl@3/bin/openssl}"
command -v "$OPENSSL" >/dev/null 2>&1 || OPENSSL=openssl
command -v "$OPENSSL" >/dev/null 2>&1 || { echo "SKIP: no openssl on PATH"; exit 0; }
"$OPENSSL" kdf -keylen 8 -kdfopt pass: -kdfopt salt: -kdfopt n:16 -kdfopt r:1 -kdfopt p:1 SCRYPT \
    >/dev/null 2>&1 || { echo "SKIP: this openssl's kdf command has no SCRYPT"; exit 0; }

mkdir -p "$BUILD"
$CC -O2 -Wall -Wextra -o "$BUILD/scrypt_cli" \
    "$ROOT/tests/unit/scrypt_cli.c" "$ROOT/c/crypto/kdf/scrypt.c" "$ROOT/c/crypto/hash/sha256.c" \
    -I"$ROOT/c/crypto" -I"$ROOT/c/crypto/kdf" || { echo "FAIL: could not build scrypt_cli"; exit 1; }

fail=0; n=0
check() {   # pw salt N r p dklen
    n=$((n+1))
    local pw="$1" salt="$2" N="$3" r="$4" p="$5" dklen="$6"
    local ours theirs
    ours=$("$BUILD/scrypt_cli" "$pw" "$salt" "$N" "$r" "$p" "$dklen" 2>/tmp/scrypt_cli.err) || {
        echo "FAIL: scrypt_cli errored on N=$N r=$r p=$p: $(cat /tmp/scrypt_cli.err)"; fail=$((fail+1)); return; }
    theirs=$("$OPENSSL" kdf -keylen "$dklen" -kdfopt "pass:$pw" -kdfopt "salt:$salt" \
             -kdfopt "n:$N" -kdfopt "r:$r" -kdfopt "p:$p" SCRYPT 2>&1 | tr -d ': \n' | tr 'A-F' 'a-f')
    if [ "$ours" = "$theirs" ]; then
        echo "ok   N=$N r=$r p=$p dklen=$dklen pw='$pw' salt='$salt'"
    else
        echo "FAIL N=$N r=$r p=$p dklen=$dklen pw='$pw' salt='$salt'"
        echo "     ours:   $ours"
        echo "     theirs: $theirs"
        fail=$((fail+1))
    fi
}

check ""                              ""                          16   1 1  64
check "password"                      "NaCl"                      1024 8 16 64
check "hunter2"                       "s0meS4lt"                  32   2 3  40
check "correct horse battery staple"  "another-salt-value-here"   64   4 1  32
check ""                              "empty-pw-nonempty-salt"    8    1 1  16
check "unicode-测试-密码"              "salt-盐"                    16   3 2  48

echo
if [ "$fail" -eq 0 ]; then echo "$n/$n scrypt/openssl agree"; exit 0
else echo "$fail/$n MISMATCHED"; exit 1; fi
