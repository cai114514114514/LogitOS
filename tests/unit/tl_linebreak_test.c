/* tl_linebreak_test.c -- c/lib/text/linebreak.c (tl_break_utf8).
 *
 *   tl_linebreak_test            hand-written exact cases + differential
 *                                against the browser's ltx_break_utf8 path
 *   tl_linebreak_test <UCD>      the whole LineBreakTest.txt corpus only
 *
 * Expected values are written as the text with a marker byte at every break
 * opportunity: A = allowed break before the next character, M = mandatory.
 * The break at the very end of the text is always mandatory (LB3) and is
 * checked separately, not written.
 *
 * -DTL_LINEBREAK_NEGCTL (linebreak.c) allows a break after every code point.
 * The exact positions below must reject that, and the corpus must too. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "linebreak.h"
#include "layout/text.h"

#define A "\x01"
#define M "\x02"

static int failures, checks;

struct hcase {
    const char *name;
    const char *exp;     /* text with A/M markers */
    int flags;
};

static const struct hcase hand[] = {
    /* spaces and hyphens */
    { "words",            "hello " A "world", 0 },
    { "three words",      "a " A "b " A "c", 0 },
    { "double space",     "a  " A "b", 0 },
    { "hyphen",           "well-" A "known", 0 },
    { "soft hyphen",      "co\xc2\xad" A "op", 0 },
    { "zwsp",             "a\xe2\x80\x8b" A "b", 0 },
    { "nbsp glues",       "a\xc2\xa0" "b", 0 },
    { "word joiner glues","a\xe2\x81\xa0" "b", 0 },
    { "no spaces",        "abcdef", 0 },
    /* mandatory */
    { "lf",               "ab\n" M "cd", 0 },
    { "crlf",             "a\r\n" M "b", 0 },
    { "cr",               "a\r" M "b", 0 },
    { "line separator",   "a\xe2\x80\xa8" M "b", 0 },
    { "vertical tab",     "a\x0b" M "b", 0 },
    { "nel",              "a\xc2\x85" M "b", 0 },
    { "trailing lf",      "a\n", 0 },
    { "lf after space",   "a \n" M "b", 0 },
    /* CJK */
    { "han ideographs",   "\xe6\x88\x91" A "\xe4\xbb\xac" A "\xe4\xbd\xbf" A "\xe7\x94\xa8", 0 },
    { "closing punct",    "\xe4\xbd\xa0" A "\xe5\xa5\xbd\xe3\x80\x82" A "\xe6\x88\x91", 0 },
    { "corner brackets",  "\xe6\x88\x91" A "\xe3\x80\x8c\xe4\xbd\xa0\xe3\x80\x8d" A "\xe5\xa5\xbd", 0 },
    { "latin then han",   "ab" A "\xe4\xbd\xa0", 0 },
    { "han then latin",   "\xe4\xbd\xa0" A "ab", 0 },
    { "hangul syllables", "\xed\x95\x9c" A "\xea\xb8\x80", 0 },
    { "thai no breaks",   "\xe0\xb8\xaa\xe0\xb8\xa7\xe0\xb8\xb1\xe0\xb8\xaa", 0 },
    /* emoji, regional indicators, combining marks */
    { "emoji zwj seq",    "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7", 0 },
    { "emoji zwj amid",   "a" A "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9" A "b", 0 },
    { "two flags",        "\xf0\x9f\x87\xaf\xf0\x9f\x87\xb5" A "\xf0\x9f\x87\xba\xf0\x9f\x87\xb8", 0 },
    { "odd regional",     "\xf0\x9f\x87\xaf\xf0\x9f\x87\xb5" A "\xf0\x9f\x87\xba", 0 },
    { "combining acute",  "e\xcc\x81" "x", 0 },
    { "combining then sp","e\xcc\x81 " A "x", 0 },
    { "combining after sp","a " A "\xcc\x81" "b", 0 },
    /* numbers, punctuation, brackets */
    { "decimal",          "3.14", 0 },
    { "percent",          "10%", 0 },
    { "currency",         "$10", 0 },
    { "number in prose",  "a " A "1,000 " A "b", 0 },
    { "exclamation",      "Hi! " A "you", 0 },
    { "parens",           "foo " A "(bar)", 0 },
    { "open paren glues", "a(b", 0 },
    /* line-break flags */
    { "small kana normal","\xe3\x81\x82\xe3\x81\x81", TL_LB_NORMAL },
    { "small kana strict","\xe3\x81\x82\xe3\x81\x81", TL_LB_STRICT },
    { "small kana loose", "\xe3\x81\x82" A "\xe3\x81\x81", TL_LB_LOOSE },
    { "prolonged normal", "\xe3\x82\xab\xe3\x83\xbc", TL_LB_NORMAL },
    { "prolonged loose",  "\xe3\x82\xab" A "\xe3\x83\xbc", TL_LB_LOOSE },
    /* word-break flags */
    { "break-all latin",  "a" A "b" A "c", TL_WB_BREAK_ALL },
    { "break-all words",  "a" A "b " A "c" A "d", TL_WB_BREAK_ALL },
    { "break-all keeps lf","a" A "b\n" M "c", TL_WB_BREAK_ALL },
    { "break-all keeps wj","a\xe2\x81\xa0" "b", TL_WB_BREAK_ALL },
    { "break-all nbsp",   "a" A "\xc2\xa0" "b", TL_WB_BREAK_ALL },
    { "keep-all cjk",     "\xe6\x88\x91\xe4\xbb\xac " A "\xe4\xbd\xbf\xe7\x94\xa8", TL_WB_KEEP_ALL },
    { "keep-all hangul",  "\xed\x95\x9c\xea\xb8\x80", TL_WB_KEEP_ALL },
    { "keep-all hyphen",  "ab-" A "cd", TL_WB_KEEP_ALL },
    { "loose+break-all",  "\xe3\x81\x82" A "\xe3\x81\x81" A "a", TL_LB_LOOSE | TL_WB_BREAK_ALL },
    /* invalid UTF-8: every bad byte is one U+FFFD (class AL) */
    { "invalid byte",     "a\xff" "b", 0 },
    { "invalid after sp", "a " A "\xff", 0 },
    { "truncated 3-byte", "\xe4\xbd", 0 },
    { "han then truncated","\xe4\xbd\xa0" A "\xe5\xa5", 0 },
    { "stray continuation","x\x80 " A "y", 0 },
};

