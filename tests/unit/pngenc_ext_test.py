#!/usr/bin/env python3
"""pngenc_ext_test.py -- the PNG encoder's SECOND oracle, and the only one that
is not this tree.

WHY A SECOND ORACLE, STATED AS THE FAILURE IT CATCHES AND NOT AS DILIGENCE.
tests/unit/pngenc_test.c round-trips our encoder through rust/src/png.rs, which
is a real differential test -- the decoder was written years earlier and for the
other direction. But the two halves are OURS, and a mistake they SHARE
round-trips perfectly:

  * a CRC-32 with the wrong polynomial, or over the wrong span (PNG CRCs cover
    type+data and NOT the length field);
  * a stored-block LEN/NLEN pair that is self-consistently wrong, or a BFINAL
    on the wrong block.

THE BLIND SPOT WAS MEASURED, NOT ASSUMED, AND THE FIRST GUESS WAS HALF WRONG.
The Adler-32 turned out to be covered by oracle 1 after all -- rust/src/
inflate.rs:272 is `if adler32(outb) != want { return -1 }`, so a wrong modulus
reddens the round-trip. What is NOT covered is the chunk CRC: `grep -ic crc
rust/src/png.rs` is 0, and a copy of the encoder's own output with every chunk
CRC overwritten with four zero bytes decoded as `png_decode -> 0, 37x11`, i.e.
perfectly. So an encoder emitting zeroes where every CRC belongs would score
100% on oracle 1 and be rejected by libpng, by every browser, and by the
binascii.crc32 call below. That one field is why this file exists; the rest of
what it checks is worth having but was not the reason.

WHAT IS CHECKED, each independently of our code:
  1. binascii.crc32 -- Python's, not ours -- over every chunk. This is the one
     that catches a shared-polynomial bug.
  2. zlib.decompress on the IDAT payload. Python's inflate, and it DOES verify
     the Adler-32 trailer, which is what makes the Adler check real.
  3. The stored-block structure, walked by hand: BTYPE must be 00, LEN must
     equal ~NLEN, and exactly the LAST block may set BFINAL. This is the check
     the round-trip cannot reach, and the bug this design is most likely to
     have -- a >65535-byte image whose first block wrongly claims to be final
     decodes to a TRUNCATED image, silently.
  4. The pixels, recomputed here from the same rule tests/unit/pngenc_test.c
     used, so a Python-side unfilter (four lines, filter 0 only) has to agree
     with the C-side round-trip about what was drawn.

THE CONTROL IS AT THE BOTTOM AND IS WATCHED FAILING: every input file is
re-checked with one byte flipped inside its IDAT payload and with one byte
flipped inside a chunk CRC, and this script FAILS if either corruption is
accepted.

Usage:  pngenc_ext_test.py <dir of .png files written by pngenc_test>
"""

import binascii
import os
import struct
import sys
import zlib

checks = 0
failures = []


def ok(what):
    global checks
    checks += 1
    print("ok  : %s" % what)


def fail(what, why):
    global checks
    checks += 1
    failures.append(what)
    print("FAIL: %s\n      %s" % (what, why))


SIG = b"\x89PNG\r\n\x1a\n"


def split_chunks(data):
    """Yield (type, payload, stored_crc, computed_crc). Raises on a malformed
    length -- a truncated file is a failure, not an empty iteration."""
    if data[:8] != SIG:
        raise ValueError("bad signature: %r" % data[:8])
    o = 8
    while o < len(data):
        if o + 8 > len(data):
            raise ValueError("truncated chunk header at %d" % o)
        (n,) = struct.unpack(">I", data[o:o + 4])
        ty = data[o + 4:o + 8]
        if o + 12 + n > len(data):
            raise ValueError("chunk %r claims %d bytes, file has %d left"
                             % (ty, n, len(data) - o - 12))
        payload = data[o + 8:o + 8 + n]
        (stored,) = struct.unpack(">I", data[o + 8 + n:o + 12 + n])
        # Python's CRC-32, over type+data. NOT ours, and not over the length.
        computed = binascii.crc32(ty + payload) & 0xFFFFFFFF
        yield ty, payload, stored, computed
        o += 12 + n


