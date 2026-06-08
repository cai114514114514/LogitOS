#!/usr/bin/env python3
# Generate include/roots_bundle.inc from the root-CA PEMs in tools/roots/.
# For each root we extract just the SubjectPublicKeyInfo (the trust anchor's
# public key): an EC point (X||Y) or an RSA (modulus, exponent). No DN, no
# expiry -- the kernel trusts a root by verifying that the top of a presented
# chain is signed by (or identical to) one of these keys.
import base64, glob, os, sys

ROOTS = os.path.join(os.path.dirname(__file__), "roots")
OUT   = os.path.join(os.path.dirname(__file__), "..", "src", "crypto", "trust", "roots_bundle.inc")

# OIDs (DER content, no tag/len)
OID_RSA = bytes([0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x01])
OID_EC  = bytes([0x2a,0x86,0x48,0xce,0x3d,0x02,0x01])
OID_P256= bytes([0x2a,0x86,0x48,0xce,0x3d,0x03,0x01,0x07])
OID_P384= bytes([0x2b,0x81,0x04,0x00,0x22])

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
        curve = 256 if curve_oid == OID_P256 else 384 if curve_oid == OID_P384 else 0
        if curve == 0:
            raise ValueError("unknown EC curve")
        if bs[0] != 0x04:
            raise ValueError("EC point not uncompressed")
        return ("EC", curve, bs[1:])             # X||Y
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

def main():
    paths = sorted(glob.glob(os.path.join(ROOTS, "*.pem")),
                   key=lambda p: list(LABELS).index(os.path.splitext(os.path.basename(p))[0])
                                 if os.path.splitext(os.path.basename(p))[0] in LABELS else 999)
    arrays, rows = [], []
    skipped = []
    for idx, p in enumerate(paths):
        slug = os.path.splitext(os.path.basename(p))[0]
        label = LABELS.get(slug, slug)
        try:
            kind = parse_root(pem_to_der(p))
        except Exception as ex:
            # Unsupported key (P-521 / Ed25519 / etc.) or malformed -- skip it
            # rather than abort the whole bundle. The kernel only does RSA + EC
            # P-256/P-384, so such a root could never verify a chain anyway.
            print(f"  SKIP {slug}: {ex}", file=sys.stderr)
            skipped.append(slug)
            continue
        if kind[0] == "EC":
            _, curve, pt = kind
            arrays.append(carr(f"r{idx}_ec", pt))
            rows.append(f"    /* {label} */\n"
                        f"    {{ ROOT_EC, {curve}, r{idx}_ec, sizeof r{idx}_ec, 0, 0, 0, 0 }},")
        else:
            _, n, e = kind
            arrays.append(carr(f"r{idx}_n", n))
            arrays.append(carr(f"r{idx}_e", e))
            rows.append(f"    /* {label} */\n"
                        f"    {{ ROOT_RSA, 0, 0, 0, r{idx}_n, sizeof r{idx}_n, r{idx}_e, sizeof r{idx}_e }},")
        print(f"  {label}: {kind[0]}", file=sys.stderr)
    with open(OUT, "w") as f:
        f.write("/* Generated by tools/genroots.py -- do not edit.\n"
                " * Built-in trusted root-CA public keys (EC point or RSA n,e). */\n\n")
        f.write("\n".join(arrays))
        f.write("\n\nconst struct root_ca aether_roots[] = {\n")
        f.write("\n".join(rows))
        f.write("\n};\nconst int aether_nroots = (int)(sizeof aether_roots / sizeof aether_roots[0]);\n")
    print(f"wrote {OUT} ({len(rows)} roots; {len(skipped)} skipped: {', '.join(skipped) if skipped else 'none'})", file=sys.stderr)

if __name__ == "__main__":
    main()
