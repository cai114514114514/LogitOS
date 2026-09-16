/* js_worker.c -- dedicated Worker, on an interleaved single-thread event loop.
 *
 * THE DECISION: EVENT LOOP, NOT A THREAD. Three findings, in order of weight:
 *
 * 1. terminate() IS UNIMPLEMENTABLE ON A THREAD HERE. `grep pthread_cancel|
 *    pthread_kill c/apps/libc/src/pthread.c` is 0 hits. A worker thread
 *    running `while(true)` could never be stopped -- the browser would hold a
 *    runaway thread for the life of the process, forever, the first time a
 *    page tried the exact test this feature has to pass
 *    (Worker-terminate-forever-during-evaluation.html). On one thread the
 *    QuickJS interrupt handler js_page.c already owns for the page IS the
 *    kill primitive, and each worker gets its own copy of the same idea
 *    (worker_slice_interrupt below) -- deliberately NOT js_page.c's shared
 *    g_slice_* statics, because lowering the budget for a worker must not
 *    lower it for the page too.
 * 2. THE BROWSER HAS NO RING-3 LOCK DISCIPLINE. js_page.c is an explicit
 *    singleton ("Exactly one page runtime exists at a time"), bfetch owns one
 *    global request table, and c/lib/image + malloc are shared. A second
 *    thread calling bfetch_sync (importScripts, or a worker's own startup
 *    fetch) would race every one of those -- nobody has done the ring-3
 *    equivalent of tools/bkl_shared.py's census. Single-threaded cooperative
 *    execution needs none of that: a worker's JS runs only when this file
 *    calls into it, never concurrently with the page's own JS or another
 *    worker's.
 * 3. QuickJS values do not cross runtimes anyway, so a thread buys no value
 *    sharing -- only the serialization boundary this file needs either way
 *    (JS_WriteObject/JS_ReadObject with JS_*_OBJ_REFERENCE, never
 *    JS_WRITE_OBJ_BYTECODE -- a function argument is a clone FAILURE, which
 *    is exactly the DataCloneError the spec wants).
 *
 * WHAT THE EVENT LOOP CANNOT DO, SAID PLAINLY, because the alternative is
 * present-and-wrong: it delivers a separate global scope, message passing,
 * and a worker that does not block the parent BETWEEN tasks. It does NOT
 * deliver parallelism. A worker task runs to completion on the main thread,
 * so a page that offloads a long synchronous computation to escape jank gets
 * the same jank, moved. navigator.hardwareConcurrency stays 1 inside every
 * worker for exactly that reason.
 *
 * SCHEDULING: ONE JAR, ONE DOOR. Every worker task -- the deferred startup
 * fetch, a message crossing in either direction, a worker's own setTimeout --
 * is a single struct wtask on ONE flat list (g_tasks), and js_worker_pending/
 * next_due/run_due (called from js_page.c's three functions of the same
 * name) are the only door onto it. A second, worker-private scheduler would
 * be invisible to the runner's drain loop, which is exactly the failure this
 * design note in js_page.h warns about.
 *
 * REFUSED BY NAME, never silently half-built (js_platform.h:66-70's rule:
 * absent is safer than present-and-wrong):
 *   - SharedWorker: stays undefined. A half-SharedWorker is the indexedDB
 *     trap; ReferenceError is the correct, currently-passing answer.
 *   - {type:'module'}: throws NotSupportedError naming module workers.
 *   - a non-empty parent-to-worker transfer list, or a MessagePort in data:
 *     throws. Worker-to-parent ports now use the native packet broker below;
 *     the old outbound C binding silently ignored its second argument.
 *     naming what was refused. A silent copy is worse than a refusal -- a
 *     page that transfers a buffer and checks `byteLength === 0` afterwards
 *     would walk on holding live data if the "transfer" were actually a copy.
 *   - SharedArrayBuffer in postMessage: refused by the same clone failure a
 *     function argument gets -- there is no second thread, so "shared" would
 *     be a lie the page cannot detect.
 *   - `new Worker` inside a worker: throws NotSupportedError naming nested
 *     workers.
 *   - importScripts on the window: never defined there. 25 of the WPT
 *     `'importScripts' is not defined` failures are `.any.js` files running
 *     in WINDOW scope, where a window correctly has no importScripts;
 *     defining it there to make a ReferenceError go away would make the
 *     browser WRONG.
 *
 * THE TERMINATION BAR: every async path this file exposes ends in a queued
 * success or a queued error event, never a Worker whose fate is undecided.
 * See the comment above worker_start() and worker_report_uncaught() for the
 * exact list; the two negative controls -DJS_WORKER_NO_TERMINATE and
 * -DJS_WORKER_SILENT_ERROR exist to prove it by making it false on purpose.
 */
#include "quickjs.h"
#include "js_worker.h"
#include "js_page.h"
#include "js_task_budget.h"
#define JS_PORTS_OPTIONAL
#include "js_ports.h"
#define JS_WEBAPI_OPTIONAL
#include "js_webapi.h"
#undef JS_WEBAPI_OPTIONAL
#define JS_URL_OPTIONAL
#include "js_url.h"
#undef JS_URL_OPTIONAL
#define JS_WASM_OPTIONAL
#include "js_wasm.h"
#undef JS_WASM_OPTIONAL
#include "bfetch.h"
#include "url.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int js_page_pump(void) LOGIT_WEAK;
LOGIT_WEAK_STUB(js_page_pump);
extern void js_page_slice_end(void) LOGIT_WEAK;
LOGIT_WEAK_STUB(js_page_slice_end);
extern int js_webapi_set_connect_policy(JSContext *,int (*)(void *,const char *),void *) LOGIT_WEAK;
LOGIT_WEAK_STUB(js_webapi_set_connect_policy);
#ifdef JS_WORKER_BLOB_ALLOC_AUDIT
/* Host-only observation of actual frees, including the direct close-all
 * path. Counting state transitions alone would miss a discarded pointer. */
extern void worker_blob_test_allocated(void *);
extern void worker_blob_test_free(void *);
#define free worker_blob_test_free
#endif

/* Diagnostic builds observe browser-owned decisions even when page code
 * handles the resulting error event. Only these fixed labels and a local id
 * may cross this boundary: never URLs, thrown text, or message payloads. The
 * default build adds no hook and changes no worker scheduling/policy. */
#ifdef JS_RUNTIME_DIAGNOSTICS
static unsigned worker_diag_lines;
static void worker_diag(int id, const char *phase, const char *kind)
{
    if (worker_diag_lines++ < 256)
        printf("[runtime-diag] worker id=%d phase=%s kind=%s\n", id, phase, kind);
}
static JSValue js_worker_diag_ctor(JSContext *ctx, JSValueConst self,
                                   int argc, JSValueConst *argv)
{
    (void)ctx; (void)self;
    if (argc && JS_VALUE_GET_TAG(argv[0]) == JS_TAG_INT) {
        int code = JS_VALUE_GET_INT(argv[0]);
        if (code == 1) worker_diag(0, "constructor", "module-unsupported");
        if (code == 2) worker_diag(0, "post-message", "transfer-unsupported");
    }
    return JS_UNDEFINED;
}
#include "js_worker_promise_diagnostics.inc"
#else
#define worker_diag(id, phase, kind) ((void)0)
#endif

/* -------------------------------------------------------------------------
 * one small helper this file needs everywhere: a DOMException, thrown from
 * C. Mirrors js_events.c's own JS-side domErr() -- fetch the realm's
 * DOMException constructor if one exists (js_platform.c installs it), call
 * it through JS_CallConstructor so `e instanceof DOMException` holds, and
 * fall back to a plain Error wearing `.name` when nothing installed one
 * (the host tests of this file, and any build without js_platform.c).
 * ---------------------------------------------------------------------- */
static JSValue throw_dom_exception(JSContext *ctx, const char *name, const char *msg)
{
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_GetPropertyStr(ctx, g, "DOMException");
    JSValue exc;
    if (JS_IsFunction(ctx, ctor)) {
        JSValue args[2];
        args[0] = JS_NewString(ctx, msg ? msg : "");
        args[1] = JS_NewString(ctx, name ? name : "Error");
        exc = JS_CallConstructor(ctx, ctor, 2, (JSValueConst *)args);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, args[1]);
        if (JS_IsException(exc)) {
            JS_FreeValue(ctx, JS_GetException(ctx));   /* the ctor itself blew up */
            exc = JS_UNDEFINED;
        }
    } else {
        exc = JS_UNDEFINED;
    }
    JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, g);
    if (JS_IsUndefined(exc)) {
        exc = JS_NewError(ctx);
        JS_SetPropertyStr(ctx, exc, "message", JS_NewString(ctx, msg ? msg : ""));
        if (name) JS_SetPropertyStr(ctx, exc, "name", JS_NewString(ctx, name));
    }
    return JS_Throw(ctx, exc);
}

/* Structured clone, the boundary between two runtimes. JS_WRITE_OBJ_BYTECODE
 * is deliberately OFF: a function or a module in the graph is then a WRITE
 * FAILURE (quickjs.c's JS_WriteObjectRec hits its `default:` case for a
 * function class and throws "unsupported object class"; a Symbol hits
 * `invalid_tag`) -- which is exactly the DataCloneError the spec wants, for
 * free, without hand-rolling a second serializer next to js_platform.c's
 * same-runtime structuredClone. The copy is taken with plain malloc, not
 * ctx's own allocator, so it can outlive the writer's runtime (a worker
 * posting to its parent moments before termination) with no lifetime tie to
 * either side. */
static unsigned char *clone_write(JSContext *ctx, JSValueConst val, size_t *outlen)
{
    size_t sz = 0;
    uint8_t *buf = JS_WriteObject(ctx, &sz, val, JS_WRITE_OBJ_REFERENCE);
    if (!buf) return NULL;                 /* exception already pending on ctx */
    unsigned char *copy = malloc(sz ? sz : 1);
    if (!copy) { js_free(ctx, buf); return NULL; }
    if (sz) memcpy(copy, buf, sz);
    js_free(ctx, buf);
    *outlen = sz;
    return copy;
}

static JSValue get_method(JSContext *ctx, JSValueConst obj, const char *name)
{
    JSValue f = JS_GetPropertyStr(ctx, obj, name);
    if (!JS_IsFunction(ctx, f)) { JS_FreeValue(ctx, f); return JS_UNDEFINED; }
    return f;
}

/* =========================================================================
 * the worker registry
 * ========================================================================= */
#define JSW_MAX_WORKERS 8

enum { WK_EMPTY = 0, WK_STARTING, WK_RUNNING, WK_DEAD };

struct pending_msg { struct pending_msg *next; unsigned char *buf; size_t len; };

struct jsworker {
    int used;
    int id;
    int state;
    JSRuntime *rt;
    JSContext *wctx;             /* the worker's own context; NULL until started */
#ifdef JS_RUNTIME_DIAGNOSTICS
    struct worker_promise_diag promise_diag;
    long long diag_begin,diag_last,diag_gap;
    const char *diag_phase;
#endif
    JSValue self_obj;             /* worker's globalThis, dup'd */
    JSContext *pctx;               /* the page context this worker belongs to */
    JSValue worker_obj;             /* the Worker instance in pctx, dup'd */
    char url[600];
    /* Capture the creator before deferred startup. Blob URLs are capabilities
     * in that realm's object-URL table, not HTTP URLs (strstr("://") used to
     * reject every blob:http(s) URL before allocating even a worker id).
     * The owned copy survives revokeObjectURL and is freed after eval/reap. */
    char creator_origin[URL_HOST_MAX + 16];
    struct url creator_url;
    int creator_valid, is_blob;
    unsigned char *blob_source;
    int blob_length;
    struct js_worker_policy policy;
    struct pending_msg *inbound_head, *inbound_tail;  /* posted while STARTING */
    /* this worker's OWN watchdog -- see the file header, point 1. Deliberately
     * separate state from js_page.c's g_slice_*, so a lower per-worker budget
     * never touches the page's. */
    long long wd_due;
    long long wd_fuel, wd_fuel_max;
    int wd_armed, wd_hit;
    int free_pending;              /* WK_DEAD, rt/ctx not yet reaped -- see reap_dead() */
};

