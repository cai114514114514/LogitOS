/* Host test for the CSSOM (c/apps/browser/js_cssom.c + css_engine.c's
 * serialisation and CSS.supports).
 *
 * WHY IT LINKS layout.c, when js_dom_test.c deliberately does not: every
 * number in CSSOM-View comes OUT of the display list, so a CSSOM-View test
 * against a build with no layout asserts that zero equals zero. It would pass
 * over an implementation that returns a constant 0, which is the exact
 * implementation the host WPT runner has (that runner links no layout, so
 * there the revived files complete and then fail -- see tests/cssom.mk). This
 * is therefore the ONLY place the geometry is actually measured, and the
 * numbers below are checked against the boxes the painter would draw.
 *
 * The assertions are written as `throw` inside the page's own JS wherever the
 * property under test is a JS-visible one, so the expected value sits next to
 * the expression that produced it instead of in a C mirror of the same logic.
 *
 * TWO NEGATIVE CONTROLS, both built from this same file by tests/cssom.mk:
 *   -DCSSOM_NEGCTL_SERIALIZE  colours serialise as #rrggbb and lengths lose
 *                             their unit. Nothing is missing, nothing throws,
 *                             only the bytes are wrong -- which is all WPT
 *                             ever compares, and the failure mode a weak test
 *                             sails straight past.
 *   -DCSSOM_NEGCTL_NOGEOM     the geometry accessors never flush layout, so
 *                             they all answer 0.
 * `make test-cssom-negctl` requires this suite to FAIL under both.
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

/* --- the kernel-only deps layout.c and the DOM reach for --- */
void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }
/* A deterministic text metric: layout must be reproducible, and a real font
 * backend would make every number below depend on which TTF is on the disk. */
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
/* One document for the whole run. #box's numbers are chosen so that every
 * box-model edge is a DIFFERENT value -- a fixture with equal padding and
 * border cannot tell clientWidth from offsetWidth minus the wrong one. */
static const char *HTML =
"<!doctype html><html><head><style>\n"
"#box { width: 300px; height: 40px; padding: 10px; border: 5px solid #ff0000;\n"
"       background: #123456; color: #0000ff; }\n"
"#gone { display: none; }\n"
"#rel { position: relative; }\n"
"@media (min-width: 50px) { #box { font-size: 20px; } }\n"
"</style></head>\n"
"<body id='b' onload=\"document.body.setAttribute('data-onload','ran')\">\n"
"<div id='box'>content</div>\n"
"<div id='gone'>invisible</div>\n"
"<div id='rel'><div id='kid' style='background:#00ff00;width:20px;height:8px'>k</div></div>\n"
"</body></html>\n";

/* ------------------------------------------------------- 1. the load gate */
/* The single largest cause of dead CSS files in the corpus: `<body onload>` is
 * a handler on the WINDOW, and until this file reflected it, a test whose
 * entire body is `<body onload="checkLayout('.target')">` registered no tests
 * at all and timed out reporting nothing. */
static void test_body_onload(void)
{
    printf("1. <body onload> is a window handler\n");
    CK(eq("String(document.body.getAttribute('data-onload'))", "null"),
       "before the load event, the body's onload attribute has not run");
    CK(evalstr("window.dispatchEvent(new Event('load')), 1") != 0,
       "window load dispatches");
    CK(eq("String(document.body.getAttribute('data-onload'))", "ran"),
       "the body's onload= ran when the WINDOW got its load event");
    /* Once, not twice. A second checkLayout() call is not a smaller bug than
     * zero: WPT rejects a duplicate subtest name. */
    CK(eq("(document.body.setAttribute('data-onload','x'),"
          " window.dispatchEvent(new Event('load')),"
          " document.body.getAttribute('data-onload'))", "x"),
       "and exactly once -- a second load event does not re-run it");
}

