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
"<body id='b' onload=\"document.body.setAttribute('data-onload','ran');\n"
"  window.__thisWasWindow = (this === window);\n"
"  window.__order = (window.__order || '') + 'body';\">\n"
"<div id='box'>content</div>\n"
"<div id='gone'>invisible</div>\n"
"<div id='rel'><div id='kid' style='background:#00ff00;width:20px;height:8px'>k</div></div>\n"
/* A real scrolling box, and the styles are INLINE on purpose: the sheet above
 * is counted rule-by-rule by the CSSOM tests further down, so a fixture that
 * needs a new box must not add a rule to it. An element with overflow:visible
 * is not a scrolling box at all and its scroll position is permanently 0;
 * this one has content taller than itself, so it has somewhere to go. */
"<div id='scroller' style='width:100px;height:30px;overflow:auto;background:#eeeeee'>\n"
"  <div style='width:400px;height:400px;background:#dddddd'>tall</div></div>\n"
"</body></html>\n";

/* ------------------------------------------------------- 1. the load gate */
/* The single largest cause of dead CSS files in the corpus: `<body onload>` is
 * a handler on the WINDOW, and until this file reflected it, a test whose
 * entire body is `<body onload="checkLayout('.target')">` registered no tests
 * at all and timed out reporting nothing. */