struct wtask;
struct js_worker_context {
    struct jsworker workers[JSW_MAX_WORKERS];
    struct wtask *tasks;
    unsigned long long sequence, task_limit, deadline;
    unsigned cursor;
    long long task_now;
    int task_phase, snapshot_active, in_turn, next_worker_id, next_timer_id;
    struct js_worker_policy policy;
    char site[600];
};
static struct js_worker_context worker_default={.next_worker_id=1,.next_timer_id=1};
static struct js_worker_context *worker_owner=&worker_default;
/* Previously these were process globals. Worker watchdog opaque pointers
 * escape into QuickJS, so moving their bytes into a temporary working table
 * would make a suspended parent's watchdog refer to a child's worker. */
#define g_workers (worker_owner->workers)
#define g_next_worker_id (worker_owner->next_worker_id)
#define g_tasks (worker_owner->tasks)
#define g_wseq (worker_owner->sequence)
#define g_next_timer_id (worker_owner->next_timer_id)
#define g_worker_owner_cursor (worker_owner->cursor)
#define g_worker_task_phase (worker_owner->task_phase)
#define g_worker_task_snapshot_active (worker_owner->snapshot_active)
#define g_worker_task_limit (worker_owner->task_limit)
#define g_worker_task_now (worker_owner->task_now)
#define g_worker_deadline (worker_owner->deadline)
#define g_worker_in_turn (worker_owner->in_turn)

struct js_worker_context *js_worker_context_create(const struct js_worker_policy *p)
{
    if(!p||!p->allow||g_worker_in_turn||
       (p->site_url&&strlen(p->site_url)>=600))return NULL;
    struct js_worker_context *s=calloc(1,sizeof *s);if(!s)return NULL;
    s->next_worker_id=s->next_timer_id=1;s->policy=*p;
    if(p->site_url)strcpy(s->site,p->site_url);
    s->policy.site_url=s->site;return s;
}
int js_worker_context_activate(struct js_worker_context *s)
{
    if(g_worker_in_turn)return 0;
    worker_owner=s?s:&worker_default;return 1;
}
int js_worker_context_destroy(struct js_worker_context *s)
{
    if(!s||s==&worker_default||s->in_turn||s->tasks)return 0;
    for(int i=0;i<JSW_MAX_WORKERS;i++)if(s->workers[i].used)return 0;
    if(worker_owner==s)worker_owner=&worker_default;
    free(s);return 1;
}
static int worker_allowed(int op,const char *url)
{return !worker_owner->policy.allow||worker_owner->policy.allow(worker_owner->policy.opaque,op,url);}
static int worker_response_allowed(struct jsworker *w,int op,const char *url)
{return !w->policy.allow||w->policy.allow(w->policy.opaque,op,url);}
static int worker_connect(void *owner,const char *url)
{
    struct jsworker *w=owner;
    return worker_response_allowed(w,JSW_CONNECT,url);
}

/* The owner sweep and task snapshot survive a boundary yield. Starting the
 * sweep at owner zero on every return would let that owner's ready reactions
 * consume every budget forever. The old sequence snapshot still excludes
 * tasks appended during the task phase, including self-rearming intervals. */
static int worker_turn_expired(void)
{
    return g_worker_in_turn &&
        js_task_budget_expired(js_page_now_ms(), g_worker_deadline);
}
static int worker_jobs_pending(const struct jsworker *w)
{
    return w->rt && w->state != WK_DEAD && JS_IsJobPending(w->rt);
}

/* Lower than js_page.c's 45 s page default on purpose (see the risk this was
 * written against: Worker-creation-happens-in-parallel.https.html spins on
 * `performance.now() < end` for up to 10 s of VIRTUAL time that this runner's
 * clock only advances when nothing else ran, so inside the busy loop it never
 * advances at all -- the watchdog is the only thing that ends it, and it
 * should cost single-digit seconds per such file, not the full page budget). */
#define JSW_SLICE_MS_DEFAULT   8000
#define JSW_SLICE_FUEL_DEFAULT 400000

static struct jsworker *find_worker(int id)
{
    if (id <= 0) return NULL;
    for (int i = 0; i < JSW_MAX_WORKERS; i++)
        if (g_workers[i].used && g_workers[i].id == id) return &g_workers[i];
    return NULL;
}

static int worker_slice_interrupt(JSRuntime *rt, void *opaque)
{
    struct jsworker *w = (struct jsworker *)opaque;
    (void)rt;
    if (!w->wd_armed) return 0;
    long long now=(long long)js_page_now_ms();
    int over_time = w->wd_due && now > w->wd_due;
    int over_fuel = ++w->wd_fuel > w->wd_fuel_max;
#ifdef JS_RUNTIME_DIAGNOSTICS
    /* Reuse the watchdog's clock sample. A gap is time between 10,000-poll
     * intervals, not proof of CPU consumption or the name of a native call. */
    long long gap=now-w->diag_last;if(gap>w->diag_gap)w->diag_gap=gap;w->diag_last=now;
#endif
    if (!over_time && !over_fuel) return 0;
    w->wd_hit = 1;
    w->wd_armed = 0;
    printf("[worker %d] watchdog: script exceeded its CPU slice (%s) -- interrupted\n",
           w->id, over_time ? "wall time" : "instruction fuel");
#ifdef JS_RUNTIME_DIAGNOSTICS
    if(worker_diag_lines++<256)printf("[runtime-diag] worker-budget id=%d phase=%s elapsed=%lld polls=%lld max-gap=%lld rail=%s\n",
        w->id,w->diag_phase?w->diag_phase:"other",now-w->diag_begin,w->wd_fuel,w->diag_gap,over_time?"time":"fuel");
#endif
    return 1;
}

static void worker_slice_begin(struct jsworker *w)
{
    if(w->wctx)JS_SetStringCodeGenerationAllowed(w->wctx,worker_response_allowed(w,JSW_EVAL,NULL));
    w->wd_due = (long long)js_page_now_ms() + JSW_SLICE_MS_DEFAULT;
#ifdef JS_RUNTIME_DIAGNOSTICS
    w->diag_begin=w->diag_last=w->wd_due-JSW_SLICE_MS_DEFAULT;w->diag_gap=0;w->diag_phase="other";
#endif
    w->wd_fuel = 0;
    w->wd_fuel_max = JSW_SLICE_FUEL_DEFAULT;
    w->wd_armed = 1;
}

#ifdef JS_RUNTIME_DIAGNOSTICS
#define WORKER_PHASE(w,name) ((w)->diag_phase=(name))
#else
#define WORKER_PHASE(w,name) ((void)0)
#endif

/* =========================================================================
 * the task queue -- ONE JAR, ONE DOOR (see the file header)
 * ========================================================================= */
enum { WTK_CALL = 0, WTK_START = 1 };
#ifdef JS_RUNTIME_DIAGNOSTICS
/* Still dispatched as an ordinary call; the diagnostic tag distinguishes
 * inbound messages from timers without reading any JS function properties. */
#define WTK_MESSAGE 2
#else
#define WTK_MESSAGE WTK_CALL
#endif

struct wtask {
    struct wtask *next;
    int wid;
    int kind;
    JSContext *ctx;          /* NULL for WTK_START; the callee's own context otherwise */
    JSValue callee;
    JSValue thisArg;
    JSValue *argv; int argc;
    long long due;
    int interval_ms;          /* >0: re-arm after firing (a worker's own setInterval) */
    int timer_id;               /* >0: cancellable by the worker's clearTimeout/Interval */
    unsigned long long seq;
    struct js_port_packet *packet; /* outbound transfer, materialized at dispatch */
};


static void task_free(struct wtask *t)
{
#ifndef JS_WORKER_TEST_LEAK_PACKETS
    if(t->packet&&LOGIT_HAVE(js_ports_discard))js_ports_discard(t->packet);
#endif
    if (t->ctx) {
        JS_FreeValue(t->ctx, t->callee);
        JS_FreeValue(t->ctx, t->thisArg);
        for (int i = 0; i < t->argc; i++) JS_FreeValue(t->ctx, t->argv[i]);
    }
    free(t->argv);
    free(t);
}

static int task_add(int wid, int kind, JSContext *ctx, JSValue callee, JSValue thisArg,
                     JSValue *argv, int argc, long long due, int interval_ms, int timer_id)
{
    struct wtask *t;
#ifdef JS_WORKER_TEST_START_OOM
    t = kind==WTK_START?NULL:calloc(1,sizeof *t);
#else
    t = calloc(1, sizeof *t);
#endif
    if (!t) {
        if (ctx) {
            JS_FreeValue(ctx, callee); JS_FreeValue(ctx, thisArg);
            for (int i = 0; i < argc; i++) JS_FreeValue(ctx, argv[i]);
        }
        free(argv);
        return 0;
    }
    t->wid = wid; t->kind = kind; t->ctx = ctx; t->callee = callee; t->thisArg = thisArg;
    t->argv = argv; t->argc = argc; t->due = due; t->interval_ms = interval_ms;
    t->timer_id = timer_id; t->seq = ++g_wseq;
    t->next = g_tasks;
    g_tasks = t;
    return 1;
}

/* `keep_parent_notify`: skip tasks BOUND FOR THE PARENT (ctx == w->pctx) --
 * a real bug lived here until this comment did: deliver_error_to_parent()
 * schedules exactly one task carrying the death notice, and the ORIGINAL
 * version of this sweep (called from mark_dead right after) deleted that
 * task in the same breath it was queued, because both share this worker's
 * `wid` and nothing distinguished "a task belonging to the dead worker" from
 * "the one task whose entire job is to say so afterward." Found by a probe
 * (`new Worker('does-not-exist.js'); w.onerror = ...`) that timed out with
 * NO printf anywhere in this file ever firing -- the notify task was gone
 * before js_worker_run_due() could see it, so nothing was silently wrong
 * inside worker_report_uncaught/deliver_error_to_parent; the task simply
 * never existed by the time anything looked for it. Internal-failure paths
 * (fetch/parse/eval failure, the watchdog) pass 1 here so their own
 * notification survives regardless of call order; explicit terminate()/
 * close() pass 0, because the spec's terminate() really does discard every
 * queued task including ones already addressed to the parent. */
static void cancel_worker_tasks(struct jsworker *w, int keep_parent_notify)
{
    struct wtask **pp = &g_tasks;
    while (*pp) {
        struct wtask *t = *pp;
        if (t->wid == w->id && !(keep_parent_notify && t->ctx == w->pctx)) {
            *pp = t->next;
            task_free(t);
        } else {
            pp = &(*pp)->next;
        }
    }
}

/* Logically terminate a worker: drop it out of scheduling immediately (so no
 * further task can find it), but do NOT free its runtime here -- we may be
 * calling this from INSIDE a JS_Call currently running that very worker's
 * script (self.close()). Freeing is deferred to reap_dead(), which only ever
 * runs at the top of js_worker_run_due(), i.e. never while a worker's JS_Call
 * is on the stack. -DJS_WORKER_NO_TERMINATE breaks exactly the first half of
 * this (see below) and is the control for it. */
static void mark_dead_ex(struct jsworker *w, int keep_parent_notify)
{
    if (!w->used || w->state == WK_DEAD) return;
    worker_diag(w->id, "exit", keep_parent_notify ? "internal-failure" : "requested");
    w->state = WK_DEAD;
    w->free_pending = 1;
    /* self.close can run inside a network reaction. Stop admission now;
     * actual socket/JSValue destruction waits for reap after it unwinds. */
    if(w->wctx&&LOGIT_HAVE(js_webapi_fetch_stop))js_webapi_fetch_stop(w->wctx);
#ifndef JS_WORKER_NO_TERMINATE
    cancel_worker_tasks(w, keep_parent_notify);
#endif
}
/* terminate()/close(): full purge, per spec. */
static void mark_dead(struct jsworker *w) { mark_dead_ex(w, 0); }
/* An internal failure (bad/missing script, parse error, uncaught throw, the
 * watchdog): purge everything EXCEPT the one notification task this same
 * call is about to schedule (or just scheduled) for the parent. */
