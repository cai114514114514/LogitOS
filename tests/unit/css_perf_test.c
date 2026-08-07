/* css_perf_test: the two "once per sheet, not once per mutation" caches.
 *
 * `make bench-css` shows what they are worth; this asserts that they are
 * actually in place, and -- the half that matters more -- that they cannot
 * serve a stale stylesheet.
 *
 * WHY COUNTERS AND NOT TIMES. The obvious test for a cache is to time the
 * second call and require it to be faster. On a machine shared with other
 * agents' QEMU instances that is a coin flip, and a flaky gate teaches people
 * to re-run it rather than to read it. css_sheet_parses() and
 * css_extra_compiles() state the claim exactly and deterministically: a
 * re-style over an unchanged sheet must do NO sheet work at all.
 *
 * NEGATIVE CONTROL. Every counter assertion below fails on the pre-cache
 * engine, and fails in the informative direction -- it reports the number of
 * redundant whole-stylesheet passes the old code did. Checked by reverting
 * each cache in turn; see the numbers in the comments at each check.
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
int text_measure(const char *s, int len, int px, int mono){ (void)s;(void)mono; return len*(px/2); }
int res_fetch(const char *u, uint8_t **b, int *l){ (void)u;(void)b;(void)l; return -1; }
void img_free(struct image *o){ (void)o; }
int img_decode(const uint8_t *p, int n, struct image *o){ (void)p;(void)n;(void)o; return -1; }

static int fail;
#define CHECK(c,m) do{ if(!(c)){printf("FAIL: %s\n",m);fail=1;} else printf("ok: %s\n",m);}while(0)
#define CHECK_EQ(got,want,m) do{ long g_=(long)(got), w_=(long)(want); \
    if(g_!=w_){printf("FAIL: %s (got %ld, want %ld)\n",m,g_,w_);fail=1;} \
    else printf("ok: %s == %ld\n",m,g_);}while(0)

static const char HTML[] =
    "<html><body><div id='a' class='card'><p class='t'>one</p>"
    "<p class='t'>two</p><span class='t'>three</span></div></body></html>";

/* border-radius is css_extra's property, not LibCSS's, so it exercises the
 * compiled-rule path; the colours exercise the LibCSS cascade. */
static const char CSS_A[] =
    ".card{border-radius:8px;background:#ffffff}"
    ".t{color:#112233}"
    "@media (min-width:100px){.card{border-radius:12px}}";
static const char CSS_B[] =
    ".card{border-radius:3px;background:#ffffff}"
    ".t{color:#445566}";

static struct node *find_id(struct node *n, const char *id)
{
    if (!n) return 0;
    if (n->type == N_ELEM) { const char *v = dom_attr(n, "id"); if (v && !strcmp(v, id)) return n; }
    for (struct node *c = n->first_child; c; c = c->next) { struct node *r = find_id(c, id); if (r) return r; }
    return 0;
}
static struct node *find_class(struct node *n, const char *cls)
{
    if (!n) return 0;
    if (n->type == N_ELEM) { const char *v = dom_attr(n, "class"); if (v && strstr(v, cls)) return n; }
    for (struct node *c = n->first_child; c; c = c->next) { struct node *r = find_class(c, cls); if (r) return r; }
    return 0;
}

