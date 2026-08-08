/* CSS Text: the line-breaking and white-space-processing test.
 *
 * Two kinds of assertion live here and they are not equally strong.
 *
 *   1. The Unicode Consortium's own LineBreakTest.txt, run whole.  16,672
 *      cases, every one of them stating both the string and every break
 *      opportunity in it, and none of them written by us.  This is the
 *      scoreboard: an implementation of UAX #14 that passes this is right for
 *      a reason that has nothing to do with what its author believed.
 *
 *   2. Hand-written assertions about the CSS layer on top -- white-space
 *      collapsing, text-transform, spacing arithmetic, alignment, and the
 *      tailorings (word-break / overflow-wrap / line-break).  Those ARE ours,
 *      and they are stated as exact positions and exact byte strings, never as
 *      "it did not crash".
 *
 * Neither kind needs a font, a frame buffer, or a browser.  Line breaking is a
 * question about positions in a string, and the answers are checkable long
 * before a reftest harness exists.
 *
 * Usage: csstext_test [/usr/share/unicode]
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "layout_text.h"

static int checks, failures;

static void ok(int cond, const char *fmt, ...)
{
    checks++;
    if (!cond) {
        va_list ap;
        failures++;
        fputs("  FAIL ", stdout);
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
        putchar('\n');
    }
}

#define CHECK(c, ...) ok((c), __VA_ARGS__)

/* ---------------------------------------------------------------- utf-8 -- */

