#!/usr/bin/env python3
"""Regenerate tests/unit/x448_wycheproof.inc from the Wycheproof XDH/curve448
vectors. Usage:

    python3 tests/unit/gen-x448-wycheproof.py > tests/unit/x448_wycheproof.inc

Fetches https://raw.githubusercontent.com/C2SP/wycheproof/main/testvectors_v1/x448_test.json
(pass a local path as argv[1] instead to regenerate offline from a copy).

Only the 56-byte-`public` cases are emitted -- x448()'s C API is a fixed
uint8_t[56], so the 12 "public key too large: 449 bits" cases in this corpus
cannot even be represented as a call, let alone exercised. That is a
structural exclusion, not a silent drop: the generated file records both the
total case count and the excluded count as macros, and x448_test.c asserts
X448_WYCHEPROOF_N + X448_WYCHEPROOF_SKIPPED == X448_WYCHEPROOF_TOTAL so a
future revision of the corpus that changes those numbers is caught rather than
quietly under-tested.
"""
import json
import sys
import urllib.request

URL = ("https://raw.githubusercontent.com/C2SP/wycheproof/main/"
       "testvectors_v1/x448_test.json")


def load():
    if len(sys.argv) > 1:
        with open(sys.argv[1]) as f:
            return json.load(f)
    with urllib.request.urlopen(URL, timeout=30) as r:
        return json.load(r)


def main():
    d = load()
    tg = d["testGroups"][0]
    assert tg["curve"] == "curve448", tg["curve"]
    tests = tg["tests"]
    usable = [t for t in tests if len(t["public"]) // 2 == 56]
    skipped = [t for t in tests if len(t["public"]) // 2 != 56]
    # every skip must be explained the same way, or this generator is quietly
    # widening what it drops
    for t in skipped:
        assert "too large" in t["comment"], t

    out = sys.stdout
    out.write("/* Wycheproof X448 test vectors, from\n")
    out.write(" * %s\n" % URL)
    out.write(" * (algorithm XDH, curve448, %d of %d cases -- %d cases whose 'public'\n"
               % (len(usable), len(tests), len(skipped)))
    out.write(" * value is 57 bytes (\"public key too large: 449 bits\") are structurally\n")
    out.write(" * inapplicable to a fixed uint8_t[56] API and are EXCLUDED here, not\n")
    out.write(" * silently dropped: see x448_test.c's count check against\n")
    out.write(" * X448_WYCHEPROOF_TOTAL/X448_WYCHEPROOF_SKIPPED below.\n")
    out.write(" *\n")
    out.write(" * DO NOT HAND-EDIT. Regenerate with tests/unit/gen-x448-wycheproof.py.\n")
    out.write(" */\n")
    out.write("#define X448_WYCHEPROOF_TOTAL %d\n" % len(tests))
    out.write("#define X448_WYCHEPROOF_SKIPPED %d\n" % len(skipped))
    out.write("\n")
    out.write("struct x448_wtv { int tcid; const char *pub, *priv, *shared; const char *result; };\n")
    out.write("static const struct x448_wtv X448_WYCHEPROOF[] = {\n")
    for t in usable:
        out.write('    {%d, "%s", "%s", "%s", "%s"},\n'
                   % (t["tcId"], t["public"], t["private"], t["shared"], t["result"]))
    out.write("};\n")
    out.write("#define X448_WYCHEPROOF_N (int)(sizeof(X448_WYCHEPROOF)/sizeof(X448_WYCHEPROOF[0]))\n")


if __name__ == "__main__":
    main()
