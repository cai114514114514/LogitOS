/* Host test for the computed-style flush -- css_ensure_styled() in
 * c/apps/browser/css_engine.c.
 *
 * WHAT IT MEASURES, AND WHY IT IS NOT tests/unit/cssom_test.c.
 *
 * cssom_test.c IS AN EMBEDDER: its main() calls css_apply() itself and
 * registers a reflow hook, exactly as browser.c's render loop does. That is
 * the right shape for testing serialisation, and it is precisely why it could
 * never have found the defect this file exists for -- it always ran the
 * cascade before it read anything.
 *
 * The defect: getComputedStyle() returned "" for EVERY property of EVERY
 * element whenever nobody had run the cascade. Not the 176 LibCSS properties
 * with no accessor wired -- all 239, including `display` on a plain <div>.
 * node->computed was NULL, because css_apply is called from browser.c's render
 * loop and from nowhere else. Any embedder that is not that loop reads back a
 * document that was never styled, and the host WPT runner is one: 10,196 css/
 * subtests failed with the signature `but got ""`.
 *
 * So THIS suite deliberately does NOT call css_apply. It builds a document,
 * opens the page, and reads. Every value below has to be produced by the
 * flush, from the three sources a cascade has here -- the UA sheet, the
 * document's own <style> elements, and inline style= attributes.
 *
 * THE EXPECTED BYTES ARE TRANSCRIBED, NOT DERIVED. `rgb(9, 8, 7)` has those
 * two spaces, `20px` keeps its unit and `50%` keeps its sign, because that is
 * what Chrome and Firefox emit and therefore what WPT recorded its
 * expectations against. Deriving a serialisation from the spec prose and
 * getting it plausibly wrong is the failure mode the second negative control
 * below is built from.
 *
 * TWO NEGATIVE CONTROLS, and `make test-csstyle-negctl` requires this suite to
 * FAIL against both:
 *
 *   -DCSSOM_NEGCTL_SERIALIZE  THE ONE THAT MATTERS. The flush is intact, the
 *                             cascade runs, every property answers a non-empty
 *                             string and nothing throws -- colours just come
 *                             back as `#090807` instead of `rgb(9, 8, 7)` and
 *                             lengths as `20` instead of `20px`. A careful,
 *                             complete-looking implementation that fails on
 *                             bytes, which is all WPT compares.
 *   -DCSS_NEGCTL_NOFLUSH      css_ensure_styled returns immediately, so every
 *                             read is "" again. This is the weaker control --
 *                             it is "remove the mechanism", which any suite
 *                             catches -- and it is here to pin which mechanism
 *                             produced the values, not to be the assertion.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "quickjs.h"
#include "dom.h"
#include "css.h"
#include "layout.h"
#include "js_dom.h"
#include "js_page.h"
#include "js_cssom.h"
#include "html_tree.h"

/* --- the kernel-only deps layout.c and the DOM reach for --- */
void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }
int text_measure(const char *s, int len, int px, int mono)
{ (void)s; (void)mono; return len * (px / 2); }
int res_fetch(const char *url, uint8_t **buf, int *len)
{ (void)url; (void)buf; (void)len; return -1; }
void img_free(struct image *o) { (void)o; }
int img_decode(const uint8_t *p, int n, struct image *out)
{ (void)p; (void)n; (void)out; return -1; }

static int fails, checks;
#define CK(c, m) do { checks++; if (!(c)) { printf("  FAIL %s\n", m); fails = 1; } \
                      else printf("  ok   %s\n", m); } while (0)

static unsigned long long g_now;
static unsigned long long clk(void) { return g_now; }

static JSContext *ctx;
static struct node *g_root;

static char *evalstr(const char *src)
{
    JSValue v = JS_Eval(ctx, src, strlen(src), "<t>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        printf("       JS exception: %s\n", m ? m : "?");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
        JS_FreeValue(ctx, v);
        return 0;
    }
    const char *s = JS_ToCString(ctx, v);
    char *out = s ? strdup(s) : 0;
    if (s) JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, v);
    return out;
}