/* "ab" A "cd" -> text "abcd", want[] over bytes */
static int decode(const char *exp, char *txt, unsigned char *want)
{
    int n = 0;
    for (; *exp; exp++) {
        unsigned char c = (unsigned char)*exp;
        if (c == 1)      want[n] = TL_BRK_ALLOWED;
        else if (c == 2) want[n] = TL_BRK_MANDATORY;
        else { want[n] = TL_BRK_NONE; }
        if (c == 1 || c == 2) {
            /* marker applies to the byte that follows: record and re-use slot */
            exp++;
            /* the next byte is real text */
            unsigned char t = (unsigned char)*exp;
            if (t == 0) { /* marker at end: not allowed in table */ return -1; }
            txt[n] = (char)t;
            n++;
        } else {
            txt[n] = (char)c;
            n++;
        }
    }
    return n;
}

static void dumpb(const char *tag, const unsigned char *b, int n)
{
    int i;
    printf("      %s", tag);
    for (i = 0; i <= n; i++) putchar(b[i] == TL_BRK_ALLOWED ? '/' :
                                     b[i] == TL_BRK_MANDATORY ? '!' : '.');
    putchar('\n');
}

static void run_hand(void)
{
    unsigned i;
    for (i = 0; i < sizeof hand / sizeof hand[0]; i++) {
        char txt[256];
        unsigned char want[257], got[257];
        int n = decode(hand[i].exp, txt, want), ok = 1, k, rc;
        checks++;
        if (n < 0) { printf("FAIL expected-breaks %s: bad table row\n", hand[i].name); failures++; continue; }
        want[n] = TL_BRK_MANDATORY;
        want[0] = (n > 0) ? want[0] : TL_BRK_MANDATORY;
        memset(got, 0x77, sizeof got);
        rc = tl_break_utf8(txt, n, got, hand[i].flags);
        if (rc != n + 1) ok = 0;
        for (k = 0; ok && k <= n; k++) if (got[k] != want[k]) ok = 0;
        if (!ok) {
            failures++;
            printf("FAIL expected-breaks %s (flags %#x, rc %d)\n", hand[i].name,
                   hand[i].flags, rc);
            dumpb("want ", want, n);
            dumpb("got  ", got, n);
        }
    }
    printf("  %u hand-written cases\n", (unsigned)(sizeof hand / sizeof hand[0]));
}