static void mark_dead_keep_notify(struct jsworker *w) { mark_dead_ex(w, 1); }

static void reap_dead(void)
{
    for (int i = 0; i < JSW_MAX_WORKERS; i++) {
        struct jsworker *w = &g_workers[i];
        if (!w->used || w->state != WK_DEAD || !w->free_pending) continue;
        /* SECOND BUG FOUND BY tests/unit/worker_test.c, fixed here: a task
         * this worker's OWN top-level script scheduled AFTER
         * cancel_worker_tasks() already ran on THIS SAME tick (e.g. a
         * setTimeout added right after self.close() -- see worker_start()'s
         * own comment) is not touched by that call, and js_worker_run_due()
         * snapshots g_wseq at the top of the pass specifically so a task
         * added mid-pass waits for the NEXT call rather than running
         * immediately. But reap_dead() runs FIRST on every call, unconditional
         * of what is still queued -- so on that next call it frees this
         * worker's JSContext/JSRuntime while the deferred task's `callee`
         * JSValue still holds a live reference into the runtime being freed.
         * quickjs.c's JS_FreeRuntime asserts its gc_obj_list is empty and
         * aborts (`Assertion failed: (list_empty(&rt->gc_obj_list))`) --
         * reproduced on the PLAIN build, no -D flag needed. Purge one more
         * time, right here, before the free: every task this returns is
         * guaranteed worker-bound (a task addressed to the PARENT is never
         * removed by cancel_worker_tasks, so this cannot drop a message the
         * parent was owed) and would only have been dropped anyway, lazily,
         * by run_due's own is_worker_call/WK_DEAD check if it survived to
         * become due -- this just makes that certain before the free instead
         * of racing it.
         *
         * keep_parent_notify=1, NOT 0: a worker killed via
         * mark_dead_keep_notify() (the fetch-failure/cross-origin/uncaught-
         * throw paths) may still have its OWN error-delivery task -- ctx ==
         * w->pctx, ADDRESSED TO THE PARENT -- sitting in g_tasks waiting for
         * next tick because js_worker_run_due()'s seq snapshot deferred it
         * past THIS pass. reap_dead() runs before that task ever gets a
         * chance to fire, on every call, so passing 0 here silently deleted
         * the very notification mark_dead_keep_notify() exists to protect --
         * an onerror that should fire and never does, the exact js_platform.h
         * hazard this whole file is built against. 1 preserves it: it only
         * ever protects tasks with ctx == w->pctx, so it changes nothing
         * about the worker-bound leftover task this purge exists for. */
        cancel_worker_tasks(w, 1);
        if (w->wctx) {
            if(LOGIT_HAVE(js_ports_close))js_ports_close(w->wctx);
            if(LOGIT_HAVE(js_webapi_fetch_close))js_webapi_fetch_close(w->wctx);
#ifndef WORKER_NO_WASM_REALM
            if(LOGIT_HAVE(js_wasm_reset))js_wasm_reset(w->wctx);
#endif
#ifdef JS_RUNTIME_DIAGNOSTICS
            worker_promise_diag_close(w->wctx, &w->promise_diag);
#endif
            JS_FreeValue(w->wctx, w->self_obj);
            JS_FreeContext(w->wctx);
        }
        if (w->rt) JS_FreeRuntime(w->rt);
        if (w->pctx && !JS_IsUndefined(w->worker_obj)) JS_FreeValue(w->pctx, w->worker_obj);
        struct pending_msg *m = w->inbound_head;
        while (m) { struct pending_msg *n = m->next; free(m->buf); free(m); m = n; }
        free(w->blob_source);
        if(w->policy.release)w->policy.release(w->policy.opaque);
        memset(w, 0, sizeof *w);
    }
}

/* =========================================================================
 * delivering INTO the parent: message + error
 * ========================================================================= */
static void deliver_error_to_parent(struct jsworker *w, const char *message,
                                    const char *filename, int line, int col)
{
#ifdef JS_WORKER_SILENT_ERROR
    /* THE CONTROL for item 8/9/10/11 of the termination bar: reproduce the
     * js_platform.h hazard on purpose by dropping the error on the floor.
     * Constructed as a no-op function rather than an #ifdef around every call
     * site, so the rest of this file reads identically in both builds and the
     * only difference under test is whether the event ever arrives. */
    (void)w; (void)message; (void)filename; (void)line; (void)col;
    return;
#else
    if (!w->pctx || JS_IsUndefined(w->worker_obj)) return;
    JSValue callee = get_method(w->pctx, w->worker_obj, "__deliverError");
    if (!JS_IsFunction(w->pctx, callee)) { JS_FreeValue(w->pctx, callee); return; }
    JSValue *argv = malloc(4 * sizeof(JSValue));
    if (!argv) { JS_FreeValue(w->pctx, callee); return; }
    argv[0] = JS_NewString(w->pctx, message ? message : "");
    argv[1] = JS_NewString(w->pctx, filename ? filename : "");
    argv[2] = JS_NewInt32(w->pctx, line);
    argv[3] = JS_NewInt32(w->pctx, col);
    task_add(w->id, WTK_CALL, w->pctx, callee, JS_DupValue(w->pctx, w->worker_obj), argv, 4,
             (long long)js_page_now_ms(), 0, 0);
#endif
}

/* An uncaught exception at the worker's top level, or from a message/timer
 * callback running inside it. THE TERMINATION-BAR RULE THIS IMPLEMENTS
 * (items 10-12): the watchdog case reports NOTHING (a forcibly-terminated
 * script does not report its exception, per spec); otherwise the worker's
 * own self.onerror (5-argument legacy form) and any addEventListener('error')
 * listener each get a chance to call preventDefault()/return true, and only
 * if NEITHER did does this propagate to the parent as an ErrorEvent. A
 * genuine parse error can never have this reach a real self.onerror, because
 * nothing in the script has executed yet to install one -- so the spec's
 * "parse errors skip the worker's own onerror" falls out of this by
 * construction rather than needing a separate code path. */

/* THE PROMISE-DRAIN FIX. MEASURED, before this existed: a probe worker whose
 * script did `Promise.resolve().then(function(){ postMessage('C') })` never
 * posted 'C', and js_worker_pending() read 0 the whole time -- the scheduler
 * believed the worker was idle while a settled promise's reaction sat in
 * w->rt's OWN job queue forever. Cause: QuickJS never runs a queued job on
 * its own (js_dom.c's identical comment on js_dom_run_jobs applies verbatim,
 * one runtime over) -- JS_Eval/JS_Call only ENQUEUE reactions, and until now
 * NOTHING ever called JS_ExecutePendingJob(w->rt, ...) for a worker's
 * runtime; js_dom_run_jobs only ever drains the PAGE's rt (js_page.c passes
 * it g_ctx, never a worker context). Every worker runtime is private to this
 * file, so this file is the only place that can drain it.
 *
 * Called at the end of every entry point that can enqueue a job in a
 * worker's own runtime -- the top-level script eval in worker_start(), each
 * buffered-message flush in worker_start()'s drain loop, and every
 * JS_Call into a worker context inside js_worker_run_due() -- so that by the
 * time control returns to js_page.c's idle check, a worker's job queue was
 * historically always empty. Correction: a finite but long reaction must
 * now return to input/painting before the next reaction. pending/next_due
 * explicitly observe JS_IsJobPending, and dispatch resumes those jobs before
 * another task in the SAME worker. Page and worker are separate event loops;
 * this does not split a page checkpoint to run a new page JS event within it.
 *
 * Same liveness cap as js_dom_run_jobs, same reason: a job that requeues
 * itself (`function f(){ Promise.resolve().then(f) } f()`, inside a
 * worker) must not wedge the one thread the whole browser runs on. */
#define WORKER_MAX_JOBS_PER_PUMP 100000
static int worker_drain_jobs(struct jsworker *w)
{
    if (!w || !w->rt) return 0;
    int n = 0;
    for (; n < WORKER_MAX_JOBS_PER_PUMP; n++) {
        /* A fetch reaction may call close(). Do not then execute a second
         * reaction or manufacture a new parent delivery from that dead realm. */
        if(w->state==WK_DEAD || worker_turn_expired())break;
        JSContext *jc = 0;
        WORKER_PHASE(w,"job");
#ifdef JS_RUNTIME_DIAGNOSTICS
        /* Completion matters as much as interruption: a finite job can miss
         * its caller's deadline without ever tripping our 8 s rail. Numeric
         * metadata only, and compiled out of ordinary builds. */
        long long diag_job_begin=(long long)js_page_now_ms();
        long long diag_job_polls=w->wd_fuel;
#endif
        int r = JS_ExecutePendingJob(w->rt, &jc);
#ifdef JS_RUNTIME_DIAGNOSTICS
        long long diag_job_ms=(long long)js_page_now_ms()-diag_job_begin;
        if(r!=0&&diag_job_ms>=100&&worker_diag_lines<256){
            worker_diag_lines++;
            printf("[runtime-diag] worker-job id=%d elapsed=%lld polls=%lld outcome=%s begin=%lld\n",
                   w->id,diag_job_ms,w->wd_fuel-diag_job_polls,
                   w->wd_hit?"interrupted":(r<0?"error":"complete"),diag_job_begin);
        }
#endif
        if (r == 0) break;               /* queue empty */
        if (r < 0) {
            /* The job itself threw uncaught -- report it the same way
             * js_dom_run_jobs does for the page (print, keep going), NOT a
             * termination event: a rejected promise nobody handles is an
             * unhandledrejection in a real browser, not a worker crash. */
            JSContext *rc = jc ? jc : w->wctx;
            JSValue e = JS_GetException(rc);
            const char *m = JS_ToCString(rc, e);
            printf("[worker %d] uncaught in queued job: %s\n", w->id, m ? m : "?");
            if (m) JS_FreeCString(rc, m);
            JS_FreeValue(rc, e);
        }
    }
    if (n >= WORKER_MAX_JOBS_PER_PUMP)
        printf("[worker %d] microtask queue did not drain in %d jobs -- giving up this turn\n",
               w->id, WORKER_MAX_JOBS_PER_PUMP);
    return n;
}

static void worker_report_uncaught(struct jsworker *w, JSValueConst e)
{
    JSContext *ctx = w->wctx;
    if (w->wd_hit) {
        worker_diag(w->id, "execution", "watchdog");
        /* item 12: silent, by design -- a forcibly-terminated script reports
         * no exception. Marked dead HERE, not left to the caller: a watchdog
         * hit during an ordinary message/timer callback (not just the
         * initial top-level eval) means this worker's JS is no longer
         * trusted to make progress, so it stops receiving further work
         * rather than being free to trip the same watchdog on every future
         * message. keep_parent_notify doesn't matter here (no task is
         * queued either way) but the _keep_notify form is used everywhere
         * in this file for one less thing to reason about. */
        w->wd_hit = 0;
        mark_dead_keep_notify(w);
        return;
    }

    worker_diag(w->id, "execution", "uncaught-exception");
    const char *m = JS_ToCString(ctx, e);
    printf("[worker %d] uncaught: %s\n", w->id, m ? m : "?");

    int handled = 0;
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue onerr = JS_GetPropertyStr(ctx, g, "onerror");
    if (JS_IsFunction(ctx, onerr)) {
        JSValue args[5];
        args[0] = JS_NewString(ctx, m ? m : "");
        args[1] = JS_NewString(ctx, w->url);
        args[2] = JS_NewInt32(ctx, 0);
        args[3] = JS_NewInt32(ctx, 0);
        args[4] = JS_DupValue(ctx, e);
        JSValue r = JS_Call(ctx, onerr, g, 5, (JSValueConst *)args);
        for (int i = 0; i < 5; i++) JS_FreeValue(ctx, args[i]);
        if (JS_IsException(r)) JS_FreeValue(ctx, JS_GetException(ctx));
        else if (JS_ToBool(ctx, r) > 0) handled = 1;
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, onerr);

    JSValue disp = JS_GetPropertyStr(ctx, g, "__reportWorkerError");
    if (JS_IsFunction(ctx, disp)) {
        JSValue args[2];
        args[0] = JS_NewString(ctx, m ? m : "");
        args[1] = JS_NewString(ctx, w->url);
        JSValue r = JS_Call(ctx, disp, g, 2, (JSValueConst *)args);
        if (JS_IsException(r)) JS_FreeValue(ctx, JS_GetException(ctx));
        else if (JS_ToBool(ctx, r) > 0) handled = 1;
        for (int i = 0; i < 2; i++) JS_FreeValue(ctx, args[i]);
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, disp);
    JS_FreeValue(ctx, g);

    if (!handled) deliver_error_to_parent(w, m ? m : "worker error", w->url, 0, 0);
    if (m) JS_FreeCString(ctx, m);
}