static int eq(const char *src, const char *want)
{
    char *got = evalstr(src);
    int ok = got && !strcmp(got, want);
    if (!ok) printf("       %s\n         -> '%s'  (wanted '%s')\n",
                    src, got ? got : "<exception>", want);
    free(got);
    return ok;
}

/* ---------------------------------------------------------------- fixture */
/* Every value here is reachable only through the cascade. There is no
 * css_apply call anywhere in this file -- see the header comment. */
static const char *HTML =
"<!doctype html><html><head><style>\n"
":root { --ink: rgb(1, 2, 3); }\n"
"#styled { color: rgb(9, 8, 7); font-size: 20px; }\n"
"#pct    { width: 50%; }\n"
"#fixed  { width: 100px; border-top: 5px solid rgb(200, 100, 50); }\n"
"#alpha  { background-color: rgba(10, 20, 30, 0.5); }\n"
"#varred { color: var(--ink); }\n"
"#hid    { display: none; }\n"
"</style></head><body>\n"
"<div id='styled'></div>\n"
"<div id='pct'></div>\n"
"<div id='fixed'></div>\n"
"<div id='alpha'></div>\n"
"<div id='varred'></div>\n"
"<div id='hid'></div>\n"
"<div id='inline' style='float: left; color: rgb(4, 5, 6)'></div>\n"
"<span id='sp'></span>\n"
"<div id='mut'></div>\n"
"</body></html>\n";

#define G(id, prop) "getComputedStyle(document.getElementById('" id "'))." prop

/* -------------------------------------------- 1. a document nobody styled */
/* The regression proper. Before the flush, every one of these was "". */
static void test_unstyled_document(void)
{
    printf("1. computed values on a document no embedder ever cascaded\n");

    /* The UA sheet. `display` is as wired as a property gets -- it is one of
     * the 63 accessors that were already in place -- and it answered "" too,
     * which is what proved the property table was never the problem. */
    CK(eq(G("styled", "display"), "block"), "a div's display comes from the UA sheet");
    CK(eq(G("sp", "display"), "inline"), "a span's does too, and differs");
    CK(eq(G("hid", "display"), "none"), "an author rule overrides the UA sheet");

    /* The document's own <style>, which nobody handed us -- the flush had to
     * collect it from the DOM. */
    CK(eq(G("styled", "color"), "rgb(9, 8, 7)"), "author <style> colour is cascaded");
    CK(eq(G("styled", "fontSize"), "20px"), "and its font-size");

    /* Inline style=, which style_node parses as a LibCSS inline sheet. */
    CK(eq(G("inline", "cssFloat"), "left"), "inline style= is cascaded");
    CK(eq(G("inline", "color"), "rgb(4, 5, 6)"), "and beats nothing else here");

    /* Inheritance, which only exists if the cascade walked the tree in order
     * rather than styling one element on demand. */
    CK(eq(G("pct", "fontSize"), "16px"), "an unstyled element inherits the root font-size");

    /* var(), which reaches the cascade only if the collected sheet went
     * through css_expand_vars on the way in. */
    CK(eq(G("varred", "color"), "rgb(1, 2, 3)"), "var() in the collected sheet resolves");
}

/* ------------------------------------------------- 2. the bytes, exactly */
/* CSSOM specifies the serialisation and WPT compares bytes. These are
 * transcribed from what browsers emit. */
static void test_canonical_bytes(void)
{
    printf("2. serialisation is canonical, not merely correct\n");
    CK(eq(G("fixed", "width"), "100px"), "a length keeps its unit");
    CK(eq(G("pct", "width"), "50%"), "a percentage stays a percentage");
    CK(eq(G("fixed", "borderTopWidth"), "5px"), "border width keeps px");
    CK(eq(G("fixed", "borderTopColor"), "rgb(200, 100, 50)"),
       "rgb() with a space after each comma, never #c86432");
    CK(eq(G("fixed", "borderTopStyle"), "solid"), "border style is a keyword");
    CK(eq(G("alpha", "backgroundColor"), "rgba(10, 20, 30, 0.5)"),
       "alpha < 1 serialises as rgba() and keeps 0.5, not 0.502");

    /* The two spellings of one property, which is an IDL rule and not a CSS
     * one: both must reach the same computed value. */
    CK(eq("getComputedStyle(document.getElementById('styled'))"
          ".getPropertyValue('font-size')", "20px"),
       "getPropertyValue takes the dashed name");
    CK(eq("getComputedStyle(document.getElementById('styled'))"
          ".getPropertyValue('FONT-SIZE')", "20px"),
       "property names are ASCII case-insensitive");
}

