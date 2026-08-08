/* Host test for the content sniffer and the terminal's binary stream guard
 * (c/apps/coreutils/logit_sniff.h).
 *
 * The bug this exists for: `show`/`cat` on a 2.2 MB TrueType font wrote every
 * byte of it onto a character grid. The fix is "look before you print", and the
 * things worth testing about a look-before-you-print rule are the ones a naive
 * implementation gets wrong:
 *
 *  1. It must identify by BYTES, not by name -- so the table is checked against
 *     real magic, including the four different sfnt flavours a font can have.
 *  2. It must not suppress real text. A guard that ate a page of Chinese would
 *     be a worse bug than the one it fixes, so UTF-8 is a first-class case: the
 *     rule only counts C0 controls and NULs, and every byte of a multi-byte
 *     UTF-8 sequence is >= 0x80.
 *  3. IT MUST HOLD FOR A BINARY THAT CONTAINS THE PROTOCOL'S OWN MAGIC. That is
 *     the case a side-band protocol has to answer out loud: a font, an
 *     executable or a random blob can contain the bytes "LRT\x01" followed by
 *     something that parses as a frame. The test builds a REAL, well-formed
 *     RT_T_IMAGE frame, proves it parses (so the case is not vacuous), embeds
 *     it in a binary, and then requires that the same bytes arriving on the
 *     TEXT stream are judged binary and never reach the grid -- while the frame
 *     parser, which reads a different fd entirely, never sees them.
 *  4. The judgement must stay LIVE. A stream that is text for a page and then
 *     dumps a font must be caught at the font, not waved through because its
 *     first block looked fine.
 *
 * Built twice by the Makefile: once normally (make test-sniff) and once with
 * -DSNIFF_NEGATIVE_CONTROL (make test-sniff-negctl), which compiles the latch
 * out of the guard and must make this suite FAIL.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define RT_NO_SYS 1
#include "logit_rich.h"
#include "logit_sniff.h"

static int fails, checks;
#define CHK(cond, ...) do { checks++; if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                                                     printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

#define BB (const unsigned char *)

/* ------------------------------------------------------------ the table --- */

static void test_magic(void)
{
    printf("magic identification (the bytes, never the name)\n");

    /* the file that started this: 0x00010000 is a TrueType sfnt version */
    static const unsigned char ttf[] = { 0x00,0x01,0x00,0x00, 0x00,0x11, 0x01,0x00,
                                         0x00,0x04, 0x00,0x10, 'c','m','a','p' };
    CHK(sniff_id(ttf, sizeof ttf) == SN_TTF, "0x00010000 is a TrueType font");
    CHK(sniff_id(BB "true\0\0\0\0", 8) == SN_TTF, "'true' is an Apple sfnt");
    CHK(sniff_id(BB "OTTOxxxx", 8) == SN_OTF, "'OTTO' is OpenType/CFF");
    CHK(sniff_id(BB "ttcfxxxx", 8) == SN_TTC, "'ttcf' is a font collection");
    CHK(sniff_id(BB "wOFFxxxx", 8) == SN_WOFF, "'wOFF' is a web font");

    CHK(sniff_id(BB "\x89PNG\r\n\x1a\n\x00\x00\x00\x0dIHDR", 20) == SN_PNG, "PNG");
    CHK(sniff_id(BB "\xff\xd8\xff\xe0\x00\x10JFIF", 12) == SN_JPEG, "JPEG");
    CHK(sniff_id(BB "GIF89a\x10\x00", 8) == SN_GIF, "GIF89a");
    CHK(sniff_id(BB "GIF87a\x10\x00", 8) == SN_GIF, "GIF87a");
    CHK(sniff_id(BB "BM\x36\x00\x00\x00", 6) == SN_BMP, "BMP");
    CHK(sniff_id(BB "RIFF\x24\x00\x00\x00WAVEfmt ", 16) == SN_WAV, "WAV");
    CHK(sniff_id(BB "RIFF\x24\x00\x00\x00WEBPVP8 ", 16) == SN_WEBP, "WebP");
    CHK(sniff_id(BB "fLaC\x00\x00\x00\x22", 8) == SN_FLAC, "FLAC");
    CHK(sniff_id(BB "OggS\x00\x02\x00\x00", 8) == SN_OGG, "Ogg");
    CHK(sniff_id(BB "ID3\x04\x00\x00\x00", 8) == SN_MP3, "MP3 with an ID3 tag");
    CHK(sniff_id(BB "\xff\xfb\x90\x64", 4) == SN_MP3, "a bare MP3 frame sync");
    CHK(sniff_id(BB "\x7f" "ELF\x02\x01\x01\x00", 8) == SN_ELF, "ELF");
    CHK(sniff_id(BB "PK\x03\x04\x14\x00\x00\x00", 8) == SN_ZIP, "ZIP");
    CHK(sniff_id(BB "\x1f\x8b\x08\x00", 4) == SN_GZIP, "gzip");
    CHK(sniff_id(BB "%PDF-1.7\n", 9) == SN_PDF, "PDF");
    CHK(sniff_id(BB "\x00\x00\x00\x18""ftypmp42", 12) == SN_MP4, "MP4/ISO-BMFF");
    CHK(sniff_id(BB "\x1a\x45\xdf\xa3\x01\x00\x00\x00", 8) == SN_MKV, "Matroska");
    CHK(sniff_id(BB "<svg width=\"4\">", 15) == SN_SVG, "SVG (text, but an image)");

    /* An ISO-BMFF file starts 00 00 00 18 -- two NULs. The order of the tests
     * matters, and this is what proves it: the MP4 case must be reached before
     * the "a NUL means binary" fallback. */
    CHK(sniff_class(SN_MP4) == SNC_OPAQUE, "an MP4 is not something show can play yet");
}