static void deliver_message_to_worker_now(struct jsworker *w, const unsigned char *buf, size_t len)
{
    JSContext *ctx = w->wctx;
    JSValue v = JS_ReadObject(ctx, buf, len, JS_READ_OBJ_REFERENCE);
    if (JS_IsException(v)) { JS_FreeValue(ctx, JS_GetException(ctx)); return; }
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue callee = get_method(ctx, g, "__deliverMessage");
    if (JS_IsFunction(ctx, callee)) {
        worker_diag(w->id, "inbound-message-dispatch", "message");
        worker_slice_begin(w);
        WORKER_PHASE(w,"startup-message");
        JSValueConst args[1]; args[0] = v;
        JSValue r = JS_Call(ctx, callee, g, 1, args);
        if (JS_IsException(r)) {
            JSValue e = JS_GetException(ctx);
            worker_report_uncaught(w, e);
            JS_FreeValue(ctx, e);
            if (w->state == WK_DEAD) { /* watchdog already marked it */ }
        }
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, callee);
    JS_FreeValue(ctx, g);
    JS_FreeValue(ctx, v);
    worker_drain_jobs(w);   /* see worker_drain_jobs's header -- a message callback is exactly the kind of entry point that can enqueue a promise reaction */
}

/* Startup messages used to be detached and drained as one unbounded batch.
 * Keep them owned by the worker until individually dispatched. New incoming
 * messages append behind a surviving startup buffer, never overtake it. */
static int worker_flush_inbound(struct jsworker *w)
{
    int ran = 0;
    while (w->state == WK_RUNNING && w->inbound_head &&
           !worker_jobs_pending(w) && !worker_turn_expired()) {
        struct pending_msg *m = w->inbound_head;
        w->inbound_head = m->next;
        if (!w->inbound_head) w->inbound_tail = NULL;
        deliver_message_to_worker_now(w, m->buf, m->len);
        free(m->buf); free(m); ran++;
    }
    return ran;
}

/* =========================================================================
 * the worker's own global scope
 * ========================================================================= */
static JSValue js__wPostMessage(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv, int wid)
{
    (void)t;
    struct jsworker *w = find_worker(wid);
    if(!w||w->state==WK_DEAD)return JS_UNDEFINED;
    JSValueConst data = argc > 0 ? argv[0] : JS_UNDEFINED;
#ifndef JS_WORKER_TEST_DROP_OUTBOUND_PORTS
    if(argc>1&&!JS_IsUndefined(argv[1])){
        /* Unlike the former ignored argument, this is a real ownership move.
         * Keep the packet in transit until the parent's task runs: attaching
         * wrappers at enqueue time would leave them alive after cancellation.
         * No foreign JSValue survives in the packet, and all allocations and
         * author property reads precede commit/detachment. The parent->worker
         * transfer path remains explicitly unsupported for now. */
        int array=JS_IsArray(ctx,argv[1]);if(array<0)return JS_EXCEPTION;
        JSValue transfer=array?JS_DupValue(ctx,argv[1]):JS_IsNull(argv[1])?JS_UNDEFINED:JS_GetPropertyStr(ctx,argv[1],"transfer");
        if(JS_IsException(transfer))return transfer;
        if(!LOGIT_HAVE(js_ports_prepare)||!LOGIT_HAVE(js_ports_commit)||
           !LOGIT_HAVE(js_ports_read)||!LOGIT_HAVE(js_ports_discard)){
            JS_FreeValue(ctx,transfer);
            return throw_dom_exception(ctx,"DataCloneError","native port transfer is unavailable");
        }
        struct js_port_packet *packet=js_ports_prepare(ctx,data,transfer);
        JS_FreeValue(ctx,transfer);if(!packet)return JS_EXCEPTION;
        JSValue callee=get_method(w->pctx,w->worker_obj,"__deliverMessage");
        if(w->state==WK_DEAD||!JS_IsFunction(w->pctx,callee)){
            JS_FreeValue(w->pctx,callee);js_ports_discard(packet);return JS_UNDEFINED;
        }
        if(!task_add(wid,WTK_CALL,w->pctx,callee,JS_DupValue(w->pctx,w->worker_obj),NULL,0,
                     (long long)js_page_now_ms(),0,0)){
            js_ports_discard(packet);return JS_ThrowOutOfMemory(ctx);
        }
        struct wtask *queued=g_tasks;
        if(!js_ports_commit(ctx,packet)){
            g_tasks=queued->next;task_free(queued);js_ports_discard(packet);return JS_EXCEPTION;
        }
        queued->packet=packet;
        worker_diag(wid,"outbound-message-enqueue","native-packet");
        return JS_UNDEFINED;
    }
#else
    if(argc>1&&!JS_IsUndefined(argv[1]))worker_diag(wid,"outbound-message","transfer-argument-ignored");
#endif
    size_t len = 0;
    unsigned char *buf = clone_write(ctx, data, &len);
    if (!buf) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return throw_dom_exception(ctx, "DataCloneError", "postMessage: value could not be cloned");
    }
    if (!w || !w->pctx || JS_IsUndefined(w->worker_obj)) { free(buf); return JS_UNDEFINED; }
    JSValue v = JS_ReadObject(w->pctx, buf, len, JS_READ_OBJ_REFERENCE);
    free(buf);
    if (JS_IsException(v)) { JS_FreeValue(w->pctx, JS_GetException(w->pctx)); return JS_UNDEFINED; }
    JSValue callee = get_method(w->pctx, w->worker_obj, "__deliverMessage");
    if (!JS_IsFunction(w->pctx, callee)) { JS_FreeValue(w->pctx, callee); JS_FreeValue(w->pctx, v); return JS_UNDEFINED; }
    JSValue *args = malloc(sizeof(JSValue));
    if (!args) { JS_FreeValue(w->pctx, callee); JS_FreeValue(w->pctx, v); return JS_UNDEFINED; }
    args[0] = v;
    if (task_add(w->id, WTK_CALL, w->pctx, callee, JS_DupValue(w->pctx, w->worker_obj), args, 1,
                 (long long)js_page_now_ms(), 0, 0))
        worker_diag(w->id, "outbound-message-enqueue", "message");
    return JS_UNDEFINED;
}

static JSValue js__wClose(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv, int wid)
{
    (void)ctx; (void)t; (void)argc; (void)argv;
    struct jsworker *w = find_worker(wid);
    if (w) mark_dead(w);           /* item 17: no error event, idempotent */
    return JS_UNDEFINED;
}

/* importScripts: synchronous by spec, so a network failure or a script error
 * propagates as a normal thrown exception right here -- it either reaches the
 * worker's own top-level catch, or (uncaught) is picked up by the SAME
 * exception path worker_start()/the task executor already give every other
 * top-level throw, per the file header's termination bar. Never returns
 * having silently loaded nothing. */
static JSValue js__wImportScripts(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv, int wid)
{
    (void)t;
    struct jsworker *w = find_worker(wid);
    if (!w) return JS_UNDEFINED;
    for (int i = 0; i < argc; i++) {
        const char *ref = JS_ToCString(ctx, argv[i]);
        if (!ref) return JS_EXCEPTION;
        char abs[600];
        int ok = (bfetch_resolve(w->url, ref, abs, sizeof abs) == 0);
        JS_FreeCString(ctx, ref);
        if (!ok) return throw_dom_exception(ctx, "SyntaxError", "importScripts: the URL could not be parsed");
        unsigned char *src = 0; int srclen = 0;
        if (!worker_response_allowed(w,JSW_IMPORT,abs))
            return throw_dom_exception(ctx,"SecurityError","importScripts blocked by worker policy");
        int fetched=w->policy.allow ?
            (w->policy.load?w->policy.load(w->policy.opaque,abs,&src,&srclen):-1):
            bfetch_sync(abs,&src,&srclen);
        if (fetched != 0)
            return throw_dom_exception(ctx, "NetworkError", "importScripts: the script could not be fetched");
        JSValue r = JS_Eval(ctx, (const char *)src, (size_t)srclen, abs, JS_EVAL_TYPE_GLOBAL);
        free(src);
        if (JS_IsException(r)) return r;
        JS_FreeValue(ctx, r);
    }
    return JS_UNDEFINED;
}

