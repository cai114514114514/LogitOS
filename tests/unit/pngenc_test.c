/* pngenc_test.c -- the PNG ENCODER (rust/src/pngenc.rs), oracle 1 of 2.
 *
 * ORACLE 1 IS rust/src/png.rs, AND IT IS A GOOD ONE PRECISELY BECAUSE IT WAS
 * NOT WRITTEN FOR THIS. It is an independent, already-gated implementation of
 * the other direction, in the tree since long before an encoder existed.
 * Encode a surface, decode it, and every pixel byte must come back identical:
 * that is a differential test, not a self-check.
 *
 * ORACLE 2 IS NOT IN THIS FILE AND IS NOT OPTIONAL. See
 * tests/unit/pngenc_ext_test.py. The failure this file structurally CANNOT see
 * is one the two halves SHARE -- and the guess about WHICH field that is came
 * back half wrong when it was measured instead of assumed:
 *
 *   Adler-32   IS covered here. rust/src/inflate.rs:272 refuses a stream whose
 *              trailer does not match, so a wrong modulus reddens control (b)
 *              below -- which is exactly what it did, printing "the decode
 *              REFUSES" rather than "the round-trip differs".
 *   chunk CRC  is NOT covered, by anything in this repository except oracle 2.
 *              `grep -ic crc rust/src/png.rs` is 0, and a copy of this gate's
 *              own c37x11.png with all four chunk CRCs zeroed decoded as
 *              `png_decode -> 0, 37x11`. An encoder emitting zeroes there
 *              would score 12/12 in this file.
 *
 * THE SIZES ARE THE ARGUMENT. Each is here because it breaks a different
 * naive encoder:
 *
 *   1x1        the degenerate case; also the only one where "stride" and
 *              "pixel" and "row" are all the same number, so an encoder that
 *              confused any two of them still passes it -- which is why it is
 *              the WEAKEST case here and never the only one.
 *   37x11      a width whose row (148 B) is not a multiple of 4, 8, 16 or 64
 *              and whose raw stream (1,639 B) is prime-ish -- nothing here
 *              lines up with a buffer size anybody would pick.
 *   1x1000     one pixel wide: 1000 rows of 1 filter byte + 4 data bytes. The
 *              filter byte is 20% of the stream, so an off-by-one in the
 *              per-row header shears the image immediately instead of subtly.
 *   200x100    >>> THE ONE THIS DESIGN IS MOST LIKELY TO GET WRONG <<<
 *              80,200 raw bytes. A stored deflate block's LEN is a u16, so
 *              this MUST split into two blocks (65535 + 14665) and only the
 *              second may carry BFINAL. Get BFINAL wrong on the first and the
 *              stream decodes to a TRUNCATED image rather than to an error --
 *              which is exactly the shape of bug a round-trip on small inputs
 *              never reaches. tests/unit/pngenc_ext_test.py checks the block
 *              structure directly with python3's zlib, from outside.
 *   256x257    65,792 px = 263,168 raw-payload bytes: FIVE stored blocks, and
 *              a height above 256 so a u8 row counter wraps visibly.
 *
 * THE CONTROL IS WATCHED FAILING, in both directions, at the bottom of main():
 * corrupt one pixel and the round-trip must differ; corrupt one output byte
 * inside the IDAT payload and the round-trip must ALSO differ. A gate that has
 * never been seen red is not evidence.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The Rust staticlib allocates through the host's kmalloc/kfree. */
void *kmalloc(unsigned long n) { return malloc(n); }
void kfree(void *p) { free(p); }

/* struct image, as rust/src/imgbuf.rs mirrors it. */
struct image { int w, h; unsigned char *rgba; };

unsigned char *png_encode_rgba(const unsigned char *px, int w, int h, int *out_len);
void png_encode_free(unsigned char *p);
int png_decode(const unsigned char *p, int n, struct image *out);
int rust_pngenc_selftest(void);

static int checks, failed;

static void ok(const char *what)   { checks++; printf("ok  : %s\n", what); }
static void fail(const char *what, const char *why)
{ checks++; failed++; printf("FAIL: %s\n      %s\n", what, why ? why : ""); }

/* A deterministic, non-uniform test image. Every channel varies along BOTH
 * axes and by a different rule, so a transposed axis, a swapped channel, a
 * dropped alpha or a stride error all have to change some byte. A gradient
 * that is a function of (x+y) alone would survive a transpose. */
