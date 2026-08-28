#!/usr/bin/env python3
"""Regenerates tests/unit/aes_gcm_siv_vectors.inc from RFC 8452 itself.

Run:  python3 tests/unit/aes_gcm_siv_gen.py > tests/unit/aes_gcm_siv_vectors.inc

Fetches the RFC's own text (not a third-party transcription) and parses
Appendix C (50 vectors: C.1 AEAD_AES_128_GCM_SIV x24, C.2
AEAD_AES_256_GCM_SIV x24, C.3 the two counter-wrap cases) plus the standalone
POLYVAL worked example in section 7 -- the a, b, a*b, dot(a,b) figures used to
introduce the field's multiplication before the AEAD is even defined.

WHY PARSE THE RFC TEXT RATHER THAN HAND-TRANSCRIBE THE NUMBERS: Appendix C is
long (50 vectors, results up to 80 bytes, several values split across a page
break in the .txt rendering) and hand-typing hex at that volume is exactly how
a transcription error gets baked into a "known answer". The parser below
self-checks every vector's byte lengths (key is 16 or 32, nonce is 12,
result is exactly len(plaintext)+16) before writing anything, which is a
weaker check than an independent second source but strong enough to catch the
one failure mode most likely from a scripted parse: a field boundary
misdetected across the page-break furniture RFC 8452's .txt rendering
inserts mid-vector (this happens at least once in Appendix C.2 -- a Key value
whose second half lands after a page footer/header pair).
"""
import re
import sys
import urllib.request

RFC_URL = "https://www.rfc-editor.org/rfc/rfc8452.txt"


def fetch(url):
    with urllib.request.urlopen(url, timeout=30) as r:
        return r.read().decode("utf-8")


def parse(text):
    lines = text.split("\n")
    clean = []
    for l in lines:
        if "Gueron, et al." in l:
            continue
        if "RFC 8452" in l and "AES-GCM-SIV" in l:
            continue
        if "[Page" in l:
            continue
        clean.append(l)
    full = "\n".join(clean)

    start = full.index("Appendix C.  Test Vectors", full.index("Appendix C.  Test Vectors") + 1)
    end = full.index("Acknowledgements", start)
    body = full[start:end]

    field_re = re.compile(r"^\s*([A-Za-z][A-Za-z0-9 /().'-]*?)\s*=\s*([0-9a-fA-F]*)\s*$")
    hexcont_re = re.compile(r"^\s{5,40}([0-9a-fA-F]+)\s*$")

    def normalize(name):
        for prefix in ("Plaintext", "AAD", "Result"):
            if name.startswith(prefix):
                return prefix
        return name

    vectors = []
    cur = {}
    cur_field = None
    group = None
    for raw in body.split("\n"):
        if raw.startswith("C.1."):
            group = "AES_128"; continue
        if raw.startswith("C.2."):
            group = "AES_256"; continue
        if raw.startswith("C.3."):
            group = "WRAP_256"; continue
        if not raw.strip():
            continue  # blank lines, INCLUDING ones straddling a page break, are not separators
        m = field_re.match(raw)
        if m:
            name = normalize(m.group(1).strip())
            hexval = m.group(2)
            if name == "Plaintext":
                if cur.get("Key"):
                    vectors.append(cur)
                cur = {"group": group}
                cur_field = "Plaintext"
                cur[cur_field] = hexval
            else:
                cur_field = name
                cur[cur_field] = cur.get(cur_field, "") + hexval
            continue
        m2 = hexcont_re.match(raw)
        if m2 and cur_field:
            cur[cur_field] = cur.get(cur_field, "") + m2.group(1)
            continue
    if cur.get("Key"):
        vectors.append(cur)
    return vectors


def parse_section7(text):
    m = re.search(
        r"a = ([0-9a-f]+) and\s*\n\s*b = ([0-9a-f]+),\s*\n"
        r"\s*then a \+ b = ([0-9a-f]+),\s*\n"
        r"\s*a \* b = ([0-9a-f]+), and\s*\n"
        r"\s*dot\(a, b\) = ([0-9a-f]+)\.",
        text)
    if not m:
        sys.exit("FAIL: could not find the section 7 POLYVAL worked example in the RFC text")
    return m.group(1), m.group(2), m.group(4), m.group(5)  # a, b, a*b, dot(a,b)


def main():
    text = fetch(RFC_URL)
    vectors = parse(text)
    a, b, axb, dot = parse_section7(text)

    ok = True
    for i, v in enumerate(vectors):
        pt = v.get("Plaintext", ""); aad = v.get("AAD", ""); res = v.get("Result", "")
        key = v.get("Key", ""); nonce = v.get("Nonce", "")
        if len(key) // 2 not in (16, 32):
            print(f"SUSPECT vector {i}: key length {len(key)//2}", file=sys.stderr); ok = False
        if len(nonce) // 2 != 12:
            print(f"SUSPECT vector {i}: nonce length {len(nonce)//2}", file=sys.stderr); ok = False
        if len(res) // 2 != len(pt) // 2 + 16:
            print(f"SUSPECT vector {i}: result length {len(res)//2}, want {len(pt)//2+16}",
                  file=sys.stderr); ok = False
    if len(vectors) != 50:
        print(f"SUSPECT: parsed {len(vectors)} vectors, RFC 8452 Appendix C has 50 "
              "(24 + 24 + 2)", file=sys.stderr); ok = False
    if not ok:
        sys.exit("FAIL: one or more parsed vectors look wrong; not writing an .inc file")

    print("/* AES-GCM-SIV known answers: RFC 8452 Appendix C (50 vectors) plus the")
    print(" * section 7 standalone POLYVAL worked example. DO NOT HAND-EDIT.")
    print(" * Regenerate with: python3 tests/unit/aes_gcm_siv_gen.py > "
          "tests/unit/aes_gcm_siv_vectors.inc")
    print(" * Source: " + RFC_URL + " (fetched and parsed at generation time,")
    print(" * not hand-transcribed -- see the generator's own header for why). */")
    print()
    print(f'#define POLYVAL_EX_A   "{a}"')
    print(f'#define POLYVAL_EX_B   "{b}"')
    print(f'#define POLYVAL_EX_AXB "{axb}"   /* a*b: raw multiply then reduce mod P(x) */')
    print(f'#define POLYVAL_EX_DOT "{dot}"   /* dot(a,b) = a*b*x^-128 -- the operator POLYVAL actually uses */')
    print()
    print("struct siv_vec { int keylen; const char *key, *nonce, *aad, *pt, *result; };")
    print()
    print("static const struct siv_vec SIV_VECTORS[] = {")
    for v in vectors:
        key = v.get("Key", ""); nonce = v.get("Nonce", "")
        aad = v.get("AAD", ""); pt = v.get("Plaintext", ""); res = v.get("Result", "")
        keylen = len(key) // 2
        print(f'    {{ {keylen}, "{key}", "{nonce}", "{aad}", "{pt}", "{res}" }},'
              f'  /* {v.get("group")} pt={len(pt)//2}B aad={len(aad)//2}B */')
    print("};")
    print("#define SIV_NVECTORS (int)(sizeof(SIV_VECTORS)/sizeof(SIV_VECTORS[0]))")


if __name__ == "__main__":
    main()