static void test_annexb(void)
{
    printf("H.264 vs H.265, told apart by which NAL header is structurally valid\n");

    /* H.264 SPS: type 7. Read as HEVC that first byte is type 51, reserved. */
    CHK(sniff_id(BB "\x00\x00\x00\x01\x67\x42\xc0\x14", 8) == SN_H264, "annex-B + 0x67 is H.264 SPS");
    CHK(sniff_id(BB "\x00\x00\x01\x67\x42\xc0\x14\xd9", 8) == SN_H264, "3-byte start code too");
    CHK(sniff_id(BB "\x00\x00\x00\x01\x09\x10\x00\x00", 8) == SN_H264, "an access-unit delimiter");
    /* H.265 VPS: type 32, layer 0, tid+1 = 1. Read as AVC it is type 0, unspecified. */
    CHK(sniff_id(BB "\x00\x00\x00\x01\x40\x01\x0c\x01", 8) == SN_H265, "annex-B + 0x4001 is H.265 VPS");
    CHK(sniff_id(BB "\x00\x00\x00\x01\x42\x01\x01\x01", 8) == SN_H265, "H.265 SPS");
    CHK(sniff_class(SN_H264) == SNC_VIDEO && sniff_class(SN_H265) == SNC_VIDEO,
        "both are video");

    /* The real fixtures, when the test is run from the repo root -- a synthetic
     * vector proves the arithmetic, a real stream proves the arithmetic is
     * about the streams this OS actually carries. */
    struct { const char *path; int want; } fx[] = {
        { "tests/fixtures/video/sample.h264", SN_H264 },
        { "tests/fixtures/video265/sample.h265", SN_H265 },
        { "fsroot/fonts/ui.ttf", SN_TTF },
        { "fsroot/fonts/mono.ttf", SN_TTF },
        { "fsroot/readme.txt", SN_TEXT },
    };
    for (unsigned i = 0; i < sizeof fx / sizeof fx[0]; i++) {
        FILE *f = fopen(fx[i].path, "rb");
        if (!f) { printf("  (skipped %s -- not found from this cwd)\n", fx[i].path); continue; }
        unsigned char b[SNIFF_PREFIX];
        int n = (int)fread(b, 1, sizeof b, f);
        fclose(f);
        int got = sniff_id(b, n);
        CHK(got == fx[i].want, "%s is %s (got %s)", fx[i].path,
            sniff_name(fx[i].want), sniff_name(got));
    }
}

/* ------------------------------------------------------------- text ------- */

static void test_text(void)
{
    printf("text stays text -- including the text that is not ASCII\n");

    CHK(sniff_id(BB "hello world\n", 12) == SN_TEXT, "ASCII");
    CHK(sniff_id(BB "", 0) == SN_TEXT, "an empty file prints as nothing, not as binary");
    CHK(sniff_id(BB "col1\tcol2\r\nval\tval\n", 19) == SN_TEXT, "tabs and CRLF are text");

    /* UTF-8: 你好，世界 -- every continuation byte is >= 0x80 and none is a C0
     * control, which is exactly why the rule is written in terms of C0. */
    static const unsigned char cjk[] = {
        0xE4,0xBD,0xA0, 0xE5,0xA5,0xBD, 0xEF,0xBC,0x8C, 0xE4,0xB8,0x96, 0xE7,0x95,0x8C, '\n'
    };
    CHK(sniff_id(cjk, sizeof cjk) == SN_TEXT, "UTF-8 Chinese is text");

    struct sniff_guard g;
    sniff_guard_reset(&g);
    for (int rep = 0; rep < 400; rep++)
        CHK(sniff_guard_feed(&g, cjk, sizeof cjk) == 0 || rep < 0, "CJK never trips the guard");
    CHK(!g.binary, "6400 bytes of Chinese are still text");

    /* A big ASCII stream, the everyday case. */
    sniff_guard_reset(&g);
    for (int i = 0; i < 20000; i++) {
        unsigned char c = (unsigned char)("abcdefghijklmnopqrstuvwxyz \n"[i % 28]);
        sniff_guard_byte(&g, c);
    }
    CHK(!g.binary, "20 KB of prose is not binary");
}

