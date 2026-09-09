#!/usr/bin/env python3
"""canvas_b64_ext_test.py -- the data: URL from toDataURL, judged from OUTSIDE.

WHY A SECOND ORACLE FOR A LAYER THAT ALREADY ROUND-TRIPS.

tests/unit/canvas_test.c decodes every data URL with `atob` and asserts the
pixels, and that is a real differential: js_canvas.c's b64_encode is a C table
lookup, js_platform.c's atob is a JS shim, and they share no line.  It catches
a wrong alphabet, a transposed shift, a dropped byte.

It cannot catch a missing '='.  atob's second statement is

    if (s.length % 4 === 0) s = s.replace(/==?$/, '');

and it refuses only `length % 4 === 1`, so unpadded base64 decodes there
perfectly.  An encoder that never emitted a pad byte would keep all 68 checks
in that file green while producing a URL that python, libpng and the URL parser
in every other browser reject.  That is exactly the shape tests/pngenc.mk found
one layer down -- "a wrong CRC polynomial that both sides compute the same way
round-trips perfectly and is rejected by every other program on earth" -- and it
gets the same answer: an oracle that shares no code with either side.

WHAT THIS CHECKS THAT THE C GATE CANNOT
  * base64.b64decode(..., validate=True) with STRICT padding, plus an explicit
    check that the pad length is the one RFC 4648 requires for the byte count.
    Python is strict where atob is lenient, and that difference IS the test.
  * the chunk CRC-32s, with binascii.crc32 -- Python's polynomial, not ours.
  * the IDAT, through python's zlib, which verifies the Adler-32 trailer.
  * a named pixel of the 4x3 canvas, so this is an assertion about the PICTURE
    and not only about the envelope.

AND IT REFUSES A CORPUS THAT CANNOT FAIL.  A base64 string only carries padding
when the byte count is not a multiple of 3, so a corpus of n%3==0 files has no
'=' in it anywhere and would pass this script with the padding deleted.  All
three residues must be present or this exits non-zero saying so -- the
"test-url reports 32/32 (100.0%) while printing that both corpora are absent"
failure, refused by construction.

Usage: canvas_b64_ext_test.py <dumpfile>   (label<TAB>dataurl per line)
"""

import base64
import binascii
import struct
import sys
import zlib

checks = 0
failed = 0


def ok(msg):
    global checks
    checks += 1
    print("ok  : %s" % msg)


def bad(msg):
    global checks, failed
    checks += 1
    failed += 1
    print("FAIL: %s" % msg)


def check(cond, msg):
    ok(msg) if cond else bad(msg)


def decode_strict(label, b64):
    """Decode with python's rules, not atob's.  Returns bytes or None."""
    # RFC 4648: the encoded form is a multiple of 4 characters, and the number
    # of '=' is determined by the input length mod 3.  atob enforces neither.
    if len(b64) % 4 != 0:
        bad("%s: base64 length %d is not a multiple of 4 -- padding is missing "
            "or wrong (atob accepts this; nothing else does)" % (label, len(b64)))
        return None
    try:
        raw = base64.b64decode(b64, validate=True)
    except Exception as e:  # noqa: BLE001 -- the message is the finding
        bad("%s: python refused this base64: %s" % (label, e))
        return None
    want_pad = (3 - len(raw) % 3) % 3
    got_pad = len(b64) - len(b64.rstrip("="))
    if want_pad != got_pad:
        bad("%s: %d payload bytes require %d pad chars, the URL carries %d"
            % (label, len(raw), want_pad, got_pad))
        return None
    return raw