/* ------------------------------------------------------- 2. CSSOM-View */
static void test_geometry(void)
{
    printf("2. CSSOM-View geometry, against the boxes layout really produced\n");
    /* content 300x40 + padding 10 all round + border 5 all round. */
    CK(eq("document.getElementById('box').offsetWidth", "330"),
       "offsetWidth is the BORDER box: 300 content + 20 padding + 10 border");
    CK(eq("document.getElementById('box').offsetHeight", "70"),
       "offsetHeight likewise: 40 + 20 + 10");
    CK(eq("document.getElementById('box').clientWidth", "320"),
       "clientWidth is the PADDING box: border box less the two borders");
    CK(eq("document.getElementById('box').clientHeight", "60"), "clientHeight likewise");
    CK(eq("document.getElementById('box').clientLeft", "5"),
       "clientLeft is the left border width");
    CK(eq("document.getElementById('box').clientTop", "5"), "clientTop likewise");
    CK(eq("document.getElementById('box').getBoundingClientRect().width", "330"),
       "getBoundingClientRect().width agrees with offsetWidth");
    CK(eq("document.getElementById('box').getBoundingClientRect().height", "70"),
       "... and .height with offsetHeight -- one box, not two answers");
    CK(eq("document.getElementById('gone').offsetWidth", "0"),
       "display:none has no box, so every number on it is 0");
    CK(eq("String(document.getElementById('gone').offsetParent)", "null"),
       "and its offsetParent is null, not the body");
    CK(eq("document.getElementById('kid').offsetParent.id", "rel"),
       "offsetParent is the nearest POSITIONED ancestor, not the parent");
    CK(eq("String(document.body.offsetParent)", "null"),
       "the body has no offsetParent");
    CK(eq("document.getElementById('box').getClientRects().length >= 1", "true"),
       "getClientRects returns at least the element's own fragment");
    CK(eq("typeof document.getElementById('box').getClientRects().item", "function"),
       "and it is a DOMRectList: item() is there");
    CK(eq("document.getElementById('box').scrollWidth >= "
          "document.getElementById('box').clientWidth", "true"),
       "scrollWidth is never smaller than clientWidth");

    /* scrollTop is remembered even though nothing scrolls: a page that writes
     * it and reads back 0 retries forever. */
    CK(eq("(document.getElementById('box').scrollTop = 17,"
          " document.getElementById('box').scrollTop)", "17"),
       "a written scrollTop reads back (nothing scrolls, but the value is kept)");

    /* DOMRect's own behaviour: a negative extent normalises. */
    CK(eq("new DOMRect(10, 10, -4, -6).left + ',' + new DOMRect(10, 10, -4, -6).top",
          "6,4"),
       "DOMRect normalises a negative width/height into left/top");
}

/* --------------------------------------------------- 3. reflow accounting */
/* The claim is "a geometry read flushes a pending layout" -- which is only
 * worth anything if it ALSO does not flush when nothing changed. A counter
 * asserts both directions; a timing measurement could assert neither. */
static void test_reflow(void)
{
    printf("3. a geometry read flushes layout -- once per mutation, not per read\n");
    /* THE EMBEDDER CLEARS THE DIRTY FLAG. That is js_dom.c's contract with
     * browser.c, which does it once a frame, and this test is the embedder
     * here -- so it does the same thing at the point a frame boundary would.
     * Without the clear the flag is sticky, every read looks like a fresh
     * mutation, and the assertion below would be measuring the test's own
     * omission rather than the code. */
    js_dom_clear_dirty();
    int before = js_cssom_layouts();
    (void)evalstr("var e = document.getElementById('box');"
                  "e.offsetWidth; e.offsetHeight; e.clientWidth; e.scrollHeight; 1");
    CK(js_cssom_layouts() == before,
       "four reads with nothing mutated cost zero reflows");
    (void)evalstr("document.getElementById('box').style.width = '100px'; 1");
    (void)evalstr("document.getElementById('box').offsetWidth");
    CK(js_cssom_layouts() == before + 1,
       "a read after a style mutation costs exactly one reflow");
    (void)evalstr("document.getElementById('box').offsetHeight");
    CK(js_cssom_layouts() == before + 1,
       "and the next read in the same episode costs none");
    /* And the reflow was not a no-op: the new width has to be visible. 100
     * content + 20 padding + 10 border. */
    CK(eq("document.getElementById('box').offsetWidth", "130"),
       "the flushed layout is the NEW one, not the old display list");
}

/* ------------------------------------------- 4. computed-value SERIALISATION */
/* The half a wrong implementation passes. WPT compares STRINGS: a correct
 * colour serialised as #0000ff is a failure, and so is a correct length
 * serialised without its unit. */
static void test_serialization(void)
{
    printf("4. getComputedStyle serialises the way CSS says\n");
    CK(eq("getComputedStyle(document.getElementById('box')).color", "rgb(0, 0, 255)"),
       "a colour is rgb(r, g, b) with the spaces -- not #0000ff");
    CK(eq("getComputedStyle(document.getElementById('box')).paddingTop", "10px"),
       "a length carries its unit: 10px, not 10");
    CK(eq("getComputedStyle(document.getElementById('box')).getPropertyValue('padding-top')",
          "10px"),
       "and the dashed spelling gets the same answer as the IDL one");
    CK(eq("getComputedStyle(document.getElementById('box')).fontSize", "20px"),
       "an @media block that HOLDS took part in the cascade (font-size: 20px)");
    CK(eq("getComputedStyle(document.getElementById('box')).display", "block"),
       "a keyword is the keyword");
    CK(eq("getComputedStyle(document.getElementById('box')).borderTopStyle", "solid"),
       "a per-edge border style survives the shorthand");
    CK(eq("getComputedStyle(document.getElementById('box')).borderTopWidth", "5px"),
       "and its width carries px");
}

