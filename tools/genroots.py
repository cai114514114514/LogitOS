#!/usr/bin/env python3
# Generate c/crypto/trust/roots_bundle.inc from the root-CA PEMs in tools/roots/.
# For each root we extract just the SubjectPublicKeyInfo (the trust anchor's
# public key): an EC point (X||Y) or an RSA (modulus, exponent). No DN, no
# expiry -- the kernel trusts a root by verifying that the top of a presented
# chain is signed by (or identical to) one of these keys.
import base64, glob, os, sys

# Usage: genroots.py [rootsdir] [outfile]
# Both default to the real trust store; the arguments exist so a test can build
# a bundle containing exactly one throwaway CA (tests/unit/run-tls-interop.sh
# does this to verify chains against a locally generated anchor).
ROOTS = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "roots")
OUT   = sys.argv[2] if len(sys.argv) > 2 else \
        os.path.join(os.path.dirname(__file__), "..", "c", "crypto", "trust", "roots_bundle.inc")

# OIDs (DER content, no tag/len)
OID_RSA = bytes([0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x01])
OID_EC  = bytes([0x2a,0x86,0x48,0xce,0x3d,0x02,0x01])
OID_P256= bytes([0x2a,0x86,0x48,0xce,0x3d,0x03,0x01,0x07])
OID_P384= bytes([0x2b,0x81,0x04,0x00,0x22])
OID_P521= bytes([0x2b,0x81,0x04,0x00,0x23])     # 1.3.132.0.35, secp521r1
OID_ED25519 = bytes([0x2b,0x65,0x70])           # 1.3.101.112, RFC 8410
OID_ED448   = bytes([0x2b,0x65,0x71])           # 1.3.101.113 -- named so the
                                                # skip message can say WHICH
                                                # algorithm, not just "unknown"

# Human-friendly order/labels (slug -> comment). Anything in tools/roots not
# listed still gets emitted, labelled by filename.
LABELS = {
    "isrg_x1":            "ISRG Root X1 (Let's Encrypt, RSA-4096)",
    "isrg_x2":            "ISRG Root X2 (Let's Encrypt, EC P-384)",
    "digicert_global_ca": "DigiCert Global Root CA (RSA-2048)",
    "digicert_global_g2": "DigiCert Global Root G2 (RSA-2048)",
    "digicert_global_g3": "DigiCert Global Root G3 (EC P-384)",
    "globalsign_r1":      "GlobalSign Root CA - R1 (RSA-2048)",
    "globalsign_r3":      "GlobalSign Root CA - R3 (RSA-2048)",
    "gts_r1":             "GTS Root R1 (Google, RSA-4096)",
    "gts_r4":             "GTS Root R4 (Google, EC P-384)",
    "usertrust_rsa":      "USERTrust RSA Certification Authority (RSA-4096)",
    "usertrust_ecc":      "USERTrust ECC Certification Authority (EC P-384)",
    "amazon_root_1":      "Amazon Root CA 1 (RSA-2048)",
    "amazon_root_3":      "Amazon Root CA 3 (EC P-256)",
    "dtrust_root3_2009":  "D-TRUST Root Class 3 CA 2 2009 (RSA-2048; SHA-512 chains)",
    "sslcom_ecc_2022":    "SSL.com TLS ECC Root CA 2022 (EC P-384, example.com)",
}

def tlv(b, i):
    """Return (tag, value_start, value_end, next_index) for the TLV at b[i]."""
    tag = b[i]; i += 1
    n = b[i]; i += 1
    if n & 0x80:
        k = n & 0x7f
        n = 0
        for _ in range(k):
            n = (n << 8) | b[i]; i += 1
    return tag, i, i + n, i + n

def children(b, start, end):
    out, i = [], start
    while i < end:
        tag, vs, ve, nx = tlv(b, i)
        out.append((tag, vs, ve))
        i = nx
    return out

def find_spki(der):
    # Certificate ::= SEQ { tbsCertificate SEQ {...}, ... }
    _, vs, ve, _ = tlv(der, 0)                   # outer SEQ
    cert = children(der, vs, ve)
    _, tbs_s, tbs_e = cert[0]                    # tbsCertificate SEQ
    for (t, s, e) in children(der, tbs_s, tbs_e):
        if t != 0x30:
            continue
        # SPKI = SEQ { AlgorithmIdentifier SEQ{ OID, params }, BIT STRING }
        kids = children(der, s, e)
        if len(kids) != 2:
            continue
        if kids[0][0] != 0x30 or kids[1][0] != 0x03:
            continue
        algkids = children(der, kids[0][1], kids[0][2])
        if not algkids or algkids[0][0] != 0x06:
            continue
        return der, kids[0], kids[1], algkids
    raise ValueError("no SPKI found")

