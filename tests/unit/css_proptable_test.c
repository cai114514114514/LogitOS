/* css_proptable_test: hold LibCSS's three POSITIONAL property tables together.
 *
 * THE FAILURE THIS EXISTS FOR IS THE ONLY SILENT ONE IN THE BORDER-RADIUS
 * SLICE, and it is the worst shape a CSS bug can have.
 *
 * Three tables must agree by INDEX and nothing upstream asserts that they do:
 *
 *   propstrings.h   enum { FIRST_PROP ... LAST_PROP }
 *   propstrings.c   stringmap[LAST_KNOWN]                 -- the names
 *   properties.c    property_handlers[LAST_PROP+1-FIRST_PROP]  -- the parsers
 *
 * parseProperty() finds the NAME at index i and calls the PARSER at index i.
 * Shear them by one and `border-radius: 4px` is handled by
 * border-right-width's parser, which happily accepts a length and writes
 * border-right-width's bytecode. The page then draws a 4px right border on
 * every rounded box. There is no crash, no drop report and no log line --
 * report_drop(property, CSS_DROP_ACCEPTED) fires, so the declaration counts as
 * a SUCCESS in `make audit-css`, the very instrument the slice was sized with.
 *
 * The mitigation in the source is structural (see propstrings.h: new
 * properties are APPENDED, never inserted, so three files take three appends
 * at one index instead of three insertions that shift everything after them).
 * This file is the gate that says so out loud.
 *
 * WHAT IT CHECKS, and what each check can and cannot see:
 *
 *   1. COUNT.  Every property index has a non-NULL handler. C zero-fills a
 *      short initializer list, so a name added without a parser is otherwise
 *      silent -- and upstream's only guard was an assert(), compiled out of
 *      the shipped browser by -DNDEBUG (Makefile, UCFLAGS).
 *   2. THE APPENDED TAIL.  The five LogitOS names must be the LAST five, in
 *      order. (A sortedness check over the upstream block was written first
 *      and REMOVED: that block is not sorted -- `columns` precedes
 *      `column-count`, `pitch` follows `pitch-range`, `speak` follows
 *      `speak-punctuation` -- so the check went red on three upstream
 *      entries and would have had to carry an exception list, i.e. a fourth
 *      copy of the table. Measured before it was believed.)
 *   3. ALIGNMENT, functionally. Anchor properties spread across the whole
 *      range are parsed with a value only they accept, and the declaration
 *      must be ACCEPTED by LibCSS's own drop hook. A shear moves some other
 *      property's parser under the anchor's name, and that parser refuses the
 *      value -- CSS_DROP_BAD_VALUE. Stated honestly: this catches a shear that
 *      CROSSES an anchor, and the anchors are ~10 properties apart, so a
 *      hypothetical local swap between two adjacent non-anchors is not seen.
 *      Its negative control swaps two adjacent handler entries that DO span an
 *      anchor and must make this file fail.
 *
 * The names in check 3 come from prop_name_at() -- the table under test
 * -- rather than being restated here, deliberately: a gate that carries its
 * own copy of the list is a fourth table, and this file exists because there
 * are already three.
 *
 * Build: the test-css-proptables rule in tests/cssweb.mk.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>

#include "dom.h"
#include "css.h"

/* css_engine.c's OWN window onto LibCSS's stringmap[] -- the accessor
 * js_cssom.c's prop_is_known() already uses. Deliberately NOT a second one:
 * this file exists because three copies of the property list already have to
 * agree, and adding a fourth to test the first three would be the joke telling
 * itself. */
static const char *prop_name_at(unsigned int idx, size_t *len)
{
    int L = 0;
    const char *s;
    if ((int) idx >= css_known_prop_count()) { if (len) *len = 0; return 0; }
    s = css_known_prop_at((int) idx, &L);
    if (len) *len = (size_t) L;
    return s;
}

/* parse/language.h */
enum { DROP_UNKNOWN = 0, DROP_BADVALUE = 1, DROP_TRAILING = 2, DROP_ACCEPTED = 3 };
extern void (*css__parse_drop_report)(const char *name, size_t nlen, int reason);

static int fails, checks;

/* The hook fires for every declaration LibCSS sees, including the UA default
 * sheet. So the verdict is matched BY NAME rather than taken as "the last
 * report" -- a probe that reads whichever report happened to be last is the
 * apparatus lying about the thing under test. */
static char g_want[64];
static int g_want_len;
static int g_verdict = -1;
static int g_hits;

static void on_drop(const char *name, size_t nlen, int reason)
{
    if (g_want_len <= 0 || (int) nlen != g_want_len) return;
    if (memcmp(name, g_want, (size_t) g_want_len) != 0) return;
    g_verdict = reason;
    g_hits++;
}

static void ck(int cond, const char *what)
{
    checks++;
    if (!cond) { printf("FAIL %s\n", what); fails++; }
    else printf("ok   %s\n", what);
}

/* Parse `#x{<name>:<value>}` and hand back the hook's verdict FOR THAT NAME.
 * -1 means the hook never saw the name at all, which is its own failure. */
static int verdict(const char *name, int nlen, const char *value)
{
    static char css[320];
    static const char *html = "<!doctype html><html><body><div id=x>x</div></body></html>";
    struct node *r;

    if (nlen <= 0 || nlen >= (int) sizeof g_want) return -1;
    memcpy(g_want, name, (size_t) nlen);
    g_want[nlen] = 0;
    g_want_len = nlen;
    g_verdict = -1; g_hits = 0;

    snprintf(css, sizeof css, "#x{%.*s:%s}", nlen, name, value);
    r = dom_parse(html, (int) strlen(html));
    if (!r) return -1;
    css_apply(r, css, (int) strlen(css));
    dom_free(r);
    g_want_len = 0;
    return g_verdict;
}