static void test_body_onload(void)
{
    printf("1. <body onload> is a window handler, and a real IDL attribute\n");
    CK(eq("String(document.body.getAttribute('data-onload'))", "null"),
       "before the load event, the body's onload attribute has not run");

    /* IT IS A PROPERTY, and that is not decoration. A handler ATTRIBUTE is an
     * IDL attribute: readable, assignable, the same one under both names. The
     * first version ran the attribute out of a window listener and published
     * nothing, so window.onload read back null -- which is exactly what broke
     * the chaining case below. */
    CK(eq("typeof window.onload", "function"),
       "window.onload reads back the compiled body attribute, not null");
    CK(eq("document.body.onload === window.onload", "true"),
       "document.body.onload is the SAME handler -- one attribute, two names");
    CK(eq("String(window.onload).indexOf('data-onload') >= 0", "true"),
       "and it stringifies to the attribute's own source");
    CK(eq("String(window.onhashchange)", "null"),
       "a Windows handler the body does NOT carry reads null, not undefined");

    /* THE LEGACY CHAINING IDIOM, measured at ZERO executions before this.
     *
     * `var old = window.onload; window.onload = wrapper;` is everywhere on
     * older sites. The old code bailed whenever it found a function in
     * window.onload -- and the wrapper IS a function -- so the body's handler
     * was silently dropped: no exception, no log, the page just did less. */
    CK(evalstr("window.__chain = [];"
               "var old = window.onload;"
               "window.onload = function (e) {"
               "  window.__order = (window.__order || '') + 'wrap';"
               "  window.__chain.push('wrapper');"
               "  if (old) old.call(this, e);"
               "}; 1") != 0,
       "a page can read the body handler out and wrap it");

    CK(evalstr("window.dispatchEvent(new Event('load')), 1") != 0,
       "window load dispatches");
    CK(eq("window.__chain.join(',')", "wrapper"), "the wrapper ran");
    CK(eq("String(document.body.getAttribute('data-onload'))", "ran"),
       "and the body's own handler ran THROUGH it -- chaining does not drop it");
    CK(eq("window.__order", "wrapbody"),
       "in that order: the wrapper is the handler, the body's is what it calls");

    /* `this` is the Window. The body element is the obvious wrong answer, and
     * it is the one the first version gave. */
    CK(eq("String(window.__thisWasWindow)", "true"),
       "`this` inside the handler is the Window, not the body element");

    /* Once, not twice. A second checkLayout() call is not a smaller bug than
     * zero: WPT rejects a duplicate subtest name. See the guard's own comment
     * in js_cssom.c for why it is still wider than it should be, and what it
     * is waiting on. */
    CK(eq("(document.body.setAttribute('data-onload','x'),"
          " window.dispatchEvent(new Event('load')),"
          " document.body.getAttribute('data-onload'))", "x"),
       "and exactly once -- a second load event does not re-run it");
    CK(eq("window.__chain.length", "2"),
       "while the page's OWN wrapper is not once-guarded and runs again");
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
     * it and reads back 0 retries forever.
     *
     * BUT ONLY ON A SCROLLING BOX. #box has overflow:visible, so it is not a
     * scrolling box at all and its scroll position is 0 by definition -- the
     * check used to be written on #box and expected 17, which is the answer
     * no browser gives. The retry-loop argument that put it there is about
     * boxes that CAN scroll, and that is #scroller. */
    CK(eq("(document.getElementById('scroller').scrollTop = 17,"
          " document.getElementById('scroller').scrollTop)", "17"),
       "a written scrollTop reads back on a scrolling box");
    CK(eq("(document.getElementById('box').scrollTop = 17,"
          " document.getElementById('box').scrollTop)", "0"),
       "and stays 0 on overflow:visible, which has no scrolling box to move");
    CK(eq("(document.getElementById('scroller').scrollTop = -5,"
          " document.getElementById('scroller').scrollTop)", "0"),
       "a negative scroll position clamps to 0");

    /* The scrolling METHODS. Element.scroll/scrollTo/scrollBy and
     * scrollIntoView did not exist at all before; in css/cssom-view their
     * absence was 437 subtests failing with `not a function`. */
    CK(eq("typeof document.getElementById('box').scrollTo", "function"),
       "Element.scrollTo exists");
    CK(eq("typeof document.getElementById('box').scrollBy", "function"),
       "Element.scrollBy exists");
    CK(eq("typeof document.getElementById('box').scrollIntoView", "function"),
       "Element.scrollIntoView exists");
    CK(eq("(document.getElementById('scroller').scrollTo({top: 9}),"
          " document.getElementById('scroller').scrollTop)", "9"),
       "scrollTo takes a ScrollToOptions dictionary");
    CK(eq("(document.getElementById('scroller').scrollBy({top: 4}),"
          " document.getElementById('scroller').scrollTop)", "13"),
       "and scrollBy is relative to where it already is");
    CK(eq("(function () { try { document.getElementById('scroller').scrollTo(25);"
          "  return 'no throw'; } catch (e) { return e.constructor.name; } })()",
          "TypeError"),
       "a single NON-dictionary argument is a TypeError, per WebIDL");
    CK(eq("(function () { try {"
          "  document.getElementById('scroller').scrollTo({behavior: 'sideways'});"
          "  return 'no throw'; } catch (e) { return e.constructor.name; } })()",
          "TypeError"),
       "and so is an unrecognised scroll behavior");
    CK(eq("(function () { try { document.getElementById('scroller').scrollTo();"
          "  return 'ok'; } catch (e) { return 'threw ' + e; } })()", "ok"),
       "while zero arguments is the empty dictionary, and legal");

    /* The window as a scrolling box, and the document root as the viewport. */
    CK(eq("typeof window.scrollTo", "function"), "window.scrollTo exists");
    CK(eq("typeof window.scrollX", "number"), "window.scrollX is a number");
    CK(eq("window.scrollX === window.pageXOffset", "true"),
       "pageXOffset is the same value under its older name");
    CK(eq("document.scrollingElement === document.documentElement", "true"),
       "document.scrollingElement is the root element in standards mode");
    CK(eq("document.documentElement.clientWidth", "800"),
       "the ROOT element's clientWidth is the viewport, not its own box");
    CK(eq("document.documentElement.clientHeight", "600"),
       "and clientHeight likewise -- this is how a page asks for the window");

    /* Hit testing. */
    CK(eq("typeof document.elementFromPoint", "function"),
       "document.elementFromPoint exists");
    CK(eq("String(document.elementFromPoint(-1, -1))", "null"),
       "a point outside the viewport hits nothing at all");
    CK(eq("(function () { try { document.elementFromPoint(); return 'no throw'; }"
          "  catch (e) { return e.constructor.name; } })()", "TypeError"),
       "both coordinates are required");
    CK(eq("document.elementsFromPoint(2, 2).indexOf(document.documentElement) >= 0",
          "true"),
       "elementsFromPoint answers the paint tree, so the root is always in it");
    CK(eq("document.elementFromPoint(2, 2) === document.elementsFromPoint(2, 2)[0]",
          "true"),
       "and elementFromPoint is its first entry -- one hit test, two spellings");

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

/* ------------------------------ 4b. specified values, and what they SAY */
/* The half of serialisation that the computed tests above cannot reach.
 * `el.style.foo` is backed by the style CONTENT ATTRIBUTE -- the author's own
 * bytes -- so it is where "everything works and nothing says the right thing"
 * lives: `color: #0000ff` paints blue whichever way it reads back, and WPT
 * compares only the way it reads back. css/CSS2/syntax/colors-007.html is
 * 1,192 subtests of exactly this and nothing else.
 *
 * Written through the element rather than against css_value_serialize()
 * directly on purpose: the C function being right is not the claim, the claim
 * is that the JS surface a page touches gives these bytes. */
static void test_specified_values(void)
{
    printf("4b. el.style hands back the CSSOM's bytes, not the author's\n");
    CK(evalstr("window.E = document.createElement('div'), 1") != 0, "a fresh element");

#define SET(prop, val) "(E.style.setProperty('" prop "', " val "), " \
                       "E.style.getPropertyValue('" prop "'))"

    CK(eq(SET("color", "'#0000ff'"), "rgb(0, 0, 255)"),
       "a six-digit hex colour is rgb(r, g, b)");
    CK(eq(SET("color", "'#00F'"), "rgb(0, 0, 255)"),
       "a three-digit hex expands by DOUBLING the nibble, and case does not matter");
    CK(eq(SET("color", "'#001'"), "rgb(0, 0, 17)"),
       "... which is 0x11 and not 0x01 -- the digit-doubling rule, stated");
    CK(eq(SET("color", "'#0000ffff'"), "rgb(0, 0, 255)"),
       "an eight-digit hex whose alpha is ff is rgb(), not rgba(x, y, z, 1)");
    CK(eq(SET("color", "'#1000'"), "rgba(17, 0, 0, 0)"),
       "and a four-digit one keeps its alpha");
    CK(eq(SET("color", "'rgb(+0%, +0%, +0%)'"), "rgb(0, 0, 0)"),
       "percentages and a leading + both normalise away");
    CK(eq(SET("color", "'rgb( 1 ,2,  3 )'"), "rgb(1, 2, 3)"),
       "one comma, one space, whatever the author spaced it as");
    CK(eq(SET("color", "'rgba(5, 7, 10, 0.5)'"), "rgba(5, 7, 10, 0.5)"),
       "a real alpha keeps rgba() and its value");
    CK(eq(SET("color", "'hsl(0, 100%, 50%)'"), "rgb(255, 0, 0)"),
       "hsl() serialises as the rgb() it means");

    /* THE ALPHA RULE, which is the one place a plausible implementation
     * quietly differs from every browser: the shortest decimal that
     * round-trips through the 8-bit alpha it came from. A fixed number of
     * places gives 0.501961 where the answer is 0.502. */
    CK(eq(SET("color", "'#00000080'"), "rgba(0, 0, 0, 0.502)"),
       "alpha 0x80 is 0.502 -- shortest decimal that round-trips the byte");

    CK(eq(SET("letter-spacing", "'.5em'"), "0.5em"),
       "a bare fraction gains its leading zero");
    CK(eq(SET("letter-spacing", "'-0px'"), "0px"),
       "-0 is 0, and the UNIT SURVIVES -- `0px` must not helpfully become `0`");
    CK(eq(SET("letter-spacing", "'1.50em'"), "1.5em"),
       "trailing zeros go");
    CK(eq(SET("background-image", "'url(http://localhost/)'"), "url(\"http://localhost/\")"),
       "a <url> is quoted whether or not the author quoted it");

    /* IDEMPOTENCE. WPT does not merely check the first answer -- it assigns
     * the value it read back and requires the second read to equal the first.
     * It is also what lets ONE serialiser serve the specified and computed
     * sides, which have no way to tell each other apart from JS. */
    CK(eq("(E.style.setProperty('color', '#0000ff'),"
          " E.style.setProperty('color', E.style.getPropertyValue('color')),"
          " E.style.getPropertyValue('color'))", "rgb(0, 0, 255)"),
       "and it round-trips: re-assigning what came out changes nothing");

    /* REJECTION. The CSSOM says an unparseable declaration is discarded, and
     * the answer to "is it parseable" has to be the cascade's own or the two
     * will differ on some value nobody thought to test. */
    /* `red` stays `red`: a NAMED colour serialises as its keyword in a
     * specified value, which is what cssom/serialize-values.html expects and
     * the one case where the hex/rgb() rule does not apply. */
    CK(eq("(E.style.setProperty('color', 'red'),"
          " E.style.setProperty('color', '#00000'),"
          " E.style.getPropertyValue('color'))", "red"),
       "a five-digit hex is not a colour: the declaration is refused outright");
    CK(eq("(E.style.setProperty('color', 'red'),"
          " E.style.setProperty('color', 'invalidValue'),"
          " E.style.getPropertyValue('color'))", "red"),
       "and so is a keyword that is not one -- the old store kept both");

    /* The other direction, and it is the expensive one to get wrong.
     * css_extra.c honours a handful of properties BEHIND LibCSS's back;
     * LibCSS drops all of them, so a setter that read "LibCSS dropped it" as
     * "invalid" would throw every one of those declarations away from script
     * and the page would silently lose its rounded corners. */
    CK(eq(SET("border-radius", "'5px'"), "5px"),
       "a property LibCSS does not know is STORED, not refused (css_extra owns it)");

    CK(eq("(E.style.setProperty('color', 'red'),"
          " E.style.removeProperty('color'),"
          " E.style.getPropertyValue('color'))", ""),
       "removeProperty still removes");
#undef SET
}

/* ------------------------------- 4c. which properties have a name at all */
/* One assertion, standing for 1,495 WPT subtests.
 *
 * css-conditional/js/CSS-supports-CSSStyleDeclaration.html asks, ~600 times,
 * whether CSS.supports(prop, "inherit") agrees with `camelCase(prop) in
 * element.style`. It is an AGREEMENT test: it does not care how much CSS this
 * engine implements, it cares that those two answers come from one set. So
 * the loop below asks it of every property name LibCSS knows -- if the two
 * ever came from different lists, this fails on the first one that drifted. */
static void test_idl_surface(void)
{
    printf("4c. the IDL attributes ARE the supported properties\n");
    int n = css_known_prop_count();
    CK(n > 100, "the property universe is LibCSS's own table, and it is populated");
    CK(js_cssom_decl_props() > 100 && js_cssom_decl_props() <= n,
       "and the CSSOM published one attribute per supported name, no more");

    int disagree = 0, published = 0;
    char first[256];
    first[0] = 0;
    for (int i = 0; i < n; i++) {
        int L = 0;
        const char *p = css_known_prop_at(i, &L);
        if (!p || L <= 0 || L > 64) continue;
        /* camelCase, the same transform the WPT file applies */
        char camel[80], expr[512], raw[80];
        int o = 0, up = 0;
        for (int k = 0; k < L; k++) {
            if (p[k] == '-') { up = 1; continue; }
            char c = p[k];
            if (up) { up = 0; if (c >= 'a' && c <= 'z') c = (char)(c - 32); }
            camel[o++] = c;
        }
        camel[o] = 0;
        memcpy(raw, p, (size_t)L);
        raw[L] = 0;
        int sup = css_supports_decl(p, L, "inherit", 7);
        if (sup) published++;
        snprintf(expr, sizeof expr, "('%s' in E.style)", camel);
        char *got = evalstr(expr);
        int have = got && !strcmp(got, "true");
        free(got);
        if (have != sup) {
            disagree++;
            if (!first[0])
                snprintf(first, sizeof first, "%s: supports=%d, in style=%d",
                         raw, sup, have);
        }
    }
    if (disagree) printf("       first disagreement: %s\n", first);
    CK(disagree == 0, "CSS.supports and `in element.style` agree on every name");
    CK(published > 100, "and the published set is the whole parser, not a sample");

    /* The dashed spelling is a THIRD IDL attribute of the same property, not a
     * fallback: WPT tests it separately. */
    CK(eq("('background-color' in E.style)", "true"),
       "the dashed spelling is reachable too");
    CK(eq("(E.style['background-color'] = '#00ff00', E.style.backgroundColor)",
          "rgb(0, 255, 0)"),
       "and it is the SAME property -- one write, both spellings see it");

    /* A property nothing here implements must be absent from BOTH answers.
     * Publishing it would be the easy way to pass half this file and fail the
     * other half. */
    CK(eq("CSS.supports('alignment-baseline', 'inherit')", "false"),
       "an unimplemented property is not supported");
    CK(eq("('alignmentBaseline' in E.style)", "false"),
       "... and therefore has no IDL attribute either");

    /* The lowercase-first spelling exists for -webkit- and nothing else; WPT
     * asserts the absence for every other prefix. LibCSS carries no prefixed
     * property today, so the correct answer here is that there are none. */
    CK(eq("('mozBoxAlign' in E.style)", "false"),
       "no -moz- IDL attribute, because no -moz- property is supported");
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

    /* The compound forms. Answering all of these `false` is SAFE and still
     * wrong: it sends a page whose detect is `(display: block) or (display:
     * banana)` down its baseline path, and that page would have rendered. */
    CK(eq("CSS.supports('(color: red) and (display: block)')", "true"),
       "and: both sides real");
    CK(eq("CSS.supports('(color: red) and (color: banana-fritter)')", "false"),
       "and: one bad side loses");
    CK(eq("CSS.supports('(color: banana-fritter) or (display: block)')", "true"),
       "or: one real side wins");
    CK(eq("CSS.supports('(color: banana-fritter) or (color: also-bad)')", "false"),
       "or: neither side");
    CK(eq("CSS.supports('not (color: banana-fritter)')", "true"),
       "not: inverts");
    CK(eq("CSS.supports('not (color: red)')", "false"), "... in both directions");
    CK(eq("CSS.supports('(color: red) and (not (color: banana-fritter))')", "true"),
       "a NESTED condition inside parens, which is where a colon-first parse breaks");
    CK(eq("CSS.supports('((color: red))')", "true"), "redundant nesting");
    CK(eq("CSS.supports('(color: red) and ((display: block) or (color: bad))')", "true"),
       "and over a parenthesised or");

    /* Mixing them at one level is a SYNTAX ERROR, not a precedence question --
     * css-conditional makes you parenthesise. Answering it as though there
     * were a precedence would be inventing a language. */
    CK(eq("CSS.supports('(color: red) and (display: block) or (color: bad)')", "false"),
       "mixing and/or unparenthesised is a syntax error, so: false");
    CK(eq("CSS.supports('(color: red')", "false"), "an unclosed paren is not a condition");
    CK(eq("CSS.supports('color: red')", "false"),
       "the one-argument form needs the parens -- a bare declaration is not a condition");

    /* <general-enclosed>: a function or blob we do not recognise is false,
     * which is both what the spec says and the only honest answer. */
    CK(eq("CSS.supports('(width >= 100px)')", "false"),
       "an unrecognised feature is general-enclosed, hence false");
    CK(eq("CSS.supports('futureFeature(x)')", "false"), "and so is an unknown function");

    CK(eq("CSS.supports('selector(div)')", "true"), "selector(): a selector we parse");
    CK(eq("CSS.supports('selector(div > p.cls)')", "true"), "... including a complex one");
    CK(eq("CSS.supports('selector(<<<)')", "false"), "and one we do not is false");
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

    /* THE ONE THAT CRASHED. Wiring matchMedia to LibCSS's own parser means a
     * SCRIPT can now hand that parser any string, where before it only ever
     * saw @media preludes the tokeniser had produced. An empty query in a
     * comma list ("," / ",," / a trailing comma) is legal input and a parse
     * error, and mq_parse_media_query dereferenced the NULL peek --
     * segfaulting the whole WPT run at
     * css/mediaqueries/match-media-parsing.html and taking the 973 files
     * after it down with it. Fixed in the vendored parser; asserted here,
     * because "it does not crash" is not observable from a pass rate.
     *
     * The last entry is the empty string, and it is the ONE that matches: an
     * empty media query list means `all`, which is true of every medium. That
     * is not the same case as ",," (a list whose members are empty), and
     * lumping them together is how a "harden it" change quietly makes every
     * responsive page take its desktop branch. */
    CK(eq("[',', ',,', '  ,  ,  ', ' foo,', '(', ')', '((', 'not', 'and', '']"
          ".map(function (q) { return matchMedia(q).matches ? 1 : 0; }).join('')",
          "0000000001"),
       "a malformed query is `not all`; an EMPTY one is `all` -- different cases");
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
    test_specified_values();
    test_idl_surface();
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
