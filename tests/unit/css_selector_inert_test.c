/* css_selector_inert_test -- only Selectors 4's `::-webkit-*` compatibility
 * pseudo-elements are inert; every other unknown pseudo invalidates its list.
 *
 * CORRECTION KEPT BESIDE THE OLD CLAIM. The first implementation made every
 * name missing from parsePseudo()'s lut inert, to save sibling selectors in
 * a comma list. WPT css/selectors/x-pseudo-element.html refuted that rule:
 * ordinary unknown pseudos invalidate the WHOLE selector list. Selectors 4
 * preserves only unknown pseudo-elements beginning `-webkit-`, ASCII
 * case-insensitively and with the trailing dash present. The checks below
 * hold both sides of that boundary so another corpus-recovery pass cannot
 * silently buy declarations by accepting invalid CSS again.
 *
 * WHY THIS FILE EXISTS AND THE PROBE IS NOT ENOUGH. The probe answers "did
 * LibCSS accept the selector list", which is a question about the parser.
 * It cannot answer "did the declaration reach struct cstyle", which is the
 * question that decides whether a page renders. This line's own scar is
 * `make test-wpt ONLY=css/css-grid` reading 531/11152 with AND WITHOUT an
 * entire grid implementation, because the runner linked layout.c and never
 * called it. So every assertion below goes through css_apply() +
 * css_extra_apply() on a real parsed DOM and reads the styled node.
 *
 * THE THREE PROPERTIES, AND THE THIRD IS THE ONE TO READ TWICE:
 *
 *   1. NARROW RECOVERY. A sibling selector survives only beside a
 *      compatibility `::-webkit-*` pseudo-element. Ordinary unknown pseudos
 *      invalidate the list.
 *   2. MATCHES NOTHING. The compatibility selector itself must style no
 *      element -- it parses to a detail select.c recognises nowhere and so
 *      falls to the trailing `else *match = false;` its PSEUDO_ELEMENT case
 *      already ends with. THAT IS CORRECT, NOT A STUB. Do not "fix" it into
 *      a matcher: matching nothing is what saves the rest of the selector
 *      list, and `::-webkit-scrollbar{display:none}` must hide nothing.
 *   3. NEVER MATCHES TOO MUCH. A rule that parses and styles the whole
 *      document is far worse than the drop it replaced, and there is
 *      exactly one construct that turns "matches nothing" into "matches
 *      everything": a negation. `:not(:popover-open)` would match every
 *      element on the page. It is refused whole, which is also what
 *      Selectors L4 requires (:not() is not a forgiving selector list).
 *      That case is not hypothetical -- the corpus contains
 *      `.Overlay[popover]:not(:popover-open)`, and had this been allowed
 *      to become always-true, every popover overlay on that page would
 *      have been display:none.
 *
 * Build: the test-css-selector-inert rule in tests/cssweb.mk. Its two
 * negative controls remove the webkit exception and restore the rejected
 * all-unknown-inert rule respectively; both must make this file fail.
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

static struct node *style(const char *html, const char *css)
{
    struct node *r = dom_parse(html, (int) strlen(html));
    static char ex[1 << 20];
    int n;
    if (!r) return 0;
    n = css_expand_vars(css, (int) strlen(css), ex, (int) sizeof ex);
    css_apply(r, ex, n);
    css_extra_apply(r, ex, n);
    return r;
}

static struct cstyle *st_of(struct node *root, const char *id)
{
    struct node *n = find_id(root, id);
    return n ? (struct cstyle *) n->style : 0;
}

/* #t is the sibling that must survive; #v carries the classes an
 * over-matching rule would reach; #w is a bystander with no class at all,
 * so a rule that matches EVERYTHING is caught even if it also happens to
 * name something #v has. */
#define HTML \
    "<!doctype html><html><body>" \
    "<div id=t>t</div>" \
    "<div id=v class='btn overlay'>v</div>" \
    "<div id=w>w</div>" \
    "</body></html>"