static JSValue wtimer_add(JSContext *ctx, int wid, int interval, int argc, JSValueConst *argv)
{
    struct jsworker *w = find_worker(wid);
    if (!w || argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_NewInt32(ctx, 0);
    int32_t ms = 0;
    if (argc > 1) JS_ToInt32(ctx, &ms, argv[1]);
    if (ms < 0) ms = 0;
    int extra = argc > 2 ? argc - 2 : 0;
    JSValue *args = 0;
    if (extra > 0) {
        args = malloc((size_t)extra * sizeof *args);
        if (args) for (int i = 0; i < extra; i++) args[i] = JS_DupValue(ctx, argv[2 + i]);
        else extra = 0;
    }
    int id = g_next_timer_id++;
    task_add(wid, WTK_CALL, ctx, JS_DupValue(ctx, argv[0]), JS_UNDEFINED, args, extra,
             (long long)js_page_now_ms() + ms, interval ? (ms > 0 ? ms : 1) : 0, id);
    return JS_NewInt32(ctx, id);
}
static JSValue js__wSetTimeout(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv, int wid)
{ (void)t; return wtimer_add(ctx, wid, 0, argc, argv); }
static JSValue js__wSetInterval(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv, int wid)
{ (void)t; return wtimer_add(ctx, wid, 1, argc, argv); }
static JSValue js__wClearTimer(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv, int wid)
{
    (void)t; (void)wid;
    int32_t id = 0;
    if (argc > 0) JS_ToInt32(ctx, &id, argv[0]);
    if (id == 0) return JS_UNDEFINED;
    for (struct wtask **pp = &g_tasks; *pp; pp = &(*pp)->next)
        if ((*pp)->timer_id == id) {
            struct wtask *victim = *pp;
            *pp = victim->next;
            task_free(victim);
            return JS_UNDEFINED;
        }
    return JS_UNDEFINED;
}

static JSValue wcon_out(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv, int wid)
{
    (void)t;
    printf("[worker %d] ", wid);
    for (int i = 0; i < argc; i++) {
        const char *s = JS_ToCString(ctx, argv[i]);
        if (!s) continue;
        if (i) printf(" ");
        printf("%s", s);
        JS_FreeCString(ctx, s);
    }
    printf("\n");
    return JS_UNDEFINED;
}
static JSValue wperf_now(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv, int wid)
{ (void)t; (void)argc; (void)argv; (void)wid; return JS_NewFloat64(ctx, (double)js_page_now_ms()); }

static JSValue wlocation_string(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv, int wid)
{
    (void)t;(void)argc;(void)argv;
    struct jsworker *w=find_worker(wid);
    return JS_NewString(ctx,w?w->url:"");
}

static int blob_reference(const char *s)
{
    const char *scheme="blob:";
    for(int i=0;i<5;i++){unsigned char c=(unsigned char)s[i];if(c>='A'&&c<='Z')c+='a'-'A';if(c!=scheme[i])return 0;}
    return 1;
}

/* The purely-JS half of the worker's global scope: a minimal, self-contained
 * DOMException (the worker context has no js_dom.c, so js_platform.c's own
 * copy is not reachable from here -- duplicating a dozen lines beats reaching
 * across a module boundary for a private string constant), the nested-worker
 * refusal, a tiny EventTarget-lite for `self`, and the two delivery points
 * the C side calls by name: __deliverMessage (item 13) and
 * __reportWorkerError (the addEventListener half of items 10-11; the
 * self.onerror half is handled directly in C, see worker_report_uncaught). */
static const char WORKER_SELF_JS[] =
"(function(){\n"
"var G = globalThis;\n"
"G.DOMException = G.DOMException || (function () {\n"
"  function DOMException(message, name) {\n"
"    var e = Error.call(this, message);\n"
"    this.message = message === undefined ? '' : String(message);\n"
"    this.name = name === undefined ? 'Error' : String(name);\n"
"    this.stack = e.stack;\n"
"  }\n"
"  DOMException.prototype = Object.create(Error.prototype);\n"
"  DOMException.prototype.constructor = DOMException;\n"
"  return DOMException;\n"
"})();\n"
"G.Worker = function Worker() {\n"
"  throw new G.DOMException('nested workers are not supported', 'NotSupportedError');\n"
"};\n"
"var listeners = Object.create(null);\n"
"G.addEventListener = function (type, cb) {\n"
"  if (typeof cb !== 'function') return;\n"
"  var l = listeners[type] || (listeners[type] = []);\n"
"  if (l.indexOf(cb) < 0) l.push(cb);\n"
"};\n"
"G.removeEventListener = function (type, cb) {\n"
"  var l = listeners[type]; if (!l) return;\n"
"  var i = l.indexOf(cb); if (i >= 0) l.splice(i, 1);\n"
"};\n"
"G.dispatchEvent = function (ev) {\n"
"  var l = (listeners[ev.type] || []).slice();\n"
"  for (var i = 0; i < l.length; i++) {\n"
"    try { l[i].call(G, ev); } catch (e) { try { console.error(e); } catch (q) {} }\n"
"  }\n"
"  return !ev.defaultPrevented;\n"
"};\n"
"G.__deliverMessage = function (data) {\n"
"  var ev = { type: 'message', data: data, origin: '', lastEventId: '', source: null, ports: [],\n"
"             defaultPrevented: false, preventDefault: function () { this.defaultPrevented = true; },\n"
"             target: G, currentTarget: G };\n"
"  if (typeof G.onmessage === 'function') {\n"
"    try { G.onmessage(ev); } catch (e) { try { console.error(e); } catch (q) {} }\n"
"  }\n"
"  G.dispatchEvent(ev);\n"
"};\n"
"G.__reportWorkerError = function (message, filename) {\n"
"  var ev = { type: 'error', message: message, filename: filename, lineno: 0, colno: 0, error: null,\n"
"             defaultPrevented: false, preventDefault: function () { this.defaultPrevented = true; },\n"
"             target: G, currentTarget: G };\n"
"  var l = (listeners['error'] || []).slice();\n"
"  for (var i = 0; i < l.length; i++) { try { l[i].call(G, ev); } catch (e) {} }\n"
"  return ev.defaultPrevented;\n"
"};\n"
"return 'ok';\n"
"})();\n";

static int worker_install_globals(struct jsworker *w)
{
    JSContext *ctx = w->wctx;
    int wid = w->id;
#ifndef WORKER_NO_PURE_GLOBALS
    /* Pure per-runtime constructors only. Installing window Web APIs here
     * would overwrite the parent's fetch/location/Blob registry singleton. */
    if(LOGIT_HAVE(js_url_install_core)&&js_url_install_core(ctx)<0)return -1;
    if(LOGIT_HAVE(js_webapi_install_encoding)&&js_webapi_install_encoding(ctx)<0)return -1;
#endif
    JSValue g = JS_GetGlobalObject(ctx);
    w->self_obj = JS_DupValue(ctx, g);
    JS_SetPropertyStr(ctx, g, "self", JS_DupValue(ctx, g));
    JS_SetPropertyStr(ctx, g, "onmessage", JS_NULL);
    JS_SetPropertyStr(ctx, g, "onerror", JS_NULL);
    JS_SetPropertyStr(ctx, g, "postMessage",
        JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js__wPostMessage, "postMessage", 1, JS_CFUNC_generic_magic, wid));
    JS_SetPropertyStr(ctx, g, "close",
        JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js__wClose, "close", 0, JS_CFUNC_generic_magic, wid));
    JS_SetPropertyStr(ctx, g, "importScripts",
        JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js__wImportScripts, "importScripts", 1, JS_CFUNC_generic_magic, wid));
    JS_SetPropertyStr(ctx, g, "setTimeout",
        JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js__wSetTimeout, "setTimeout", 2, JS_CFUNC_generic_magic, wid));
    JS_SetPropertyStr(ctx, g, "setInterval",
        JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js__wSetInterval, "setInterval", 2, JS_CFUNC_generic_magic, wid));
    JS_SetPropertyStr(ctx, g, "clearTimeout",
        JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js__wClearTimer, "clearTimeout", 1, JS_CFUNC_generic_magic, wid));
    JS_SetPropertyStr(ctx, g, "clearInterval",
        JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js__wClearTimer, "clearInterval", 1, JS_CFUNC_generic_magic, wid));
    JSValue con = JS_NewObject(ctx);
    JSValue logf = JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)wcon_out, "log", 1, JS_CFUNC_generic_magic, wid);
    JS_SetPropertyStr(ctx, con, "log", JS_DupValue(ctx, logf));
    JS_SetPropertyStr(ctx, con, "info", JS_DupValue(ctx, logf));
    JS_SetPropertyStr(ctx, con, "debug", JS_DupValue(ctx, logf));
    JS_SetPropertyStr(ctx, con, "warn", JS_DupValue(ctx, logf));
    JS_SetPropertyStr(ctx, con, "error", logf);
    JS_SetPropertyStr(ctx, g, "console", con);
    JSValue perf = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, perf, "now",
        JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)wperf_now, "now", 0, JS_CFUNC_generic_magic, wid));
    JS_SetPropertyStr(ctx, g, "performance", perf);
    JSValue nav = JS_NewObject(ctx);
    /* NOT parallelism -- see the file header. A worker that read this as a
     * real core count would spawn N of itself expecting N cores. */
    JS_SetPropertyStr(ctx, nav, "hardwareConcurrency", JS_NewInt32(ctx, 1));
    JS_SetPropertyStr(ctx, nav, "userAgent", JS_NewString(ctx, "Mozilla/5.0 (LogitOS; x86_64) Logit/1.0"));
    JS_SetPropertyStr(ctx, g, "navigator", nav);
    JSValue loc = JS_NewObject(ctx);
    /* WorkerLocation is a URL stringifier, not [object Object]. Adapters
     * legitimately pass location itself as the base argument to new URL. */
    JS_DefinePropertyValueStr(ctx, loc, "href", JS_NewString(ctx, w->url), JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(ctx, loc, "origin", JS_NewString(ctx, w->creator_origin), JS_PROP_ENUMERABLE);
    JS_SetPropertyStr(ctx,loc,"toString",JS_NewCFunctionMagic(ctx,(JSCFunctionMagic*)wlocation_string,"toString",0,JS_CFUNC_generic_magic,wid));
    JS_SetPropertyStr(ctx, g, "location", loc);
    JS_SetPropertyStr(ctx, g, "origin", JS_NewString(ctx, w->creator_origin));
    JS_FreeValue(ctx, g);

    JSValue r = JS_Eval(ctx, WORKER_SELF_JS, sizeof WORKER_SELF_JS - 1, "<js_worker self>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        worker_diag(w->id, "install", "exception");
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        fprintf(stderr, "js_worker: self-scope install failed: %s\n", m ? m : "?");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
        JS_FreeValue(ctx, r);
        return -1;
    }
    JS_FreeValue(ctx, r);
#ifndef WORKER_NO_FETCH
    if(LOGIT_HAVE(js_webapi_fetch_install)&&
       js_webapi_fetch_install(ctx,w->url,w->creator_origin,
           worker_owner->policy.allow?worker_owner->policy.site_url:w->creator_origin)<0)return -1;
    if(worker_owner->policy.allow&&LOGIT_HAVE(js_webapi_fetch_install)&&
       (!LOGIT_HAVE(js_webapi_set_connect_policy)||
        !js_webapi_set_connect_policy(ctx,worker_connect,w)))return -1;
#endif
#ifndef WORKER_NO_WASM_REALM
    /* Embedded CSP's wasm code-generation gate is not implemented here.
     * Keep WebAssembly absent in embedded workers until it has that gate. */
    if(!worker_owner->policy.allow&&LOGIT_HAVE(js_wasm_install))js_wasm_install(ctx);
#endif
    if(LOGIT_HAVE(js_ports_install)&&!js_ports_install(ctx))return -1;
    return 0;
}

/* =========================================================================
 * starting a worker -- the deferred task every `new Worker()` schedules
 * ========================================================================= */
/* Every ending this function can reach:
 *   - cross-origin script URL           -> deliver_error_to_parent, mark_dead (item 8)
 *   - fetch fails / 404 / empty URL     -> deliver_error_to_parent, mark_dead (item 9)
 *   - out of memory starting a runtime  -> deliver_error_to_parent, mark_dead
 *   - the script does not compile/run   -> worker_report_uncaught, mark_dead (items 10-11)
 *   - the watchdog fires during the
 *     top-level run                     -> silent, mark_dead (item 12)
 *   - success                           -> WK_RUNNING, queued inbound messages drained
 * Never returns leaving the worker in WK_STARTING. */
