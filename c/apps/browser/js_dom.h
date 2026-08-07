#ifndef LOGIT_JS_DOM_H
#define LOGIT_JS_DOM_H

#include "quickjs.h"

struct node;

/* Install `document` + the Element/Event classes into ctx, bound to the live
 * page DOM. Call once per JS context before JS_Eval.
 *
 * The context this is called with is remembered: it is the one dispatch calls
 * back into, so it must stay alive for as long as the page does. js_page.c owns
 * that lifetime; nothing else may free the context without calling
 * js_dom_cleanup() first. */
void js_dom_init(JSContext *ctx, struct node *root);

/* 1 if JS mutated the DOM since the last clear (caller should re-layout). */
int  js_dom_dirty(void);
void js_dom_clear_dirty(void);

/* Number of registered event listeners across the whole document. */
int  js_dom_listener_count(void);

/* Release listener + wrapper state; call before JS_FreeContext/JS_FreeRuntime. */
void js_dom_cleanup(JSContext *ctx);

/* Drain the QuickJS job queue (promise reactions, async function resumptions)
 * to exhaustion. Must run after every script evaluation, every event callback
 * and every timer -- QuickJS only enqueues jobs, it never runs them by itself,
 * so without this `Promise.then` never fires and `await` stops at the first
 * suspension point. Returns the number of jobs executed. */
int  js_dom_run_jobs(JSContext *ctx);

/* The node that events bubble to (the N_DOCUMENT root), or NULL. `window` and
 * `document` listeners are registered here. */
struct node *js_dom_root(void);

/* Where diagnostics (uncaught handler exceptions) go, in addition to printf.
 * js_page.c points this at its console buffer so the status bar shows them. */
void js_dom_set_note(void (*fn)(const char *));

/* Give an object (in practice `window`) the EventTarget surface + on* handler
 * properties, bound to the document root. Call after js_dom_init. */
void js_dom_bind_event_target(JSContext *ctx, JSValueConst obj);

/* ---- native event dispatch ----
 *
 * How a real input event becomes a DOM event. Everything not named here is left
 * at the DOM default (0 / ""), which is what an Event initialised from a
 * dictionary with missing members gets anyway. */
struct js_event_init {
    int bubbles, cancelable;
    int client_x, client_y;             /* MouseEvent: viewport-local px */
    int button, buttons;                /* 0 = left, 1 = middle, 2 = right */
    int shift, ctrl, alt, meta;
    int detail;                         /* UIEvent: click count / wheel notches */
    double delta_x, delta_y;            /* WheelEvent */
    const char *key, *code;             /* KeyboardEvent */
    int key_code;
    int repeat;
};

/* Dispatch a trusted event of `type` at `target` (a text node is lifted to its
 * nearest element ancestor; NULL targets the document). Runs the full
 * capture/target/bubble walk and drains microtasks afterwards.
 *
 * Returns 1 if the DEFAULT ACTION should proceed and 0 if a listener called
 * preventDefault(). With no page runtime live it returns 1 -- a page without
 * script must never lose its links. */
int js_dom_dispatch(struct node *target, const char *type,
                    const struct js_event_init *init);

#endif /* LOGIT_JS_DOM_H */
