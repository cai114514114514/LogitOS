#ifndef LOGIT_JS_TOKENLIST_H
#define LOGIT_JS_TOKENLIST_H

#include "quickjs.h"

/* Install the DOM's DOMTokenList over Element.prototype.classList.
 *
 * Called from js_select_install() rather than from js_page.c: js_page.c is
 * edited by several lines at once and this needs no ordering of its own beyond
 * "after js_dom_init", which js_select already has.
 *
 * Idempotent: a second call is a no-op. */
void js_tokenlist_install(JSContext *ctx);

#endif /* LOGIT_JS_TOKENLIST_H */
