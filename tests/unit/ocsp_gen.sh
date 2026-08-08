#!/usr/bin/env bash
# Build a small PKI and get REAL OCSP responses out of `openssl ocsp`, for
# tests/unit/ocsp_test.c.
#
# The point of generating rather than committing: the DER our parser reads is
# DER openssl wrote. A fixture we encoded ourselves would only prove that our
# reader agrees with our writer, and the fields most likely to be wrong -- the
# implicit/explicit tagging of certStatus, the CertID hash preimages, the
# optional nextUpdate -- are exactly the ones an own-encoder would get
# consistently wrong in both directions.
#
# Six cases, each isolating one decision:
#
#   good.der        status good, signed by the issuer itself      -> accept
#   revoked.der     status revoked                                -> REFUSE
#   unknown.der     a serial the responder does not know          -> REFUSE
#   delegated.der   signed by a responder cert with OCSPSigning   -> accept
#   nodeleg.der     signed by a cert the CA issued that does NOT
#                   have OCSPSigning                              -> REFUSE
#   othercert.der   a valid good response about a DIFFERENT cert  -> REFUSE
#
# `nodeleg.der` is the one that matters most: it is a perfectly valid signature
# by a certificate the CA really issued. A verifier that trusts any certificate
# in the response bag accepts it, and that verifier can be told "good" about a
# revoked certificate by anyone who can buy a cert from the same CA.
#
#   ocsp_gen.sh <outdir>
set -eu

OUT="${1:?usage: ocsp_gen.sh <outdir>}"
mkdir -p "$OUT"
cd "$OUT"
rm -rf pki; mkdir pki; cd pki

mkdir -p db
: > db/index.txt
echo 1000 > db/serial
echo 1000 > db/crlnumber

cat > ca.cnf <<'EOF'
[ ca ]
default_ca = CA_default
[ CA_default ]
dir              = .
database         = ./db/index.txt
serial           = ./db/serial
crlnumber        = ./db/crlnumber
new_certs_dir    = ./db
certificate      = ./ca.pem
private_key      = ./ca.key
default_md       = sha256
default_days     = 365
policy           = pol
email_in_dn      = no
rand_serial      = no
unique_subject   = no
copy_extensions  = none
[ pol ]
commonName = supplied
[ req ]
distinguished_name = dn
prompt = no
[ dn ]
CN = placeholder
[ v3_leaf ]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature,keyEncipherment
[ v3_ocsp ]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature
extendedKeyUsage = critical,OCSPSigning
[ v3_nodeleg ]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature
extendedKeyUsage = critical,serverAuth
EOF

q() { openssl "$@" >/dev/null 2>&1; }

# --- the CA (self-signed root; it is also the direct issuer here, which is the
# common stapling shape once the intermediate is the issuer) -----------------
q req -x509 -newkey rsa:2048 -nodes -keyout ca.key -out ca.pem -days 3650 \
     -subj "/CN=LogitOS OCSP Test CA" -addext "basicConstraints=critical,CA:TRUE" \
     -addext "keyUsage=critical,keyCertSign,cRLSign"

issue() { # issue <name> <section> <subject>
    q req -newkey rsa:2048 -nodes -keyout "$1.key" -out "$1.csr" -subj "$3" -config ca.cnf
    q ca -batch -config ca.cnf -extfile ca.cnf -extensions "$2" -in "$1.csr" -out "$1.pem"
}

issue leaf     v3_leaf    "/CN=leaf.example"
issue other    v3_leaf    "/CN=other.example"
issue revoked  v3_leaf    "/CN=revoked.example"
issue ocspsign v3_ocsp    "/CN=LogitOS OCSP Responder"
issue nodeleg  v3_nodeleg "/CN=Not A Responder"

q ca -batch -config ca.cnf -revoke revoked.pem

# A certificate the responder's index has never heard of: issued by a DIFFERENT
# CA, so `openssl ocsp` answers "unknown" rather than refusing the request.
q req -x509 -newkey rsa:2048 -nodes -keyout foreign.key -out foreignca.pem -days 3650 \
     -subj "/CN=Some Other CA" -addext "basicConstraints=critical,CA:TRUE"

respond() { # respond <out> <cert.pem> <rsigner.pem> <rkey>
    q ocsp -issuer ca.pem -cert "$2" -reqout req.der -no_nonce
    q ocsp -index db/index.txt -CA ca.pem -rsigner "$3" -rkey "$4" \
           -reqin req.der -respout "$1" -no_nonce -ndays 7
}

respond good.der      leaf.pem    ca.pem       ca.key
respond revoked.der   revoked.pem ca.pem       ca.key
respond othercert.der other.pem   ca.pem       ca.key
respond delegated.der leaf.pem    ocspsign.pem ocspsign.key
respond nodeleg.der   leaf.pem    nodeleg.pem  nodeleg.key

# "unknown": a certificate the CA really signed but that never went through
# `openssl ca`, so it has no row in the index. Signed with `x509 -req` on
# purpose -- that is what makes the responder answer [2] unknown about a
# certificate we can actually hold and hand to the verifier.
q req -newkey rsa:2048 -nodes -keyout stray.key -out stray.csr -subj "/CN=stray.example" -config ca.cnf
q x509 -req -in stray.csr -CA ca.pem -CAkey ca.key -set_serial 0x5ACAFE -days 365 -out stray.pem
respond unknown.der stray.pem ca.pem ca.key

# DER copies of what the test needs to hold as certificates. Prefixed cert_,
# because `revoked` names BOTH a certificate and a response and the unprefixed
# form had one clobber the other -- the test then parsed an OCSP response as an
# X.509 certificate and failed somewhere else entirely.
for n in leaf other revoked stray ca; do
    q x509 -in "$n.pem" -outform DER -out "../cert_$n.der"
done
for f in good.der revoked.der unknown.der delegated.der nodeleg.der othercert.der; do
    cp "$f" "../$f"
done

cd ..
ls -l *.der | sed 's/^/  /'
echo "ocsp_gen: responses in $OUT"