def chunks(label, raw):
    """Walk the PNG chunk list, verifying every CRC with binascii.crc32."""
    if raw[:8] != b"\x89PNG\r\n\x1a\n":
        bad("%s: not a PNG signature" % label)
        return None
    out = []
    o = 8
    while o + 8 <= len(raw):
        (n,) = struct.unpack(">I", raw[o:o + 4])
        typ = raw[o + 4:o + 8]
        data = raw[o + 8:o + 8 + n]
        if len(data) != n:
            bad("%s: chunk %r claims %d bytes, %d present" % (label, typ, n, len(data)))
            return None
        (want,) = struct.unpack(">I", raw[o + 8 + n:o + 12 + n])
        got = binascii.crc32(typ + data) & 0xFFFFFFFF
        if got != want:
            bad("%s: chunk %r CRC is %08x, file says %08x" % (label, typ, got, want))
            return None
        out.append((typ, data))
        o += 12 + n
    if o != len(raw):
        bad("%s: %d trailing bytes after the last chunk" % (label, len(raw) - o))
        return None
    return out


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    try:
        with open(path) as f:
            lines = [l.rstrip("\n") for l in f if l.strip()]
    except OSError as e:
        print("canvas_b64_ext_test: cannot read the dump: %s" % e)
        print("  The C gate writes it when CANVAS_URL_DUMP is set. No dump means")
        print("  the corpus is ABSENT, which is not the same as a passing corpus.")
        return 2

    entries = []
    for l in lines:
        if "\t" not in l:
            bad("malformed dump line: %r" % l[:60])
            continue
        label, url = l.split("\t", 1)
        entries.append((label, url))

    # Refuse a short corpus by name.  The C gate dumps four; three fewer means
    # it returned early on a failure and this script would otherwise report a
    # clean count over a corpus that never reached the interesting sizes.
    if len(entries) < 4:
        print("canvas_b64_ext_test: the dump holds %d URLs, expected 4 "
              "(1x1, 2x1, 1x2, 4x3-known)." % len(entries))
        print("  A short corpus is a broken run, not a small one -- refusing "
              "rather than reporting a count over it.")
        return 2

    residues = set()
    known = None

    for label, url in entries:
        prefix = "data:image/png;base64,"
        if not url.startswith(prefix):
            bad("%s: the URL does not declare image/png: %r" % (label, url[:40]))
            continue
        raw = decode_strict(label, url[len(prefix):])
        if raw is None:
            continue
        residues.add(len(raw) % 3)
        cs = chunks(label, raw)
        if cs is None:
            continue
        names = [t for t, _ in cs]
        check(names[0] == b"IHDR" and names[-1] == b"IEND",
              "%s: %d chunks, %s, every CRC verified by binascii.crc32"
              % (label, len(cs), b"/".join(names).decode()))
        idat = b"".join(d for t, d in cs if t == b"IDAT")
        try:
            # zlib.decompress verifies the Adler-32 trailer.
            pix = zlib.decompress(idat)
        except Exception as e:  # noqa: BLE001
            bad("%s: python's zlib refused the IDAT: %s" % (label, e))
            continue
        w, h, depth, ctype = struct.unpack(">IIBB", cs[0][1][:10])
        want = h * (1 + 4 * w)
        check(len(pix) == want and depth == 8 and ctype == 6,
              "%s: zlib gives %d bytes = %dx%d RGBA8 scanlines (Adler-32 ok)"
              % (label, len(pix), w, h))
        if label == "4x3-known":
            known = (w, h, pix)

    # The picture, not just the envelope.  canvas_test.c fills 4x3 red then
    # paints one blue pixel at (0,0); asserting that from out here means this
    # script is measuring what was DRAWN and not the shape of a file.
    if known is None:
        bad("the 4x3-known canvas is missing from the dump; nothing here "
            "checked a pixel")
    else:
        w, h, pix = known
        stride = 1 + 4 * w
        check(pix[0] == 0, "4x3-known: row 0 filter byte is 0 (None)")
        check(tuple(pix[1:5]) == (0, 0, 255, 255),
              "4x3-known: pixel (0,0) is blue, decoded by python's zlib")
        check(tuple(pix[5:9]) == (255, 0, 0, 255),
              "4x3-known: pixel (1,0) is red")
        check(tuple(pix[stride + 1:stride + 5]) == (255, 0, 0, 255),
              "4x3-known: pixel (0,1) is red")

    # The coverage assertion, and it is the one that keeps this file honest.
    if residues != {0, 1, 2}:
        print()
        print("canvas_b64_ext_test: the corpus covers byte-length residues %s "
              "mod 3, not {0, 1, 2}." % sorted(residues))
        print("  Padding only EXISTS when the length is not a multiple of 3, so")
        print("  a corpus missing a residue cannot fail the padding check and a")
        print("  green result here would mean nothing. Refusing.")
        return 2
    ok("the corpus covers all three length residues mod 3, so the padding "
       "check can actually fire")

    print()
    if failed:
        print("canvas_b64_ext_test: %d/%d checks FAILED" % (failed, checks))
        return 1
    print("canvas_b64_ext_test: %d checks pass" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