/* ------------------------------------------------------------ the guard --- */

static unsigned char font[4096];

static void make_font(void)
{
    font[0] = 0x00; font[1] = 0x01; font[2] = 0x00; font[3] = 0x00;
    font[4] = 0x00; font[5] = 0x11;                    /* numTables */
    for (unsigned i = 6; i < sizeof font; i++)
        font[i] = (unsigned char)((i * 37) ^ (i >> 3));
}

static void test_guard(void)
{
    printf("the guard: a binary never reaches the grid\n");
    make_font();

    struct sniff_guard g;
    sniff_guard_reset(&g);
    int trip = sniff_guard_feed(&g, font, (int)sizeof font);
    CHK(trip == 1, "a font trips the guard");
    CHK(g.binary, "and the stream is latched binary");
    CHK(g.bytes == sizeof font, "every byte was counted (%lu)", g.bytes);
    CHK(g.suppressed == sizeof font - 1, "all but the tripping byte were withheld (%lu)", g.suppressed);
    CHK(sniff_guard_kind(&g) == SN_TTF,
        "and by the time the summary is drawn it can NAME the format (%s)",
        sniff_name(sniff_guard_kind(&g)));

    /* Latching is one-way: text arriving after the binary does not un-latch. */
    CHK(sniff_guard_feed(&g, BB "back to text now\n", 17) == 0, "no second announcement");
    CHK(g.binary, "still binary");

    /* Live judgement: a page of text, then a font. */
    sniff_guard_reset(&g);
    for (int i = 0; i < 1200; i++) sniff_guard_byte(&g, (unsigned char)("hello \n"[i % 7]));
    CHK(!g.binary, "1200 bytes of text are fine");
    trip = sniff_guard_feed(&g, font, 64);
    CHK(trip == 1, "the font that follows is still caught");
    CHK(sniff_guard_kind(&g) == SN_UNKNOWN,
        "and is honestly reported as generic binary, because the head was text");

    /* An ELF -- the other thing a Finder double-click can hand a terminal. */
    sniff_guard_reset(&g);
    CHK(sniff_guard_feed(&g, BB "\x7f" "ELF\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x3e\x00", 20) == 1,
        "an ELF trips the guard");
    CHK(sniff_guard_kind(&g) == SN_ELF, "and is named as an executable");

    /* An ESC byte. LRT/1 has no in-band control language, so an escape stream
     * is not "text with colours in it" here -- it is evidence. */
    sniff_guard_reset(&g);
    const char *esc = "hi\x1b[31mred\x1b[0m and \x1b[1mbold\x1b[0m";
    CHK(sniff_guard_feed(&g, (const unsigned char *)esc, (int)strlen(esc)) == 1,
        "a stream of ANSI escapes is caught rather than half-rendered");
}

/* ------- the case that matters: a binary carrying the protocol's magic ----- */

