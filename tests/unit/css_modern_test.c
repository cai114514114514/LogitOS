/* css_modern_test: the constructs a 2020s stylesheet is written in.
 *
 * These are not "more properties". They are the three ways the corpus in
 * tests/fixtures/cssweb loses a WHOLE STYLESHEET rather than a declaration, plus
 * the one property family that silently moves every box on a modern page. Each
 * assertion here corresponds to a measured line in `make audit-css`:
 *
 *   @layer      tailwind.com wraps all 692 KiB of its CSS in five @layer blocks.
 *               An at-rule LibCSS does not know is discarded WITH ITS BLOCK, so
 *               the page had 0 flex containers and 0 grid containers. With the
 *               group-rule patch (third_party/css/libcss/src/parse/language.c)
 *               it has 153 and 72, and 69% of its elements are inside a flex
 *               container instead of 0%.
 *   @supports   7 of the 15 corpus pages use it; tailwind alone 655 times.
 *               Evaluated for real against the property table and the value
 *               handlers, because answering `true` unconditionally would enable
 *               every `@supports not (...)` FALLBACK block at the same time as
 *               the modern one -- both branches of a feature test at once.
 *   @container  70 uses on tailwind, 23 on github. Not evaluable without used
 *               sizes; entered, because the modern branch is the intended one.
 *   logical     margin-inline-start and family, 460+ declarations over 6 pages.
 *               A lost margin moves text; this is not cosmetic.
 *
 * The shape of every test is the same and it is the shape that matters: a rule
 * placed AFTER the construct must still reach the cascade. That is what
 * separates "the block was understood" from "the block was skipped without
 * taking the rest of the sheet with it", and only the first one renders a page.
 *
 * Build: the test-css-modern rule in tests/cssweb.mk.
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

/* Style one fixture and hand back the document. Caller keeps it alive while it
 * reads styles out of it (cstyle lives on the nodes). */
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

#define HTML \
    "<!doctype html><html><body>" \
    "<div id=inside>in</div><div id=after>after</div><div id=other>o</div>" \
    "</body></html>"

