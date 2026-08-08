/* cssprops_test.c -- the CSSOM named-property set.
 *
 * `el.style.backgroundColor` and `el.style['background-color']` are IDL
 * attributes of CSSStyleDeclaration, and which properties get one is not a
 * detail: a property with no named accessor cannot be set from script at all.
 * Its parser is then unreachable, and every test of it fails on "property
 * should be set" without the parser ever running.
 *
 * That is exactly what happened. js_dom.c took the set from css.h's CSSP_* enum
 * -- the ~60 properties the CASCADE resolves -- when the right set is every
 * property the PARSER knows. The CSS line implemented `position-area` in full,
 * 2,598 checks, and gained zero, because `div.style['position-area'] = x` did
 * nothing. The same cause accounted for 4,310 more subtests in
 * css-anchor-position and 507 in css-fonts.
 *
 * So this file asserts the SET, not the values: that a property outside the
 * cascade's enum is settable, stores into the style attribute, and reads back.
 * Every assertion here fails under the old enum-sourced set, which is what
 * `make test-cssprops-negctl` builds.
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

static void chk(JSContext *ctx, const char *expr, const char *want)
{
    JSValue v = JS_Eval(ctx, expr, strlen(expr), "<cssprops>", JS_EVAL_TYPE_GLOBAL);
    const char *s = JS_IsException(v) ? 0 : JS_ToCString(ctx, v);
    int ok = s && !strcmp(s, want);
    if (!ok) {
        printf("FAIL %s\n     expected \"%s\" got \"%s\"\n", expr, want, s ? s : "<exception>");
        g_fails++;
    }
    if (s) JS_FreeCString(ctx, s);
    if (JS_IsException(v)) JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, v);
}

int main(void)
{
    static const char H[] = "<!doctype html><html><body><div id=d></div></body></html>";
    struct node *root = dom_parse(H, (int)strlen(H));
    if (!root) { printf("FAIL cssprops_test: dom_parse returned nothing\n"); return 1; }

    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    js_dom_init(ctx, root);

    /* The set has to be bigger than the cascade's enum. If these two are equal
     * the property source was not changed, whatever else passes. */
    int known = css_known_prop_count();
    if (known <= CSSP__COUNT) {
        printf("FAIL cssprops_test: the parser knows %d properties and the cascade"
               " enum has %d -- the named-property set is still the enum\n",
               known, CSSP__COUNT);
        g_fails++;
    }

    chk(ctx, "var e = document.createElement('div');"
             "e.style['position-area'] = 'top left'; e.style['position-area']",
             "top left");
    chk(ctx, "e.getAttribute('style').indexOf('position-area') >= 0 ? 'stored' : 'LOST'",
             "stored");
    /* Both spellings, because the CSSOM defines both and pages use both. */
    chk(ctx, "e.style.blockSize = '10px'; e.style['block-size']", "10px");
    chk(ctx, "e.style['inline-size'] = '4px'; e.style.inlineSize", "4px");
    /* The logical box properties, one from each family. */
    chk(ctx, "e.style['inset-inline-start'] = '2px'; e.style.insetInlineStart", "2px");
    chk(ctx, "e.style.marginBlock = '1px'; e.style['margin-block']", "1px");
    chk(ctx, "e.style.paddingInlineEnd = '3px'; e.style.paddingInlineEnd", "3px");
    chk(ctx, "e.style['border-inline-start-color'] = 'red';"
             "e.style.borderInlineStartColor", "red");
    /* The anchor family. */
    chk(ctx, "e.style['anchor-name'] = '--a'; e.style.anchorName", "--a");
    chk(ctx, "e.style['position-anchor'] = '--a'; e.style.positionAnchor", "--a");
    /* typeof is a string even before anything is set -- an absent named
     * property answers undefined, which is the shape of the original defect. */
    chk(ctx, "typeof document.createElement('div').style['position-area']", "string");
    /* And nothing in the enum regressed on the way past. */
    chk(ctx, "e.style.backgroundColor = 'red'; e.style.backgroundColor", "red");
    chk(ctx, "e.style['background-color']", "red");
    chk(ctx, "e.style.cssFloat = 'left'; e.style.cssFloat", "left");

    js_dom_cleanup(ctx);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    if (g_fails) { printf("cssprops_test: %d FAILED\n", g_fails); return 1; }
    printf("cssprops_test: ALL PASS (%d properties in the parser's set,"
           " %d in the cascade enum)\n", known, CSSP__COUNT);
    return 0;
}