int main(void)
{
    css_init();
    css_viewport(760, 540);
    css_set_post_pass(css_extra_apply);

    struct node *root = dom_parse(HTML, (int)sizeof HTML - 1);
    CHECK(root != NULL, "dom_parse");

    /* ---- 1. the load: one parse, one compile ---- */
    int p0 = css_sheet_parses(), c0 = css_extra_compiles();
    css_apply(root, CSS_A, (int)sizeof CSS_A - 1);
    css_extra_apply(root, CSS_A, (int)sizeof CSS_A - 1);
    CHECK_EQ(css_sheet_parses() - p0, 1, "a load parses the author sheet once");
    CHECK_EQ(css_extra_compiles() - c0, 1, "a load compiles the extra rules once");
    CHECK(css_extra_rules() > 0, "the extra rules compiled (did not fall back to a text scan)");

    struct node *card = find_id(root, "a");
    struct node *leaf = find_class(root, "t");
    CHECK(card && leaf, "found the fixture nodes");

    /* The @media block holds at 760px wide, so the later rule wins. This is
     * the value the cache must keep reproducing. */
    CHECK_EQ(((struct cstyle *)card->style)->radius, 12, "border-radius from the active @media block");

    /* ---- 2. THE NEGATIVE CONTROL ----
     * 40 scoped re-styles over a stylesheet nobody touched. This is a live page
     * mutating one element per tick, which is the case the user is complaining
     * about.
     *
     * Without the author-sheet cache in css_engine.c this is 40 LibCSS parses
     * of the whole stylesheet. Without the compiled-rule cache in css_extra.c
     * it is another 40 full textual scans -- more, in fact, because
     * css_apply_scoped runs the post-pass once per node in scope.
     *
     * With both, it is ZERO of either. That is the entire claim. */
    int p1 = css_sheet_parses(), c1 = css_extra_compiles();
    for (int i = 0; i < 40; i++)
        css_apply_scoped(leaf, 1, CSS_A, (int)sizeof CSS_A - 1);
    CHECK_EQ(css_sheet_parses() - p1, 0, "40 scoped re-styles re-parse the sheet 0 times (was 40)");
    CHECK_EQ(css_extra_compiles() - c1, 0, "40 scoped re-styles re-compile the extra rules 0 times (was >=40)");

    /* A full re-apply over the same bytes is just as free -- browser.c does one
     * per stylesheet arrival and per script-driven whole-document fallback. */
    int p2 = css_sheet_parses(), c2 = css_extra_compiles();
    for (int i = 0; i < 5; i++) {
        css_apply(root, CSS_A, (int)sizeof CSS_A - 1);
        css_extra_apply(root, CSS_A, (int)sizeof CSS_A - 1);
    }
    CHECK_EQ(css_sheet_parses() - p2, 0, "5 full re-applies of the same sheet re-parse 0 times (was 5)");
    CHECK_EQ(css_extra_compiles() - c2, 0, "5 full re-applies of the same sheet re-compile 0 times (was 5)");

    /* ...and produced the same answer, which is the point of caching rather
     * than skipping. */
    CHECK_EQ(((struct cstyle *)card->style)->radius, 12, "the cached sheet still yields radius 12");

    /* ---- 3. THE DANGEROUS DIRECTION: a changed sheet must NOT be cached ----
     * A cache keyed on the wrong thing does not render slowly, it renders the
     * previous page. CSS_B has the same shape and a different value, and
     * browser.c hands the expanded sheet back in the SAME buffer every time --
     * so pointer identity would say "unchanged" here and be wrong. */
    static char buf[512];
    int blen = (int)sizeof CSS_B - 1;
    memcpy(buf, CSS_B, blen);
    int p3 = css_sheet_parses(), c3 = css_extra_compiles();
    css_apply(root, buf, blen);
    css_extra_apply(root, buf, blen);
    CHECK_EQ(css_sheet_parses() - p3, 1, "a CHANGED sheet re-parses");
    CHECK_EQ(css_extra_compiles() - c3, 1, "a CHANGED sheet re-compiles");
    CHECK_EQ(((struct cstyle *)card->style)->radius, 3, "and the new border-radius took effect");
    CHECK_EQ(((struct cstyle *)leaf->style)->color, 0x445566, "and the new colour took effect");

    /* Same LENGTH, different bytes: the case a length-only key would miss.
     * CSS_B with one digit changed is byte-for-byte the same size. */
    char *r3 = strstr(buf, "border-radius:3px");
    CHECK(r3 != NULL, "found the radius declaration to mutate in place");
    r3[14] = '7';
    int p4 = css_sheet_parses();
    css_apply(root, buf, blen);
    css_extra_apply(root, buf, blen);
    CHECK_EQ(css_sheet_parses() - p4, 1, "a same-length, different-bytes sheet re-parses");
    CHECK_EQ(((struct cstyle *)card->style)->radius, 7, "and that one-character edit reached the style");

    /* ---- 4. the @media key: same sheet, different viewport ----
     * css_extra resolves @media at COMPILE time, so a rule list compiled for
     * one viewport must not be reused at another. At 50px wide the
     * (min-width:100px) block no longer holds and the radius falls back to 8. */
    memcpy(buf, CSS_A, (int)sizeof CSS_A - 1);
    blen = (int)sizeof CSS_A - 1;
    css_apply(root, buf, blen);
    css_extra_apply(root, buf, blen);
    CHECK_EQ(((struct cstyle *)card->style)->radius, 12, "at 760px the @media block holds");

    css_viewport(50, 540);
    int c5 = css_extra_compiles();
    css_apply(root, buf, blen);
    css_extra_apply(root, buf, blen);
    CHECK_EQ(css_extra_compiles() - c5, 1, "a viewport change re-compiles the extra rules");
    CHECK_EQ(((struct cstyle *)card->style)->radius, 8, "and the @media block correctly stopped applying");

    dom_free(root);
    printf(fail ? "\nCSS PERF TEST FAILED\n" : "\nCSS PERF TEST PASSED\n");
    return fail;
}