static void run_edges(void)
{
    unsigned char out[8];
    static char big[TL_BREAK_MAX + 8];
    static unsigned char bo[TL_BREAK_MAX + 9];
    int rc;

    checks++;
    out[0] = 0x77;
    rc = tl_break_utf8("", 0, out, 0);
    if (rc != 1 || out[0] != TL_BRK_MANDATORY) {
        failures++; printf("FAIL expected-breaks empty string (rc %d out %d)\n", rc, out[0]);
    }
    checks++;
    rc = tl_break_utf8("a", 1, out, 0);
    if (rc != 2 || out[0] != TL_BRK_NONE || out[1] != TL_BRK_MANDATORY) {
        failures++; printf("FAIL expected-breaks single char\n");
    }
    /* the cap: exactly TL_BREAK_MAX code points is served, one more refused */
    memset(big, 'a', sizeof big);
    checks++;
    rc = tl_break_utf8(big, TL_BREAK_MAX, bo, 0);
    if (rc != TL_BREAK_MAX + 1) { failures++; printf("FAIL expected-breaks cap exact (rc %d)\n", rc); }
    checks++;
    rc = tl_break_utf8(big, TL_BREAK_MAX + 1, bo, 0);
    if (rc != -1 || bo[TL_BREAK_MAX + 1] != TL_BRK_MANDATORY || bo[5] != TL_BRK_NONE) {
        failures++; printf("FAIL expected-breaks cap overflow (rc %d)\n", rc);
    }
}

/* -------------------------------------------- differential vs browser ----- */

static uint32_t rng = 0x9e3779b9u;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static int enc(uint32_t cp, char *o)
{
    if (cp < 0x80)    { o[0] = (char)cp; return 1; }
    if (cp < 0x800)   { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000) { o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 63));
                        o[2] = (char)(0x80 | (cp & 63)); return 3; }
    o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 63));
    o[2] = (char)(0x80 | ((cp >> 6) & 63)); o[3] = (char)(0x80 | (cp & 63)); return 4;
}

static const uint32_t alpha[] = {
    'a','b','c','x','Z','0','1','5',' ',' ',' ','-','.',',','(',')','!','?','$','%',
    '"','\'','/','_','\n','\r','\t',0xA0,0xAD,0x200B,0x200D,0x2060,0x2010,0x2028,
    0x85,0x301,0x300,0x3099,0x4E2D,0x6587,0x3002,0x300C,0x300D,0x3041,0x30FC,0x3042,
    0xAC00,0xAE00,0x1100,0x1161,0x11A8,0x1F468,0x1F469,0x1F3FB,0x1F1EF,0x1F1F5,
    0x5D0,0x627,0xE01,0xE31,0x2026,0x25CC,0x0915,0x094D,0xFFFD,
};

