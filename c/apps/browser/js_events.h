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
#include "../../../include/weaksym.h"   /* LOGIT_WEAK/_STUB: see the header */
#ifdef JS_EVENTS_OPTIONAL
#  define EVENTS_FN LOGIT_WEAK
#else
#  define EVENTS_FN
#endif

EVENTS_FN void js_events_install(JSContext *ctx);

/* The Mach-O half of the weak declarations above (include/weaksym.h): an
 * undefined weak reference is an ELF property, so each optional entry point
 * needs a weak definition in the TU that may not link the provider. Emitted
 * only under JS_EVENTS_OPTIONAL, i.e. only in that TU. */
#ifdef JS_EVENTS_OPTIONAL
LOGIT_WEAK_STUB(js_events_install);
#endif

#endif /* LOGIT_JS_EVENTS_H */