int main(void)
{
    css_init();
    css_viewport(980, 700);
    css_set_post_pass(css_extra_apply);

    /* ---- 1. Only the compatibility selector preserves its sibling ----- */

    /* The exact shape the whole change is about, and the shape every
     * component stylesheet on the web is written in. */
    {
        struct node *r = style(HTML,
            "#t, ::-webkit-scrollbar { display: flex }");
        struct cstyle *t = st_of(r, "t");
        ck(t && t->display == DISP_FLEX,
           "unknown ::-webkit-* pseudo-element preserves sibling #t");
        dom_free(r);
    }
    {
        struct node *r = style(HTML,
            "#t, :popover-open { display: flex }");
        struct cstyle *t = st_of(r, "t");
        ck(t && t->display != DISP_FLEX,
           "unknown pseudo-class invalidates the whole selector list");
        dom_free(r);
    }
    /* Functional ordinary pseudos are invalid too. Nested parens make sure
     * refusal does not desynchronise the rest of the stylesheet. */
    {
        struct node *r = style(HTML,
            "#t, :-moz-any(.a, .b:not(.c)) { display: flex }");
        struct cstyle *t = st_of(r, "t");
        ck(t && t->display != DISP_FLEX,
           "unknown functional pseudo invalidates the whole selector list");
        dom_free(r);
    }
    {
        struct node *r = style(HTML,
            "#t, ::-WeBkIt-sOmEtHiNg-NoNeXiSt123 { display: flex }");
        struct cstyle *t = st_of(r, "t");
        ck(t && t->display == DISP_FLEX,
           "the -webkit- compatibility prefix is case-insensitive");
        dom_free(r);
    }
    {
        struct node *r = style(HTML,
            "#t, ::-webkitfoo { display: flex }");
        struct cstyle *t = st_of(r, "t");
        ck(t && t->display != DISP_FLEX,
           "::-webkitfoo is invalid without the compatibility dash");
        dom_free(r);
    }
    {
        struct node *r = style(HTML,
            "#t, ::x-something-nobody-would-think-of { display: flex }");
        struct cstyle *t = st_of(r, "t");
        ck(t && t->display != DISP_FLEX,
           "non-webkit unknown pseudo-element invalidates the whole list");
        dom_free(r);
    }
    /* The rule AFTER the offending one must also survive -- "the block was
     * understood" vs "the sheet was abandoned here" look identical from one
     * assertion. */
    {
        struct node *r = style(HTML,
            "::-webkit-scrollbar-thumb { display: none }"
            "#t { display: grid }");
        struct cstyle *t = st_of(r, "t");
        ck(t && t->display == DISP_GRID,
           "the sheet continues past an unknown-pseudo rule");
        dom_free(r);
    }

    /* ---- 2. COMPATIBILITY PSEUDOS MATCH NOTHING (not a stub) ---------- */

    {
        struct node *r = style(HTML, "::-webkit-scrollbar { display: none }");
        struct cstyle *t = st_of(r, "t"), *v = st_of(r, "v"), *w = st_of(r, "w");
        ck(t && t->display != DISP_NONE, "unknown pseudo-element matches no #t");
        ck(v && v->display != DISP_NONE, "unknown pseudo-element matches no #v");
        ck(w && w->display != DISP_NONE, "unknown pseudo-element matches no #w");
        dom_free(r);
    }
    /* The next cases are ordinary-invalid, not inert. They ensure dropping
     * their rules cannot leak a declaration onto a matching compound or the
     * following rule; the comma-list checks above distinguish invalidation
     * from compatibility recovery. */
    {
        struct node *r = style(HTML, ":popover-open { display: none }");
        struct cstyle *t = st_of(r, "t"), *w = st_of(r, "w");
        ck(t && t->display != DISP_NONE, "invalid pseudo-class styles no #t");
        ck(w && w->display != DISP_NONE, "invalid pseudo-class styles no #w");
        dom_free(r);
    }
    /* Attached to a class that DOES exist: refusing the rule must not quietly
     * drop only the unknown component, which would style the wrong element. */
    {
        struct node *r = style(HTML, ".overlay:popover-open { display: none }");
        struct cstyle *v = st_of(r, "v");
        ck(v && v->display != DISP_NONE,
           "invalid .overlay:popover-open does not degrade to .overlay");
        dom_free(r);
    }
    {
        struct node *r = style(HTML, ".btn:-moz-any(.overlay) { display: none }");
        struct cstyle *v = st_of(r, "v");
        ck(v && v->display != DISP_NONE,
           "invalid functional pseudo does not degrade to .btn");
        dom_free(r);
    }

    /* ---- 3. NEVER MATCHES TOO MUCH ------------------------------------ */

    /* THE ONE UNSAFE POSITION. An always-false detail under a negation is
     * an always-TRUE selector. The corpus really contains
     * `.Overlay[popover]:not(:popover-open)`; if this refusal is ever
     * relaxed, that rule hides every overlay on the page. */
    {
        struct node *r = style(HTML, ".overlay:not(:popover-open) { display: none }");
        struct cstyle *v = st_of(r, "v");
        ck(v && v->display != DISP_NONE,
           ":not(<unknown>) is refused, not inverted into always-true");
        dom_free(r);
    }
    {
        struct node *r = style(HTML, ":not(::-webkit-scrollbar) { display: none }");
        struct cstyle *t = st_of(r, "t"), *v = st_of(r, "v"), *w = st_of(r, "w");
        ck(t && t->display != DISP_NONE &&
           v && v->display != DISP_NONE &&
           w && w->display != DISP_NONE,
           ":not(<unknown>) alone does not style the whole document");
        dom_free(r);
    }
    /* A name that IS known but is written as a function is a syntax error,
     * and must stay refused: consuming its argument would leave a detail
     * named "hover", which select.c matches for real -- turning a typo into
     * a working :hover. */
    {
        struct node *r = style(HTML, ".btn:hover(x) { display: none }");
        struct cstyle *v = st_of(r, "v");
        ck(v && v->display != DISP_NONE,
           ":hover(x) stays refused -- a known name is not made inert");
        dom_free(r);
    }

    /* ---- 4. @supports MUST NOT BECOME MORE OF A LIAR ------------------ */

    /* This change adds no capability, so `selector()` must go on answering
     * NO -- erring toward NO is the rule, because a page told YES takes the
     * modern branch and never runs its fallback. Asserted rather than
     * assumed: `selector(` is a FUNCTION token, and supports_in_parens()
     * skips and refuses every function it does not introspect; making
     * unknown pseudos PARSE is exactly the change that could have been
     * mistaken for making them SUPPORTED. */
    {
        struct node *r = style(HTML,
            "@supports selector(:popover-open){#t{display:flex}}"
            "#w{display:grid}");
        struct cstyle *t = st_of(r, "t"), *w = st_of(r, "w");
        ck(t && t->display != DISP_FLEX,
           "@supports selector(<now-parseable>) still answers NO");
        ck(w && w->display == DISP_GRID,
           "...and the sheet continues past it");
        dom_free(r);
    }
    /* The fallback branch must be the one that runs. */
    {
        struct node *r = style(HTML,
            "@supports not selector(:popover-open){#t{display:grid}}");
        struct cstyle *t = st_of(r, "t");
        ck(t && t->display == DISP_GRID,
           "@supports not selector(...) takes the fallback, as it did before");
        dom_free(r);
    }

    printf("%s: %d/%d checks\n", fails ? "FAIL" : "PASS", checks - fails, checks);
    return fails ? 1 : 0;
}