static void test_magic_in_binary(void)
{
    printf("a binary whose bytes contain LRT/1's own magic\n");

    /* Build a REAL frame: u8 kind, u16 w, u16 h, str path -- exactly what
     * `show` sends and what the terminal's handle_image() reads. */
    static struct rt_enc e;
    rt_reset(&e);
    rt_u8(&e, RT_IMG_PATH);
    rt_u16(&e, 640); rt_u16(&e, 0);
    rt_str(&e, "/media/dot.png");
    unsigned char frame[256];
    int flen = 0;
    rt_hdr(frame, RT_T_IMAGE, 0, (unsigned)e.n);
    flen = RT_HDR;
    memcpy(frame + flen, e.b, (size_t)e.n);
    flen += e.n;

    /* (a) The case is not vacuous: on the RICH channel these bytes really are a
     * frame the terminal would act on. */
    {
        static struct rt_parser p;
        rt_parser_init(&p);
        rt_parser_feed(&p, frame, flen);
        struct rt_frame f;
        int got = rt_parser_next(&p, &f);
        CHK(got == 1 && f.type == RT_T_IMAGE, "the embedded bytes ARE a valid image frame");
        if (got) {
            struct rt_rd r; rt_rd_init(&r, &f);
            char path[64];
            rt_rd_u8(&r); rt_rd_u16(&r); rt_rd_u16(&r);
            rt_rd_str(&r, path, sizeof path);
            CHK(strcmp(path, "/media/dot.png") == 0, "and it names a real path");
            rt_parser_done(&p, &f);
        }
    }

    /* (b) The same bytes inside a binary, arriving on the TEXT stream. Two
     * things must hold, and they are different claims:
     *   - the guard judges the stream binary, so nothing is painted;
     *   - the frame parser never sees it, because text and frames are different
     *     fds. That is the structural half of LRT/1 and it is what makes this
     *     class of attack impossible rather than merely handled. */
    make_font();
    unsigned char blob[4096 + 256];
    memcpy(blob, font, 300);
    memcpy(blob + 300, frame, (size_t)flen);
    memcpy(blob + 300 + flen, font + 300, 500);
    int blen = 300 + flen + 500;

    struct sniff_guard g;
    sniff_guard_reset(&g);
    int trip = sniff_guard_feed(&g, blob, blen);
    CHK(trip == 1, "the blob is judged binary");
    CHK(g.binary && g.suppressed == (unsigned long)blen - 1,
        "and every byte after the first is withheld from the grid (%lu of %d)",
        g.suppressed, blen);
    CHK(sniff_guard_kind(&g) == SN_TTF, "the summary still names it correctly");

    /* And the reverse framing worry: a magic that lands in a TEXT file. It is
     * printable, so it is printed -- and that is CORRECT, because the text pipe
     * is not the frame pipe. Nothing acts on it. */
    {
        const char *txt = "log line one\nLRT\x01 appears in this log\nlog line three\n";
        struct sniff_guard t;
        sniff_guard_reset(&t);
        int tripped = sniff_guard_feed(&t, (const unsigned char *)txt, (int)strlen(txt));
        CHK(tripped == 0 && !t.binary,
            "a log line that happens to say LRT\\x01 is still printed as text");
        CHK(sniff_id((const unsigned char *)txt, (int)strlen(txt)) == SN_TEXT,
            "one stray control byte in fifty does not make a file binary");
        /* This is the whole argument for a side band in one assertion: those
         * bytes are on fd 1. The frame parser reads fd 3. It is not that the
         * terminal checks and declines to act -- it is that the two never
         * meet, which is why no amount of magic in a text stream can inject a
         * frame. */
    }
}

/* ------------------------------------------------------------- hexdump ---- */

static void test_hex(void)
{
    printf("the hexdump the refusal prints\n");
    char out[64];
    int n = sniff_hex(BB "\x00\x01\x00\x00\xde\xad", 6, out, (int)sizeof out);
    CHK(strcmp(out, "00 01 00 00 de ad") == 0, "hexdump is '%s'", out);
    CHK(n == 17, "and reports its own length (%d)", n);

    char tiny[6];
    sniff_hex(BB "\x00\x01\x00\x00", 4, tiny, (int)sizeof tiny);
    CHK(strlen(tiny) < sizeof tiny, "a short buffer truncates rather than overflows ('%s')", tiny);
}

static void test_class(void)
{
    printf("what each kind means to a consumer\n");
    CHK(sniff_class(SN_TEXT) == SNC_TEXT, "text");
    CHK(sniff_class(SN_PNG) == SNC_IMAGE, "png -> image");
    CHK(sniff_class(SN_TTF) == SNC_OPAQUE, "a font is opaque: nothing here can draw it");
    CHK(sniff_class(SN_ELF) == SNC_OPAQUE, "so is an executable");
    CHK(sniff_opener(SN_PNG) && strcmp(sniff_opener(SN_PNG), "show") == 0, "an image says what opens it");
    CHK(sniff_opener(SN_TTF) == 0, "a font has nothing to suggest, and says so");
    CHK(strcmp(sniff_name(SN_TTF), "TrueType font") == 0, "names are for humans");
}

int main(void)
{
    test_magic();
    test_annexb();
    test_text();
    test_guard();
    test_magic_in_binary();
    test_hex();
    test_class();
    printf(fails ? "SOME FAILED (%d of %d)\n" : "ALL PASS (%d failures, %d checks)\n",
           fails, checks);
    return fails != 0;
}