static void run_differential(void)
{
    int it, fl, bad = 0;
    static const int lbs[] = { TL_LB_NORMAL, TL_LB_STRICT, TL_LB_LOOSE };
    static const int wbs[] = { 0, TL_WB_BREAK_ALL, TL_WB_KEEP_ALL };
    for (it = 0; it < 20000; it++) {
        char s[200];
        unsigned char got[201], ref[201];
        int len = 0, n = (int)(rnd() % 40), k;
        for (k = 0; k < n && len < 180; k++) {
            uint32_t r = rnd();
            if (r % 29 == 0) s[len++] = (char)(0x80 + rnd() % 128);   /* stray byte */
            else len += enc(alpha[r % (sizeof alpha / sizeof alpha[0])], s + len);
        }
        for (fl = 0; fl < 9; fl++) {
            struct ltx_lbopt o;
            int flags = lbs[fl / 3] | wbs[fl % 3], i;
            memset(&o, 0, sizeof o);
            o.line_break = (unsigned char)(fl / 3 == 0 ? LTX_LB_NORMAL :
                                           fl / 3 == 1 ? LTX_LB_STRICT : LTX_LB_LOOSE);
            o.word_break = (unsigned char)(fl % 3 == 0 ? LTX_WB_NORMAL :
                                           fl % 3 == 1 ? LTX_WB_BREAK_ALL : LTX_WB_KEEP_ALL);
            ltx_break_utf8(s, len, &o, ref);
            if (tl_break_utf8(s, len, got, flags) != len + 1) { bad++; continue; }
            for (i = 0; i <= len; i++)
                if (got[i] != ref[i]) {
                    if (bad++ < 5) {
                        printf("  differential mismatch it %d flags %#x byte %d: tl %d ltx %d\n",
                               it, flags, i, got[i], ref[i]);
                    }
                    break;
                }
            checks++;
        }
    }
    if (bad) { failures++; printf("FAIL differential: %d mismatches\n", bad); }
    else printf("  differential: 20000 strings x 9 flag sets agree with ltx_break_utf8\n");
}

/* ------------------------------------------------- the UAX #14 corpus ----- */

static int run_corpus(const char *ucd)
{
    char path[512], line[8192];
    FILE *fh;
    int cases = 0, bad = 0, lineno = 0;
    snprintf(path, sizeof path, "%s/auxiliary/LineBreakTest.txt", ucd);
    fh = fopen(path, "r");
    if (!fh) {
        printf("FAIL LineBreakTest: cannot open %s (pass a Unicode 16.0.0 UCD)\n", path);
        return 1;
    }
    while (fgets(line, sizeof line, fh)) {
        uint32_t cp[1024];
        unsigned char want[1025], got[8200];
        int off[1025];
        char txt[4200];
        char *p = line, *hash;
        int n = 0, expect_mark = 1, len = 0, i;
        lineno++;
        hash = strchr(line, '#'); if (hash) *hash = 0;
        want[0] = 255;
        while (*p) {
            if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') { p++; continue; }
            if ((unsigned char)p[0] == 0xC3 &&
                ((unsigned char)p[1] == 0x97 || (unsigned char)p[1] == 0xB7)) {
                if (!expect_mark) break;
                want[n] = ((unsigned char)p[1] == 0xB7);
                expect_mark = 0; p += 2; continue;
            }
            if ((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'F') || (*p >= 'a' && *p <= 'f')) {
                char *end; unsigned long v = strtoul(p, &end, 16);
                if (n >= 1023) { p = end; continue; }
                cp[n++] = (uint32_t)v; expect_mark = 1; p = end; continue;
            }
            p++;
        }
        if (n == 0 || expect_mark) continue;
        for (i = 0; i < n; i++) { off[i] = len; len += enc(cp[i], txt + len); }
        off[n] = len;
        if (tl_break_utf8(txt, len, got, 0) != len + 1) { bad++; continue; }
        for (i = 0; i <= n; i++) {
            int g = got[off[i]] != TL_BRK_NONE;
            if (g != (want[i] != 0)) {
                if (bad < 8) printf("  line %d position %d: want %s got %s\n", lineno, i,
                                    want[i] ? "break" : "no-break", g ? "break" : "no-break");
                bad++;
                break;
            }
        }
        cases++;
    }
    fclose(fh);
    checks += cases;
    printf("  LineBreakTest.txt: %d cases, %d failed\n", cases, bad);
    if (cases < 10000) { printf("FAIL LineBreakTest: only %d cases parsed (expect ~16k)\n", cases); return 1; }
    if (bad) { printf("FAIL LineBreakTest: %d cases disagree\n", bad); return 1; }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        int rc = run_corpus(argv[1]);
        printf("tl_linebreak_test: corpus %s\n", rc ? "FAILED" : "ok");
        return rc ? 1 : 0;
    }
    run_hand();
    run_edges();
    run_differential();
    printf("tl_linebreak_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
