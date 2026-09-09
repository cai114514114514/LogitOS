/* css_border_radius_test: border-radius as a REAL cascaded property.
 *
 * WHAT THIS SLICE CHANGED, AND WHY THE TESTS BELOW ARE THE ONES THEY ARE.
 *
 * border-radius was the single largest unknown-property drop on the corpus
 * (`make audit-css`: 26 declarations on the bing fixture alone, plus its four
 * corner longhands). LibCSS's property table did not have the name, so every
 * one of those declarations was refused by the parser -- and the page still
 * came out rounded, because c/apps/browser/css_extra.c carried a SECOND
 * producer: a raw-text scan over the sheet, applied after the cascade.
 *
 * That second producer is now deleted and the five names are in LibCSS. So the
 * interesting assertions are not "is the box rounded" -- it was rounded before
 * -- they are the things the raw-text scan could not do:
 *
 *   SPECIFICITY.  css_extra applied declarations in SOURCE ORDER, last wins.
 *                 `.card .thumb {border-radius:8px}` followed by
 *                 `.thumb {border-radius:0}` therefore squared BOTH thumbs.
 *                 The cascade rounds the one inside .card and squares the
 *                 stray -- and this is the test that tells the new path from
 *                 the old one, because a bare `.x{border-radius:8px}` fixture
 *                 passes either way. (This line's own scar is a WPT runner
 *                 that read 531/11152 with and without an entire grid
 *                 implementation; a gate that both implementations pass is
 *                 not measuring the implementation.)
 *   THE SELECTOR. css_extra's matcher (last_compound()) truncated a selector
 *                 at its last compound and cut it at ':', so a descendant
 *                 chain over-matched and every pseudo-class was discarded.
 *   !important.   Its value scan cut at '!', so `!important` was invisible.
 *   PER CORNER.   It took the FIRST integer of the value list and gave it to
 *                 all four corners, so `border-radius: 12px 12px 0 0` -- a
 *                 card rounded at the top -- came out rounded at the bottom.
 *   NUMBERS.      It read decimal DIGITS only: `.5rem` was zero, `0.5em` was
 *                 zero, and `border-radius: 0.75rem` -- which is what every
 *                 utility framework on the corpus writes -- rounded nothing.
 *
 * Two of those (em/rem lengths and the per-corner list) are assertions NO
 * BUILD OF THE OLD PRODUCER CAN PASS, which is what makes this file evidence
 * that the new path ran rather than evidence that something did.
 *
 * Build: the test-css-border-radius rule in tests/cssweb.mk. Its negative
 * control removes the five names from LibCSS's property table and must make
 * this file fail.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "dom.h"
#include "css.h"

static int fails, checks;

static void ck(int cond, const char *what)
{
    checks++;
    if (!cond) { printf("FAIL %s\n", what); fails++; }
    else printf("ok   %s\n", what);
}

static struct node *find_id(struct node *n, const char *id)
{
    if (!n) return 0;
    if (n->type == N_ELEM) {
        const char *v = dom_attr(n, "id");
        if (v && !strcmp(v, id)) return n;
    }
    for (struct node *c = n->first_child; c; c = c->next) {
        struct node *r = find_id(c, id);
        if (r) return r;
    }
    return 0;
}

/* Style one fixture. BOTH passes run -- css_apply() and then the post-pass
 * css_extra_apply() -- because the claim is about which of the two produces
 * the value, and a harness that ran only one could not tell. */
static struct node *style(const char *html, const char *css)
{
    struct node *r = dom_parse(html, (int)strlen(html));
    if (!r) return 0;
    static char ex[1 << 20];
    int n = css_expand_vars(css, (int)strlen(css), ex, (int)sizeof ex);
    css_apply(r, ex, n);
    css_extra_apply(r, ex, n);
    return r;
}

static struct cstyle *st_of(struct node *root, const char *id)
{
    struct node *n = find_id(root, id);
    return n ? (struct cstyle *)n->style : 0;
}

/* TL TR BR BL */
static int radii_are(struct cstyle *s, int tl, int tr, int br, int bl)
{
    return s && s->radius[0] == tl && s->radius[1] == tr &&
                s->radius[2] == br && s->radius[3] == bl;
}

static void show(const char *tag, struct cstyle *s)
{
    if (!s) { printf("       %s: <no style>\n", tag); return; }
    printf("       %s: radius {%d,%d,%d,%d} pct {%d,%d,%d,%d}\n", tag,
           s->radius[0], s->radius[1], s->radius[2], s->radius[3],
           s->radius_pct[0], s->radius_pct[1], s->radius_pct[2], s->radius_pct[3]);
}

