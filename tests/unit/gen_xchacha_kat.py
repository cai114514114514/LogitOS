#!/usr/bin/env python3
"""Regenerate tests/unit/xchacha_kat.inc from a Wycheproof AEAD test file.

    python3 tests/unit/gen_xchacha_kat.py /path/to/xchacha20_poly1305_test.json \
        > tests/unit/xchacha_kat.inc

Source: https://raw.githubusercontent.com/C2SP/wycheproof/main/testvectors_v1/xchacha20_poly1305_test.json
(project.wycheproof, C2SP fork). Emits every vector from the ONE test group
whose sizes match this tree's fixed-array API (ivSize=192 bits / 24 bytes,
keySize=256 bits / 32 bytes, tagSize=128 bits / 16 bytes) -- the file also
carries nine one-vector groups at other ivSizes (0, 64, 88, ... 256 bits)
whose whole point is "a conforming implementation must reject this nonce
length"; xchacha20_poly1305_seal/open take a fixed uint8_t[24] and have no
length parameter to reject with, so those nine do not apply to this API and
are intentionally not emitted -- noted here rather than silently dropped.
"""
import json, sys, hashlib

def main():
    if len(sys.argv) != 2:
        sys.stderr.write("usage: gen_xchacha_kat.py <wycheproof json>\n")
        return 1
    path = sys.argv[1]
    with open(path, "rb") as f:
        raw = f.read()
    digest = hashlib.sha256(raw).hexdigest()
    d = json.loads(raw)
    assert d["algorithm"] == "XCHACHA20-POLY1305"

    group = None
    for g in d["testGroups"]:
        if g["ivSize"] == 192 and g["keySize"] == 256 and g["tagSize"] == 128:
            group = g
            break
    assert group is not None, "expected group not found"

    print("/* XChaCha20-Poly1305 known answers. DO NOT HAND-EDIT.")
    print(" * Regenerate with:")
    print(" *   python3 tests/unit/gen_xchacha_kat.py <wycheproof.json> > tests/unit/xchacha_kat.inc")
    print(" *")
    print(" * Source: Wycheproof (C2SP fork) xchacha20_poly1305_test.json,")
    print(f" *   sha256 of the source file at generation time: {digest}")
    print(" *   https://raw.githubusercontent.com/C2SP/wycheproof/main/testvectors_v1/xchacha20_poly1305_test.json")
    print(f" * Emits all {len(group['tests'])} vectors from the ivSize=192/keySize=256/tagSize=128")
    print(" * group (the nine wrong-nonce-length groups do not apply to this API's")
    print(" * fixed uint8_t[24] nonce -- see this generator's own docstring).")
    print(" * tcId 1 is draft-irtf-cfrg-xchacha-03 Appendix A.1 verbatim, also checked")
    print(" * standalone (with the HChaCha20-only A.1/2.2.1 vector) in the test's")
    print(" * XCHACHA_DRAFT_* constants below, independent of this array.")
    print(" */")
    print("struct xchacha_kat {")
    print("    int tcid;")
    print("    const char *comment;")
    print("    const char *key, *iv, *aad, *msg, *ct, *tag;")
    print("    int valid;")
    print("};")
    print()
    print("static const struct xchacha_kat XCHACHA_KAT[] = {")
    for t in group["tests"]:
        comment = t.get("comment", "").replace("\\", "\\\\").replace('"', '\\"')
        valid = 1 if t["result"] == "valid" else 0
        print("    { %d, \"%s\", \"%s\", \"%s\", \"%s\", \"%s\", \"%s\", \"%s\", %d }," % (
            t["tcId"], comment, t["key"], t["iv"], t["aad"], t["msg"], t["ct"], t["tag"], valid))
    print("};")
    print("#define XCHACHA_KAT_N (int)(sizeof(XCHACHA_KAT) / sizeof(XCHACHA_KAT[0]))")
    return 0

if __name__ == "__main__":
    sys.exit(main())