/* ------------------------------------------------------ 5. the CSS object */
static void test_css_object(void)
{
    printf("5. CSS.supports / CSS.escape\n");
    CK(eq("typeof CSS", "object"), "the CSS namespace object exists at all");
    CK(eq("CSS.supports('color', 'red')", "true"), "supports a real declaration");
    CK(eq("CSS.supports('color', 'banana-fritter')", "false"),
       "and REFUSES a bad VALUE for a real property -- the case a name table gets wrong");
    CK(eq("CSS.supports('not-a-real-property', '1px')", "false"),
       "and a property that does not exist");
    CK(eq("CSS.supports('(color: red)')", "true"),
       "the one-argument condition form");
    CK(eq("CSS.supports('(color: banana-fritter)')", "false"),
       "... which refuses the same bad value");
    CK(eq("CSS.supports('--x', 'anything at all')", "true"),
       "a custom property takes any balanced value, by definition");

    CK(eq("CSS.escape('a#b')", "a\\#b"), "escape: a bare punctuation char is backslashed");
    CK(eq("CSS.escape('1a')", "\\31 a"),
       "a leading digit becomes a hex escape with its terminating space");
    CK(eq("CSS.escape('-')", "\\-"), "a lone hyphen is escaped");
    CK(eq("CSS.escape('hello')", "hello"), "an ordinary identifier is untouched");
    CK(eq("CSS.escape('a b')", "a\\ b"), "a space is escaped, not dropped");
}

/* ---------------------------------------------------- 6. document.styleSheets */
static void test_stylesheets(void)
{
    printf("6. document.styleSheets and the CSSRule tree\n");
    CK(eq("document.styleSheets.length", "1"), "the document's one <style> is one sheet");
    CK(eq("document.styleSheets[0].cssRules.length", "4"),
       "four rules: #box, #gone, #rel and the @media block");
    CK(eq("document.styleSheets[0].cssRules[0].type", "1"),
       "a style rule's type is CSSRule.STYLE_RULE (1)");
    CK(eq("document.styleSheets[0].cssRules[0].selectorText", "#box"),
       "selectorText is the selector the AUTHOR wrote -- which LibCSS does not keep");
    CK(eq("document.styleSheets[0].cssRules[0].style.getPropertyValue('width')", "300px"),
       "a rule's declaration block reads back by name");
    CK(eq("document.styleSheets[0].cssRules[0].style.length >= 5", "true"),
       "and knows how many declarations it holds");
    CK(eq("document.styleSheets[0].cssRules[3].type", "4"),
       "the @media block is CSSRule.MEDIA_RULE (4)");
    CK(eq("document.styleSheets[0].cssRules[3].conditionText", "(min-width: 50px)"),
       "with its condition text");
    CK(eq("document.styleSheets[0].cssRules[3].cssRules.length", "1"),
       "and its own nested rule list");
    CK(eq("document.styleSheets[0].cssRules[3].cssRules[0].selectorText", "#box"),
       "whose rules are real rules");
    CK(eq("document.styleSheets[0].cssRules[3].matches", "true"),
       "and whose @media verdict comes from the cascade's own evaluator");
    CK(eq("document.styleSheets[0].rules === document.styleSheets[0].cssRules", "true"),
       ".rules is the same list, not a copy");

    /* insertRule has to change the SHEET, not just the view of it -- otherwise
     * the CSSOM and the cascade disagree from the next frame on. */
    CK(eq("document.styleSheets[0].insertRule('#gone { color: lime }', 0)", "0"),
       "insertRule returns the index it inserted at");
    CK(eq("document.styleSheets[0].cssRules.length", "5"), "the list grew");
    CK(eq("document.styleSheets[0].cssRules[0].selectorText", "#gone"),
       "at the position asked for");
    CK(eq("(function(){var s=document.querySelector('style');"
          " return s.textContent.indexOf('color: lime') >= 0;})()", "true"),
       "and the <style> element's TEXT changed, so the next cascade sees it too");
    CK(eq("(document.styleSheets[0].deleteRule(0),"
          " document.styleSheets[0].cssRules.length)", "4"),
       "deleteRule removes it again");
    CK(eq("(function(){try{document.styleSheets[0].deleteRule(99);return 'no throw';}"
          "catch(e){return e.name;}})()", "RangeError"),
       "an out-of-range deleteRule throws rather than corrupting the list");
}