static void worker_start(struct jsworker *w)
{
    worker_diag(w->id, "start", "begin");
    struct url docu=w->creator_url;
    int have_doc = w->creator_valid;
    struct url wu;
    if (!w->is_blob && have_doc && url_parse(w->url, &wu) == 0) {
        int cross = (wu.https != docu.https) || wu.port != docu.port || strcmp(wu.host, docu.host);
        if (cross) {
            worker_diag(w->id, "start", "origin-rejected");
            deliver_error_to_parent(w, "cross-origin script URL for Worker", w->url, 0, 0);
            mark_dead_keep_notify(w);
            return;
        }
    }
    /* have_doc == 0, or w->url does not parse as an http(s) URL at all (the
     * WPT test runner's bfetch_resolve resolves into a local FILE PATH, not a
     * URL -- see js_worker.c's callers): cannot determine origin, so this
     * does not block. That is the correct default for "cannot tell" and it
     * matches what a page running from about:blank or a synthetic name
     * already gets from every other same-origin check in this tree. */

    unsigned char *src = 0; int srclen = 0;
    worker_diag(w->id, "fetch", "begin");
    int fetched;
    if(w->is_blob){src=w->blob_source;srclen=w->blob_length;w->blob_source=0;fetched=src?0:-1;}
    else if(worker_owner->policy.allow){
        struct js_worker_policy response={0};
        fetched=worker_owner->policy.entry?
            worker_owner->policy.entry(worker_owner->policy.opaque,w->url,sizeof w->url,&src,&srclen,&response):-1;
        if(fetched==0){
            /* The document admits the entry request, but only its response
             * defines the network worker's script/connect/eval policy. Blob
             * workers instead keep the copied creator callbacks. */
            if(!response.allow){
                free(src);src=0;
                if(response.release)response.release(response.opaque);
                fetched=-1;
            }else w->policy=response;
        }
    }else fetched=bfetch_sync(w->url, &src, &srclen);
    if (fetched != 0) {
        worker_diag(w->id, "fetch", "failed");
        deliver_error_to_parent(w, "could not fetch the worker script", w->url, 0, 0);
        mark_dead_keep_notify(w);
        return;
    }
    worker_diag(w->id, "fetch", "complete");

    JSRuntime *rt = JS_NewRuntime();
    if (!rt) {
        worker_diag(w->id, "start", "runtime-allocation");
        free(src);
        deliver_error_to_parent(w, "out of memory starting the worker", w->url, 0, 0);
        mark_dead_keep_notify(w);
        return;
    }
    /* Same bound as the page's own runtime (js_page.c:590) and the same
     * reason: the ring-3 stack is 8 MiB, and the guard must leave room below
     * it for the THROW that reports its own overflow. */
    JS_SetMaxStackSize(rt, 2 * 1024 * 1024);
    w->rt = rt;
    JS_SetInterruptHandler(rt, worker_slice_interrupt, w);
    JSContext *wctx = JS_NewContext(rt);
    if (!wctx) {
        worker_diag(w->id, "start", "context-allocation");
        JS_FreeRuntime(rt);
        w->rt = 0;
        free(src);
        deliver_error_to_parent(w, "out of memory starting the worker", w->url, 0, 0);
        mark_dead_keep_notify(w);
        return;
    }
    w->wctx = wctx;
#ifdef JS_RUNTIME_DIAGNOSTICS
    worker_promise_diag_install(wctx, &w->promise_diag, w->id);
#endif
    if(worker_install_globals(w)<0){
        free(src);
        /* Pure-global installation has no parent/worker JS callbacks yet. */
        JS_FreeValue(wctx,JS_GetException(wctx));
        deliver_error_to_parent(w,"could not initialize worker globals",w->url,0,0);
        mark_dead_keep_notify(w);return;
    }

    worker_diag(w->id, "eval", "begin");
    worker_slice_begin(w);
    WORKER_PHASE(w,"eval");
    JSValue r = JS_Eval(wctx, (const char *)src, (size_t)srclen, w->url, JS_EVAL_TYPE_GLOBAL);
    free(src);
    if (JS_IsException(r)) {
        worker_diag(w->id, "eval", w->wd_hit ? "watchdog" : "exception");
        JS_FreeValue(wctx, r);
        JSValue e = JS_GetException(wctx);
        worker_report_uncaught(w, e);   /* items 10-12: handles the watchdog case too */
        JS_FreeValue(wctx, e);
        mark_dead_keep_notify(w);       /* idempotent if worker_report_uncaught already marked it */
        return;
    }
    worker_diag(w->id, "eval", "complete");
    JS_FreeValue(wctx, r);
    worker_drain_jobs(w);   /* the top-level script itself can enqueue a job, e.g. `Promise.resolve().then(f)` at global scope -- drain before the WK_DEAD check below, since a drained job can itself call close() */
    /* BUG FOUND BY tests/unit/worker_test.c's quiescence gate, fixed here:
     * the top-level script itself can call close() (WORKER_SELF_JS's `close`
     * -> js__wClose -> mark_dead) before this line ever runs -- exactly the
     * same hazard the loop below already guards against ("the message itself
     * could close() it") but that guard did nothing for the script that ran
     * BEFORE it. Without this check, a worker whose first line is close()
     * got unconditionally resurrected from WK_DEAD back to WK_RUNNING right
     * here: free_pending stayed 1 but state was no longer WK_DEAD, so
     * reap_dead() could never reap it (its own guard is `state != WK_DEAD ||
     * !free_pending`), and any task the closing script scheduled afterward
     * (e.g. a setTimeout meant to never fire once closed) ran normally
     * instead of being dropped by run_due's WK_DEAD check -- a silently
     * un-terminated worker holding a runtime forever, discovered because the
     * negative control this bug should have tripped (-DJS_WORKER_NO_TERMINATE)
     * was not what caught it: this reproduced on the PLAIN build. */
    if (w->state == WK_DEAD) return;
    w->state = WK_RUNNING;
    worker_diag(w->id, "start", "running");

    worker_flush_inbound(w);
}

/* =========================================================================
 * the parent-side C bindings -- __workerCreate/__workerRegister/
 * __workerPostToWorker/__workerTerminate, called from the JS prelude below
 * ========================================================================= */
static JSValue js__workerCreate(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    if (argc < 1) return JS_ThrowTypeError(ctx, "Worker requires a URL");
    const char *href = JS_ToCString(ctx, argv[0]);
    if (!href) return JS_EXCEPTION;
    int is_blob=0;
#ifndef JS_WORKER_NO_BLOB
    is_blob=blob_reference(href);
#endif

    /* A directly-absolute reference is validated with THIS tree's own URL
     * parser before ever reaching bfetch_resolve. That extra step matters
     * specifically under the WPT test runner: its bfetch_resolve
     * (tests/unit/wpt_test.c:264) is a local-file-path stitcher that resolves
     * ANY string and never fails, so a malformed absolute URL --
     * `https://{{host}}:{{ports[http][0]}}/...`, the literal, unsubstituted
     * template WPT's same-origin-check.sub.html embeds -- would otherwise
     * sail through as if it were an ordinary relative script path instead of
     * failing synchronously the way the spec requires. url_parse rejects it
     * on the port digits (c/net/http/url.c:39-44), which is the actual
     * reason this specific corpus passes rather than an assumption.
     *
     * A SECOND check next to it, for a case url_parse alone does not catch:
     * it stops consuming the port at the first non-digit rather than
     * requiring the authority to END there, so `http://invalid:123$` --
     * WPT's own literal example for this exact test -- "parses" as host
     * `invalid` port `123` with the `$` silently dropped. This walks the
     * authority (the "://" up to the next '/') and requires the WHOLE of it
     * to be host/port grammar; anything else is the SyntaxError url_parse
     * missed. General URL hygiene, not a rule about this one string. */
    if (!is_blob && strstr(href, "://")) {
        struct url probe;
        int bad = (url_parse(href, &probe) != 0);
        if (!bad) {
            const char *auth = strstr(href, "://") + 3;
            const char *slash = strchr(auth, '/');
            size_t authlen = slash ? (size_t)(slash - auth) : strlen(auth);
            for (size_t i = 0; i < authlen && !bad; i++) {
                char c = auth[i];
                int ok_char = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                             (c >= '0' && c <= '9') || c == '.' || c == '-' ||
                             c == ':' || c == '_' || c == '[' || c == ']';
                if (!ok_char) bad = 1;
            }
        }
        if (bad) {
            worker_diag(0, "constructor", "invalid-url");
            JS_FreeCString(ctx, href);
            return throw_dom_exception(ctx, "SyntaxError", "Worker: the script URL could not be parsed");
        }
    }

    char abs[600];
    int ok;
    if(is_blob){size_t n=strlen(href);ok=n<sizeof abs;if(ok){memcpy(abs,href,n+1);memcpy(abs,"blob:",5);}}
    else ok=(bfetch_resolve(worker_owner->policy.allow?js_page_location():0, href, abs, sizeof abs) == 0);
    JS_FreeCString(ctx, href);
    if (!ok) {
        worker_diag(0, "constructor", "unresolved-url");
        return throw_dom_exception(ctx, "SyntaxError", "Worker: the script URL could not be parsed");
    }

    int slot = -1;
    if(worker_owner->policy.allow){
        if(!is_blob&&!worker_owner->policy.entry)return throw_dom_exception(ctx,"NotSupportedError","Embedded network Worker policy is not implemented");
        if(!worker_allowed(JSW_CREATE,abs))return throw_dom_exception(ctx,"SecurityError","Worker blocked by document policy");
    }
    for (int i = 0; i < JSW_MAX_WORKERS; i++) if (!g_workers[i].used) { slot = i; break; }
    if (slot < 0) {
        worker_diag(0, "constructor", "capacity");
        return throw_dom_exception(ctx, "NotSupportedError", "Worker: too many workers are already running");
    }

    struct jsworker *w = &g_workers[slot];
    memset(w, 0, sizeof *w);
    w->used = 1;
    w->id = g_next_worker_id++;
    worker_diag(w->id, "constructor", "created");
    w->state = WK_STARTING;
    w->pctx = ctx;
    w->worker_obj = JS_UNDEFINED;
    w->self_obj = JS_UNDEFINED;
    w->is_blob=is_blob;
    w->policy=worker_owner->policy;w->policy.release=0;
    w->creator_valid=url_parse(js_page_location(),&w->creator_url)==0;
    if(w->creator_valid){
        const struct url *u=&w->creator_url;
        if(u->port==(u->https?443:80))snprintf(w->creator_origin,sizeof w->creator_origin,"%s://%s",u->https?"https":"http",u->host);
        else snprintf(w->creator_origin,sizeof w->creator_origin,"%s://%s:%u",u->https?"https":"http",u->host,(unsigned)u->port);
    }else strcpy(w->creator_origin,"null");
    if(is_blob&&LOGIT_HAVE(js_webapi_blob_snapshot)){
        /* Eight workers already bound the registry. A separate 8 MiB source
         * cap bounds pinned copies even if a caller revokes and reuses its
         * object-URL quota before these deferred startups are dispatched. */
        int found=js_webapi_blob_snapshot(ctx,abs,&w->blob_source,&w->blob_length,
            8*1024*1024,w->creator_origin,sizeof w->creator_origin);
#ifdef JS_WORKER_BLOB_ALLOC_AUDIT
        if(found==1)worker_blob_test_allocated(w->blob_source);
#endif
        if(found!=1)worker_diag(w->id,"constructor","blob-unavailable");
    }
    { size_t n = strlen(abs); if (n >= sizeof w->url) n = sizeof w->url - 1;
      memcpy(w->url, abs, n); w->url[n] = 0; }

    /* Deferred, always -- even a URL that will turn out to fail cross-origin
     * or 404 must not decide the Worker's fate before this call returns: the
     * constructor's contract is "never a Worker whose fate is undecided AT
     * RETURN", and a script set to run synchronously right here could not be
     * un-run if `w.onerror = fn` was the very next statement. */
    if(!task_add(w->id, WTK_START, NULL, JS_UNDEFINED, JS_UNDEFINED, NULL, 0,
             (long long)js_page_now_ms(), 0, 0)){
        /* Returning a live id without its only startup task strands the
         * worker forever. No instance has been registered yet, so roll back
         * the slot/source now and report the constructor allocation error. */
        free(w->blob_source);memset(w,0,sizeof *w);
        return JS_ThrowOutOfMemory(ctx);
    }
    return JS_NewInt32(ctx, w->id);
}

static JSValue js__workerRegister(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    if (argc < 2) return JS_UNDEFINED;
    int32_t id = 0; JS_ToInt32(ctx, &id, argv[0]);
    struct jsworker *w = find_worker(id);
    if (w) w->worker_obj = JS_DupValue(ctx, argv[1]);
    return JS_UNDEFINED;
}

static JSValue js__workerPostToWorker(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    if (argc < 1) return JS_UNDEFINED;
    int32_t id = 0; JS_ToInt32(ctx, &id, argv[0]);
    struct jsworker *w = find_worker(id);
    JSValueConst data = argc > 1 ? argv[1] : JS_UNDEFINED;

    size_t len = 0;
    unsigned char *buf = clone_write(ctx, data, &len);
    if (!buf) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return throw_dom_exception(ctx, "DataCloneError", "postMessage: value could not be cloned");
    }
#ifndef JS_WORKER_NO_TERMINATE
    /* item 15: a dead worker drops the message and returns undefined --
     * never a throw, never a queue that fills forever waiting on a peer that
     * cannot ever answer. -DJS_WORKER_NO_TERMINATE removes exactly this
     * check (see the control below) so the message queues onto a worker that
     * will never run it. */
    if (!w || w->state == WK_DEAD) { free(buf); return JS_UNDEFINED; }
#else
    if (!w) { free(buf); return JS_UNDEFINED; }
