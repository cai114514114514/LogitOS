#!/usr/bin/env bash
# Generate P-521 ECDSA vectors with OPENSSL, for tests/unit/ecdsa_p521_test.c.
#
# The point is that openssl chooses the key, the nonce and the message -- we
# only get to check the answer. A vector our own code produced would prove that
# our verifier agrees with our signer, which is not the question; the question
# is whether it agrees with the rest of the world.
#
# Emits one line per signature:  Ux Uy r s sha512(msg)   (all hex, no 0x)
set -u
OPENSSL="${OPENSSL:-openssl}"
OUT="${1:-/dev/stdout}"
N="${2:-12}"

command -v "$OPENSSL" >/dev/null || { echo "SKIP: no openssl" >&2; : > "$OUT"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

: > "$OUT"
for i in $(seq 1 "$N"); do
    "$OPENSSL" ecparam -name secp521r1 -genkey -noout -out "$TMP/k.pem" 2>/dev/null
    head -c $((RANDOM % 200 + 1)) /dev/urandom > "$TMP/msg"
    "$OPENSSL" dgst -sha512 -sign "$TMP/k.pem" -out "$TMP/sig.der" "$TMP/msg" 2>/dev/null

    # The public point and the DER signature both need real parsing; python is
    # the shortest honest way to do it, and it is already a build dependency.
    python3 - "$TMP/k.pem" "$TMP/sig.der" "$TMP/msg" >> "$OUT" <<'PY'
import sys, subprocess, hashlib

key, sigf, msgf = sys.argv[1], sys.argv[2], sys.argv[3]

# Public point: `openssl ec -text` prints the uncompressed point as colon-
# separated bytes under "pub:". Taking it from openssl's own printer rather
# than hand-parsing the SPKI keeps this script honest about where the number
# came from.
txt = subprocess.run(["openssl","ec","-in",key,"-text","-noout"],
                     capture_output=True, text=True).stdout
lines, grab, hexs = txt.splitlines(), False, []
for ln in lines:
    s = ln.strip()
    if s.startswith("pub:"):
        grab = True; continue
    if grab:
        if ":" in s and all(c in "0123456789abcdef:" for c in s):
            hexs.append(s.replace(":", ""))
        else:
            break
pt = bytes.fromhex("".join(hexs))
assert pt[0] == 4 and len(pt) == 133, (pt[:1].hex(), len(pt))
ux, uy = pt[1:67].hex(), pt[67:133].hex()

# DER SEQUENCE { INTEGER r, INTEGER s }
der = open(sigf, "rb").read()
def rd(b, i):
    assert b[i] == 0x02, "not an INTEGER"
    ln = b[i+1]
    assert ln < 0x80, "long-form length not expected for P-521 r/s"
    return b[i+2:i+2+ln], i+2+ln
assert der[0] == 0x30
i = 2 if der[1] < 0x80 else 3
r, i = rd(der, i)
s, _ = rd(der, i)

h = hashlib.sha512(open(msgf,"rb").read()).hexdigest()
print(ux, uy, r.hex(), s.hex(), h)
PY
done
