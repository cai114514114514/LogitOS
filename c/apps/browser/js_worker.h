#ifndef LOGIT_JS_WORKER_H
#define LOGIT_JS_WORKER_H

#include "quickjs.h"

/* Dedicated Worker (classic, same-origin): a second JSRuntime per worker, run
 * to completion on THIS thread by an interleaved single-thread event loop.
 * See js_worker.c's own header for the full design note -- why a thread was
 * rejected (terminate() is unimplementable here: `grep pthread_cancel|
 * pthread_kill c/apps/libc/src/pthread.c` is 0 hits, and a worker thread
 * running `while(true)` could never be stopped), the termination bar every
 * async path is held to, and the long list of things REFUSED BY NAME rather
 * than half-built: SharedWorker, module workers, transferables, a
 * MessagePort crossing the worker boundary, nested workers, importScripts on
 * the window.
 *
 * NOT PARALLELISM, AND SAY SO EVERYWHERE THIS MATTERS. A worker task runs to
 * completion on the main thread before the next one starts -- this gives a
 * separate global scope and message passing, never a second core. A page
 * that offloads a long synchronous computation to escape jank gets the same
 * jank, moved. navigator.hardwareConcurrency stays 1 inside every worker for
 * exactly that reason; do not let it become a number that invites a page to
 * spawn N workers expecting N cores.
 *
 * Install LAST in js_page_open, after js_platform_install and
 * js_events_install: a Worker's parent-side event delivery (onmessage /
 * onerror) reaches for G.DOMException / G.MessageEvent / G.ErrorEvent when
 * they exist and falls back to a plain object shape when they do not, so
 * running after them is strictly better and not a hard dependency --
 * js_worker_install itself works with neither present. */
#include "../../../include/weaksym.h"   /* LOGIT_WEAK/_STUB: see the header */
#ifdef JS_WORKER_OPTIONAL
#  define WORKER_FN LOGIT_WEAK
#else
#  define WORKER_FN
#endif

/* Install `Worker` into `ctx` (the PAGE's context, never a worker's own --
 * see js__wSelfClose/nested-worker refusal in js_worker.c for why). */
WORKER_FN void js_worker_install(JSContext *ctx);

/* ---- folded into js_page.c's own scheduling, per js_worker.c's rule: ONE
 * JAR, ONE DOOR. js_page_pending/next_due/run_due call these three so a page
 * holding only a live worker (no timer of its own) does not read as idle,
 * and a worker's own setTimeout advances the SAME virtual clock the page's
 * timers do -- a worker queue invisible to those three page functions is
 * exactly the failure mode js_page.h's drain-loop comment warns about: every
 * worker test reports "registered no tests and raised nothing". */
WORKER_FN int       js_worker_pending(void);
WORKER_FN long long js_worker_next_due(void);
WORKER_FN int       js_worker_run_due(void);
/* Same dispatch with the page's shared turn deadline. A yield keeps queued
 * tasks and worker microtasks live in pending/next_due. Never re-enters the
 * browser from inside a synchronous worker/native call. */
WORKER_FN int       js_worker_run_due_until(unsigned long long deadline_ms);
/* Page integration: after a batch of parent notifications, return before
 * another worker enters JS and set *parent_handoff. The page must service
 * its network checkpoint and yield to input/paint before calling again.
 * The standalone pumps above have no page phase to hand work back to. */
WORKER_FN int       js_worker_run_due_for_page(unsigned long long deadline_ms,
                                               int *parent_handoff);

/* Terminate every live worker and free both runtimes. MUST be called from
 * js_page_close() BEFORE the page context itself is freed: every worker
 * holds a JSValue reference back into that context (the Worker instance its
 * __deliverMessage/__deliverError calls land on), and those references have
 * to let go before JS_FreeRuntime tears the page's runtime down. Safe to
 * call with no worker open. */
WORKER_FN void js_worker_close_all(void);

/* The Mach-O half of the weak declarations above (include/weaksym.h). */
#ifdef JS_WORKER_OPTIONAL
LOGIT_WEAK_STUB(js_worker_install);
LOGIT_WEAK_STUB(js_worker_pending);
LOGIT_WEAK_STUB(js_worker_next_due);
LOGIT_WEAK_STUB(js_worker_run_due);
LOGIT_WEAK_STUB(js_worker_run_due_until);
LOGIT_WEAK_STUB(js_worker_run_due_for_page);
LOGIT_WEAK_STUB(js_worker_close_all);
#endif

#endif /* LOGIT_JS_WORKER_H */