#endif

    if (w->state == WK_STARTING || w->inbound_head) {
        struct pending_msg *m = malloc(sizeof *m);
        if (!m) { free(buf); return JS_UNDEFINED; }
        m->buf = buf; m->len = len; m->next = NULL;
        if (w->inbound_tail) w->inbound_tail->next = m; else w->inbound_head = m;
        w->inbound_tail = m;
        return JS_UNDEFINED;
    }

    JSValue v = JS_ReadObject(w->wctx, buf, len, JS_READ_OBJ_REFERENCE);
    free(buf);
    if (JS_IsException(v)) { JS_FreeValue(w->wctx, JS_GetException(w->wctx)); return JS_UNDEFINED; }
    JSValue g = JS_GetGlobalObject(w->wctx);
    JSValue callee = get_method(w->wctx, g, "__deliverMessage");
    JS_FreeValue(w->wctx, g);
    if (!JS_IsFunction(w->wctx, callee)) { JS_FreeValue(w->wctx, callee); JS_FreeValue(w->wctx, v); return JS_UNDEFINED; }
    JSValue *args = malloc(sizeof(JSValue));
    if (!args) { JS_FreeValue(w->wctx, callee); JS_FreeValue(w->wctx, v); return JS_UNDEFINED; }
    args[0] = v;
    task_add(w->id, WTK_MESSAGE, w->wctx, callee, JS_UNDEFINED, args, 1,
             (long long)js_page_now_ms(), 0, 0);
    return JS_UNDEFINED;
}

static JSValue js__workerTerminate(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)ctx; (void)t;
    if (argc < 1) return JS_UNDEFINED;
    int32_t id = 0; JS_ToInt32(ctx, &id, argv[0]);
    struct jsworker *w = find_worker(id);
    if (w && w->pctx==ctx) {
        mark_dead(w);   /* Previously always reaped on the next run_due pass. */
#ifndef JS_WORKER_NO_TERMINATE
        /* This entry executes in the parent, so no child JS_Call is active.
         * Reap now: terminate must release its last network resource without
         * keeping js_worker_pending true just for cleanup. self.close still
         * uses the deferred path because its own runtime IS on the stack. */
        reap_dead();
#endif
    }
    return JS_UNDEFINED;
}

/* The Worker constructor, its instance surface, and the two delivery points
 * the C side calls by name (__deliverMessage / __deliverError). A
 * self-contained EventTarget-lite rather than js_events.c's real one: that
 * file's whole install is gated on a native `Event` class existing
 * (js_events.c:93, `if (!NEvent || !NEvent.prototype) return`), which only
 * js_dom_init publishes -- reaching for it here would make Worker's
 * availability depend on the DOM layer having run, for no benefit a worker
 * needs. MessageEvent/ErrorEvent ARE reused when js_platform.c/js_events.c
 * installed real ones (both are DOM-independent), with a duck-typed fallback
 * otherwise -- see __deliverMessage/__deliverError below. */
static const char WORKER_PARENT_JS[] =
#ifdef JS_RUNTIME_DIAGNOSTICS
"(function (__workerDiag) {\n"
#else
"(function () {\n"
#endif
"var G = globalThis;\n"
"if (G.Worker) return 'skip';\n"
"function DE(msg, name) {\n"
"  if (typeof G.DOMException === 'function') { try { return new G.DOMException(msg, name); } catch (q) {} }\n"
"  var e = new Error(msg); e.name = name; return e;\n"
"}\n"
"function Worker(url, options) {\n"
"  if (!new.target) throw new TypeError(\"Constructor 'Worker' requires 'new'\");\n"
"  if (arguments.length < 1) throw new TypeError('Worker requires 1 argument');\n"
"  var href = String(url);\n"
"  var opts = (options && typeof options === 'object') ? options : {};\n"
"  if (opts.type === 'module') {\n"
#ifdef JS_RUNTIME_DIAGNOSTICS
"    __workerDiag(1);\n"
#endif
"    throw DE('module workers are not supported', 'NotSupportedError'); }\n"
"  var id = __workerCreate(href);\n"
"  this._wid = id;\n"
"  this._listeners = Object.create(null);\n"
"  this.onmessage = null;\n"
"  this.onmessageerror = null;\n"
"  this.onerror = null;\n"
"  __workerRegister(id, this);\n"
"}\n"
"Worker.prototype.postMessage = function (data) {\n"
"  if (arguments.length > 1) {\n"
"    var opt = arguments[1];\n"
"    var xfer = Array.isArray(opt) ? opt : (opt && typeof opt === 'object' ? opt.transfer : undefined);\n"
#ifdef JS_RUNTIME_DIAGNOSTICS
"    if (xfer && xfer.length > 0) __workerDiag(2);\n"
#endif
"    if (xfer && xfer.length > 0) throw DE('transferable objects are not supported', 'DataCloneError');\n"
"  }\n"
"  __workerPostToWorker(this._wid, data);\n"
"};\n"
"Worker.prototype.terminate = function () { __workerTerminate(this._wid); };\n"
"Worker.prototype.addEventListener = function (type, cb) {\n"
"  if (typeof cb !== 'function') return;\n"
"  var l = this._listeners[type] || (this._listeners[type] = []);\n"
"  if (l.indexOf(cb) < 0) l.push(cb);\n"
"};\n"
"Worker.prototype.removeEventListener = function (type, cb) {\n"
"  var l = this._listeners[type]; if (!l) return;\n"
"  var i = l.indexOf(cb); if (i >= 0) l.splice(i, 1);\n"
"};\n"
"Worker.prototype.dispatchEvent = function (ev) {\n"
"  var l = (this._listeners[ev.type] || []).slice();\n"
"  for (var i = 0; i < l.length; i++) {\n"
"    try { l[i].call(this, ev); } catch (e) { if (G.reportError) { try { G.reportError(e); } catch (q) {} } }\n"
"  }\n"
"  var onx = this['on' + ev.type];\n"
"  if (typeof onx === 'function') {\n"
"    try { onx.call(this, ev); } catch (e) { if (G.reportError) { try { G.reportError(e); } catch (q) {} } }\n"
"  }\n"
"  return !ev.defaultPrevented;\n"
"};\n"
"Worker.prototype.__deliverMessage = function (data, ports) {\n"
"  var ev;\n"
"  if (typeof G.MessageEvent === 'function') { try { ev = new G.MessageEvent('message', { data: data, ports: ports || [] }); } catch (q) {} }\n"
"  if (!ev) ev = { type: 'message', data: data, ports: ports || [], defaultPrevented: false,\n"
"                  preventDefault: function () { this.defaultPrevented = true; } };\n"
"  this.dispatchEvent(ev);\n"
"};\n"
"Worker.prototype.__deliverError = function (message, filename, lineno, colno) {\n"
"  var ev;\n"
"  if (typeof G.ErrorEvent === 'function') {\n"
"    try { ev = new G.ErrorEvent('error', { message: message, filename: filename, lineno: lineno, colno: colno }); } catch (q) {}\n"
"  }\n"
"  if (!ev) ev = { type: 'error', message: message, filename: filename, lineno: lineno, colno: colno,\n"
"                  error: null, defaultPrevented: false,\n"
"                  preventDefault: function () { this.defaultPrevented = true; } };\n"
"  this.dispatchEvent(ev);\n"
"};\n"
"G.Worker = Worker;\n"
"return 'ok';\n"
#ifdef JS_RUNTIME_DIAGNOSTICS
"})\n";
#else
"})();\n";
#endif

void js_worker_install(JSContext *ctx)
{
    if (!ctx) return;
#ifdef JS_RUNTIME_DIAGNOSTICS
    worker_diag_lines = 0;
#endif
    JSValue g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "__workerCreate", JS_NewCFunction(ctx, js__workerCreate, "__workerCreate", 1));
    JS_SetPropertyStr(ctx, g, "__workerRegister", JS_NewCFunction(ctx, js__workerRegister, "__workerRegister", 2));
    JS_SetPropertyStr(ctx, g, "__workerPostToWorker", JS_NewCFunction(ctx, js__workerPostToWorker, "__workerPostToWorker", 2));
    JS_SetPropertyStr(ctx, g, "__workerTerminate", JS_NewCFunction(ctx, js__workerTerminate, "__workerTerminate", 1));
    JS_FreeValue(ctx, g);

    JSValue r = JS_Eval(ctx, WORKER_PARENT_JS, sizeof WORKER_PARENT_JS - 1, "<js_worker>", JS_EVAL_TYPE_GLOBAL);
#ifdef JS_RUNTIME_DIAGNOSTICS
    if (!JS_IsException(r)) {
        JSValue hook = JS_NewCFunction(ctx, js_worker_diag_ctor, "workerDiagnostic", 1);
        JSValue result = JS_Call(ctx, r, JS_UNDEFINED, 1, (JSValueConst *)&hook);
        JS_FreeValue(ctx, hook); JS_FreeValue(ctx, r); r = result;
    }
#endif
    if (JS_IsException(r)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        fprintf(stderr, "js_worker: install failed: %s\n", m ? m : "?");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, r);
}

/* =========================================================================
 * the door js_page.c calls through -- see js_worker.h
 * ========================================================================= */
int js_worker_pending(void)
{
    if(g_tasks)return 1;
    for(int i=0;i<JSW_MAX_WORKERS;i++)if(g_workers[i].used&&g_workers[i].free_pending)return 1;
    for(int i=0;i<JSW_MAX_WORKERS;i++){
        struct jsworker *w=&g_workers[i];
        if(!w->used||w->state!=WK_RUNNING)continue;
        if(w->inbound_head)return 1;
        if(LOGIT_HAVE(js_ports_pending)&&js_ports_pending(w->wctx))return 1;
#ifndef JS_TASK_HIDE_PENDING_JOBS
        if(worker_jobs_pending(w))return 1;
#endif
    }
    if(LOGIT_HAVE(js_webapi_fetch_pending))for(int i=0;i<JSW_MAX_WORKERS;i++){
        struct jsworker *w=&g_workers[i];
        if(w->used&&w->state==WK_RUNNING&&js_webapi_fetch_pending(w->wctx))return 1;
    }
    g_worker_owner_cursor=0;g_worker_task_phase=0;g_worker_task_snapshot_active=0;
    return 0;
}

long long js_worker_next_due(void)
{
    long long best = -1;
    /* Termination can remove the last timer and stop the last socket. Keep
     * one scheduler turn alive so deferred destruction itself still runs. */
    for(int i=0;i<JSW_MAX_WORKERS;i++)if(g_workers[i].used&&g_workers[i].free_pending)return (long long)js_page_now_ms();
    for(int i=0;i<JSW_MAX_WORKERS;i++){
        struct jsworker *w=&g_workers[i];
        if(!w->used||w->state!=WK_RUNNING)continue;
        if(w->inbound_head)return (long long)js_page_now_ms();
        if(LOGIT_HAVE(js_ports_pending)&&js_ports_pending(w->wctx))return (long long)js_page_now_ms();
#ifndef JS_TASK_HIDE_PENDING_JOBS
        if(worker_jobs_pending(w))return (long long)js_page_now_ms();
#endif
    }
    for (struct wtask *t = g_tasks; t; t = t->next)
        if (best < 0 || t->due < best) best = t->due;
    if(LOGIT_HAVE(js_webapi_fetch_next_due))for(int i=0;i<JSW_MAX_WORKERS;i++){
        struct jsworker *w=&g_workers[i];
        if(w->used&&w->state==WK_RUNNING){
            long long due=js_webapi_fetch_next_due(w->wctx);
            if(due>=0&&(best<0||due<best))best=due;
        }
    }
    return best;
}

static int worker_task_is_parent(const struct wtask *t)
{
    if(t->kind==WTK_START)return 0;
    struct jsworker *w=find_worker(t->wid);
    return !w||!w->wctx||t->ctx!=w->wctx;
}