/* ------------------------------- 3. write and read back in the SAME turn */
/* The half of this that is a real-browser bug and not a runner one. A script
 * that sets a style and reads it back before yielding used to get the previous
 * frame's cascade, because the render loop had not run yet. */
static void test_write_then_read(void)
{
    printf("3. a style written and read in one turn\n");

    CK(eq(G("mut", "color"), "rgb(0, 0, 0)"), "before: the initial colour");

    CK(evalstr("document.getElementById('mut').style.color = 'rgb(11, 22, 33)', 1") != 0,
       "the write goes through");
    CK(eq(G("mut", "color"), "rgb(11, 22, 33)"),
       "and the computed read sees it immediately -- no frame in between");

    /* Removing a declaration has to fall back through the cascade, not linger. */
    CK(evalstr("document.getElementById('mut').style.color = '', 1") != 0,
       "the declaration is removed");
    CK(eq(G("mut", "color"), "rgb(0, 0, 0)"), "and the value falls back");

    /* setProperty is the other spelling of the same write. */
    CK(evalstr("document.getElementById('mut').style"
               ".setProperty('font-size', '31px'), 1") != 0, "setProperty writes");
    CK(eq(G("mut", "fontSize"), "31px"), "and is visible to the next computed read");

    /* A class change is the case the scoped re-style path exists for: the
     * element's own attributes did not change, a selector's match did. */
    CK(evalstr("document.getElementById('mut').id = 'hid', 1") != 0, "an id changes");
    CK(eq("getComputedStyle(document.querySelector('#hid')).display", "none"),
       "and a rule that now matches takes effect on the next read");
}

/* ------------------------------------------------- 4. the flush is cheap */
/* The guard on the expensive direction. A flush that ran on every read would
 * re-cascade the document once per property a page asks for, and the only
 * evidence either way is the counter -- timing it would be a flaky test. */
static void test_flush_is_cheap(void)
{
    printf("4. an unmutated document is not re-cascaded per read\n");
    int before = css_style_flushes();
    for (int i = 0; i < 20; i++) {
        char *s = evalstr(G("styled", "color"));
        free(s);
    }
    int after = css_style_flushes();
    CK(after == before, "20 reads with nothing mutated cost 0 extra cascades");
    if (after != before)
        printf("       %d flushes for 20 reads\n", after - before);

    /* ...and the flush IS what produced the values, rather than something
     * else having run the cascade behind our back. */
    CK(before > 0, "the values above came from a flush that really ran");
}

int main(void)
{
    struct dom_doc *doc = 0;
    g_root = html_parse(&doc, HTML, (int)strlen(HTML));
    if (!g_root) { printf("FAIL: parse\n"); return 1; }

    /* DELIBERATELY ABSENT: css_apply(), css_extra_apply(), and a reflow hook.
     * This test is an embedder that never styles anything, which is the state
     * every value below has to be produced from. Adding a css_apply call here
     * would make the suite pass over the exact defect it exists to catch. */

    js_page_set_clock(clk);
    if (!js_page_open(g_root)) { printf("FAIL: js_page_open\n"); return 1; }
    ctx = js_page_ctx();

    test_unstyled_document();
    test_canonical_bytes();
    test_write_then_read();
    test_flush_is_cheap();

    js_page_close();
    dom_free(g_root);

    if (fails) printf("\ncsstyle_test: FAILURES (%d checks run)\n", checks);
    else       printf("\ncsstyle_test: ALL PASS (%d checks)\n", checks);
    return fails;
}