def walk_stored_blocks(z):
    """Walk a zlib stream that must be made only of STORED deflate blocks.
    Returns a list of (len, bfinal) or raises. This is deliberately a hand
    walk and not zlib's -- zlib.decompress would happily accept a stream with
    BFINAL on the first of two blocks by simply stopping early, which is the
    exact bug being looked for."""
    if len(z) < 6:
        raise ValueError("zlib stream is %d bytes, too short" % len(z))
    cmf, flg = z[0], z[1]
    if cmf & 0x0F != 8:
        raise ValueError("CM is %d, not 8 (deflate)" % (cmf & 0x0F))
    if flg & 0x20:
        raise ValueError("FDICT is set")
    if ((cmf << 8) | flg) % 31 != 0:
        raise ValueError("FCHECK fails: 0x%02x%02x %% 31 == %d"
                         % (cmf, flg, ((cmf << 8) | flg) % 31))
    o = 2
    end = len(z) - 4          # the Adler-32 trailer
    blocks = []
    while True:
        if o + 5 > end:
            raise ValueError("ran out of stream at block header, offset %d" % o)
        h = z[o]
        bfinal = h & 1
        btype = (h >> 1) & 3
        if btype != 0:
            raise ValueError("BTYPE is %d, not 0 (stored) at offset %d" % (btype, o))
        if h >> 3:
            raise ValueError("padding bits after BTYPE are not zero: 0x%02x" % h)
        ln = z[o + 1] | (z[o + 2] << 8)
        nln = z[o + 3] | (z[o + 4] << 8)
        if ln != (~nln & 0xFFFF):
            raise ValueError("LEN %d and NLEN 0x%04x are not complements" % (ln, nln))
        o += 5 + ln
        if o > end:
            raise ValueError("block payload of %d runs past the stream end" % ln)
        blocks.append((ln, bfinal))
        if bfinal:
            break
    if o != end:
        raise ValueError("BFINAL block ended at %d, Adler trailer starts at %d "
                         "(%d bytes of blocks the final flag orphaned)"
                         % (o, end, end - o))
    return blocks


def expect_px(w, h):
    """The same rule tests/unit/pngenc_test.c's make_px() uses. Duplicated on
    purpose: an oracle that imported the value under test would not be one."""
    out = bytearray()
    for y in range(h):
        for x in range(w):
            out.append((x * 7 + 3) & 0xFF)
            out.append((y * 13 + 5) & 0xFF)
            out.append(((x * 31) ^ (y * 17)) & 0xFF)
            out.append((255 - ((x + y) & 0xFF)) & 0xFF)
    return bytes(out)