def parse_root(der):
    der, _alg_seq, bitstr, algkids = find_spki(der)
    oid = der[algkids[0][1]:algkids[0][2]]
    bs = der[bitstr[1]:bitstr[2]]
    if bs[0] != 0x00:
        raise ValueError("BIT STRING unused bits != 0")
    bs = bs[1:]
    if oid == OID_EC:
        curve_oid = der[algkids[1][1]:algkids[1][2]]
        curve = (256 if curve_oid == OID_P256 else
                 384 if curve_oid == OID_P384 else
                 521 if curve_oid == OID_P521 else 0)
        if curve == 0:
            # The honest failure mode, and it is still doing real work. P-256,
            # P-384 and P-521 are all verifiable now, so what lands here is a
            # curve the kernel genuinely cannot check -- and the ONLY safe thing
            # to do with a root whose signatures cannot be verified is to leave
            # it out of the store and say so. The caller records the name in the
            # skip list, which is compiled in and printed by the kernel, so a
            # dropped root is visible rather than a silent gap.
            raise ValueError("unsupported EC curve %s" % curve_oid.hex())
        if bs[0] != 0x04:
            raise ValueError("EC point not uncompressed")
        return ("EC", curve, bs[1:])             # X||Y
    elif oid == OID_ED25519:
        # RFC 8410 3: no AlgorithmIdentifier parameters, and the BIT STRING is
        # the 32-byte key raw -- no 0x04, no curve. Both are checked, because
        # the failure mode of accepting 31 or 33 bytes is a root that is in the
        # store and can never match anything.
        if len(algkids) != 1:
            raise ValueError("Ed25519 SPKI carries parameters (RFC 8410 forbids it)")
        if len(bs) != 32:
            raise ValueError("Ed25519 key is %d bytes, expected 32" % len(bs))
        return ("ED25519", bs)
    elif oid == OID_ED448:
        raise ValueError("Ed448 (1.3.101.113) -- no verifier in this kernel")
    elif oid == OID_RSA:
        # bs = SEQ { INTEGER modulus, INTEGER exponent }
        _, vs, ve, _ = tlv(bs, 0)
        kids = children(bs, vs, ve)
        n = bs[kids[0][1]:kids[0][2]]
        e = bs[kids[1][1]:kids[1][2]]
        n = n.lstrip(b"\x00")
        e = e.lstrip(b"\x00")
        return ("RSA", n, e)
    raise ValueError("unknown key algorithm")

def pem_to_der(path):
    body = []
    keep = False
    for line in open(path):
        if "BEGIN CERTIFICATE" in line:
            keep = True; continue
        if "END CERTIFICATE" in line:
            break
        if keep:
            body.append(line.strip())
    return base64.b64decode("".join(body))

def carr(name, data):
    lines = [f"static const uint8_t {name}[] = {{"]
    for i in range(0, len(data), 12):
        chunk = ", ".join(f"0x{b:02x}" for b in data[i:i+12])
        lines.append(f"    {chunk},")
    lines.append("};")
    return "\n".join(lines)

def _order(p):
    """Sort key for the root PEMs. It must be a TOTAL order.

    It used to return a bare 999 for every root not in LABELS, which is not an
    order at all -- 100+ roots compared equal, so Python's stable sort left them
    in whatever sequence glob returned, i.e. **filesystem order**. That is
    alphabetical on NTFS and arbitrary on ext4, so `make check-roots` passed in
    the Windows worktree and failed in every /tmp clone with "roots_bundle.inc
    is stale" -- which is where agents do their clean verification runs, so the
    bundle looked perpetually stale to everyone who checked it properly.

    Falling back to the slug makes the key total, so the generated bundle is a
    function of its inputs and nothing else.
    """
    slug = os.path.splitext(os.path.basename(p))[0]
    order = list(LABELS)
    return (order.index(slug) if slug in LABELS else len(order), slug)


