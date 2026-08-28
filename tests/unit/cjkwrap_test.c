/* tests/unit/cjkwrap_test.c -- does the BROWSER take the line breaks that
 * c/apps/browser/layout_text.c computes?
 *
 * WHY THIS IS A SECOND GATE AND NOT MORE CASES IN csstext_test.c.  That suite
 * measures layout_text.c against the Unicode Consortium's own corpus and it
 * has been green, at 103 checks plus 16,672 conformance cases, while the
 * browser called none of it: `nm build/browser.elf | grep -c layout_text` was
 * 0.  A gate on a module cannot see whether anything consumes the module.
 * This one asks the only question that was open -- where does layout.c put the
 * line ends -- and it asks it through layout_page(), the entry point
 * browser.c calls, so it cannot pass by linking a file and never running it.
 *
 * THE MEASURE.  text_measure() below is one em per non-ASCII code point and
 * half an em per ASCII one, which is what every CJK font actually does and is
 * the same monospace approximation every other host layout harness uses.  The
 * assertions are about WHICH CHARACTER a line starts and ends with, never
 * about a pixel column, so they survive any change to the advance model.
 *
 * THE RULES BEING CHECKED are the two every reader of Chinese notices
 * instantly and neither of which a space-only breaker can honour:
 *   - a line may not BEGIN with closing punctuation (。、，）」!?:; ...)
 *     -- UAX #14 LB13/LB16, class CL/CP/NS/EX/IS
 *   - a line may not END with opening punctuation (（「《 ...)  -- LB14, class OP
 * Both are prohibitions, so both are falsifiable by a wrong implementation and
 * neither can be satisfied by accident on a long enough paragraph: with breaks
 * chosen at arbitrary character boundaries the probability that no line of
 * twenty starts with a full stop is small, and the corpus below makes it zero
 * by putting the punctuation everywhere.
 *
 * THE CONTROL is -DLAYOUT_NO_UAX14, which makes layout.c's lb_opps() return
 * NULL and puts the file back on the any-boundary cut it shipped with.  That
 * is not "delete line breaking": it is the plausible wrong answer, the one
 * that is indistinguishable from correct on English.  tests/csstext.mk
 * requires that build to FAIL this suite.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "layout.h"
#include "css.h"
#include "dom.h"

void *kmalloc(unsigned long n){ return malloc(n); }
void  kfree(void *p){ free(p); }

/* One em per ideograph, half an em per ASCII character. Decoding here rather
 * than counting bytes matters: a three-byte ideograph counted as three
 * half-ems is 1.5x its real advance and every line would come out short. */
int text_measure(const char *s, int len, int px, int mono)
{
    (void)mono;
    int w = 0, i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        int adv = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
        if (i + adv > len) adv = 1;
        w += (c < 0x80) ? px / 2 : px;
        i += adv;
    }
    return w;
}
int res_fetch(const char *u, uint8_t **b, int *l){ (void)u;(void)b;(void)l; return -1; }
void img_free(struct image *o){ (void)o; }
int img_decode(const uint8_t *p, int n, struct image *o){ (void)p;(void)n;(void)o; return -1; }

static int checks, fails;
#define CHECK(c,m) do{ checks++; if(!(c)){ printf("  FAIL: %s\n",(m)); fails++; } }while(0)

/* ------------------------------------------------------------- lines ----- */

/* The text items layout produced, in document order, grouped into lines by y.
 * A "line" here is what the reader sees: every IT_TEXT sharing a top edge. */
#define MAXL 256
struct line { int y; char text[512]; int n; };
static struct line g_line[MAXL];
static int g_nline;

static int by_y(const void *a, const void *b)
{
    const struct line *p = a, *q = b;
    return p->y - q->y;
}

