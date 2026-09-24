#ifndef LOGIT_JS_IDL_SHAPE_H
#define LOGIT_JS_IDL_SHAPE_H

#include "quickjs.h"

/* The WebIDL shape pass -- js/runtime/idl_shape.c. Moves what the installers
 * put on the platform singletons (navigator, screen, history, performance,
 * crypto, document, document.body) onto their interface prototypes with
 * receiver brand checks, mints the Screen and Crypto interfaces, gives every
 * platform interface prototype its @@toStringTag, and chains the prototypes a
 * browser chains to EventTarget. See the .c for what it leaves alone and why.
 *
 * Call it LAST in js_page_open: it moves what is there, so it must run after
 * every installer that puts something there. It is idempotent -- a second call
 * moves only what has been re-added since (frame/passive.c calls it again after
 * frame_focus_install defines document.activeElement on a network frame's
 * document). Weak under JS_IDL_SHAPE_OPTIONAL, the convention every optional
 * browser module here follows: a link without idl_shape.c keeps today's shape
 * exactly. */
#include "../../../../../../include/weaksym.h"   /* LOGIT_WEAK/_STUB */
#ifdef JS_IDL_SHAPE_OPTIONAL
#  define IDL_SHAPE_FN LOGIT_WEAK
#else
#  define IDL_SHAPE_FN
#endif

IDL_SHAPE_FN void js_idl_shape_install(JSContext *ctx);

#ifdef JS_IDL_SHAPE_OPTIONAL
LOGIT_WEAK_STUB(js_idl_shape_install);
#endif

#endif /* LOGIT_JS_IDL_SHAPE_H */