int main(void)
{
    css_init();
    css_viewport(980, 700);
    css_set_post_pass(css_extra_apply);

    /* ---------------- @layer ---------------------------------------------
     * The block's rules take part in the cascade, AND -- the half that was
     * actually costing whole pages -- the sheet continues afterwards. */
    {
        struct node *r = style(HTML,
            "@layer base, utilities;"
            "@layer base{#inside{display:flex}}"
            "#after{display:grid}");
        struct cstyle *a = st_of(r, "inside"), *b = st_of(r, "after");
        ck(a && a->display == DISP_FLEX, "@layer: its rules cascade");
        ck(b && b->display == DISP_GRID, "@layer: the sheet continues after the block");
        dom_free(r);
    }

    /* Nested inside @media, which is how a real sheet ships it. */
    {
        struct node *r = style(HTML,
            "@media (min-width:100px){@layer u{#inside{display:flex}}}"
            "#after{display:grid}");
        struct cstyle *a = st_of(r, "inside"), *b = st_of(r, "after");
        ck(a && a->display == DISP_FLEX, "@layer inside a matching @media cascades");
        ck(b && b->display == DISP_GRID, "...and the sheet continues");
        dom_free(r);
    }

    /* A NON-matching @media must still suppress the layer inside it: making
     * the group rule transparent must not make it transparent to its parent's
     * condition too. */
    {
        struct node *r = style(HTML,
            "@media (min-width:5000px){@layer u{#inside{display:flex}}}"
            "#after{display:grid}");
        struct cstyle *a = st_of(r, "inside"), *b = st_of(r, "after");
        ck(a && a->display != DISP_FLEX,
           "@layer inside a FAILING @media does not cascade");
        ck(b && b->display == DISP_GRID, "...and the sheet still continues");
        dom_free(r);
    }

    /* ---------------- @supports, evaluated ---------------------------------
     * The whole point of evaluating rather than assuming: the true branch and
     * the `not` branch must not both apply. */
    {
        struct node *r = style(HTML,
            "@supports (display:grid){#inside{display:grid}}"
            "@supports not (display:grid){#inside{display:flex}}"
            "#after{display:grid}");
        struct cstyle *a = st_of(r, "inside"), *b = st_of(r, "after");
        ck(a && a->display == DISP_GRID,
           "@supports (display:grid) is TRUE, and its `not` twin is not applied");
        ck(b && b->display == DISP_GRID, "@supports: the sheet continues after it");
        dom_free(r);
    }
    {
        /* A property this engine really does not have. The fallback branch is
         * the one that must win -- this is the arm that a blanket `true` gets
         * exactly backwards. */
        struct node *r = style(HTML,
            "@supports (backdrop-filter:blur(4px)){#inside{display:grid}}"
            "@supports not (backdrop-filter:blur(4px)){#inside{display:flex}}");
        struct cstyle *a = st_of(r, "inside");
        ck(a && a->display == DISP_FLEX,
           "@supports for a property we lack is FALSE, so the fallback applies");
        dom_free(r);
    }
    {
        struct node *r = style(HTML,
            "@supports (display:grid) and (color:red){#inside{display:grid}}"
            "@supports (display:grid) and (backdrop-filter:blur(1px)){#after{display:flex}}"
            "@supports (backdrop-filter:blur(1px)) or (display:grid){#other{display:flex}}");
        struct cstyle *a = st_of(r, "inside"), *b = st_of(r, "after"), *c = st_of(r, "other");
        ck(a && a->display == DISP_GRID, "@supports `and` of two true arms is true");
        ck(b && b->display != DISP_FLEX, "@supports `and` with a false arm is false");
        ck(c && c->display == DISP_FLEX, "@supports `or` with one true arm is true");
        dom_free(r);
    }
    {
        /* A value the handler refuses, with a property it knows: the test is
         * about the declaration, not just the name. */
        struct node *r = style(HTML,
            "@supports (display:sideways-lr){#inside{display:flex}}"
            "@supports not (display:sideways-lr){#after{display:flex}}");
        struct cstyle *a = st_of(r, "inside"), *b = st_of(r, "after");
        ck(a && a->display != DISP_FLEX,
           "@supports asks the VALUE handler, not only the property name");
        ck(b && b->display == DISP_FLEX, "...so its `not` twin applies");
        dom_free(r);
    }

    /* ---------------- @container ------------------------------------------ */
    {
        struct node *r = style(HTML,
            "@container (min-width:10px){#inside{display:flex}}"
            "#after{display:grid}");
        struct cstyle *a = st_of(r, "inside"), *b = st_of(r, "after");
        ck(a && a->display == DISP_FLEX, "@container: its rules cascade");
        ck(b && b->display == DISP_GRID, "@container: the sheet continues after it");
        dom_free(r);
    }

    /* ---------------- logical properties ----------------------------------
     * Resolved to physical edges for LTR / horizontal-tb, which is the only
     * writing mode this engine has. */
    {
        struct node *r = style(HTML,
            "#inside{margin-inline-start:12px;margin-inline-end:4px;"
            "padding-block-start:6px;padding-block-end:2px}"
            "#after{margin-inline:9px;padding-inline:3px 7px}"
            "#other{position:absolute;inset:1px 2px 3px 4px}");
        struct cstyle *a = st_of(r, "inside"), *b = st_of(r, "after"), *c = st_of(r, "other");
        ck(a && a->ml == 12 && a->mr == 4, "margin-inline-start/end -> left/right");
        ck(a && a->pt == 6 && a->pb == 2, "padding-block-start/end -> top/bottom");
        ck(b && b->ml == 9 && b->mr == 9, "margin-inline: one value sets both");
        ck(b && b->pl == 3 && b->pr == 7, "padding-inline: two values are start then end");
        ck(c && c->has_top && c->top == 1 && c->right == 2 && c->bottom == 3 && c->left == 4,
           "inset: the physical 1-4 shorthand order, with has_* set");
        dom_free(r);
    }
    {
        /* A logical longhand must beat the shorthand that precedes it, and a
         * value we cannot turn into pixels (auto, a percentage) must leave the
         * edge alone rather than write a zero. */
        struct node *r = style(HTML,
            "#inside{margin-inline:20px;margin-inline-start:5px}"
            "#after{margin-left:11px;margin-inline-start:auto}");
        struct cstyle *a = st_of(r, "inside"), *b = st_of(r, "after");
        ck(a && a->ml == 5 && a->mr == 20,
           "a logical longhand overrides the shorthand before it");
        ck(b && b->ml == 11,
           "a logical value we cannot resolve leaves the physical one intact");
        dom_free(r);
    }
    {
        /* Inline style= goes through the same pass. */
        struct node *r = style(
            "<!doctype html><html><body>"
            "<div id=inside style=\"margin-inline-start:14px\">x</div>"
            "<div id=after>y</div></body></html>", "");
        struct cstyle *a = st_of(r, "inside");
        ck(a && a->ml == 14, "logical properties work in an inline style= too");
        dom_free(r);
    }

    printf("\ncss_modern_test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