static unsigned char *make_px(int w, int h)
{
    unsigned char *p = (unsigned char *)malloc((size_t)w * h * 4);
    if (!p) return NULL;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            unsigned char *d = p + ((size_t)y * w + x) * 4;
            d[0] = (unsigned char)(x * 7 + 3);          /* R varies fast in x  */
            d[1] = (unsigned char)(y * 13 + 5);         /* G varies fast in y  */
            d[2] = (unsigned char)((x * 31) ^ (y * 17));/* B mixes them        */
            d[3] = (unsigned char)(255 - ((x + y) & 0xff)); /* A is not opaque */
        }
    return p;
}

/* The Rust staticlib is ONE codegen unit, so linking the encoder drags every
 * decoder's registration in with it. tests/unit/rust_host_shim.c supplies
 * img_register_anim and deliberately NOT img_register (its own header explains
 * why: the tests that decode images link the real c/lib/image/img.c and a
 * strong definition there would collide). This gate decodes nothing through
 * img.c, so it supplies the other half itself. Never called. */
void img_register(void *d, void *f) { (void)d; (void)f; }

/* Encode, decode, compare. Returns the encoded bytes through enc/enclen so
 * the caller can write them out for the external oracle; NULL on any failure
 * (which is reported here). */
static int roundtrip(const char *label, int w, int h,
                     unsigned char **enc, int *enclen)
{
    char buf[256];
    unsigned char *px = make_px(w, h);
    if (!px) { fail(label, "out of memory building the source pixels"); return 0; }

    int n = 0;
    unsigned char *out = png_encode_rgba(px, w, h, &n);
    if (!out || n <= 0) {
        snprintf(buf, sizeof buf, "png_encode_rgba returned %p, len %d", (void *)out, n);
        fail(label, buf); free(px); return 0;
    }

    /* THE ENCODED BYTES GO BACK TO THE CALLER EVEN WHEN THE ROUND-TRIP FAILS,
     * and that is not tidiness -- it is a hole this file had and that the
     * -Fpngenc-always-final control walked straight into. With the two large
     * cases failing here, the old code returned early and never wrote their
     * .png, so tests/unit/pngenc_ext_test.py found three files instead of
     * five, checked those three, and printed "10 checks, 0 failed" over a
     * BROKEN encoder. That is the "test-url reports 32/32 (100.0%) while
     * printing that both corpora are absent" shape, reproduced. The external
     * oracle now gets every file that encoded at all, and refuses to run
     * against a short manifest. */
    size_t bytes = (size_t)w * h * 4;
    if (enc) { *enc = out; *enclen = n; }

    struct image img; memset(&img, 0, sizeof img);
    int r = png_decode(out, n, &img);
    int good = 1;
    if (r != 0 || !img.rgba) {
        snprintf(buf, sizeof buf, "png_decode refused our own output (r=%d), %d bytes", r, n);
        fail(label, buf); good = 0;
    } else if (img.w != w || img.h != h) {
        snprintf(buf, sizeof buf, "decoded %dx%d, encoded %dx%d", img.w, img.h, w, h);
        fail(label, buf); good = 0;
    } else if (memcmp(img.rgba, px, bytes)) {
        size_t i = 0; while (i < bytes && img.rgba[i] == px[i]) i++;
        snprintf(buf, sizeof buf,
                 "pixels differ at byte %lu of %lu (px %lu, channel %lu): "
                 "encoded 0x%02x, decoded 0x%02x",
                 (unsigned long)i, (unsigned long)bytes,
                 (unsigned long)(i / 4), (unsigned long)(i % 4),
                 px[i], img.rgba[i]);
        fail(label, buf); good = 0;
    }
    if (good) {
        snprintf(buf, sizeof buf, "%s (%dx%d -> %d bytes, %.4gx raw)",
                 label, w, h, n, (double)n / (double)bytes);
        ok(buf);
    }
    free(px);
    if (img.rgba) free(img.rgba);
    if (!enc) png_encode_free(out);
    return good;
}

static void write_out(const char *dir, const char *name, const unsigned char *p, int n)
{
    if (!dir) return;
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { printf("      (could not write %s)\n", path); return; }
    fwrite(p, 1, (size_t)n, f);
    fclose(f);
}