int main(void)
{
    css_init();
    css_viewport(980, 700);
    css_set_post_pass(css_extra_apply);

    /* ---- 1. it arrives at all, and on all four corners ---------------- */
    {
        struct node *r = style(
            "<!doctype html><html><body><div id=a>a</div></body></html>",
            "#a{border-radius:8px}");
        struct cstyle *a = st_of(r, "a");
        ck(radii_are(a, 8, 8, 8, 8), "border-radius:8px -> all four corners 8");
        if (!radii_are(a, 8, 8, 8, 8)) show("a", a);
    }

    /* ---- 2. THE DISCRIMINATING CASE: specificity, not source order ----
     * The old producer applied in source order and squared both. */
    {
        struct node *r = style(
            "<!doctype html><html><body>"
            "<div class=card><div id=inner class=thumb>i</div></div>"
            "<div id=stray class=thumb>s</div>"
            "</body></html>",
            ".card .thumb{border-radius:8px}"
            ".thumb{border-radius:0}");
        struct cstyle *in = st_of(r, "inner"), *sy = st_of(r, "stray");
        ck(radii_are(in, 8, 8, 8, 8),
           ".card .thumb beats a later .thumb (specificity, not source order)");
        ck(radii_are(sy, 0, 0, 0, 0),
           "...and the .thumb OUTSIDE .card is square: the rule matched only where it should");
        if (!radii_are(in, 8, 8, 8, 8) || !radii_are(sy, 0, 0, 0, 0)) {
            show("inner", in); show("stray", sy);
        }
    }

    /* ---- 3. a pseudo-class the engine does not evaluate must not apply
     * its declaration to the un-hovered element. css_extra cut the selector
     * at ':' and so rounded every .btn always. */
    {
        struct node *r = style(
            "<!doctype html><html><body><div id=b class=btn>b</div></body></html>",
            ".btn{border-radius:6px}"
            ".btn:hover{border-radius:0}");
        struct cstyle *b = st_of(r, "b");
        ck(radii_are(b, 6, 6, 6, 6),
           ":hover does not un-round an un-hovered .btn");
        if (!radii_are(b, 6, 6, 6, 6)) show("b", b);
    }

    /* ---- 4. !important --------------------------------------------- */
    {
        struct node *r = style(
            "<!doctype html><html><body><div id=c>c</div></body></html>",
            "#c{border-radius:0 !important}"
            "#c{border-radius:9px}");
        struct cstyle *c = st_of(r, "c");
        ck(radii_are(c, 0, 0, 0, 0), "!important beats a later declaration");
        if (!radii_are(c, 0, 0, 0, 0)) show("c", c);
    }

    /* ---- 5. PER CORNER: the 1/2/3/4 fill rule ------------------------
     * TL TR BR BL, clockwise. `12px 12px 0 0` is the rounded-top card, and
     * the old producer painted it rounded on all four. */
    {
        struct node *r = style(
            "<!doctype html><html><body>"
            "<div id=two>2</div><div id=three>3</div><div id=four>4</div>"
            "</body></html>",
            "#two{border-radius:1px 2px}"
            "#three{border-radius:1px 2px 3px}"
            "#four{border-radius:12px 12px 0 0}");
        struct cstyle *t2 = st_of(r, "two"), *t3 = st_of(r, "three"),
                      *t4 = st_of(r, "four");
        ck(radii_are(t2, 1, 2, 1, 2), "two values: TL,BR then TR,BL");
        ck(radii_are(t3, 1, 2, 3, 2), "three values: TL, TR+BL, BR");
        ck(radii_are(t4, 12, 12, 0, 0), "a card rounded at the TOP ONLY");
        if (!radii_are(t2, 1, 2, 1, 2)) show("two", t2);
        if (!radii_are(t3, 1, 2, 3, 2)) show("three", t3);
        if (!radii_are(t4, 12, 12, 0, 0)) show("four", t4);
    }

    /* ---- 6. the corner longhands, and a longhand after a shorthand --- */
    {
        struct node *r = style(
            "<!doctype html><html><body><div id=d>d</div></body></html>",
            "#d{border-radius:4px;border-top-left-radius:9px}");
        struct cstyle *d = st_of(r, "d");
        ck(radii_are(d, 9, 4, 4, 4),
           "border-top-left-radius overrides the shorthand before it");
        if (!radii_are(d, 9, 4, 4, 4)) show("d", d);
    }

    /* ---- 7. NUMBERS the old scan could not read ----------------------
     * It consumed decimal digits only, so `.5rem` and `0.75rem` were 0 and
     * every utility-framework rounded corner was square. */
    {
        struct node *r = style(
            "<!doctype html><html><body>"
            "<div id=em style=\"font-size:20px\">e</div>"
            "<div id=rem>r</div><div id=dec>x</div>"
            "</body></html>",
            "html{font-size:16px}"
            "#em{border-radius:0.5em}"
            "#rem{border-radius:.75rem}"
            "#dec{border-radius:2.5px}");
        struct cstyle *e = st_of(r, "em"), *rr = st_of(r, "rem"),
                      *dc = st_of(r, "dec");
        ck(radii_are(e, 10, 10, 10, 10), "0.5em of a 20px font is 10px");
        ck(radii_are(rr, 12, 12, 12, 12), ".75rem of a 16px root is 12px");
        ck(radii_are(dc, 3, 3, 3, 3), "2.5px rounds to 3 (it was 0)");
        if (!radii_are(e, 10, 10, 10, 10)) show("em", e);
        if (!radii_are(rr, 12, 12, 12, 12)) show("rem", rr);
        if (!radii_are(dc, 3, 3, 3, 3)) show("dec", dc);
    }

    /* ---- 8. percentages stay percentages ----------------------------- */
    {
        struct node *r = style(
            "<!doctype html><html><body><div id=p>p</div></body></html>",
            "#p{border-radius:50%}");
        struct cstyle *p = st_of(r, "p");
        ck(p && p->radius_pct[0] == 50 && p->radius_pct[1] == 50 &&
              p->radius_pct[2] == 50 && p->radius_pct[3] == 50 &&
              p->radius[0] == 0,
           "50% is carried as a percentage, resolved against the box at paint");
        if (p) show("p", p);
    }

    /* ---- 9. NOT INHERITED ------------------------------------------- */
    {
        struct node *r = style(
            "<!doctype html><html><body>"
            "<div id=par><div id=kid>k</div></div></body></html>",
            "#par{border-radius:16px}");
        struct cstyle *k = st_of(r, "kid");
        ck(radii_are(k, 0, 0, 0, 0), "a child of a rounded box is NOT rounded");
        if (!radii_are(k, 0, 0, 0, 0)) show("kid", k);
    }

    /* ---- 10. invalid values are REFUSED, not clamped to something ----
     * A negative radius is invalid CSS. Accepting it as 0 would swallow the
     * declaration silently; refusing it lets an earlier valid declaration
     * stand, which is what a browser does. */
    {
        struct node *r = style(
            "<!doctype html><html><body><div id=n>n</div></body></html>",
            "#n{border-radius:5px}"
            "#n{border-radius:-4px}");
        struct cstyle *n = st_of(r, "n");
        ck(radii_are(n, 5, 5, 5, 5),
           "border-radius:-4px is refused, so the earlier 5px stands");
        if (!radii_are(n, 5, 5, 5, 5)) show("n", n);
    }

    /* ---- 11. the sheet continues past it ----------------------------- */
    {
        struct node *r = style(
            "<!doctype html><html><body><div id=q>q</div></body></html>",
            "#q{border-radius:3px;color:#010203}");
        struct cstyle *q = st_of(r, "q");
        ck(q && q->color == 0x010203,
           "a declaration after border-radius in the same block still cascades");
    }

    /* ---- 12. inline style="" goes through the same door -------------- */
    {
        struct node *r = style(
            "<!doctype html><html><body>"
            "<div id=i style=\"border-radius:7px\">i</div></body></html>",
            "#i{border-radius:2px}");
        struct cstyle *i = st_of(r, "i");
        ck(radii_are(i, 7, 7, 7, 7), "an inline style= border-radius wins");
        if (!radii_are(i, 7, 7, 7, 7)) show("i", i);
    }

    /* ---- 13. THE ELLIPTICAL PAIR, and the corner CSS calls SQUARE -----
     * This painter draws a circular arc, so the (horizontal, vertical) pair
     * has to become one number, and the rule is min(). `25px 0` is a SQUARE
     * corner in CSS and rounding it would paint over a corner the author
     * filled; `40px / 10px` curves by 10, not 40. WPT's
     * css-backgrounds/border-bottom-left-radius-010 is the first of these and
     * went red when this conversion took the horizontal radius instead. */
    {
        struct node *r = style(
            "<!doctype html><html><body>"
            "<div id=z>z</div><div id=e>e</div></body></html>",
            "#z{border-bottom-left-radius:25px 0}"
            "#e{border-radius:40px / 10px}");
        struct cstyle *z = st_of(r, "z"), *e = st_of(r, "e");
        ck(radii_are(z, 0, 0, 0, 0),
           "`25px 0` is a SQUARE corner, not a 25px one");
        ck(radii_are(e, 10, 10, 10, 10),
           "`40px / 10px` curves by the SMALLER of the pair");
        if (!radii_are(z, 0, 0, 0, 0)) show("z", z);
        if (!radii_are(e, 10, 10, 10, 10)) show("e", e);
    }

    /* ---- 14. the rule nobody guesses: a COLLAPSED TABLE has no radius --
     * CSS Backgrounds 3 sec 5.5. Before border-radius existed here the engine
     * passed WPT's ttwf-reftest-borderRadius by accident, because it ignored
     * the property everywhere; implementing it is what turned an accidental
     * pass into a requirement. The `separate` twin is the control -- if the
     * branch were unconditional this pair could not tell. */
    {
        struct node *r = style(
            "<!doctype html><html><body>"
            "<table id=tc><tr><td id=cc>c</td></tr></table>"
            "<table id=ts><tr><td id=cs>s</td></tr></table>"
            "</body></html>",
            "#tc{border-collapse:collapse;border-radius:9px}"
            "#tc td{border-radius:9px}"
            "#ts{border-collapse:separate;border-radius:9px}"
            "#ts td{border-radius:9px}");
        struct cstyle *tc = st_of(r, "tc"), *cc = st_of(r, "cc");
        struct cstyle *ts = st_of(r, "ts"), *cs2 = st_of(r, "cs");
        ck(radii_are(tc, 0, 0, 0, 0), "border-collapse:collapse -> the table has no radius");
        ck(radii_are(cc, 0, 0, 0, 0), "...and neither does its cell (border-collapse inherits)");
        ck(radii_are(ts, 9, 9, 9, 9), "border-collapse:separate -> the table keeps its radius");
        ck(radii_are(cs2, 9, 9, 9, 9), "...and so does its cell");
        if (!radii_are(tc, 0, 0, 0, 0)) show("tc", tc);
        if (!radii_are(ts, 9, 9, 9, 9)) show("ts", ts);
    }

    /* ---- 15. @supports MUST NOT HAVE BECOME MORE OF A LIAR ------------
     *
     * border-radius's capability answer moved doors in this change: it used
     * to come from css_extra.c's hand-maintained logit_css_extra_supports_name
     * list (which the deletion removed the name from), and now comes from
     * supports_decl()'s FIRST_PROP..LAST_PROP walk of the property table --
     * the same table that decides whether the declaration renders. One door.
     *
     * Both directions are checked, because the failure has two shapes: a NO
     * for something we honour sends a progressive-enhancement page down its
     * fallback for no reason, and a YES for something we do not honour is the
     * worse one -- the author's fallback never runs at all.
     *
     * The four corner longhands are the part that genuinely changed answer:
     * they were absent from BOTH doors before, so @supports said no while the
     * declaration was also dropped -- consistent, and now consistent the
     * other way. */
    {
        struct node *r = style(
            "<!doctype html><html><body>"
            "<div id=s>s</div><div id=l>l</div><div id=v>v</div>"
            "</body></html>",
            "@supports (border-radius:8px){#s{color:#010203}}"
            "@supports not (border-radius:8px){#s{color:#040506}}"
            "@supports (border-top-left-radius:8px){#l{color:#010203}}"
            "@supports not (border-top-left-radius:8px){#l{color:#040506}}"
            "@supports (border-radius:bananas){#v{color:#040506}}"
            "@supports not (border-radius:bananas){#v{color:#010203}}");
        struct cstyle *s = st_of(r, "s"), *l = st_of(r, "l"), *v = st_of(r, "v");
        ck(s && s->color == 0x010203,
           "@supports (border-radius:8px) is YES, and we honour it");
        ck(l && l->color == 0x010203,
           "@supports (border-top-left-radius:8px) is YES, and we honour it");
        ck(v && v->color == 0x010203,
           "@supports (border-radius:bananas) is NO -- the VALUE is asked, not just the name");
        if (s && s->color != 0x010203) printf("       s color %06x\n", s->color);
        if (l && l->color != 0x010203) printf("       l color %06x\n", l->color);
        if (v && v->color != 0x010203) printf("       v color %06x\n", v->color);
    }

    printf("\ncss_border_radius_test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