def check_file(path, quiet=False):
    """Full external verification. Returns None on success, or a reason."""
    with open(path, "rb") as f:
        data = f.read()
    try:
        chunks = list(split_chunks(data))
    except ValueError as e:
        return "chunk structure: %s" % e

    types = [c[0] for c in chunks]
    if types[0] != b"IHDR":
        return "first chunk is %r, not IHDR" % types[0]
    if types[-1] != b"IEND":
        return "last chunk is %r, not IEND" % types[-1]

    for ty, _p, stored, computed in chunks:
        if stored != computed:
            return ("chunk %r CRC: file says 0x%08x, binascii.crc32 says 0x%08x"
                    % (ty, stored, computed))

    w, h, depth, ctype, comp, filt, ilace = struct.unpack(">IIBBBBB", chunks[0][1])
    if (depth, ctype, comp, filt, ilace) != (8, 6, 0, 0, 0):
        return ("IHDR is depth=%d ctype=%d comp=%d filter=%d interlace=%d; "
                "this encoder documents 8/6/0/0/0" % (depth, ctype, comp, filt, ilace))

    idat = b"".join(p for ty, p, _s, _c in chunks if ty == b"IDAT")
    if not idat:
        return "no IDAT"

    try:
        blocks = walk_stored_blocks(idat)
    except ValueError as e:
        return "deflate stored-block structure: %s" % e

    # Python's inflate, which DOES check the Adler-32 trailer.
    try:
        raw = zlib.decompress(idat)
    except zlib.error as e:
        return "zlib.decompress: %s" % e

    stride = w * 4
    if len(raw) != (stride + 1) * h:
        return "raw stream is %d bytes, want (%d+1)*%d = %d" % (
            len(raw), stride, h, (stride + 1) * h)

    px = bytearray()
    for y in range(h):
        row = raw[y * (stride + 1): (y + 1) * (stride + 1)]
        if row[0] != 0:
            return "row %d has filter type %d; this encoder documents 0 (None)" % (y, row[0])
        px += row[1:]

    want = expect_px(w, h)
    if bytes(px) != want:
        i = next(k for k in range(len(want)) if px[k] != want[k])
        return ("pixels differ at byte %d (px %d, channel %d): file 0x%02x, "
                "expected 0x%02x" % (i, i // 4, i % 4, px[i], want[i]))

    if not quiet:
        nfinal = sum(b[1] for b in blocks)
        print("      %dx%d, %d chunk(s), %d stored block(s) %s, %d final, "
              "%d bytes of PNG for %d of pixels"
              % (w, h, len(chunks), len(blocks), [b[0] for b in blocks],
                 nfinal, len(data), w * h * 4))
    return None


def check_with_pil(path):
    """THIRD LEG, and the strongest one when it is available: a complete,
    unrelated PNG DECODER (Pillow, over libpng) reads the file and its pixels
    are compared byte for byte. zlib + binascii above prove the container is
    well-formed; this proves a real-world decoder agrees about the picture.

    Returns None on success, a reason on failure, or the string 'skip: ...'
    when Pillow is not installed -- which must SKIP LOUDLY and must not be
    mistaken for a pass. CLAUDE.md rule: 'a gate that cannot run on this host
    must skip loudly ... and never pass silently.'"""
    try:
        from PIL import Image  # noqa: PLC0415
    except Exception as e:     # noqa: BLE001
        return "skip: Pillow is not installed (%s); `python3 -m pip install " \
               "Pillow` would settle it" % type(e).__name__
    # A REJECTION IS A RESULT, NOT A CRASH. Pillow raises
    # UnidentifiedImageError on a bad chunk CRC and OSError("broken data
    # stream") on a truncated deflate stream -- both of which are exactly what
    # the negative controls produce, and a traceback there is unreadable as a
    # verdict. Both are caught and reported as the finding they are.
    try:
        im = Image.open(path)
        im.load()
    except Exception as e:  # noqa: BLE001
        return "Pillow REFUSED the file: %s: %s" % (type(e).__name__, e)
    if im.mode != "RGBA":
        # Pillow reports the file's own colour type here, so this catches an
        # IHDR that says 6 while the data is something else.
        return "Pillow read mode %r, not RGBA" % im.mode
    got = im.tobytes()
    want = expect_px(im.width, im.height)
    if got != want:
        i = next(k for k in range(len(want)) if got[k] != want[k])
        return ("Pillow's pixels differ at byte %d (px %d, channel %d): "
                "0x%02x vs 0x%02x" % (i, i // 4, i % 4, got[i], want[i]))
    return None


def main():
    if len(sys.argv) < 2:
        print("usage: pngenc_ext_test.py <dir>")
        return 2
    d = sys.argv[1]
    files = sorted(f for f in os.listdir(d) if f.endswith(".png"))
    # THE MANIFEST IS CHECKED, AND IT IS NOT BELT-AND-BRACES -- it is a hole
    # that was watched opening. Running the encoder with the
    # `pngenc-always-final` negative control on made tests/unit/pngenc_test.c
    # fail on exactly the two >65535-byte cases; that file used to return early
    # on failure and so never wrote their .png, this script found three files
    # instead of five, and printed "10 checks, 0 failed" over a knowingly
    # broken encoder. Counting what ARRIVED is the only thing that catches a
    # producer which silently produced less -- the same shape as "test-url
    # reports 32/32 (100.0%) while printing that both corpora are absent".
    EXPECT = ["c1x1.png", "c1x1000.png", "c200x100.png", "c256x257.png", "c37x11.png"]
    if not files:
        print("FAIL: no .png files in %s -- the C gate did not write any, so "
              "this oracle measured nothing" % d)
        return 1
    missing = [f for f in EXPECT if f not in files]
    if missing:
        print("FAIL: %s is missing %d of the %d files tests/unit/pngenc_test.c "
              "writes: %s" % (d, len(missing), len(EXPECT), " ".join(missing)))
        print("      A file is absent because the ENCODER FAILED at that size, "
              "not because this script was pointed at the wrong directory.")
        print("      Verifying only the files that arrived reports green for a "
              "broken encoder; that is measured, not feared.")
        return 1

    print("== pngenc: external verification (python3 zlib + binascii, oracle 2 of 2) ==")
    print("   python %s, zlib %s" % (sys.version.split()[0], zlib.ZLIB_VERSION))
    for f in files:
        why = check_file(os.path.join(d, f))
        if why is None:
            ok("%s verifies against python3's zlib and binascii.crc32" % f)
        else:
            fail("%s" % f, why)

    print("\n== third leg: a complete, unrelated PNG decoder (Pillow/libpng) ==")
    pil_skipped = None
    for f in files:
        why = check_with_pil(os.path.join(d, f))
        if why is None:
            ok("%s: Pillow decodes it as RGBA and every pixel matches" % f)
        elif why.startswith("skip: "):
            pil_skipped = why[6:]
            break
        else:
            fail("%s (Pillow)" % f, why)
    if pil_skipped:
        # Loud, once, naming the missing capability and the command that would
        # settle it -- and NOT counted as a check, so it cannot read as a pass.
        print("SKIP: the Pillow leg did not run: %s" % pil_skipped)
        print("      zlib + binascii still ran and still gate this file; what "
              "is missing is the confirmation from a real-world DECODER.")

    # ---------------------------------------------------------------- control
    # Watched failing, and in the two places the two halves of this script
    # cover: a byte inside the IDAT payload (caught by the pixel comparison or
    # by zlib) and a byte inside a chunk CRC (caught by binascii.crc32, and by
    # NOTHING else here -- which is the whole reason this file exists).
    print("\n== control: both corruptions must be REJECTED ==")
    import tempfile
    # The two LARGEST, not the first two: control (a) pokes a byte at a fixed
    # derived offset and a 73-byte 1x1 PNG does not have one there.
    biggest = sorted(files, key=lambda f: os.path.getsize(os.path.join(d, f)),
                     reverse=True)[:2]
    for f in biggest:                         # two is enough; each is a full run
        path = os.path.join(d, f)
        with open(path, "rb") as fh:
            good = bytearray(fh.read())

        # (a) a PIXEL byte inside the first stored block's payload. The offset
        # is DERIVED (sig + IHDR chunk + IDAT length/type + zlib header + stored
        # block header + 16) rather than written as one number, because a fixed
        # 64 landed past the end of the 73-byte 1x1 file and the control then
        # passed for the wrong reason -- it reported "chunk structure", which is
        # a rejection of a truncated file, not of a wrong pixel.
        off = 8 + (12 + 13) + 8 + 2 + 5 + 16
        idat_payload_end = 8 + (12 + 13) + 8 + len(
            b"".join(p for ty, p, _s, _c in split_chunks(bytes(good)) if ty == b"IDAT"))
        if off + 1 < idat_payload_end - 4:
            bad = bytearray(good)
            bad[off] ^= 0xFF
            with tempfile.NamedTemporaryFile(suffix=".png", delete=False) as t:
                t.write(bad); tmp = t.name
            why = check_file(tmp, quiet=True)
            os.unlink(tmp)
            if why and ("pixels differ" in why or "CRC" in why):
                ok("control (a) %s: a flipped IDAT pixel byte is rejected (%s)"
                   % (f, why.split(":")[0]))
            elif why:
                fail("control (a) %s" % f,
                     "rejected, but for the wrong reason (%s) -- the byte did "
                     "not land where this control thinks it did" % why)
            else:
                fail("control (a) %s" % f,
                     "a flipped IDAT byte VERIFIED -- this script is not "
                     "reading the pixels it claims to")
        else:
            print("      (control (a) skipped for %s: it is smaller than the "
                  "offset this control pokes; that is why files[] below picks "
                  "the two LARGEST files, not the first two)" % f)

        # (b) a byte of the IHDR chunk's CRC. Nothing but binascii.crc32 looks
        # at it, so if this passes, the CRC check is decorative.
        bad = bytearray(good)
        bad[8 + 8 + 13] ^= 0x01               # first byte of IHDR's CRC
        with tempfile.NamedTemporaryFile(suffix=".png", delete=False) as t:
            t.write(bad); tmp = t.name
        why = check_file(tmp, quiet=True)
        os.unlink(tmp)
        if why and "CRC" in why:
            ok("control (b) %s: a flipped chunk CRC is rejected BY THE CRC CHECK" % f)
        elif why:
            fail("control (b) %s" % f,
                 "rejected, but for the wrong reason (%s) -- the CRC check did "
                 "not fire, so it is not the thing being exercised" % why)
        else:
            fail("control (b) %s" % f,
                 "a flipped chunk CRC VERIFIED -- the CRC check is decorative, "
                 "which is precisely the shared-polynomial failure this oracle exists for")

    print("\n%d checks, %d failed" % (checks, len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