static int enc(uint32_t cp, char *o)
{
    if (cp < 0x80)    { o[0] = (char)cp; return 1; }
    if (cp < 0x800)   { o[0] = (char)(0xC0 | (cp >> 6));
                        o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { o[0] = (char)(0xE0 | (cp >> 12));
                        o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        o[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static int str_of(const char *s, char *out)      /* UTF-8 -> buffer, returns len */
{
    int n = (int)strlen(s);
    memcpy(out, s, (size_t)n);
    return n;
}

/* ------------------------------------------- 1. the conformance corpus ---- */

/* A line of LineBreakTest.txt is a sequence of × (no break here) and ÷ (break
 * here) alternating with hex code points, e.g.
 *     × 0023 × 0023 ÷    # ...
 * so the marks ARE the expected output, one per position including the two at
 * the ends. */
static int run_conformance(const char *ucd)
{
    char path[512];
    FILE *fh;
    char line[8192];
    int lineno = 0, cases = 0, bad = 0, shown = 0, malformed = 0;

    snprintf(path, sizeof path, "%s/auxiliary/LineBreakTest.txt", ucd);
    fh = fopen(path, "r");
    if (!fh) {
        printf("csstext_test: cannot open %s\n", path);
        printf("  (install the Unicode Character Database, or pass its path)\n");
        return -1;
    }

    while (fgets(line, sizeof line, fh)) {
        /* The corpus ends with a section of real prose samples, one of which is
         * over 250 code points long, so a 64-entry buffer silently truncates
         * the hardest cases in the file and reports a failure at exactly the
         * truncation point. */
        uint32_t cp[1024];
        unsigned char want[1025], got[1025];
        struct ltx_lbopt opt;
        char *p = line, *hash;
        int n = 0, i, expect_mark = 1, ncp;

        lineno++;
        hash = strchr(line, '#');
        if (hash) *hash = 0;

        /* Tokenise.  × is U+00D7 (0xC3 0x97), ÷ is U+00F7 (0xC3 0xB7). */
        want[0] = 255;
        while (*p) {
            if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') { p++; continue; }
            if ((unsigned char)p[0] == 0xC3 &&
                ((unsigned char)p[1] == 0x97 || (unsigned char)p[1] == 0xB7)) {
                if (!expect_mark) { malformed++; break; }
                want[n] = ((unsigned char)p[1] == 0xB7);   /* ÷ = break */
                expect_mark = 0;
                p += 2;
                continue;
            }
            if ((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'F') ||
                (*p >= 'a' && *p <= 'f')) {
                char *end;
                unsigned long v = strtoul(p, &end, 16);
                if (n >= 1023) { malformed++; p = end; continue; }
                cp[n] = (uint32_t)v;
                n++;
                expect_mark = 1;
                p = end;
                continue;
            }
            p++;
        }
        if (n == 0 || expect_mark) continue;    /* comment/blank/degenerate */
        ncp = n;

        memset(&opt, 0, sizeof opt);
        ltx_break_cp(cp, ncp, &opt, got);

        for (i = 0; i <= ncp; i++) {
            int g = (got[i] != LTX_BRK_PROHIBITED);
            if (g != (want[i] != 0)) {
                bad++;
                if (shown < 12) {
                    int k;
                    shown++;
                    printf("  FAIL line %d position %d: expected %s, got %s\n",
                           lineno, i, want[i] ? "break" : "no-break",
                           g ? "break" : "no-break");
                    printf("        ");
                    for (k = 0; k < ncp; k++)
                        printf("%s%04X(%s) ", (got[k] != LTX_BRK_PROHIBITED) ? "/" : "x",
                               cp[k], ltx_class_name(ltx_class(cp[k])));
                    printf("%s\n", (got[ncp] != LTX_BRK_PROHIBITED) ? "/" : "x");
                }
                break;
            }
        }
        cases++;
    }
    fclose(fh);

    checks += cases;
    failures += bad;
    printf("  LineBreakTest.txt: %d cases, %d failed%s\n", cases, bad,
           malformed ? " (and some lines did not parse -- corpus format changed?)"
                     : "");
    if (cases < 10000) {
        printf("  FAIL only %d cases parsed; the corpus should have ~16k\n", cases);
        failures++;
        return 1;
    }
    return bad;
}

/* -------------------------------------------- 2. break positions by hand -- */

/* Every break opportunity in `s`, as a comma-separated list of byte offsets,
 * so an expectation can be written as a literal string. */
static const char *breaks_of(const char *s, const struct ltx_lbopt *opt)
{
    static char out[512];
    unsigned char *b;
    int len = (int)strlen(s), i, o = 0;
    b = malloc((size_t)len + 2);
    ltx_break_utf8(s, len, opt, b);
    out[0] = 0;
    for (i = 1; i <= len; i++)
        if (b[i] != LTX_BRK_PROHIBITED)
            o += snprintf(out + o, sizeof out - (size_t)o, "%s%d",
                          o ? "," : "", i);
    free(b);
    return out;
}

static void test_breaks(void)
{
    struct ltx_lbopt opt;
    memset(&opt, 0, sizeof opt);

    /* English prose: after the spaces, and at the end. */
    CHECK(!strcmp(breaks_of("one two", &opt), "4,7"),
          "english breaks: got %s want 4,7", breaks_of("one two", &opt));

    /* THE CASE THE NAIVE IMPLEMENTATION GETS WRONG.  Four Han ideographs, no
     * space anywhere.  UAX #14 gives an opportunity between each pair (LB31,
     * ID ÷ ID); a space-splitter gives exactly one, at the end, and then has
     * to chop the "word" at an arbitrary column. */
    CHECK(!strcmp(breaks_of("中文测试", &opt), "3,6,9,12"),
          "CJK breaks between ideographs: got %s want 3,6,9,12",
          breaks_of("中文测试", &opt));

    /* ...but NOT before a closing bracket or a full stop.  This is the other
     * half of the same point: "break anywhere in CJK" is as wrong as "never".
     * U+3002 IDEOGRAPHIC FULL STOP is CL, U+300D RIGHT CORNER BRACKET is CL. */
    CHECK(!strcmp(breaks_of("中文。", &opt), "3,9"),
          "no break before an ideographic full stop: got %s want 3,9",
          breaks_of("中文。", &opt));

    /* Hiragana small kana is a nonstarter: no break before it (LB21 × NS). */
    CHECK(!strcmp(breaks_of("キャ", &opt), "6"),
          "no break before small ya: got %s want 6",
          breaks_of("キャ", &opt));

    /* A hyphenated word breaks after the hyphen but not before it. */
    CHECK(!strcmp(breaks_of("e-mail", &opt), "2,6"),
          "hyphen: got %s want 2,6", breaks_of("e-mail", &opt));

    /* A number does not break at its decimal point or its thousands comma. */
    CHECK(!strcmp(breaks_of("1,234.5", &opt), "7"),
          "number stays whole: got %s want 7", breaks_of("1,234.5", &opt));

    /* A URL breaks after a '/' but NOT inside the host name (the dot is IS and
     * LB15d forbids breaking before it, LB29 after it) -- which is exactly why
     * pages have to set overflow-wrap on long links instead of hoping. */
    CHECK(!strcmp(breaks_of("http://a.example/b/c", &opt), "7,17,19,20"),
          "url breaks only after its slashes: got %s want 7,17,19,20",
          breaks_of("http://a.example/b/c", &opt));

    /* A soft hyphen is a break opportunity; `hyphens: none` removes it. */
    CHECK(!strcmp(breaks_of("ab­cd", &opt), "4,6"),
          "soft hyphen breaks: got %s want 4,6", breaks_of("ab­cd", &opt));
    opt.hyphens_none = 1;
    CHECK(!strcmp(breaks_of("ab­cd", &opt), "6"),
          "hyphens:none suppresses the soft hyphen: got %s want 6",
          breaks_of("ab­cd", &opt));
    opt.hyphens_none = 0;

    /* A no-break space glues both sides (LB12 / LB12a). */
    CHECK(!strcmp(breaks_of("a b", &opt), "4"),
          "nbsp glues: got %s want 4", breaks_of("a b", &opt));

    /* ZWSP is the manual override: it creates an opportunity inside a word. */
    CHECK(!strcmp(breaks_of("ab​cd", &opt), "5,7"),
          "zwsp breaks: got %s want 5,7", breaks_of("ab​cd", &opt));

    /* --- tailorings --- */
    opt.word_break = LTX_WB_BREAK_ALL;
    CHECK(!strcmp(breaks_of("abc", &opt), "1,2,3"),
          "word-break:break-all breaks between letters: got %s want 1,2,3",
          breaks_of("abc", &opt));
    CHECK(!strcmp(breaks_of("a b", &opt), "2,3"),
          "break-all still refuses to break before a space: got %s want 2,3",
          breaks_of("a b", &opt));
    opt.word_break = 0;

    opt.word_break = LTX_WB_KEEP_ALL;
    CHECK(!strcmp(breaks_of("中文测试", &opt), "12"),
          "word-break:keep-all keeps CJK together: got %s want 12",
          breaks_of("中文测试", &opt));
    CHECK(!strcmp(breaks_of("one two", &opt), "4,7"),
          "keep-all leaves latin alone: got %s want 4,7",
          breaks_of("one two", &opt));
    opt.word_break = 0;

    opt.line_break = LTX_LB_ANYWHERE;
    CHECK(!strcmp(breaks_of("a b", &opt), "1,2,3"),
          "line-break:anywhere breaks around spaces too: got %s want 1,2,3",
          breaks_of("a b", &opt));
    opt.line_break = 0;

    /* line-break: loose lets a small kana start a line (CJ resolves to ID). */
    opt.line_break = LTX_LB_LOOSE;
    CHECK(!strcmp(breaks_of("キャ", &opt), "3,6"),
          "line-break:loose allows a break before small ya: got %s want 3,6",
          breaks_of("キャ", &opt));
    opt.line_break = 0;
}

/* ------------------------------------------- 3. white-space processing ---- */

static const char *collapse(const char *in, int wsc)
{
    static char out[512];
    struct ltx_wsstate st;
    int n;
    memset(&st, 0, sizeof st);
    n = ltx_collapse(in, (int)strlen(in), wsc, &st, out, sizeof out - 1);
    if (n < 0) return "<overflow>";
    out[n] = 0;
    return out;
}

static void test_whitespace(void)
{
    /* Leading and trailing collapsible spaces are both gone: the leading one
     * because a line does not start with one, the trailing one because it is
     * left OWED in the state and only materialises if another run follows. */
    CHECK(!strcmp(collapse("  a   b  ", LTX_WSC_COLLAPSE), "a b"),
          "collapse: got '%s' want 'a b'", collapse("  a   b  ", LTX_WSC_COLLAPSE));
    CHECK(!strcmp(collapse("a\n\nb", LTX_WSC_COLLAPSE), "a b"),
          "segment breaks collapse to one space: got '%s'",
          collapse("a\n\nb", LTX_WSC_COLLAPSE));
    CHECK(!strcmp(collapse("a \n b", LTX_WSC_COLLAPSE), "a b"),
          "a segment break absorbs the spaces around it: got '%s'",
          collapse("a \n b", LTX_WSC_COLLAPSE));
    CHECK(!strcmp(collapse("a\tb", LTX_WSC_COLLAPSE), "a b"),
          "tab collapses to a space: got '%s'", collapse("a\tb", LTX_WSC_COLLAPSE));
    CHECK(!strcmp(collapse("a\r\nb", LTX_WSC_COLLAPSE), "a b"),
          "CRLF is one segment break: got '%s'", collapse("a\r\nb", LTX_WSC_COLLAPSE));

    CHECK(!strcmp(collapse("  a   b  ", LTX_WSC_PRESERVE), "  a   b  "),
          "pre preserves everything: got '%s'",
          collapse("  a   b  ", LTX_WSC_PRESERVE));
    CHECK(!strcmp(collapse("a\r\nb", LTX_WSC_PRESERVE), "a\nb"),
          "pre normalises CRLF to LF: got '%s'",
          collapse("a\r\nb", LTX_WSC_PRESERVE));

    CHECK(!strcmp(collapse("  a  \n  b ", LTX_WSC_PRESERVE_BREAKS), "a\nb"),
          "pre-line keeps breaks, collapses spaces: got '%s'",
          collapse("  a  \n  b ", LTX_WSC_PRESERVE_BREAKS));

    /* U+200B on either side of a segment break removes the break entirely
     * (CSS Text 3 §4.1.2) -- the one part of the segment-break transformation
     * every browser does implement. */
    CHECK(!strcmp(collapse("a​\nb", LTX_WSC_COLLAPSE), "a​b"),
          "zwsp swallows the segment break: got '%s'",
          collapse("a​\nb", LTX_WSC_COLLAPSE));

    /* The East Asian rule, off by default and on when asked. */
    {
        static char out[64];
        struct ltx_wsstate st;
        int n;
        memset(&st, 0, sizeof st);
        n = ltx_collapse("中\n文", 7, LTX_WSC_COLLAPSE, &st, out, sizeof out);
        out[n] = 0;
        CHECK(!strcmp(out, "中 文"),
              "default: CJK segment break becomes a space: got '%s'", out);
        memset(&st, 0, sizeof st);
        st.ea_segbreak = 1;
        n = ltx_collapse("中\n文", 7, LTX_WSC_COLLAPSE, &st, out, sizeof out);
        out[n] = 0;
        CHECK(!strcmp(out, "中文"),
              "ea_segbreak: CJK segment break is removed: got '%s'", out);
    }

    /* Collapsing crosses a run boundary: "<b>a </b>b" is "a b", one space. */
    {
        static char out[64];
        struct ltx_wsstate st;
        int n, m;
        memset(&st, 0, sizeof st);
        n = ltx_collapse("a ", 2, LTX_WSC_COLLAPSE, &st, out, sizeof out);
        m = ltx_collapse(" b", 2, LTX_WSC_COLLAPSE, &st, out + n, (int)sizeof out - n);
        out[n + m] = 0;
        CHECK(!strcmp(out, "a b"),
              "collapse across a run boundary: got '%s' want 'a b'", out);
    }

    /* Trailing white space hangs at the end of a line -- except in
     * `break-spaces`, where it is real. */
    CHECK(ltx_trim_end("ab   ", 5, LTX_WSC_COLLAPSE) == 2, "trim_end collapse");
    CHECK(ltx_trim_end("ab   ", 5, LTX_WSC_BREAK_SPACES) == 5,
          "break-spaces does not hang its trailing spaces");

    /* The shorthand expands the way the longhands say it does. */
    {
        int c, w;
        ltx_white_space(LTX_WS_PRE_WRAP, &c, &w);
        CHECK(c == LTX_WSC_PRESERVE && w == LTX_WRAP_WRAP, "pre-wrap expansion");
        ltx_white_space(LTX_WS_NOWRAP, &c, &w);
        CHECK(c == LTX_WSC_COLLAPSE && w == LTX_WRAP_NOWRAP, "nowrap expansion");
        ltx_white_space(LTX_WS_PRE_LINE, &c, &w);
        CHECK(c == LTX_WSC_PRESERVE_BREAKS && w == LTX_WRAP_WRAP, "pre-line expansion");
    }
}

/* --------------------------------------------------- 4. text-transform ---- */

static const char *xform(const char *in, int tt)
{
    static char out[256];
    int ws = 1, n = ltx_text_transform(in, (int)strlen(in), tt, &ws, out,
                                       sizeof out - 1);
    if (n < 0) return "<overflow>";
    out[n] = 0;
    return out;
}

static void test_transform(void)
{
    CHECK(!strcmp(xform("hello world", LTX_TT_UPPERCASE), "HELLO WORLD"),
          "uppercase ascii: got '%s'", xform("hello world", LTX_TT_UPPERCASE));
    CHECK(!strcmp(xform("Hello World", LTX_TT_LOWERCASE), "hello world"),
          "lowercase ascii: got '%s'", xform("Hello World", LTX_TT_LOWERCASE));
    CHECK(!strcmp(xform("hello world", LTX_TT_CAPITALIZE), "Hello World"),
          "capitalize: got '%s'", xform("hello world", LTX_TT_CAPITALIZE));
    CHECK(!strcmp(xform("o'clock", LTX_TT_CAPITALIZE), "O'clock"),
          "capitalize does not restart at an apostrophe: got '%s'",
          xform("o'clock", LTX_TT_CAPITALIZE));

    /* Non-ASCII is the reason this needs a Unicode table and not toupper(). */
    CHECK(!strcmp(xform("été", LTX_TT_UPPERCASE), "ÉTÉ"),
          "uppercase accented latin: got '%s'",
          xform("été", LTX_TT_UPPERCASE));
    CHECK(!strcmp(xform("тест", LTX_TT_UPPERCASE),
                  "ТЕСТ"),
          "uppercase cyrillic: got '%s'",
          xform("тест", LTX_TT_UPPERCASE));
    /* One code point becoming two: the full mapping from SpecialCasing.txt. */
    CHECK(!strcmp(xform("straße", LTX_TT_UPPERCASE), "STRASSE"),
          "uppercase eszett is two letters: got '%s'",
          xform("straße", LTX_TT_UPPERCASE));
    /* Greek final sigma is positional, so we can and do get it right. */
    CHECK(!strcmp(xform("ΟΔΟΣ", LTX_TT_LOWERCASE),
                  "οδος"),
          "final sigma: got '%s'",
          xform("ΟΔΟΣ", LTX_TT_LOWERCASE));

    CHECK(!strcmp(xform("AB1", LTX_TT_FULL_WIDTH), "ＡＢ１"),
          "full-width: got '%s'", xform("AB1", LTX_TT_FULL_WIDTH));
    CHECK(!strcmp(xform("キャ", LTX_TT_FULL_SIZE_KANA), "キヤ"),
          "full-size-kana: got '%s'", xform("キャ", LTX_TT_FULL_SIZE_KANA));
    CHECK(!strcmp(xform("abc", LTX_TT_NONE), "abc"), "none is identity");
}

/* ------------------------------------------------ 5. the measure + lines -- */

/* A stub font: 10px per narrow character, 20px per East Asian one.  Exact by
 * construction, which is the point -- every width below is arithmetic, not a
 * rendering. */
static int stub_measure(void *ctx, const char *s, int len,
                        const struct ltx_style *st)
{
    int i = 0, w = 0;
    (void)ctx; (void)st;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80)            { i += 1; w += 10; }
        else if ((c & 0xE0) == 0xC0) { i += 2; w += 10; }
        else if ((c & 0xF0) == 0xE0) { i += 3; w += 20; }
        else                     { i += 4; w += 20; }
    }
    return w;
}

static void base_style(struct ltx_style *st)
{
    memset(st, 0, sizeof *st);
    st->font_px = 16;
    st->line_px = 20;
    st->tab_size = 8;
    st->wsc = LTX_WSC_COLLAPSE;
    st->wrap = LTX_WRAP_WRAP;
    st->hyphens = LTX_HY_MANUAL;
}

static void base_env(struct ltx_env *e, int avail)
{
    memset(e, 0, sizeof *e);
    e->measure = stub_measure;
    e->avail = avail;
    e->align = LTX_ALIGN_START;
}

static void test_measure(void)
{
    struct ltx_env e;
    struct ltx_style st;
    base_env(&e, 1000);
    base_style(&st);

    CHECK(ltx_measure_run(&e, &st, "ab cd", 5) == 50, "plain advance");
    st.letter_spacing = 2;
    CHECK(ltx_measure_run(&e, &st, "ab cd", 5) == 60,
          "letter-spacing adds once per character unit (got %d want 60)",
          ltx_measure_run(&e, &st, "ab cd", 5));
    st.word_spacing = 5;
    CHECK(ltx_measure_run(&e, &st, "ab cd", 5) == 65,
          "word-spacing adds once per U+0020 (got %d want 65)",
          ltx_measure_run(&e, &st, "ab cd", 5));
    st.letter_spacing = 0; st.word_spacing = 0;
    /* A code point is the unit, not a byte: three-byte CJK counts once. */
    st.letter_spacing = 3;
    CHECK(ltx_measure_run(&e, &st, "中文", 6) == 46,
          "letter-spacing counts code points not bytes (got %d want 46)",
          ltx_measure_run(&e, &st, "中文", 6));
}

static int lay(const char *text, struct ltx_style *st, struct ltx_env *e,
               struct ltx_layout *out)
{
    struct ltx_run run;
    memset(&run, 0, sizeof run);
    run.text = text; run.len = (int)strlen(text); run.style = st;
    return ltx_layout_runs(&run, 1, e, out);
}

static void test_lines(void)
{
    struct ltx_env e;
    struct ltx_style st;
    struct ltx_layout l;

    base_env(&e, 100);
    base_style(&st);

    /* "aaa bbb ccc": words are 30px, spaces 10px.  100px holds "aaa bbb"
     * (70px) but not "aaa bbb ccc" (110px). */
    CHECK(lay("aaa bbb ccc", &st, &e, &l) == 0, "layout ok");
    CHECK(l.nline == 2, "two lines for 110px of text in 100px (got %d)", l.nline);
    if (l.nline == 2) {
        CHECK(l.lines[0].w == 70, "first line is 70px (got %d)", l.lines[0].w);
        CHECK(l.lines[1].w == 30, "second line is 30px (got %d)", l.lines[1].w);
        CHECK(l.lines[1].y == 20, "second line sits one line-height down (got %d)",
              l.lines[1].y);
    }
    ltx_layout_free(&l);

    /* THE CJK CASE.  Eight ideographs at 20px each is 160px; there is no space
     * anywhere in it.  A correct breaker puts five on the first line (100px)
     * and three on the second.  A space-splitter puts all eight on one line
     * and overflows the box by 60px -- which is exactly what the browser does
     * today, and exactly what nobody notices until they open a Chinese page. */
    CHECK(lay("中文测试中文测试",
              &st, &e, &l) == 0, "cjk layout ok");
    CHECK(l.nline == 2, "CJK wraps without spaces (got %d lines)", l.nline);
    if (l.nline >= 1)
        CHECK(l.lines[0].w == 100,
              "first CJK line fills the measure exactly (got %d want 100)",
              l.lines[0].w);
    CHECK(l.width <= 100, "no line overflows the measure (widest %d)", l.width);
    ltx_layout_free(&l);

    /* nowrap: one line, overflowing, by request. */
    st.wrap = LTX_WRAP_NOWRAP;
    CHECK(lay("aaa bbb ccc", &st, &e, &l) == 0, "nowrap layout ok");
    CHECK(l.nline == 1, "nowrap gives one line (got %d)", l.nline);
    ltx_layout_free(&l);
    st.wrap = LTX_WRAP_WRAP;

    /* A word longer than the measure overflows unless overflow-wrap says
     * otherwise -- that IS the CSS behaviour, and the difference between the
     * two is the whole reason pages set the property. */
    CHECK(lay("aaaaaaaaaaaaaaaa", &st, &e, &l) == 0, "long word layout ok");
    CHECK(l.nline == 1 && l.lines[0].w == 160,
          "an over-long word overflows by default (lines %d width %d)",
          l.nline, l.nline ? l.lines[0].w : -1);
    ltx_layout_free(&l);

    st.overflow_wrap = LTX_OW_ANYWHERE;
    CHECK(lay("aaaaaaaaaaaaaaaa", &st, &e, &l) == 0, "overflow-wrap layout ok");
    CHECK(l.nline == 2 && l.lines[0].w == 100,
          "overflow-wrap:anywhere splits it at the measure (lines %d width %d)",
          l.nline, l.nline ? l.lines[0].w : -1);
    ltx_layout_free(&l);
    st.overflow_wrap = 0;

    /* Preserved newline forces a break even inside a narrow-enough line. */
    st.wsc = LTX_WSC_PRESERVE;
    CHECK(lay("a\nb", &st, &e, &l) == 0, "pre layout ok");
    CHECK(l.nline == 2, "a preserved newline forces a line (got %d)", l.nline);
    ltx_layout_free(&l);

    /* Tab stops: tab-size 8 with a 10px space is a 80px stop. */
    CHECK(lay("a\tb", &st, &e, &l) == 0, "tab layout ok");
    if (l.nfrag >= 2)
        CHECK(l.frags[1].x == 80, "tab advances to the 80px stop (got %d)",
              l.frags[1].x);
    else
        CHECK(0, "tab produced %d fragments", l.nfrag);
    ltx_layout_free(&l);

    /* A tab at the START of a line -- every indented line of every <pre> --
     * must advance the pen, not be handed to the font.  Two tabs in a row
     * reach the second stop rather than the first twice. */
    CHECK(lay("\tb", &st, &e, &l) == 0, "leading tab ok");
    CHECK(l.nfrag == 1 && l.frags[0].x == 80 && l.frags[0].len == 1,
          "a leading tab advances and is not drawn (frags=%d x=%d len=%d)",
          l.nfrag, l.nfrag ? l.frags[0].x : -1, l.nfrag ? l.frags[0].len : -1);
    ltx_layout_free(&l);
    e.avail = 400;
    CHECK(lay("\t\tb", &st, &e, &l) == 0, "double tab ok");
    CHECK(l.nfrag == 1 && l.frags[0].x == 160,
          "two tabs reach the second stop (x=%d want 160)",
          l.nfrag ? l.frags[0].x : -1);
    ltx_layout_free(&l);
    e.avail = 100;

    /* tab-size in px rather than in space advances. */
    st.tab_px = 1; st.tab_size = 25;
    CHECK(lay("a\tb", &st, &e, &l) == 0, "px tab layout ok");
    if (l.nfrag >= 2)
        CHECK(l.frags[1].x == 25, "tab-size in px (got %d want 25)",
              l.frags[1].x);
    else
        CHECK(0, "px tab produced %d fragments", l.nfrag);
    ltx_layout_free(&l);
    st.tab_px = 0; st.tab_size = 8;
    st.wsc = LTX_WSC_COLLAPSE;

    /* Alignment moves the whole line and nothing else. */
    e.align = LTX_ALIGN_RIGHT;
    CHECK(lay("aaa", &st, &e, &l) == 0, "right align ok");
    CHECK(l.nfrag == 1 && l.frags[0].x == 70,
          "text-align:right pushes the line to the margin (x=%d want 70)",
          l.nfrag ? l.frags[0].x : -1);
    ltx_layout_free(&l);

    e.align = LTX_ALIGN_CENTER;
    CHECK(lay("aaa", &st, &e, &l) == 0, "center align ok");
    CHECK(l.nfrag == 1 && l.frags[0].x == 35,
          "text-align:center halves the slack (x=%d want 35)",
          l.nfrag ? l.frags[0].x : -1);
    ltx_layout_free(&l);

    /* Justify: the first line stretches to the margin, the last does not. */
    e.align = LTX_ALIGN_JUSTIFY;
    CHECK(lay("aa bb cc dd", &st, &e, &l) == 0, "justify ok");
    if (l.nline >= 1) {
        CHECK(l.lines[0].w == 100,
              "a justified line fills the measure exactly (got %d)", l.lines[0].w);
        CHECK(l.lines[l.nline - 1].w < 100 || l.nline == 1,
              "the last line is not justified (got %d)", l.lines[l.nline - 1].w);
        /* and the rightmost fragment ends exactly on the margin */
        {
            const struct ltx_frag *f = &l.frags[l.lines[0].frag0 +
                                                l.lines[0].nfrag - 1];
            CHECK(f->x + f->w == 100,
                  "justify lands the last fragment on the margin (%d)", f->x + f->w);
        }
    }
    ltx_layout_free(&l);
    e.align = LTX_ALIGN_START;

    /* JUSTIFYING CHINESE.  There is no space in the line to stretch, so an
     * inter-word implementation leaves the right edge ragged on a block that
     * asked for a flush one -- and reports success.  `text-justify: auto` has
     * to fall back to distributing between characters. */
    e.align = LTX_ALIGN_JUSTIFY;
    e.avail = 110;
    CHECK(lay("中文测试中文", &st, &e, &l) == 0, "cjk justify ok");
    if (l.nline >= 2) {
        CHECK(l.lines[0].w == 110,
              "a justified CJK line reaches the margin (got %d want 110)",
              l.lines[0].w);
        CHECK(l.lines[0].nfrag == 5,
              "inter-character justify splits per ideograph (got %d frags)",
              l.lines[0].nfrag);
    } else {
        CHECK(0, "cjk justify produced %d lines", l.nline);
    }
    ltx_layout_free(&l);
    /* ...and text-justify:none turns it off again. */
    e.justify = LTX_TJ_NONE;
    CHECK(lay("中文测试中文", &st, &e, &l) == 0, "cjk justify:none ok");
    CHECK(l.nline < 1 || l.lines[0].w == 100,
          "text-justify:none leaves the line unstretched (got %d want 100)",
          l.nline ? l.lines[0].w : -1);
    ltx_layout_free(&l);
    e.justify = LTX_TJ_AUTO;
    e.align = LTX_ALIGN_START;
    e.avail = 100;

    /* text-align-last overrides what the last line does. */
    e.align = LTX_ALIGN_JUSTIFY;
    e.align_last = LTX_ALAST_RIGHT;
    CHECK(lay("aa bb cc dd", &st, &e, &l) == 0, "align-last ok");
    if (l.nline >= 2) {
        const struct ltx_frag *f = &l.frags[l.nfrag - 1];
        CHECK(f->x + f->w == 100,
              "text-align-last:right puts the last line on the margin (%d)",
              f->x + f->w);
    }
    ltx_layout_free(&l);
    e.align = LTX_ALIGN_START; e.align_last = LTX_ALAST_AUTO;

    /* white-space: break-spaces -- the preserved spaces are real, they take
     * width, they do not hang, and a run of them may be split across lines.
     * `pre-wrap` differs on every one of those points, which is why the value
     * exists. */
    st.wsc = LTX_WSC_BREAK_SPACES;
    e.avail = 30;
    CHECK(lay("a    b", &st, &e, &l) == 0, "break-spaces ok");
    CHECK(l.nline >= 2, "break-spaces splits a run of spaces (got %d lines)",
          l.nline);
    if (l.nline >= 1)
        CHECK(l.lines[0].w == 30,
              "break-spaces measures its trailing spaces (got %d want 30)",
              l.lines[0].w);
    ltx_layout_free(&l);
    st.wsc = LTX_WSC_COLLAPSE;
    e.avail = 100;

    /* A soft hyphen breaks the word AND has to leave a visible mark, because
     * U+00AD itself paints nothing. */
    e.avail = 60;
    CHECK(lay("aaaaa\xC2\xAD" "bbbbb", &st, &e, &l) == 0, "soft hyphen ok");
    CHECK(l.nline == 2, "soft hyphen breaks the word (got %d lines)", l.nline);
    if (l.nline == 2)
        CHECK(l.lines[0].hyphen == 1 && l.lines[1].hyphen == 0,
              "the broken line asks for a hyphen (%d,%d)",
              l.lines[0].hyphen, l.lines[1].hyphen);
    ltx_layout_free(&l);
    e.avail = 100;

    /* text-indent shifts the first line only. */
    e.indent = 20;
    CHECK(lay("aa bb", &st, &e, &l) == 0, "indent ok");
    CHECK(l.nfrag >= 1 && l.frags[0].x == 20,
          "text-indent moves the first line (x=%d want 20)",
          l.nfrag ? l.frags[0].x : -1);
    ltx_layout_free(&l);

    e.indent_hanging = 1;
    CHECK(lay("aaa bbb ccc", &st, &e, &l) == 0, "hanging indent ok");
    if (l.nline == 2)
        CHECK(l.frags[0].x == 0 && l.lines[1].x == 20,
              "hanging indent skips the first line (x0=%d x1=%d)",
              l.frags[0].x, l.lines[1].x);
    ltx_layout_free(&l);
    e.indent = 0; e.indent_hanging = 0;

    /* Two runs, one inline formatting context: the break opportunity between
     * them is real, and the collapse crosses the boundary. */
    {
        struct ltx_style s2;
        struct ltx_run rr[2];
        base_style(&s2);
        memset(rr, 0, sizeof rr);
        rr[0].text = "aaa "; rr[0].len = 4; rr[0].style = &st;
        rr[1].text = "bbb ccc"; rr[1].len = 7; rr[1].style = &s2;
        CHECK(ltx_layout_runs(rr, 2, &e, &l) == 0, "two-run layout ok");
        CHECK(l.nline == 2, "two runs wrap as one paragraph (got %d)", l.nline);
        CHECK(!strncmp(l.text, "aaa bbb ccc", 11),
              "the processed text is the two runs joined: '%.*s'",
              l.text_len, l.text);
        ltx_layout_free(&l);
    }
}

/* ------------------------------------------------------- 6. the fuzz ----- */

/* Every string this module sees comes off the network, most of it from people
 * who did not intend it to be well formed.  The corpus above is all VALID
 * UTF-8 by construction, so it never once asks what happens to a truncated
 * three-byte sequence, a lone continuation byte, or a 200 KB run of U+00AD.
 *
 * Deterministic on purpose (xorshift, fixed seed): a fuzz that finds a crash
 * on Tuesday and cannot reproduce it on Wednesday is a rumour.  Run it under
 * -fsanitize=address,undefined -- `make test-csstext-asan` -- where "it did
 * not crash" stops being a weak assertion, because the sanitiser is checking
 * every access rather than waiting for one to land somewhere fatal. */
static uint32_t rng_state = 0x1234567u;
static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void test_fuzz(int iters)
{
    static const char *seeds[] = {
        "", " ", "\n", "\t\t\t", "\xC2", "\xE4\xB8", "\x80\x80", "\xF0\x9F",
        "a", "中", "\xC2\xAD", "\xE2\x80\x8B", "\xEF\xBB\xBF", "\xE2\x80\xA8",
        "\r\n", "-", "1.5", "\"", "\xC2\xA0", "\xF0\x9F\x87\xA6",
    };
    struct ltx_env e;
    struct ltx_style st;
    struct ltx_layout l;
    char buf[512];
    int it;

    base_env(&e, 0);
    for (it = 0; it < iters; it++) {
        int n = 0, k, pieces = (int)(rnd() % 24);
        unsigned char *b;
        struct ltx_lbopt opt;

        for (k = 0; k < pieces; k++) {
            const char *s;
            int sl;
            if (rnd() & 3) {
                s = seeds[rnd() % (sizeof seeds / sizeof seeds[0])];
                sl = (int)strlen(s);
            } else {
                static char one[2];
                one[0] = (char)(rnd() & 0xFF); one[1] = 0;
                s = one; sl = 1;
            }
            if (n + sl >= (int)sizeof buf - 1) break;
            memcpy(buf + n, s, (size_t)sl);
            n += sl;
        }
        buf[n] = 0;

        memset(&opt, 0, sizeof opt);
        opt.line_break    = (unsigned char)(rnd() % 5);
        opt.word_break    = (unsigned char)(rnd() % 4);
        opt.overflow_wrap = (unsigned char)(rnd() % 3);
        opt.ai_is_id      = (unsigned char)(rnd() & 1);
        opt.hyphens_none  = (unsigned char)(rnd() & 1);

        b = malloc((size_t)n + 2);
        ltx_break_utf8(buf, n, &opt, b);
        free(b);

        {   /* collapse in every mode, into a buffer that is exactly big enough */
            char outb[512];
            struct ltx_wsstate wss;
            int mode;
            for (mode = LTX_WSC_COLLAPSE; mode <= LTX_WSC_BREAK_SPACES; mode++) {
                memset(&wss, 0, sizeof wss);
                wss.ea_segbreak = (unsigned char)(rnd() & 1);
                ltx_collapse(buf, n, mode, &wss, outb, (int)sizeof outb);
            }
        }
        {   /* transform, where the output can be three times the input */
            char outb[2048];
            int tt, wstart = 1;
            for (tt = LTX_TT_NONE; tt <= LTX_TT_FULL_SIZE_KANA; tt++)
                ltx_text_transform(buf, n, tt, &wstart, outb, (int)sizeof outb);
        }

        base_style(&st);
        st.wsc            = (unsigned char)(rnd() % 5);
        st.wrap           = (unsigned char)(rnd() % 5);
        st.word_break     = opt.word_break;
        st.overflow_wrap  = opt.overflow_wrap;
        st.line_break     = opt.line_break;
        st.text_transform = (unsigned char)(rnd() % 6);
        st.letter_spacing = (int)(rnd() % 7) - 3;
        st.word_spacing   = (int)(rnd() % 7) - 3;
        st.tab_size       = (int)(rnd() % 9);
        st.tab_px         = (unsigned char)(rnd() & 1);
        e.avail           = (int)(rnd() % 140) - 4;   /* including 0 and negative */
        e.align           = (unsigned char)(rnd() % 6);
        e.align_last      = (unsigned char)(rnd() % 7);
        e.justify         = (unsigned char)(rnd() % 4);
        e.indent          = (int)(rnd() % 40) - 10;
        e.indent_each     = (unsigned char)(rnd() & 1);
        e.indent_hanging  = (unsigned char)(rnd() & 1);
        e.rtl             = (unsigned char)(rnd() & 1);

        if (lay(buf, &st, &e, &l) == 0) {
            int f;
            /* Not just "it returned": every fragment must point INSIDE the
             * text this module owns, or a painter would read past it. */
            for (f = 0; f < l.nfrag; f++) {
                if (l.frags[f].off < 0 || l.frags[f].len < 0 ||
                    l.frags[f].off + l.frags[f].len > l.text_len) {
                    CHECK(0, "fuzz iter %d: fragment %d out of range (%d+%d "
                             "of %d)", it, f, l.frags[f].off, l.frags[f].len,
                          l.text_len);
                    break;
                }
            }
            for (f = 0; f < l.nline; f++) {
                if (l.lines[f].frag0 < 0 ||
                    l.lines[f].frag0 + l.lines[f].nfrag > l.nfrag) {
                    CHECK(0, "fuzz iter %d: line %d spans bad fragments", it, f);
                    break;
                }
            }
            ltx_layout_free(&l);
        }
    }
    checks++;   /* one check for "the whole sweep completed" */
}

/* --------------------------------------------------------------- main ---- */

int main(int argc, char **argv)
{
    const char *ucd = argc > 1 ? argv[1] : "/usr/share/unicode";
    int conf;

    printf("csstext_test: CSS Text -- UAX #14 line breaking + white-space\n");
    conf = run_conformance(ucd);
    test_breaks();
    test_whitespace();
    test_transform();
    test_measure();
    test_lines();
    test_fuzz(argc > 2 ? atoi(argv[2]) : 4000);

    printf("  %d checks, %d failures\n", checks, failures);
    if (conf < 0) {
        printf("csstext_test: FAIL (no conformance corpus -- the scoreboard is\n"
               "  the point, so a run without it is not a pass)\n");
        return 1;
    }
    if (failures) { printf("csstext_test: FAIL\n"); return 1; }
    printf("csstext_test: ALL PASS\n");
    return 0;
}
