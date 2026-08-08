/* cssdcanon_test.c -- the CSSOM's settable-property set, sourced from canon.c,
 * and the setter that has to arrive with it.
 *
 * TWO HALVES, AND THE POINT OF THIS FILE IS THAT NEITHER IS THE FIX ALONE.
 *
 * (1) THE SET. `el.style.gridTemplateColumns` is an IDL attribute, and a
 *     property with no named accessor cannot be assigned from script at all --
 *     its parser is unreachable and every test of it fails on "property should
 *     be set" without the parser ever running. js_dom.c publishes LibCSS's own
 *     property table plus the properties canon.c claims, and that second half
 *     used to be ~50 names transcribed by hand into a CSSD_EXTRA array. It is
 *     now read from canon.c through css_canon_prop_count/at, so a serializer
 *     added there is settable in the same commit.
 *
 * (2) THE REFUSAL. For a property LibCSS has never heard of there is no
 *     validity step anywhere in this browser: the declaration goes into the
 *     style attribute unconditionally and is canonicalised only on the way back
 *     out. So publishing an accessor for `grid-template-columns` converts
 *     `el.style['grid-template-columns'] = '-10px'` from a no-op into a store,
 *     and every WPT "-invalid" subtest asserting the property stays unset goes
 *     red. Those subtests were passing VACUOUSLY before: nothing was stored,
 *     so "should not set the property value" was true for a reason that had
 *     nothing to do with validity. Measured on css/css-grid/parsing, the set
 *     alone is +128 valid subtests and -115 refusals given back.
 *
 * (3) THE BOUNDARY, which is the reason this is safe. canon.c answers three
 *     ways, and CSS_CANON_PASS -- "not my property" -- is most of CSS. A PASS
 *     must behave exactly as it did before, or wiring the refusal in would put
 *     a second validity opinion in front of every declaration the browser
 *     already honours, including the handful css_extra.c honours behind
 *     LibCSS's back. The `display: banana` assertions below are that boundary
 *     and they are the ones to read first if this file ever goes red.
 *
 * THE CONTROLS. Every assertion here is required to fail under one of two
 * sabotages, and `make test-cssd-canon-negctl` builds both:
 *   -DCSSD_NO_CANON_REFUSE   the half-implementation: the set adopted, the
 *                            setter left alone. Not a straw man -- it is the
 *                            obvious way to do this and it is a net LOSS on
 *                            the two -invalid files it touches first.
 *   -DCSSD_PROPS_FROM_ENUM   the set taken from the cascade's CSSP_* enum, the
 *                            state before any of this.
 *
 * js_cssom.c is deliberately NOT linked. Its setProperty wrapper refuses an
 * INVALID declaration too, and linking it would let this file pass with
 * js_dom.c's own setter doing nothing -- which is exactly the state the WPT
 * corpus reaches for a canon-only property, because that wrapper's named
 * accessors cover only the properties LibCSS knows.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs.h"
#include "dom.h"
#include "css.h"
#include "js_dom.h"

void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }

static int g_fails;
static int g_checks;

static void chk(JSContext *ctx, const char *expr, const char *want)
{
    JSValue v = JS_Eval(ctx, expr, strlen(expr), "<cssdcanon>", JS_EVAL_TYPE_GLOBAL);
    const char *s = JS_IsException(v) ? 0 : JS_ToCString(ctx, v);
    int ok = s && !strcmp(s, want);
    g_checks++;
    if (!ok) {
        printf("  FAIL %s\n       expected \"%s\" got \"%s\"\n",
               expr, want, s ? s : "<exception>");
        g_fails++;
    }
    if (s) JS_FreeCString(ctx, s);
    if (JS_IsException(v)) JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, v);
}

static void fail(const char *fmt, const char *a, int b)
{
    printf("  FAIL ");
    printf(fmt, a, b);
    printf("\n");
    g_fails++;
}

int main(void)
{
    static const char H[] = "<!doctype html><html><body><div id=d></div></body></html>";
    struct node *root = dom_parse(H, (int)strlen(H));
    if (!root) { printf("FAIL cssdcanon_test: dom_parse returned nothing\n"); return 1; }

    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    js_dom_init(ctx, root);

    /* ---- (1) the set comes from canon.c, and it is not empty ---------- */
    int nc = css_canon_prop_count();
    g_checks++;
    if (nc < 40) {
        fail("css_canon_prop_count() is %s%d -- the enumeration is not being read", "", nc);
    }

    /* The four grid track properties are the drift this change exists to end:
     * they landed in canon.c with a full <track-list> parser and were absent
     * from the hand-written copy, so a correct serializer measured as nothing.
     * Assert the ENUMERATION carries them rather than assuming it. */
    static const char *const want[] = {
        "grid-template-columns", "grid-template-rows",
        "grid-auto-columns", "grid-auto-rows",
        "position-area", "anchor-name", "inset-inline-start", "accent-color", 0
    };
    for (int w = 0; want[w]; w++) {
        int found = 0;
        for (int i = 0; i < nc; i++) {
            const char *p = css_canon_prop_at(i);
            if (p && !strcmp(p, want[w])) { found = 1; break; }
        }
        g_checks++;
        if (!found) fail("canon.c's enumeration does not carry %s (%d names)", want[w], nc);
    }

    /* EVERY enumerated name must have a named accessor. This is the assertion
     * that a hand-written second copy cannot satisfy structurally -- it can
     * only satisfy it by being up to date, which is the property that kept
     * being lost. An absent named property reads `undefined`, not "". */
    int missing = 0;
    for (int i = 0; i < nc; i++) {
        const char *p = css_canon_prop_at(i);
        if (!p || !*p) continue;
        char e[192];
        snprintf(e, sizeof e, "typeof document.createElement('div').style['%s']", p);
        JSValue v = JS_Eval(ctx, e, strlen(e), "<cssdcanon>", JS_EVAL_TYPE_GLOBAL);
        const char *s = JS_IsException(v) ? 0 : JS_ToCString(ctx, v);
        if (!s || strcmp(s, "string")) {
            if (missing < 4) printf("  FAIL no named accessor for '%s'\n", p);
            missing++;
        }
        if (s) JS_FreeCString(ctx, s);
        if (JS_IsException(v)) JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, v);
    }
    g_checks++;
    if (missing) { fail("%s%d enumerated properties have no named accessor", "", missing); }

    /* ...and they are settable, in both spellings. */
    chk(ctx, "var e = document.createElement('div');"
             "e.style['grid-template-columns'] = 'repeat(2, 1fr)';"
             "e.style['grid-template-columns']", "repeat(2, 1fr)");
    chk(ctx, "e.getAttribute('style').indexOf('grid-template-columns') >= 0"
             " ? 'stored' : 'LOST'", "stored");
    chk(ctx, "e.style.gridAutoRows = 'minmax(10px, 1fr)'; e.style.gridAutoRows",
             "minmax(10px, 1fr)");

    /* ---- (2) the refusal ---------------------------------------------- */
    /* A negative literal is a parse error in a track list. The property must
     * stay unset -- WPT's grid-template-columns-invalid.html is 42 subtests of
     * exactly this shape. */
    chk(ctx, "var f = document.createElement('div');"
             "f.style['grid-template-columns'] = '-10px';"
             "f.style['grid-template-columns']", "");
    chk(ctx, "f.getAttribute('style') === null ? 'clean'"
             " : (f.getAttribute('style').indexOf('grid-template') >= 0 ? 'STORED' : 'clean')",
             "clean");
    /* An invalid assignment leaves the PREVIOUS value, it does not clear it. */
    chk(ctx, "f.style.gridTemplateRows = '1fr';"
             "f.style.gridTemplateRows = 'banana';"
             "f.style.gridTemplateRows", "1fr");
    /* `minmax(5fr, X)` -- <inflexible-breadth> excludes <flex>, which is the
     * asymmetry the grammar has two names for. */
    chk(ctx, "f.style.gridAutoColumns = 'minmax(5fr, 1fr)'; f.style.gridAutoColumns", "");
    /* A negative calc() is NOT a parse error: whether it is negative is a used
     * value question. Both spellings are in the corpus, adjacent. */
    chk(ctx, "var g = document.createElement('div');"
             "g.style.gridAutoRows = 'calc(-0.5em + 10px)'; g.style.gridAutoRows.length > 0"
             " ? 'kept' : 'DROPPED'", "kept");
    /* The colour family canon.c claims and LibCSS's table does not -- and the
     * sharpest statement of how narrow CSS_CANON_INVALID is. canon.c claims
     * the colour PROPERTIES but only the colour FUNCTIONS among their values:
     * a bare keyword is LibCSS's, which reads it back correctly today, so
     * canon.c PASSES on it however implausible it looks. A malformed FUNCTION
     * is nobody's and is refused. Claiming a property is not claiming every
     * value of it, and this pair is what says so. */
    chk(ctx, "var h = document.createElement('div');"
             "h.style['accent-color'] = 'color(srgb 1 2)'; h.style['accent-color']", "");
    chk(ctx, "h.style['accent-color'] = 'not-a-colour'; h.style['accent-color']",
             "not-a-colour");
    chk(ctx, "h.style['accent-color'] = 'rgb(1, 2, 3)';"
             "h.style['accent-color'].length > 0 ? 'kept' : 'DROPPED'", "kept");

    /* ---- (3) the boundary: CSS_CANON_PASS is unchanged ----------------- */
    /* `display` is not canon.c's property, so canon.c PASSES on it whatever the
     * value, and this file's setter must not form an opinion. The cascade drops
     * `display: banana` -- that is the cascade's job and it is not this one.
     * If these two go red, the refusal has grown past canon.c's INVALID and is
     * now judging declarations the browser already honours. */
    char buf[256];
    int len = 0;
    g_checks++;
    if (css_specified_canon("display", -1, "banana", 6, buf, (int)sizeof buf, &len)
            != CSS_SPEC_PASS)
        fail("css_specified_canon no longer PASSES on a property it does not "
             "claim%s (%d)", "", 0);
    chk(ctx, "var k = document.createElement('div');"
             "k.style.display = 'banana'; k.style.display", "banana");
    chk(ctx, "k.style.display = 'flex'; k.style.display", "flex");
    /* An empty value is a REMOVAL and a removal is never invalid, whatever
     * canon.c would say about the property. */
    chk(ctx, "var m = document.createElement('div');"
             "m.style.gridAutoRows = '1fr'; m.style.gridAutoRows = '';"
             "m.style.gridAutoRows", "");
    /* setProperty() takes the same refusal as the named setter. */
    chk(ctx, "var q = document.createElement('div');"
             "q.style.setProperty('grid-template-columns', '-10px');"
             "q.style.getPropertyValue('grid-template-columns')", "");
    chk(ctx, "q.style.setProperty('grid-template-columns', '1fr');"
             "q.style.getPropertyValue('grid-template-columns')", "1fr");
    /* ...and removeProperty still removes. */
    chk(ctx, "q.style.removeProperty('grid-template-columns');"
             "q.style.getPropertyValue('grid-template-columns')", "");
    /* Nothing in LibCSS's half of the set regressed on the way past. */
    chk(ctx, "k.style.backgroundColor = 'red'; k.style.backgroundColor", "red");
    chk(ctx, "k.style.cssFloat = 'left'; k.style.cssFloat", "left");

    js_dom_cleanup(ctx);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    if (g_fails) {
        printf("cssdcanon_test: %d of %d FAILED\n", g_fails, g_checks);
        return 1;
    }
    printf("cssdcanon_test: ALL PASS (%d checks; %d properties enumerated from"
           " canon.c, %d from LibCSS)\n", g_checks, nc, css_known_prop_count());
    return 0;
}
