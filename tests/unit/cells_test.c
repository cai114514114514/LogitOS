/* The cell/byte rule (c/apps/coreutils/logit_cells.h) against references that
 * are not it.
 *
 * The header converts a BYTE index in a UTF-8 string to the COLUMN the terminal
 * grid draws it at, and back. Two binaries depend on that number agreeing --
 * /bin/sh publishes its edit cursor in it, the Terminal draws a caret at it --
 * so "the two sides look consistent" proves nothing: they would look consistent
 * if both were wrong. Every claim here is therefore checked against something
 * written by somebody else:
 *
 *   1. THE DECODER against c/lib/text/utf8.c, which is the decoder the renderer
 *      itself walks the string with (c/lib/text/shape.c calls utf8_next). If
 *      the header disagrees about where a character starts, the caret lands
 *      inside one. Checked over every 1- and 2-byte sequence exhaustively, every
 *      valid code point's canonical encoding, and a large crafted set of
 *      3- and 4-byte forms including every malformed class.
 *
 *   2. THE WIDTH TABLE against /usr/share/unicode/EastAsianWidth.txt, re-derived
 *      from the UCD on every run for all 1,114,112 code points. The table in the
 *      header is generated, not typed; this is what keeps it that way, and it is
 *      the same arrangement tests/text.mk uses for the bidi tables.
 *
 *   3. THE RENDERER, measured rather than asserted. c/lib/text/shape.c decides a
 *      character's cell count from THIS FONT's advance for THIS glyph
 *      (shape.c:1215, `adv > cell * 3 / 2`), which is not a function of the code
 *      point and cannot be one -- a glyph missing from the shipped subset falls
 *      back to a narrow .notdef, and a glyph from the proportional fallback font
 *      can be wide while being an ordinary letter. So this section prints the
 *      size of the disagreement instead of demanding there be none, and hard-
 *      asserts only the part that must never move: printable ASCII is one cell
 *      in both, which is the compatibility claim for every line that exists
 *      today.
 *
 * `make test-cells-negctl` compiles this same file with -DCELLS_NEGATIVE_CONTROL
 * and REQUIRES it to fail: that define puts the header back to one cell per
 * byte, an identity inverse and a byte-at-a-time cursor, i.e. the behaviour
 * /bin/sh and the Terminal had before the header existed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "logit_cells.h"     /* the thing under test */
#include "utf8.h"            /* reference 1: c/lib/text/utf8.c */
#include "ttf.h"             /* reference 3: the real fonts, through */
#include "shape.h"           /*              the real cell-grid layout */

static int fails, checks;

#define CHK(cond, ...) do { checks++; if (!(cond)) { \
        if (fails < 40) { printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                          printf(__VA_ARGS__); printf("\n"); } \
        fails++; } } while (0)

/* ------------------------------------------------------------------------- */
/* 1. THE DECODER vs c/lib/text/utf8.c                                       */
/* ------------------------------------------------------------------------- */

/* One sequence, compared both ways. `buf` must be NUL-terminated at `len`,
 * because that is the only condition under which utf8_next has a bound at all:
 * it stops on the NUL, this header stops on the length, and the two are the
 * same stop. (The header additionally has to answer for a cursor sitting inside
 * a sequence that has not finished arriving, which utf8_next cannot be asked;
 * that case is covered by the truncation test below.) */
static void cmp_decode(const unsigned char *buf, int len, const char *what)
{
    char z[16];
    if (len > 12) len = 12;
    memcpy(z, buf, (size_t)len);
    z[len] = 0;

    int i = 0;
    while (i < len) {
        unsigned mine = 0;
        int adv = lc_decode(z, len, i, &mine);

        uint32_t theirs = 0;
        const char *q = utf8_next(z + i, &theirs);
        int tadv = (int)(q - (z + i));

        checks++;
        if (adv != tadv || mine != theirs) {
            if (fails < 40)
                printf("FAIL %s at byte %d: header gave U+%04X +%d, "
                       "c/lib/text/utf8.c gave U+%04X +%d\n",
                       what, i, mine, adv, theirs, tadv);
            fails++;
            return;                       /* the walk has desynchronised */
        }
        i += adv;
    }
    checks++;
    if (i != len) { printf("FAIL %s: walk ended at %d of %d\n", what, i, len); fails++; }
}

