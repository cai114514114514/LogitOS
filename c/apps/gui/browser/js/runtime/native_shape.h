#ifndef LOGIT_JS_NATIVE_SHAPE_H
#define LOGIT_JS_NATIVE_SHAPE_H

#include "quickjs.h"

/* Platform functions that look native -- see native_shape.c.
 *
 * js_native_shape_install(ctx, out) runs ONCE per realm, after every
 * installer and BEFORE any page code: it declares every function compiled
 * in the realm so far to be host code (JS_MarkNativeCode), then walks the
 * platform's objects and gives each operation and accessor a built-in's
 * shape (no `prototype`, no [[Construct]], a WebIDL name) and each interface
 * object a built-in's spelling. Calling it after page code has run would
 * mark page code; that is the caller's contract, not something it can
 * check. `out` may be NULL. Returns 0, or -1 if the walk ran out of memory
 * (whatever was marked stays marked).
 *
 * Weak under JS_NATIVE_SHAPE_OPTIONAL, the convention js_events.h /
 * js_webapi.h use: host harnesses that link page.c from a hand-kept source
 * list do not link this TU and must still link. */
struct js_native_shape_report {
    int code;        /* function codes declared host code */
    int functions;   /* distinct platform functions found on the walk */
    int operations;  /* given the operation shape (no prototype/construct) */
    int interfaces;  /* constructors: spelling only */
    int kept_class;  /* asked to strip, refused: evidently used as a class */
    int renamed;     /* '' / 'value' / 'get' / bare-key names made WebIDL's */
    int objects;     /* objects walked */
};

#include "../../../../../../include/weaksym.h"   /* LOGIT_WEAK/_STUB */
#ifdef JS_NATIVE_SHAPE_OPTIONAL
#  define NATIVE_SHAPE_FN LOGIT_WEAK
#else
#  define NATIVE_SHAPE_FN
#endif

NATIVE_SHAPE_FN int js_native_shape_install(JSContext *ctx, struct js_native_shape_report *out);

#ifdef JS_NATIVE_SHAPE_OPTIONAL
LOGIT_WEAK_STUB(js_native_shape_install);
#endif

#endif /* LOGIT_JS_NATIVE_SHAPE_H */