int main(int argc, char **argv)
{
    /* argv[1], when given, is a directory to drop the encoded PNGs into --
     * that is how tests/unit/pngenc_ext_test.py gets the SAME bytes this file
     * just round-tripped, rather than re-encoding them and possibly measuring
     * a different build. */
    const char *dir = argc > 1 ? argv[1] : NULL;

    printf("== pngenc: round-trip against rust/src/png.rs (oracle 1 of 2) ==\n");

    static const struct { const char *name; const char *file; int w, h; } CASES[] = {
        { "1x1",                       "c1x1.png",     1,    1 },
        { "37x11 (row 148B, no nice alignment)", "c37x11.png", 37, 11 },
        { "1x1000 (filter byte is 20% of the stream)", "c1x1000.png", 1, 1000 },
        { "200x100 (80,200 raw B: TWO stored blocks)", "c200x100.png", 200, 100 },
        { "256x257 (263,168 raw B: FIVE stored blocks)", "c256x257.png", 256, 257 },
    };
    for (unsigned i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        unsigned char *enc = NULL; int n = 0;
        roundtrip(CASES[i].name, CASES[i].w, CASES[i].h, &enc, &n);
        if (enc) { write_out(dir, CASES[i].file, enc, n); png_encode_free(enc); }
    }

    /* The in-crate self-test (the one a boot harness can call). */
    if (rust_pngenc_selftest() == 0) ok("rust_pngenc_selftest");
    else fail("rust_pngenc_selftest", "the encoder's own round-trip failed");

    /* Refusals. A zero or negative dimension must return NULL, not a
     * zero-byte file: a caller that got 0 bytes back and no error would write
     * an empty PNG somewhere and find out much later. */
    {
        unsigned char one[4] = { 1, 2, 3, 4 };
        int n = 7;
        if (!png_encode_rgba(one, 0, 1, &n) && n == 0) ok("width 0 is refused, and out_len is zeroed");
        else fail("width 0 is refused", "it returned a buffer or left out_len alone");
        n = 7;
        if (!png_encode_rgba(one, 1, -3, &n) && n == 0) ok("a negative height is refused");
        else fail("a negative height is refused", "it returned a buffer");
        n = 7;
        if (!png_encode_rgba(NULL, 1, 1, &n)) ok("a NULL pixel pointer is refused");
        else fail("a NULL pixel pointer is refused", "it returned a buffer");
        png_encode_free(NULL); /* must not crash */
        ok("png_encode_free(NULL) is a no-op");
    }

    /* ------------------------------------------------------------------ */
    /* THE CONTROL, WATCHED FAILING. Two directions, because they catch    */
    /* different halves: a wrong PIXEL proves the comparison is looking at */
    /* the image at all; a wrong OUTPUT BYTE proves the decode is looking  */
    /* at the bytes we produced and not at something cached.               */
    /* ------------------------------------------------------------------ */
    printf("\n== control: the round-trip must be watchable failing ==\n");
    {
        const int W = 37, H = 11;
        unsigned char *px = make_px(W, H);
        int n = 0;
        unsigned char *out = png_encode_rgba(px, W, H, &n);
        if (!out) { fail("control setup", "encode failed"); free(px); return 1; }

        /* (a) corrupt a source pixel AFTER encoding: the decode must differ. */
        px[(5 * W + 9) * 4 + 2] ^= 0x40;
        struct image img; memset(&img, 0, sizeof img);
        if (png_decode(out, n, &img) == 0 && img.rgba) {
            if (memcmp(img.rgba, px, (size_t)W * H * 4))
                ok("control (a): one flipped source pixel makes the round-trip differ");
            else
                fail("control (a)", "a flipped pixel compared EQUAL -- the comparison "
                                    "is not looking at the image");
            free(img.rgba);
        } else fail("control (a)", "decode of a good buffer failed");
        free(px);

        /* (b) corrupt one byte inside the IDAT payload. With STORED deflate
         * blocks that byte IS a pixel byte, so the image comes back decodable
         * and WRONG -- which is the interesting case: it proves the assertion
         * would catch a real encoder bug, not merely a broken file. The byte
         * is chosen well past the 8+25+8 header prefix and well inside the
         * first block's payload. */
        unsigned char *px2 = make_px(W, H);
        int idx = 8 + 25 + 8 + 2 + 5 + 40;   /* sig + IHDR + IDAT hdr + zlib + block hdr */
        out[idx] ^= 0xFF;
        memset(&img, 0, sizeof img);
        int r = png_decode(out, n, &img);
        if (r != 0 || !img.rgba) {
            /* Also an acceptable outcome -- the point is that it is NOT
             * silently equal. Say which happened so the log is readable. */
            ok("control (b): one flipped IDAT byte makes the decode REFUSE");
        } else if (memcmp(img.rgba, px2, (size_t)W * H * 4)) {
            ok("control (b): one flipped IDAT byte makes the round-trip differ");
        } else {
            fail("control (b)", "a flipped IDAT byte compared EQUAL -- the decode "
                                "is not reading the bytes we produced");
        }
        if (img.rgba) free(img.rgba);
        free(px2);
        png_encode_free(out);
    }

    printf("\n%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
