#ifndef LOGIT_JS_EVENTS_H
#define LOGIT_JS_EVENTS_H

#include "quickjs.h"

/* The DOM event layer: the Event constructor hierarchy, the legacy Event
 * members, document.createEvent, a constructible EventTarget, and the parts of
 * addEventListener's options surface the native store in js_dom.c does not
 * carry (handleEvent objects, `signal`, the default-passive rule).
 *
 * Install LAST -- after js_dom_init, js_dom_bind_event_target, js_webapi and
 * js_platform. It layers over all four: it needs js_dom.c's native Event
 * classes to exist, js_webapi.c's AbortSignal for the `signal` option, and it
 * deliberately replaces the placeholder EventTarget/PromiseRejectionEvent that
 * js_platform.c installs when nobody better has.
 *
 * Safe to call on a context with no DOM: it checks for globalThis.Event and
 * returns without touching anything if js_dom.c never ran.
 *
 * Weak under JS_EVENTS_OPTIONAL, the same convention js_webapi.h /
 * js_platform.h / js_media.h use: js_page.c's own host tests -- and the stock
 * WPT runner, whose source list this line does not own -- link without this
 * TU and must still link. `if (js_events_install)` at the call site is what
 * makes that work. */
#ifdef JS_EVENTS_OPTIONAL
#  define EVENTS_FN __attribute__((__weak__))
#else
#  define EVENTS_FN
#endif

EVENTS_FN void js_events_install(JSContext *ctx);

#endif /* LOGIT_JS_EVENTS_H */