static void collect(const char *html, int cw)
{
    static char css[8192], exp[16384];
    struct node *root = dom_parse(html, (int)strlen(html));
    css[0] = 0;
    int el = css_expand_vars(css, 0, exp, (int)sizeof exp);
    css_apply(root, exp, el);
    layout_page(root, cw);

    g_nline = 0;
    const struct item *it = layout_items();
    int n = layout_count();
    for (int i = 0; i < n; i++) {
        if (it[i].type != IT_TEXT || it[i].len <= 0 || !it[i].text) continue;
        int k;
        for (k = 0; k < g_nline; k++) if (g_line[k].y == it[i].y) break;
        if (k == g_nline) {
            if (g_nline >= MAXL) continue;
            g_nline++; g_line[k].y = it[i].y; g_line[k].n = 0; g_line[k].text[0] = 0;
        }
        /* One display-list item is one thing layout decided not to split, and
         * the inter-word space is a PEN ADVANCE, not text in the item -- so
         * concatenating items byte for byte would spell "thequickbrownfox" and
         * a suite that read that as one word would report a bug that is its
         * own. Items are joined with a space, which is what the reader sees. */
        if (g_line[k].n && g_line[k].n + 1 < (int)sizeof g_line[k].text)
            g_line[k].text[g_line[k].n++] = ' ';
        int room = (int)sizeof g_line[k].text - g_line[k].n - 1;
        int cp = it[i].len < room ? it[i].len : room;
        if (cp > 0) {
            memcpy(g_line[k].text + g_line[k].n, it[i].text, (size_t)cp);
            g_line[k].n += cp; g_line[k].text[g_line[k].n] = 0;
        }
    }
    qsort(g_line, (size_t)g_nline, sizeof g_line[0], by_y);
}

/* The x of the first text item whose bytes are exactly `t`. -1 if absent, which
 * an assertion against a positive column can never be confused with. */
static int layout_x_of(const char *t)
{
    const struct item *it = layout_items();
    int n = layout_count(), tl = (int)strlen(t);
    for (int i = 0; i < n; i++)
        if (it[i].type == IT_TEXT && it[i].len == tl && it[i].text &&
            !memcmp(it[i].text, t, (size_t)tl)) return it[i].x;
    return -1;
}

static uint32_t first_cp(const char *s)
{
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) return c;
    if ((c & 0xE0) == 0xC0) return (uint32_t)((c & 0x1F) << 6) | ((unsigned char)s[1] & 0x3F);
    if ((c & 0xF0) == 0xE0) return (uint32_t)((c & 0x0F) << 12) |
                                   (uint32_t)(((unsigned char)s[1] & 0x3F) << 6) |
                                   ((unsigned char)s[2] & 0x3F);
    return 0xFFFD;
}
static uint32_t last_cp(const char *s, int n)
{
    int i = n - 1;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    return first_cp(s + i);
}

/* Closing punctuation: no line may begin with one of these. */
static int no_start(uint32_t c)
{
    static const uint32_t t[] = {
        0x3002,/* 。*/ 0xFF0C,/* ，*/ 0x3001,/* 、*/ 0xFF09,/* ）*/ 0x300D,/* 」*/
        0xFF01,/* ！*/ 0xFF1F,/* ？*/ 0xFF1A,/* ：*/ 0xFF1B,/* ；*/ 0x300B,/* 》*/
        0xFF0E,/* ．*/ 0x2019,/* ’ */ 0 };
    for (int i = 0; t[i]; i++) if (t[i] == c) return 1;
    return 0;
}
/* Opening punctuation: no line may end with one of these. */
static int no_end(uint32_t c)
{
    static const uint32_t t[] = { 0xFF08,/* （*/ 0x300C,/* 「*/ 0x300A,/* 《*/
                                  0x3010,/* 【*/ 0x2018,/* ‘ */ 0 };
    for (int i = 0; t[i]; i++) if (t[i] == c) return 1;
    return 0;
}

static void dump(const char *what)
{
    printf("  %s -- %d line(s)\n", what, g_nline);
    for (int i = 0; i < g_nline && i < 8; i++)
        printf("      y=%-4d %s\n", g_line[i].y, g_line[i].text);
    if (g_nline > 8) printf("      ... %d more\n", g_nline - 8);
}

/* Every line of the last collect(): none begins with closing punctuation and
 * none ends with opening punctuation. Reports the offending line, because
 * "some line is wrong" is not a finding anyone can act on. */
static void punctuation_is_placed(const char *what)
{
    int bad_start = -1, bad_end = -1;
    for (int i = 0; i < g_nline; i++) {
        if (!g_line[i].n) continue;
        if (i > 0 && no_start(first_cp(g_line[i].text)) && bad_start < 0) bad_start = i;
        if (i + 1 < g_nline && no_end(last_cp(g_line[i].text, g_line[i].n)) && bad_end < 0)
            bad_end = i;
    }
    checks++;
    if (bad_start >= 0) {
        printf("  FAIL: %s -- line %d BEGINS with closing punctuation: \"%s\"\n",
               what, bad_start, g_line[bad_start].text);
        fails++;
    }
    checks++;
    if (bad_end >= 0) {
        printf("  FAIL: %s -- line %d ENDS with opening punctuation: \"%s\"\n",
               what, bad_end, g_line[bad_end].text);
        fails++;
    }
}

