#ifndef LOGIT_JS_FRAME_H
#define LOGIT_JS_FRAME_H

#include "quickjs.h"

/* js_frame.c -- a SAME-ORIGIN second browsing context: a fresh JSContext on
 * the PAGE's own JSRuntime, given to a <iframe>'s already-built
 * DOMParser-backed document (js_domparser.c's dp_arena) so a <script>
 * inserted into that document actually RUNS, instead of being data nothing
 * ever reads (js_platform.c:2534-2660's own comment, until this file).
 *
 * See js_frame.c's own header for the full design note: why a second
 * CONTEXT and not a second RUNTIME (the opposite of js_worker.c's choice,
 * and for the opposite reason -- js_worker.c:1-72), why cross-origin frames
 * are OUT OF SCOPE and stay refused at js_platform.c's existing gate, and
 * the measured reason a real DOM inside the frame is not attempted
 * (js_dom.c:4364's unguarded JS_NewClassID -- a second js_dom_init would
 * overwrite the parent's g_root/g_document/elem_cid, not merely leak them).
 *
 * Install from js_platform_install(), NOT js_page.c -- see js_frame.c's
 * header for why js_page.c is off-limits to this pass. Self-registers the
 * script sink with js_domparser.c; installs `__frameAdopt` into the page's
 * global object, which js_platform.c's iframe prelude calls once a frame's
 * document is settled. */
#include "../../../include/weaksym.h"   /* LOGIT_WEAK/_STUB: see the header */
#ifdef JS_FRAME_OPTIONAL
#  define FRAME_FN LOGIT_WEAK
#else
#  define FRAME_FN
#endif

FRAME_FN void js_frame_install(JSContext *pctx);
/* Auxiliary same-origin realms inside an active network document expose
 * fresh intrinsics but do not acquire the legacy script/DOM execution path. */
FRAME_FN void js_frame_install_inert(JSContext *pctx);
FRAME_FN void js_frame_close_context(JSContext *pctx);
FRAME_FN void js_frame_refresh_policy(JSContext *pctx);

/* Free every live frame's JSContext. MUST run before JS_FreeRuntime(g_rt) --
 * see js_page_close()'s own ordering comment for js_worker_close_all(), the
 * same invariant one runtime over. Called from js_platform_close(), which
 * js_page_close() already calls before JS_FreeContext/JS_FreeRuntime -- see
 * js_frame.c's header for why that (and not a new js_page.c hook) is where
 * this belongs. Safe with no frame open. */
FRAME_FN void js_frame_close_all(void);

#ifdef JS_FRAME_OPTIONAL
LOGIT_WEAK_STUB(js_frame_install);
LOGIT_WEAK_STUB(js_frame_close_all);
LOGIT_WEAK_STUB(js_frame_install_inert);
LOGIT_WEAK_STUB(js_frame_close_context);
LOGIT_WEAK_STUB(js_frame_refresh_policy);
#endif

#endif /* LOGIT_JS_FRAME_H */