/* An anchor is (offset from FIRST_PROP, a value that property accepts and
 * that its table neighbours do not). The OFFSET is the anchor, not the name:
 * the name is read out of the table, so if the table shifted, the name shifts
 * with it and the value stops fitting -- which is exactly the signal. */
struct anchor { unsigned int off; const char *value; const char *expect_name; };

int main(void)
{
    unsigned int nprop = 0, i;

    css__parse_drop_report = on_drop;
    css_init();
    css_viewport(980, 700);

    /* ---- 1. COUNT: how many properties are there, and do they all parse?
     * css__prop_name_at returns NULL past LAST_PROP. */
    while (prop_name_at(nprop, NULL) != NULL) nprop++;
    printf("       property block: %u names\n", nprop);
    ck(nprop > 100, "the property table is populated");

    /* A property whose handler slot is NULL is refused by the LogitOS guard in
     * parseProperty() and reported as UNKNOWN_PROP even though the NAME was
     * found. So: every name in the table must parse to something other than
     * UNKNOWN. `inherit` is the one value EVERY property handler accepts. */
    {
        int bad = -1;
        for (i = 0; i < nprop; i++) {
            size_t len = 0;
            const char *nm = prop_name_at(i, &len);
            int v;
            if (!nm) continue;
            v = verdict(nm, (int) len, "inherit");
            if (v == DROP_UNKNOWN) { bad = (int)i; break; }
        }
        if (bad >= 0) {
            size_t len = 0;
            const char *nm = prop_name_at((unsigned)bad, &len);
            printf("       first offender: index %d, name \"%.*s\"\n",
                   bad, (int)len, nm ? nm : "?");
        }
        ck(bad < 0, "every property name in the table has a live handler");
    }

    /* ---- 2. THE APPENDED TAIL is the last five, in order. */
    {
        static const char *const tail[] = {
            "border-radius",
            "border-top-left-radius",
            "border-top-right-radius",
            "border-bottom-right-radius",
            "border-bottom-left-radius"
        };
        const unsigned int ntail = sizeof tail / sizeof tail[0];
        int tail_ok = 1;

        for (i = 0; i < ntail; i++) {
            const char *nm = prop_name_at(nprop - ntail + i, NULL);
            if (!nm || strcmp(nm, tail[i]) != 0) {
                printf("       tail slot %u is \"%s\", expected \"%s\"\n",
                       i, nm ? nm : "(null)", tail[i]);
                tail_ok = 0;
            }
        }
        ck(tail_ok, "the five appended LogitOS names are the LAST five, in order");
    }

    /* ---- 3. ALIGNMENT: anchors spread across the range.
     * Each value is one only that property accepts. */
    {
        static const struct anchor a[] = {
            {   3, "45deg",                    "azimuth"                   },
            {  15, "collapse",                 "border-collapse"           },
            {  25, "4px 8px",                  "border-spacing"            },
            {  39, "rect(1px,2px,3px,4px)",    "clip"                      },
            {  57, "crosshair",                "cursor"                    },
            {  69, "wrap-reverse",             "flex-wrap"                 },
            {  84, "outside",                  "list-style-position"       },
            {  99, "invert",                   "outline-color"             },
            { 120, "\"a\" \"b\"",              "quotes"                    },
            { 136, "bidi-override",            "unicode-bidi"              },
            { 145, "vertical-rl",              "writing-mode"              },
            /* the LogitOS tail */
            { 147, "8px 8px 0 0",              "border-radius"             },
            { 148, "9px",                      "border-top-left-radius"    },
            { 151, "1px 2px",                  "border-bottom-left-radius" },
        };
        const unsigned int na = sizeof a / sizeof a[0];
        char what[224];

        for (i = 0; i < na; i++) {
            size_t len = 0;
            const char *nm;
            int v;
            if (a[i].off >= nprop) {
                printf("FAIL anchor %u is past the end of the table\n", i);
                checks++; fails++;
                continue;
            }
            nm = prop_name_at(a[i].off, &len);
            v = verdict(nm, (int) len, a[i].value);
            snprintf(what, sizeof what,
                     "anchor %2u: name[%u] is \"%.*s\" and its handler takes `%s`",
                     i, a[i].off, (int)len, nm, a[i].value);
            /* Both halves must hold. The name check is what makes the anchor
             * an anchor -- if the NAMES shifted, the value would still be
             * offered to a matching handler and nothing would look wrong. */
            if ((int)len != (int)strlen(a[i].expect_name) ||
                    memcmp(nm, a[i].expect_name, len) != 0) {
                printf("FAIL %s -- expected the name \"%s\"\n", what,
                       a[i].expect_name);
                checks++; fails++;
                continue;
            }
            if (v != DROP_ACCEPTED) {
                printf("FAIL %s -- verdict %d (%s), not ACCEPTED\n", what, v,
                       v == DROP_BADVALUE ? "bad value" :
                       v == DROP_UNKNOWN ? "unknown property" :
                       v == DROP_TRAILING ? "trailing junk" : "no report at all");
                checks++; fails++;
                continue;
            }
            checks++;
            printf("ok   %s\n", what);
        }
    }

    printf("\ncss_proptable_test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