/* ------------------------------------------------------------- cases ----- */

/* Chinese prose with punctuation at every position a greedy any-boundary cut
 * could land on. Deliberately ONE text node with no space in it: that is what
 * arrives from a real page and it is the input the old breaker turned into one
 * enormous word. */
#define P1 \
  "\xe4\xbb\x8a\xe5\xa4\xa9\xe5\xa4\xa9\xe6\xb0\x94\xe5\xbe\x88\xe5\xa5\xbd\xef\xbc\x8c" \
  "\xe6\x88\x91\xe4\xbb\xac\xe5\x8e\xbb\xe5\x85\xac\xe5\x9b\xad\xe6\x95\xa3\xe6\xad\xa5\xe3\x80\x82" \
  "\xe4\xbb\x96\xe8\xaf\xb4\xef\xbc\x9a\xe3\x80\x8c\xe8\xbf\x99\xe6\x98\xaf\xe4\xb8\x80" \
  "\xe4\xb8\xaa\xe5\xa5\xbd\xe4\xb8\xbb\xe6\x84\x8f\xe3\x80\x8d\xef\xbc\x81" \
  "\xe7\x84\xb6\xe5\x90\x8e\xef\xbc\x88\xe5\xa4\xa7\xe5\xae\xb6\xe9\x83\xbd\xe7\xac\x91" \
  "\xe4\xba\x86\xef\xbc\x89\xe3\x80\x82\xe6\x98\x8e\xe5\xa4\xa9\xe5\x91\xa2\xef\xbc\x9f" \
  "\xe6\x98\x8e\xe5\xa4\xa9\xe5\x86\x8d\xe8\xaf\xb4\xe3\x80\x82"