static int enc_utf8(unsigned cp, unsigned char *o)
{
    if (cp < 0x80)    { o[0] = (unsigned char)cp; return 1; }
    if (cp < 0x800)   { o[0] = (unsigned char)(0xC0 | (cp >> 6));
                        o[1] = (unsigned char)(0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000) { o[0] = (unsigned char)(0xE0 | (cp >> 12));
                        o[1] = (unsigned char)(0x80 | ((cp >> 6) & 63));
                        o[2] = (unsigned char)(0x80 | (cp & 63)); return 3; }
    o[0] = (unsigned char)(0xF0 | (cp >> 18));
    o[1] = (unsigned char)(0x80 | ((cp >> 12) & 63));
    o[2] = (unsigned char)(0x80 | ((cp >> 6) & 63));
    o[3] = (unsigned char)(0x80 | (cp & 63));
    return 4;
}

static unsigned rng_state = 0x12345678u;
static unsigned rng(void)
{ rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5; return rng_state; }

static void t_decoder(void)
{
    unsigned char b[8];

    /* every single byte, alone -- this is where every bad lead byte lives */
    for (unsigned c = 1; c < 256; c++) { b[0] = (unsigned char)c; cmp_decode(b, 1, "1-byte"); }

    /* every two-byte pair. 65,280 sequences: all the C0/C1 overlongs, all the
     * truncated leads, and all the well-formed two-byte forms. */
    for (unsigned c = 1; c < 256; c++)
        for (unsigned d = 1; d < 256; d++) {
            b[0] = (unsigned char)c; b[1] = (unsigned char)d;
            cmp_decode(b, 2, "2-byte");
        }

    /* every valid code point's canonical encoding, surrogates included as the
     * malformed cases they are */
    for (unsigned cp = 1; cp <= 0x10FFFFu; cp++) {
        int n = enc_utf8(cp, b);
        cmp_decode(b, n, "canonical");
    }

    /* crafted 3- and 4-byte forms: random tails behind every lead byte, which
     * is how the surrogate, overlong and bad-continuation branches get hit in
     * combinations no encoder would produce */
    for (int k = 0; k < 400000; k++) {
        int n = 3 + (int)(rng() & 1u);
        b[0] = (unsigned char)(0xE0 + (rng() & 0x1Fu));
        for (int j = 1; j < n; j++) b[j] = (unsigned char)(rng() & 0xFFu);
        cmp_decode(b, n, "crafted");
    }

    /* TRUNCATION, which utf8_next cannot be asked about: a lead byte whose
     * continuations are outside the measured length must consume exactly one
     * byte, or the shell would read past its own cursor. */
    b[0] = 0xE4; b[1] = 0xB8; b[2] = 0xAD; b[3] = 0;
    for (int len = 1; len <= 2; len++) {
        unsigned cp = 0;
        int adv = lc_decode((char *)b, len, 0, &cp);
        CHK(adv == 1 && cp == 0xFFFDu,
            "truncated 3-byte at len=%d gave U+%04X +%d, want U+FFFD +1", len, cp, adv);
    }
}

/* ------------------------------------------------------------------------- */
/* 2. THE WIDTH TABLE vs EastAsianWidth.txt                                  */
/* ------------------------------------------------------------------------- */

#define NCP 0x110000

static void t_width(const char *ucd_path)
{
    FILE *f = fopen(ucd_path, "r");
    if (!f) {
        printf("FAIL cannot open %s -- the width table has no reference to be "
               "checked against, so this gate cannot pass\n", ucd_path);
        fails++; checks++;
        return;
    }

    unsigned char *want = calloc(NCP, 1);
    if (!want) { printf("FAIL out of memory\n"); fails++; fclose(f); return; }

    char line[512];
    long ranges = 0;
    while (fgets(line, sizeof line, f)) {
        char *h = strchr(line, '#'); if (h) *h = 0;
        char *semi = strchr(line, ';'); if (!semi) continue;
        *semi = 0;
        char *w = semi + 1;
        while (*w == ' ' || *w == '\t') w++;
        /* W and F only. A (ambiguous) is narrow here -- the wcwidth default
         * outside a CJK locale, and the header says so. */
        if (!(w[0] == 'W' || w[0] == 'F') ||
            !(w[1] == 0 || w[1] == ' ' || w[1] == '\t' || w[1] == '\n' || w[1] == '\r'))
            continue;
        unsigned lo = 0, hi = 0;
        char *dots = strstr(line, "..");
        if (dots) { *dots = 0; lo = (unsigned)strtoul(line, 0, 16); hi = (unsigned)strtoul(dots + 2, 0, 16); }
        else      { lo = hi = (unsigned)strtoul(line, 0, 16); }
        if (hi >= NCP) hi = NCP - 1;
        for (unsigned cp = lo; cp <= hi; cp++) want[cp] = 1;
        ranges++;
    }
    fclose(f);

    CHK(ranges > 100, "%s parsed to only %ld W/F ranges -- wrong file?", ucd_path, ranges);

    long wide = 0, bad = 0;
    for (unsigned cp = 0; cp < NCP; cp++) {
        int got = lc_wide(cp) ? 1 : 0;
        if (want[cp]) wide++;
        checks++;
        if (got != want[cp]) {
            if (bad < 8)
                printf("FAIL U+%04X: header says %s, EastAsianWidth.txt says %s\n",
                       cp, got ? "wide" : "narrow", want[cp] ? "wide" : "narrow");
            bad++; fails++;
        }
    }
    printf("  width: %ld code points wide per the UCD, %ld disagreements\n", wide, bad);
    free(want);
}

/* ------------------------------------------------------------------------- */
/* 3. THE CONVERSION ITSELF                                                  */
/* ------------------------------------------------------------------------- */

/* Every string here is written as escaped bytes rather than literal UTF-8, so
 * that a source file re-encoded by an editor cannot silently change what is
 * being tested. */
struct scase { const char *s; int bytes; int cells; const char *what; };

static const struct scase CASES[] = {
    { "hello",                       5,  5, "ascii" },
    { "",                            0,  0, "empty" },
    { "\xE4\xB8\xAD",                3,  2, "U+4E2D, one wide char" },
    { "\xE4\xB8\xAD\xE6\x96\x87",    6,  4, "two wide chars" },
    { "a\xE4\xB8\xAD" "b",           5,  4, "narrow-wide-narrow" },
    { "\xC3\xA9",                    2,  1, "U+00E9, 2 bytes 1 cell" },
    { "e\xCC\x81",                   3,  2, "e + combining acute: TWO cells here" },
    { "\xF0\x9F\x98\x80",            4,  2, "U+1F600, 4 bytes 2 cells" },
    { "\xEF\xBD\xB1",                3,  1, "U+FF71 halfwidth katakana is NARROW" },
    { "\xEF\xBC\xA1",                3,  2, "U+FF21 fullwidth A is wide" },
    { "\xED\x95\x9C",                3,  2, "U+D55C hangul syllable" },
    { "\xFF\xFE",                    2,  2, "two invalid bytes, one cell each" },
    { "a\xE4\xB8",                   3,  3, "a + a truncated lead: 1 + 1 + 1" },
    { 0, 0, 0, 0 }
};

static void t_convert(void)
{
    for (int i = 0; CASES[i].s; i++) {
        const struct scase *c = &CASES[i];
        CHK((int)strlen(c->s) == c->bytes, "%s: case declares %d bytes, strlen says %d",
            c->what, c->bytes, (int)strlen(c->s));
        CHK(lc_cells(c->s, c->bytes) == c->cells,
            "%s: lc_cells = %d, want %d", c->what, lc_cells(c->s, c->bytes), c->cells);

        /* THE INVERSE IS A LEFT INVERSE, not a bijection: a wide character
         * covers two columns and only the first is a byte boundary, so
         * lc_bytes(cells(i)) == i for every boundary i, and a column inside a
         * wide character comes back to that character's own start. */
        for (int b = 0; b <= c->bytes; b = lc_next(c->s, c->bytes, b)) {
            int col = lc_cells(c->s, b);
            CHK(lc_bytes(c->s, c->bytes, col) == b,
                "%s: byte %d -> col %d -> byte %d", c->what, b, col, lc_bytes(c->s, c->bytes, col));
            if (b == c->bytes) break;
        }
        for (int col = 0; col <= c->cells; col++) {
            int b = lc_bytes(c->s, c->bytes, col);
            CHK(lc_cells(c->s, b) <= col, "%s: col %d -> byte %d sits at col %d (past it)",
                c->what, col, b, lc_cells(c->s, b));
            CHK(lc_prev(c->s, c->bytes, b) <= b, "%s: lc_prev ran forwards", c->what);
            /* and it IS a boundary: stepping to it from the start must land on it */
            int k = 0; while (k < b) k = lc_next(c->s, c->bytes, k);
            CHK(k == b, "%s: col %d -> byte %d is not a character boundary", c->what, col, b);
        }

        /* lc_next walks every boundary forwards; lc_prev walks the same set
         * backwards. A cursor is only ever moved by these two, so a mismatch
         * here is a cursor that can be parked inside a character. */
        int fwd[64], nf = 0;
        for (int b = 0; b < c->bytes && nf < 64; b = lc_next(c->s, c->bytes, b)) fwd[nf++] = b;
        int back[64], nb = 0;
        for (int b = c->bytes; b > 0 && nb < 64; b = lc_prev(c->s, c->bytes, b)) back[nb++] = lc_prev(c->s, c->bytes, b);
        CHK(nf == nb, "%s: %d boundaries forwards, %d backwards", c->what, nf, nb);
        for (int k = 0; k < nf && k < nb; k++)
            CHK(fwd[k] == back[nb - 1 - k], "%s: boundary %d is %d forwards, %d backwards",
                c->what, k, fwd[k], back[nb - 1 - k]);
    }

    /* The cursor claim, spelled out with the exact number from the header
     * comment: three wide characters typed, the cursor at the end of the
     * buffer. Bytes say 9, cells say 6, and the terminal draws in cells. */
    {
        const char *s = "\xE4\xB8\xAD\xE6\x96\x87\xE5\xAD\x97";
        CHK(strlen(s) == 9, "corpus string is %d bytes", (int)strlen(s));
        CHK(lc_cells(s, 9) == 6, "three wide chars: %d cells, want 6", lc_cells(s, 9));
        CHK(lc_cells(s, 9) != 9, "the byte count and the cell count are the same number "
                                 "-- this string cannot show the bug");
    }
}

/* ------------------------------------------------------------------------- */
/* 4. THE RENDERER, measured                                                 */
/* ------------------------------------------------------------------------- */

#define MAXCP_S 512
static uint32_t sh_cps[MAXCP_S];
static uint8_t  sh_lv[MAXCP_S];
static int      sh_ord[MAXCP_S];
static struct text_run   sh_runs[32];
static struct shape_glyph sh_gl[1024];
static uint8_t  sh_bidi[64 * 1024];
static struct ttf_font    sh_fonts[3];
static struct shape_font_set sh_fs;

static void sh_scratch(struct shape_scratch *sc)
{
    sc->cps = sh_cps; sc->levels = sh_lv; sc->order = sh_ord;
    sc->glyphs = sh_gl; sc->runs = sh_runs; sc->bidi = sh_bidi;
    sc->ncp_cap = MAXCP_S; sc->nglyph_cap = 1024; sc->nrun_cap = 32;
    sc->bidi_cap = (int)sizeof sh_bidi;
}

static int load_font(const char *path, struct ttf_font *f)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { printf("FAIL cannot open font %s\n", path); fails++; return -1; }
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, fp) != (size_t)n) { fclose(fp); printf("FAIL short read %s\n", path); fails++; return -1; }
    fclose(fp);
    if (ttf_parse(b, (int)n, f) != 0) { printf("FAIL cannot parse %s\n", path); fails++; return -1; }
    return 0;
}