def main():
    paths = sorted(glob.glob(os.path.join(ROOTS, "*.pem")), key=_order)
    arrays, rows, names = [], [], []
    skipped = []
    for idx, p in enumerate(paths):
        slug = os.path.splitext(os.path.basename(p))[0]
        label = LABELS.get(slug, slug)
        try:
            kind = parse_root(pem_to_der(p))
        except Exception as ex:
            # A key type this kernel cannot verify, or a malformed PEM: skip it
            # rather than abort the whole bundle. What still lands here after
            # the P-521 and Ed25519 work is Ed448 and anything unrecognised --
            # and it MUST keep landing here. A root whose signatures nothing can
            # check is not a weaker root, it is one that can never validate a
            # chain, so emitting it would grow the store's advertised size
            # without growing what it can actually trust.
            print(f"  SKIP {slug}: {ex}", file=sys.stderr)
            skipped.append((slug, str(ex)))
            continue
        if kind[0] == "EC":
            _, curve, pt = kind
            arrays.append(carr(f"r{idx}_ec", pt))
            rows.append(f"    /* {label} */\n"
                        f"    {{ ROOT_EC, {curve}, r{idx}_ec, sizeof r{idx}_ec, 0, 0, 0, 0 }},")
        elif kind[0] == "ED25519":
            # Reuses the ec/eclen fields for the raw 32 bytes; curve stays 0.
            # See the comment on ROOT_ED25519 in c/crypto/trust/roots.h.
            _, pt = kind
            arrays.append(carr(f"r{idx}_ed", pt))
            rows.append(f"    /* {label} */\n"
                        f"    {{ ROOT_ED25519, 0, r{idx}_ed, sizeof r{idx}_ed, 0, 0, 0, 0 }},")
        else:
            _, n, e = kind
            arrays.append(carr(f"r{idx}_n", n))
            arrays.append(carr(f"r{idx}_e", e))
            rows.append(f"    /* {label} */\n"
                        f"    {{ ROOT_RSA, 0, 0, 0, r{idx}_n, sizeof r{idx}_n, r{idx}_e, sizeof r{idx}_e }},")
        # One name per emitted row, appended in the same pass that appends the
        # row, so the two arrays cannot drift: a branch that forgot this would
        # be caught by the assert below rather than by a kernel printing the
        # wrong CA's name for a row. The slug (the PEM basename) is the
        # identifier, not the pretty LABELS string -- it is what a human types
        # to add a root and what they delete to revoke one.
        names.append(slug)
        print(f"  {label}: {kind[0]}", file=sys.stderr)
    assert len(names) == len(rows), "root name/row arrays drifted"
    # The skip list is part of the artefact, not a build-time aside. A root that
    # is silently dropped makes the trust store smaller than everyone believes
    # it is, and nobody re-reads generator stderr -- so the names are compiled
    # in and the kernel prints them (see tls.c's trust-store banner) the first
    # time anything opens a TLS connection.
    skipnames = "".join(f'    "{slug}: {reason}",\n' for slug, reason in skipped)
    # Same argument one row over, for the roots that DID make it in. A count is
    # not an answer to "do you trust this CA": until this array existed, struct
    # root_ca held eight fields of key material and no name, so the machine
    # could say how many anchors it had and nothing about which. Emitted as a
    # PARALLEL array rather than a ninth field because the row is walked by two
    # byte-comparison loops in net/x509.c (:422, :447) that have no use for it.
    rootnames = "".join(f'    "{slug}",\n' for slug in names)
    with open(OUT, "w") as f:
        f.write("/* Generated by tools/genroots.py -- do not edit.\n"
                " * Built-in trusted root-CA public keys (EC point or RSA n,e). */\n\n")
        f.write("\n".join(arrays))
        f.write("\n\nconst struct root_ca logit_roots[] = {\n")
        f.write("\n".join(rows))
        f.write("\n};\nconst int logit_nroots = (int)(sizeof logit_roots / sizeof logit_roots[0]);\n")
        f.write("\n/* Parallel to logit_roots[]: the PEM basename each row was built from, in\n"
                " * the same order, exactly logit_nroots entries. This is the identifier the\n"
                " * trust store is administered by -- tools/roots/<slug>.pem is what you add\n"
                " * to trust a CA and what you DELETE to revoke one -- so it is also the only\n"
                " * name by which a running kernel can report what it trusts. */\n")
        f.write("const char *const logit_root_names[] = {\n" + rootnames + "};\n")
        f.write("\n/* PEMs in the source directory whose key type this kernel cannot verify.\n"
                " * NUL-terminated so the array is never empty (an empty C initialiser is\n"
                " * not valid); logit_nroots_skipped is the real count. */\n")
        f.write("const char *const logit_roots_skipped[] = {\n" + skipnames + "    0,\n};\n")
        f.write(f"const int logit_nroots_skipped = {len(skipped)};\n")

    print(f"wrote {OUT} ({len(rows)} roots; {len(skipped)} skipped)", file=sys.stderr)
    if skipped:
        print("!! SKIPPED ROOTS -- the trust store is smaller than tools/roots/ suggests:",
              file=sys.stderr)
        for slug, reason in skipped:
            print(f"!!   {slug}: {reason}", file=sys.stderr)

if __name__ == "__main__":
    main()