/* --------------------------------------------------------- 7. matchMedia */
static void test_matchmedia(void)
{
    printf("7. matchMedia, over the cascade's own evaluator\n");
    CK(eq("matchMedia('(min-width: 1px)').matches", "true"), "a query that holds");
    CK(eq("matchMedia('(min-width: 999999px)').matches", "false"), "one that does not");
    CK(eq("matchMedia('(min-width: 1px)').media", "(min-width: 1px)"),
       ".media echoes the query");
    CK(eq("typeof matchMedia('all').addListener", "function"),
       "the legacy listener surface is present (inert, but present)");
    /* The property that matters: the same verdict as the cascade. Asking both
     * about the SAME query is the whole point -- two evaluators fail by
     * disagreeing, not by being approximate. */
    CK(eq("matchMedia('(min-width: 50px)').matches === "
          "document.styleSheets[0].cssRules[3].matches", "true"),
       "matchMedia and the @media rule agree, because they are one evaluator");
}

/* -------------------------------------------------------- 8. document.fonts */
static void test_fonts(void)
{
    printf("8. document.fonts -- the gate 165 corpus files die on\n");
    CK(eq("typeof document.fonts", "object"), "document.fonts exists");
    CK(eq("typeof document.fonts.ready.then", "function"), "and .ready is a thenable");
    CK(eq("document.fonts.status", "loaded"),
       "status is loaded: fonts come off the disk at boot, nothing is in flight");
    CK(eq("typeof FontFace", "function"), "FontFace is constructible");
    CK(eq("new FontFace('X', 'url(x.ttf)').family", "X"), "and keeps its family");
}

/* The embedder's reflow: cascade, then layout. See js_cssom_set_reflow(). */
static char g_sheet[4096];
static int  g_sheetlen;
static void reflow(void)
{
    css_apply(g_root, g_sheet, g_sheetlen);
    layout_page(g_root, 800);
}

int main(void)
{
    printf("cssom_test: the CSSOM against a real cascade and a real layout\n");
#ifdef CSSOM_NEGCTL_SERIALIZE
    printf("  [negative control: serialisation sabotaged]\n");
#endif
#ifdef CSSOM_NEGCTL_NOGEOM
    printf("  [negative control: geometry sabotaged]\n");
#endif

    g_root = dom_parse(HTML, (int)strlen(HTML));
    if (!g_root) { printf("FAIL: fixture did not parse\n"); return 1; }

    css_init();
    css_viewport(800, 600);

    /* The page's own <style>, handed to the cascade the way browser.c does. */
    char *sheet = g_sheet;
    int slen = 0;
    {
        struct node *st = 0, *stack[256]; int sp = 0;
        stack[sp++] = g_root;
        while (sp) {
            struct node *n = stack[--sp];
            if (n->type == N_ELEM && !strcmp(n->tag, "style")) { st = n; break; }
            for (struct node *c = n->first_child; c; c = c->next)
                if (sp < 256) stack[sp++] = c;
        }
        if (st)
            for (struct node *c = st->first_child; c; c = c->next)
                if (c->type == N_TEXT && c->text && slen + c->textlen < (int)sizeof g_sheet) {
                    memcpy(sheet + slen, c->text, (size_t)c->textlen);
                    slen += c->textlen;
                }
    }
    sheet[slen] = 0;
    g_sheetlen = slen;

    css_apply(g_root, sheet, slen);
    layout_page(g_root, 800);

    /* What a reflow means here. The embedder owns this (see js_cssom.h): a
     * geometry read on a mutated document must re-run the CASCADE and then
     * layout, and only the embedder knows the document's stylesheet set. This
     * test is the embedder, so it registers the same two calls it made above.
     * Without it, `el.style.width = ...` followed by a measurement would read
     * the previous cascade's boxes -- which the last assertion in
     * test_reflow() is there to catch. */
    js_cssom_set_reflow(reflow);

    js_page_set_clock(clk);
    if (!js_page_open(g_root)) { printf("FAIL: js_page_open\n"); return 1; }
    ctx = js_page_ctx();

    test_body_onload();
    test_serialization();
    test_css_object();
    test_stylesheets();
    test_matchmedia();
    test_fonts();
    test_geometry();
    test_reflow();

    js_page_close();
    dom_free(g_root);

    if (fails) printf("\ncssom_test: FAILURES (%d checks run)\n", checks);
    else       printf("\ncssom_test: ALL PASS (%d checks)\n", checks);
    return fails;
}