/* What the renderer will actually do with this code point, in cells. The font
 * set is the one c/kernel/gui/text.c builds for the Terminal (tl_fonts with
 * prefer = F_MONO): mono first, then the UI font, then DejaVu. */
static int renderer_cells(unsigned cp, int px, int cell)
{
    unsigned char b[8];
    int n = enc_utf8(cp, b);
    struct shape_scratch sc; sh_scratch(&sc);
    int w = shape_line(&sh_fs, (const char *)b, n, px, cell, 0, NULL, &sc);
    return w / cell;
}

static void t_renderer(const char *mono, const char *ui, const char *text)
{
    if (load_font(mono, &sh_fonts[0]) || load_font(ui, &sh_fonts[1]) || load_font(text, &sh_fonts[2]))
        return;
    sh_fs.n = 3; sh_fs.f[0] = &sh_fonts[0]; sh_fs.f[1] = &sh_fonts[1]; sh_fs.f[2] = &sh_fonts[2];

    struct shape_scratch sc; sh_scratch(&sc);
    const int px = 16;
    int cell = shape_line(&sh_fs, "M", 1, px, 0, 0, NULL, &sc);
    CHK(cell > 0, "the mono advance measured %d", cell);
    if (cell <= 0) return;

    /* THE HARD ASSERTION, and it is the compatibility claim: every printable
     * ASCII character is one cell in the header AND one cell in the renderer,
     * so nothing that renders correctly today moves. */
    for (unsigned cp = 0x20; cp <= 0x7E; cp++) {
        int r = renderer_cells(cp, px, cell);
        CHK(r == 1 && lc_cp_cells(cp) == 1,
            "U+%04X: renderer %d cells, rule %d cells -- ASCII must be 1 in both",
            cp, r, lc_cp_cells(cp));
    }

    /* THE MEASUREMENT. Everything above U+00A0 that either side calls wide.
     * This is printed, not asserted: the renderer's answer depends on which
     * glyphs survived font subsetting and on the proportional fallback's
     * advances, so it is not a function of the code point and no second binary
     * can reproduce it. The number is here so that it moves when somebody fixes
     * shape.c, instead of a paragraph claiming it is small. */
    long both = 0, only_rule = 0, only_renderer = 0, agree_narrow = 0;
    unsigned first_only_rule = 0, first_only_rend = 0;
    for (unsigned cp = 0xA0; cp <= 0xFFFF; cp++) {
        if (cp >= 0xD800 && cp <= 0xDFFF) continue;
        int r = renderer_cells(cp, px, cell) == 2;
        int m = lc_wide(cp);
        if (r && m) both++;
        else if (m) { if (!only_rule) first_only_rule = cp; only_rule++; }
        else if (r) { if (!only_renderer) first_only_rend = cp; only_renderer++; }
        else agree_narrow++;
    }
    printf("  renderer vs rule over U+00A0..U+FFFF at %dpx, cell=%d:\n", px, cell);
    printf("    agree wide      %ld\n", both);
    printf("    agree narrow    %ld\n", agree_narrow);
    printf("    rule wide only  %ld  (first U+%04X -- glyph absent from the subset)\n",
           only_rule, first_only_rule);
    printf("    rend wide only  %ld  (first U+%04X -- proportional fallback advance)\n",
           only_renderer, first_only_rend);
    CHK(only_rule + only_renderer > 0,
        "the renderer and the code-point rule agree everywhere -- then the note in "
        "logit_cells.h about shape.c:1215 is stale and should be deleted");
}

/* ------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *ucd  = argc > 1 ? argv[1] : "/usr/share/unicode/EastAsianWidth.txt";
    const char *mono = argc > 2 ? argv[2] : "fsroot/fonts/mono.ttf";
    const char *ui   = argc > 3 ? argv[3] : "fsroot/fonts/ui.ttf";
    const char *text = argc > 4 ? argv[4] : "third_party/fonts/DejaVuSans.ttf";

    t_decoder();
    t_width(ucd);
    t_convert();
    t_renderer(mono, ui, text);

    printf(fails ? "SOME FAILED (%d of %d checks)\n" : "ALL PASS (%d failures, %d checks)\n",
           fails, checks);
    return fails != 0;
}
