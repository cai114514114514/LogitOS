#ifndef LOGIT_JS_CSSOM_H
#define LOGIT_JS_CSSOM_H

#include "quickjs.h"

/* The CSSOM: the half of CSS that scripts TALK TO rather than the half that
 * paints. Three separate surfaces, in one file because they share one thing --
 * they all have to reach the live cascade and the live display list:
 *
 *   CSSOM-View    offsetWidth/Height/Top/Left, offsetParent, clientWidth/
 *                 Height/Top/Left, scrollWidth/Height, getClientRects(),
 *                 getBoundingClientRect(), DOMRect. These read OUT OF LAYOUT.
 *   CSSOM proper  document.styleSheets, CSSStyleSheet, CSSRule and its
 *                 subclasses, insertRule/deleteRule.
 *   CSS + media   the CSS namespace object (supports/escape) and matchMedia.
 *
 * Plus one thing that is not CSSOM at all and is here because nothing else in
 * the tree does it (see the comment on the window-reflected handlers in the
 * .c): `<body onload=...>` is a handler on the WINDOW, not on the body.
 *
 * INSTALL ORDER: last, after js_dom_init, js_webapi_install and
 * js_platform_install. It takes the Element prototype those published and adds
 * to it, and it deliberately REPLACES two things they installed --
 * getBoundingClientRect (which must flush layout first) and matchMedia (which
 * must be the same media evaluator the cascade uses; see css.h). Installing it
 * earlier means those two get overwritten again by the older versions.
 *
 * OPTIONAL, like js_platform.c's installers and for the same reason: several
 * host test runners link a subset of the browser and must keep building
 * without this file. `__weak__` rather than `weak` because mini-libc's
 * features.h is force-included into every browser TU and #defines the plain
 * spelling (see the same note in js_webapi.h). */
#ifdef JS_CSSOM_OPTIONAL
#  define CSSOM_FN __attribute__((__weak__))
#else
#  define CSSOM_FN
#endif

CSSOM_FN void js_cssom_install(JSContext *ctx);

/* Drop the node lookup cache and the per-page side tables. Call from the page
 * teardown path, before JS_FreeContext. Safe to call twice. */
CSSOM_FN void js_cssom_close(JSContext *ctx);

/* What a REFLOW means, registered by the embedder.
 *
 * A geometry read on a mutated document has to make layout current, and doing
 * that properly means re-running the CASCADE and then layout -- an el.style
 * write changes the style attribute, and nothing moves until the cascade sees
 * it. The cascade needs the document's whole stylesheet set (external sheets,
 * var() expansion, the css_extra post pass), which browser.c assembles and
 * this file has no access to. So the embedder supplies the verb.
 *
 * With nothing registered the fallback is layout_page() alone, which means
 * geometry after a style write reflects the PREVIOUS cascade. That is a real
 * wrong answer, stated rather than hidden. NULL clears it. */
void js_cssom_set_reflow(void (*fn)(void));

/* Test seams. `js_cssom_layouts()` counts the reflows the geometry accessors
 * forced -- the assertion that measure-after-mutate actually re-laid-out, and
 * equally that a hundred reads in a row did NOT. Cumulative for the life of
 * the process. */
int  js_cssom_layouts(void);

#endif /* LOGIT_JS_CSSOM_H */