static struct wtask *worker_ready(long long now,unsigned long long limit,int parent_only)
{
    struct wtask *best=NULL;
    for(struct wtask *t=g_tasks;t;t=t->next){
        if(t->due>now||t->seq>limit||(parent_only&&!worker_task_is_parent(t)))continue;
        if(!best||t->due<best->due||(t->due==best->due&&t->seq<best->seq))best=t;
    }
    return best;
}

static void worker_dispatch_task(struct wtask *best,long long now)
{
        struct wtask **pp = &g_tasks;
        while (*pp && *pp != best) pp = &(*pp)->next;
        if (*pp == best) *pp = best->next;

        if (best->kind == WTK_START) {
            struct jsworker *w = find_worker(best->wid);
            task_free(best);
            if (w && w->state == WK_STARTING) worker_start(w);
            return;
        }

        struct jsworker *w = find_worker(best->wid);
        int is_worker_call = (w && w->wctx && best->ctx == w->wctx);

        if (best->interval_ms > 0 && w && w->state == WK_RUNNING) {
            JSValue callee2 = JS_DupValue(best->ctx, best->callee);
            JSValue this2   = JS_DupValue(best->ctx, best->thisArg);
            JSValue *args2 = NULL;
            if (best->argc > 0) {
                args2 = malloc((size_t)best->argc * sizeof *args2);
                if (args2) for (int i = 0; i < best->argc; i++) args2[i] = JS_DupValue(best->ctx, best->argv[i]);
            }
            task_add(best->wid, WTK_CALL, best->ctx, callee2, this2, args2, args2 ? best->argc : 0,
                     now + best->interval_ms, best->interval_ms, best->timer_id);
        }

        /* Drop only a WORKER-bound call whose worker is gone -- never a
         * PARENT-bound one. THE BUG THIS REPLACES: `!w || w->state ==
         * WK_DEAD` used to drop BOTH, so the one task
         * deliver_error_to_parent()/mark_dead_keep_notify() exist to protect
         * was queued correctly, survived cancel_worker_tasks() correctly,
         * and was then silently thrown away RIGHT HERE the moment
         * reap_dead() (at the top of this function, on THIS SAME pass) freed
         * the worker struct that scheduled it -- `w` came back NULL for a
         * task whose `ctx`/`callee`/`thisArg` were never invalidated by
         * that, being independently reference-counted JSValues into the
         * PAGE's own context, which nothing here had any reason to free. A
         * worker-bound task can never outlive its worker to reach this
         * point: cancel_worker_tasks() removes every task with `ctx ==
         * w->wctx` unconditionally, keep_parent_notify or not -- so
         * `is_worker_call` alone is the correct and sufficient guard. */
        if (is_worker_call && w->state == WK_DEAD) { task_free(best); return; }

        if(best->packet){
            JSValue *args=malloc(2*sizeof *args);
            if(!args){task_free(best);return;}
            args[0]=js_ports_read(best->ctx,best->packet,&args[1]);
            if(JS_IsException(args[0])){
                JS_FreeValue(best->ctx,JS_GetException(best->ctx));
                JS_FreeValue(best->ctx,args[1]);free(args);task_free(best);return;
            }
            js_ports_discard(best->packet);best->packet=NULL;
            best->argv=args;best->argc=2;
        }

        if (is_worker_call) {worker_slice_begin(w);WORKER_PHASE(w,"task");}
        else js_page_slice_begin();   /* a delivery INTO the parent gets the page's own watchdog */

#ifdef JS_RUNTIME_DIAGNOSTICS
        if (is_worker_call && best->kind == WTK_MESSAGE)
            worker_diag(w->id, "inbound-message-dispatch", "message");
#endif
        JSValue r = JS_Call(best->ctx, best->callee, best->thisArg, best->argc, (JSValueConst *)best->argv);
        if (JS_IsException(r)) {
            JSValue e = JS_GetException(best->ctx);
            if (is_worker_call) {
                worker_report_uncaught(w, e);
                JS_FreeValue(best->ctx, e);
            } else {
                const char *m = JS_ToCString(best->ctx, e);
                printf("[worker] exception delivering to the page: %s\n", m ? m : "?");
                if (m) JS_FreeCString(best->ctx, m);
                JS_FreeValue(best->ctx, e);
            }
        }
        JS_FreeValue(best->ctx, r);
        if (is_worker_call) worker_drain_jobs(w);   /* the call just made can enqueue a promise reaction in w->rt; see worker_drain_jobs's header */
        else {
            if(LOGIT_HAVE(js_page_pump))js_page_pump();
            /* The old delivery armed the page watchdog without closing its
             * entry. With document contexts that leaks g_entry_depth and
             * every later switch is refused, even after all JS returned. */
            if(LOGIT_HAVE(js_page_slice_end))js_page_slice_end();
        }
        task_free(best);
}

static int worker_run_due_until(unsigned long long deadline_ms,int *parent_handoff)
{
    if(parent_handoff)*parent_handoff=0;
    reap_dead();
    int ran=0,was_in_turn=g_worker_in_turn;
    unsigned long long previous_deadline=g_worker_deadline;
    g_worker_in_turn=1;g_worker_deadline=deadline_ms;

    /* A completed worker may have posted its result just before yielding.
     * Deliver already-due parent notifications before entering a different
     * owner's next long reaction. Ordering remains due/sequence FIFO within
     * this parent task source; worker agents have independent event loops. */
#ifndef JS_TASK_NO_PARENT_SWEEP
    long long parent_now=(long long)js_page_now_ms();
    unsigned long long parent_limit=g_wseq;
    for(;;){
        if(worker_turn_expired())goto out;
        struct wtask *best=worker_ready(parent_now,parent_limit,1);
        if(!best)break;
        worker_dispatch_task(best,parent_now);ran++;
#ifndef JS_TASK_NO_PARENT_FETCH_HANDOFF
        if(parent_handoff)*parent_handoff=1;
#endif
    }
#ifndef JS_TASK_NO_PARENT_FETCH_HANDOFF
    /* A parent callback can open a fetch without sending its HTTP request:
     * the page's network phase ran before this worker phase. Return to that
     * owner now, even when the callback used less than the time budget. A
     * different worker's next native call may otherwise block the send. */
    if(parent_handoff&&*parent_handoff)goto out;
#endif
#endif

    if(!g_worker_task_phase){
        while(g_worker_owner_cursor<JSW_MAX_WORKERS){
            if(worker_turn_expired())goto out;
            struct jsworker *w=&g_workers[g_worker_owner_cursor++];
            if(!w->used||w->state!=WK_RUNNING)continue;
            worker_slice_begin(w);
            ran+=worker_drain_jobs(w);
            if(w->state==WK_DEAD||worker_jobs_pending(w)||worker_turn_expired())continue;
            ran+=worker_flush_inbound(w);
            if(w->state==WK_DEAD||worker_jobs_pending(w)||w->inbound_head||worker_turn_expired())continue;
            if(LOGIT_HAVE(js_ports_pump)){
                WORKER_PHASE(w,"port");
                ran+=js_ports_pump(w->wctx);ran+=worker_drain_jobs(w);
                if(w->state==WK_DEAD||worker_jobs_pending(w)||worker_turn_expired())continue;
            }
#ifndef WORKER_FETCH_NO_PUMP
            if(LOGIT_HAVE(js_webapi_fetch_pump)){
                WORKER_PHASE(w,"fetch");
                int work=js_webapi_fetch_pump(w->wctx);
                ran+=work;
                if(work)ran+=worker_drain_jobs(w);
            }
#endif
        }
        g_worker_task_phase=1;
        if(!g_worker_task_snapshot_active){
            g_worker_task_now=(long long)js_page_now_ms();
            g_worker_task_limit=g_wseq;
            g_worker_task_snapshot_active=1;
        }
    }

    for(;;){
        if(worker_turn_expired())goto out;
        struct wtask *best=worker_ready(g_worker_task_now,g_worker_task_limit,0);
        if(!best)break;
        struct jsworker *w=find_worker(best->wid);
        if(best->kind!=WTK_START&&w&&best->ctx==w->wctx&&
           (worker_jobs_pending(w)||w->inbound_head)){
            worker_slice_begin(w);
            ran+=worker_drain_jobs(w);
            if(w->state!=WK_DEAD)ran+=worker_flush_inbound(w);
            /* Either callback may close the worker and free 'best'. Select
             * again rather than retaining a queued-task pointer across JS. */
            if(worker_jobs_pending(w)||w->inbound_head)goto out;
            continue;
        }
        int parent_call=worker_task_is_parent(best);
        worker_dispatch_task(best,g_worker_task_now);ran++;
#ifndef JS_TASK_NO_PARENT_FETCH_HANDOFF
        if(parent_handoff&&parent_call){*parent_handoff=1;goto out;}
#else
        (void)parent_call;
#endif
    }
    g_worker_owner_cursor=0;g_worker_task_phase=0;g_worker_task_snapshot_active=0;
out:
    /* A task can create a live worker or suspend its startup buffer after
     * the owner sweep already passed that slot. Resume owners on the next
     * UI turn, not only after every old task in the snapshot is exhausted.
     * Keep the task snapshot itself: this is fairness between agents, not
     * admission of this turn's newly queued timers into its old batch. */
    if(g_worker_task_phase){g_worker_task_phase=0;g_worker_owner_cursor=0;}
    g_worker_in_turn=was_in_turn;g_worker_deadline=previous_deadline;
    (void)js_worker_pending(); /* reset a completed, now-empty sweep */
    return ran;
}

int js_worker_run_due_until(unsigned long long deadline_ms)
{return worker_run_due_until(deadline_ms,NULL);}

int js_worker_run_due_for_page(unsigned long long deadline_ms,int *parent_handoff)
{return worker_run_due_until(deadline_ms,parent_handoff);}

int js_worker_run_due(void)
{return js_worker_run_due_until(js_page_now_ms()+JS_TASK_TURN_MS);}

void js_worker_close_all(void)
{
    g_worker_owner_cursor=0;g_worker_task_phase=0;g_worker_task_snapshot_active=0;
    for (int i = 0; i < JSW_MAX_WORKERS; i++)
        if (g_workers[i].used) mark_dead(&g_workers[i]);
    /* Reap unconditionally, bypassing free_pending: js_page_close() calls
     * this before the page context is freed and nothing is executing inside
     * any worker at that point, so it is always safe here even under
     * -DJS_WORKER_NO_TERMINATE (which only disables the LAZY reap check,
     * not this one -- a navigation must not leak every worker runtime the
     * page ever opened). */
    for (int i = 0; i < JSW_MAX_WORKERS; i++) {
        struct jsworker *w = &g_workers[i];
        if (!w->used) continue;
        if (w->wctx) {
            if(LOGIT_HAVE(js_ports_close))js_ports_close(w->wctx);
            if(LOGIT_HAVE(js_webapi_fetch_close))js_webapi_fetch_close(w->wctx);
#ifndef WORKER_NO_WASM_REALM
            if(LOGIT_HAVE(js_wasm_reset))js_wasm_reset(w->wctx);
#endif
#ifdef JS_RUNTIME_DIAGNOSTICS
            worker_promise_diag_close(w->wctx, &w->promise_diag);
#endif
            JS_FreeValue(w->wctx, w->self_obj); JS_FreeContext(w->wctx);
        }
        if (w->rt) JS_FreeRuntime(w->rt);
        if (w->pctx && !JS_IsUndefined(w->worker_obj)) JS_FreeValue(w->pctx, w->worker_obj);
        struct pending_msg *m = w->inbound_head;
        while (m) { struct pending_msg *n = m->next; free(m->buf); free(m); m = n; }
        /* Navigation can happen before the deferred task transfers the
         * constructor's source snapshot into worker_start's local owner. */
        free(w->blob_source);
        if(w->policy.release)w->policy.release(w->policy.opaque);
        memset(w, 0, sizeof *w);
    }
    while (g_tasks) { struct wtask *t = g_tasks; g_tasks = t->next; task_free(t); }
    g_next_worker_id = 1;
}