int main(void)
{
    printf("cjkwrap_test: does the browser take layout_text.c's line breaks?\n");

    /* 1. The headline case. A Chinese paragraph in a 200px column at 20px
     *    type is ten ideographs per line and must break between them. */
    collect("<html><body style=\"margin:0;padding:0\">"
            "<p style=\"margin:0;font-size:20px;width:200px\">" P1 "</p>"
            "</body></html>", 400);
    dump("a Chinese paragraph, 200px measure, 20px type");
    CHECK(g_nline >= 4, "the paragraph wrapped onto several lines");
    punctuation_is_placed("Chinese prose");

    /* 2. The same text at a different measure. A rule that happened to hold at
     *    one width and not another is a coincidence, not an implementation --
     *    and a breaker with an off-by-one lands differently at every width. */
    collect("<html><body style=\"margin:0;padding:0\">"
            "<p style=\"margin:0;font-size:20px;width:140px\">" P1 "</p>"
            "</body></html>", 400);
    dump("the same text at a 140px measure");
    CHECK(g_nline >= 6, "a narrower measure produced more lines");
    punctuation_is_placed("Chinese prose, narrow");

    collect("<html><body style=\"margin:0;padding:0\">"
            "<p style=\"margin:0;font-size:20px;width:330px\">" P1 "</p>"
            "</body></html>", 400);
    dump("the same text at a 330px measure");
    punctuation_is_placed("Chinese prose, wide");

    /* 2b. THE SWEEP. Two widths is two coincidences; the rule has to hold at
     *     every measure the text can be poured into. Fourteen of them, and at
     *     each one two independent properties:
     *
     *       - no line begins with closing punctuation or ends with an opening
     *         one (the readability rule);
     *       - no line was cut when the whole rest of the paragraph would have
     *         fitted on it (the NOT-TOO-EAGER rule).
     *
     *     The second one is here because the first version of this wiring
     *     failed it: preferring the last legal opportunity is right when a cut
     *     is being made and wrong when the remainder fits whole, and with only
     *     the punctuation rule the suite called that green. A gate that can
     *     only catch breaks that are too late is half a gate. */
    {
        int viol_punct = 0, viol_eager = 0, widths = 0;
        for (int w = 100; w <= 360; w += 20) {
            char html[1024];
            snprintf(html, sizeof html,
                     "<html><body style=\"margin:0;padding:0\">"
                     "<p style=\"margin:0;font-size:20px;width:%dpx\">" P1 "</p>"
                     "</body></html>", w);
            collect(html, 400);
            widths++;
            for (int i = 0; i < g_nline; i++) {
                if (!g_line[i].n) continue;
                if (i > 0 && no_start(first_cp(g_line[i].text))) viol_punct++;
                if (i + 1 < g_nline && no_end(last_cp(g_line[i].text, g_line[i].n)))
                    viol_punct++;
                if (i + 1 < g_nline) {
                    int both = text_measure(g_line[i].text, g_line[i].n, 20, 0) +
                               text_measure(g_line[i+1].text, g_line[i+1].n, 20, 0);
                    if (both <= w) {
                        if (!viol_eager)
                            printf("  FAIL: at %dpx, \"%s\" and \"%s\" fit on one "
                                   "line (%dpx) and were split\n",
                                   w, g_line[i].text, g_line[i+1].text, both);
                        viol_eager++;
                    }
                }
            }
        }
        printf("  swept %d measures from 100px to 360px\n", widths);
        checks++;
        if (viol_punct) {
            printf("  FAIL: %d punctuation violation(s) across the sweep\n", viol_punct);
            fails++;
        }
        checks++;
        if (viol_eager) {
            printf("  FAIL: %d line(s) split with the whole remainder fitting\n",
                   viol_eager);
            fails++;
        }
    }

    /* 3. THE FALLBACK MUST SURVIVE. A long Latin word has no legal break in it
     *    at all (UAX #14 puts none between two AL characters), so a breaker
     *    that only ever broke at opportunities would refuse to break it and the
     *    box would overflow. layout.c must still cut it at a character
     *    boundary -- this is the property `min-width:auto` on a flex item rests
     *    on, and the reason lb_opps() is a preference and not a requirement. */
    collect("<html><body style=\"margin:0;padding:0\">"
            "<p style=\"margin:0;font-size:20px;width:100px\">"
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa</p>"
            "</body></html>", 400);
    dump("forty 'a's in a 100px measure");
    CHECK(g_nline >= 4, "an unbreakable Latin word is still cut to fit");

    /* 4. Ordinary English still breaks at spaces and nowhere else: no line may
     *    end in the middle of a word. This is the regression half -- the whole
     *    risk of wiring UAX #14 in is that it starts breaking English
     *    somewhere new. */
    collect("<html><body style=\"margin:0;padding:0\">"
            "<p style=\"margin:0;font-size:20px;width:200px\">"
            "the quick brown fox jumps over the lazy dog and then it "
            "runs away into the deep dark wood</p>"
            "</body></html>", 400);
    dump("English prose, 200px measure");
    {
        /* Every token layout emitted must be a whole word of the source. A
         * fragment like "quic" appears here only if something broke inside a
         * word, which UAX #14 forbids between two AL characters -- so this is
         * the assertion that catches a wiring that starts breaking English
         * somewhere new. Checked with the delimiters attached so "the" cannot
         * match inside "then". */
        static const char *WORDS =
            " the quick brown fox jumps over lazy dog and then it "
            "runs away into deep dark wood ";
        int badline = -1; char badtok[64] = "";
        for (int i = 0; i < g_nline && badline < 0; i++) {
            const char *a = g_line[i].text;
            int p = 0;
            while (p < g_line[i].n) {
                int q = p; while (q < g_line[i].n && a[q] != ' ') q++;
                if (q > p) {
                    char w[64]; int wl = q - p; if (wl > 61) wl = 61;
                    w[0] = ' '; memcpy(w + 1, a + p, (size_t)wl);
                    w[wl + 1] = ' '; w[wl + 2] = 0;
                    if (!strstr(WORDS, w)) {
                        badline = i; memcpy(badtok, w + 1, (size_t)wl); badtok[wl] = 0;
                        break;
                    }
                }
                p = q + 1;
            }
        }
        checks++;
        if (badline >= 0) {
            printf("  FAIL: English line %d holds \"%s\", which is not a word of "
                   "the source -- a word was broken\n", badline, badtok);
            fails++;
        }
    }
    CHECK(g_nline >= 2, "English prose wrapped");

    /* 5. Mixed CJK and Latin in one run: the Latin word must stay whole and
     *    the Chinese around it must still break. */
    collect("<html><body style=\"margin:0;padding:0\">"
            "<p style=\"margin:0;font-size:20px;width:160px\">"
            "\xe6\x88\x91\xe4\xbb\xac\xe4\xbd\xbf\xe7\x94\xa8" "LogitOS"
            "\xe6\xb5\x8f\xe8\xa7\x88\xe5\x99\xa8\xe3\x80\x82"
            "\xe5\xae\x83\xe5\xbe\x88\xe5\xbf\xab\xef\xbc\x8c\xe4\xb9\x9f\xe5\xbe\x88\xe5\xb0\x8f\xe3\x80\x82"
            "</p></body></html>", 400);
    dump("mixed Chinese and Latin");
    punctuation_is_placed("mixed run");
    {
        int split = 1;
        for (int i = 0; i < g_nline; i++) if (strstr(g_line[i].text, "LogitOS")) split = 0;
        checks++;
        if (split) {
            printf("  FAIL: \"LogitOS\" was split across lines\n");
            fails++;
        }
    }

    /* 6. THE OWED SPACE (CSS Text 3 §4.1), which is the other half of "a page
     *    a person can read" and was a separate defect from the breaking.
     *
     *    layout.c advanced the pen by one space between any two runs on a
     *    started line, whatever the source said. The sharpest way to state
     *    what is wrong with that is not "the gap is 10px too wide": it is that
     *    `<b>ab</b><i>cd</i>` and `<b>ab</b> <i>cd</i>` are DIFFERENT
     *    DOCUMENTS and the engine rendered them identically. An assertion on
     *    one of them alone can be satisfied by a constant; the pair cannot. */
    {
        int x_join = -1, x_split = -1, x_cjk = -1, x_mid = -1;
        collect("<html><body style=\"margin:0\">"
                "<p style=\"margin:0;font-size:20px\"><b>ab</b><i>cd</i></p>"
                "</body></html>", 600);
        dump("<b>ab</b><i>cd</i>  -- no space in the source");
        x_join = layout_x_of("cd");
        collect("<html><body style=\"margin:0\">"
                "<p style=\"margin:0;font-size:20px\"><b>ab</b> <i>cd</i></p>"
                "</body></html>", 600);
        dump("<b>ab</b> <i>cd</i> -- one space in the source");
        x_split = layout_x_of("cd");

        checks++;
        if (x_join != 20) {
            printf("  FAIL: \"cd\" starts at x=%d with no space in the source; "
                   "\"ab\" is 20px wide, so it must start at 20\n", x_join);
            fails++;
        }
        checks++;
        if (x_split != 30) {
            printf("  FAIL: \"cd\" starts at x=%d with a space in the source; "
                   "20px of \"ab\" plus a 10px space is 30\n", x_split);
            fails++;
        }
        checks++;
        if (x_join == x_split) {
            printf("  FAIL: the two documents laid out IDENTICALLY (x=%d) -- the "
                   "engine cannot tell \"abcd\" from \"ab cd\"\n", x_join);
            fails++;
        }

        /* The Chinese case it costs the most on: a navigation bar. */
        collect("<html><body style=\"margin:0\"><p style=\"margin:0;font-size:20px\">"
                "<a>\xe9\xa6\x96\xe9\xa1\xb5</a><a>\xe8\xa7\x86\xe9\xa2\x91</a>"
                "</p></body></html>", 600);
        dump("<a>\xe9\xa6\x96\xe9\xa1\xb5</a><a>\xe8\xa7\x86\xe9\xa2\x91</a> -- adjacent links");
        x_cjk = layout_x_of("\xe8\xa7\x86\xe9\xa2\x91");
        checks++;
        if (x_cjk != 40) {
            printf("  FAIL: the second link starts at x=%d; two 20px ideographs "
                   "are 40px and there is no space between the elements\n", x_cjk);
            fails++;
        }

        /* One word split by markup is still one word. */
        collect("<html><body style=\"margin:0\"><p style=\"margin:0;font-size:20px\">"
                "ab <b>cd</b>ef</p></body></html>", 600);
        dump("ab <b>cd</b>ef -- \"cdef\" is one word");
        x_mid = layout_x_of("ef");
        checks++;
        if (x_mid != 50) {
            printf("  FAIL: \"ef\" starts at x=%d; it must abut \"cd\" at 50\n", x_mid);
            fails++;
        }
    }

    printf("\ncjkwrap_test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
