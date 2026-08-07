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

/* ---- invalidation ----
 *
 * A live page mutates ONE element per tick. The old answer to that was a single
 * global boolean, and the browser's response to it was to re-run the cascade
 * over every element in the document and rebuild the whole layout -- so a
 * setInterval nudging one leaf cost a full-document re-style every 16 ms.
 *
 * So a mutation now records BOTH what kind of change it was and WHERE, and the
 * embedder re-styles that scope instead of the document.
 *
 * The tiers, in increasing cost. They are ordered, and a batch of mutations
 * reports the maximum:
 *
 *   INVAL_NONE    nothing was mutated.
 *   INVAL_PAINT   the cascade must run again over the marked scope, but only
 *                 properties that move no box and resize no box can have
 *                 changed (a colour, opacity, visibility, z-order). A future
 *                 incremental repaint can skip layout entirely on this tier.
 *   INVAL_STYLE   the cascade must run again over the marked scope, and
 *                 whether layout follows is decided by what it produces --
 *                 css_apply_scoped() compares the new computed styles against
 *                 the old ones and answers CSS_CHANGED_NONE when a class
 *                 toggle turned out to match no rule at all.
 *   INVAL_LAYOUT  the box tree itself changed (nodes inserted, removed, text
 *                 replaced), so layout must run whatever the cascade says.
 *
 * HONEST LIMITATION: our display list is built by layout_page(), which is also
 * what fills in every painted colour -- so today INVAL_PAINT still costs a
 * display-list rebuild, exactly like INVAL_STYLE. The tier is recorded and
 * exposed so the incremental-repaint path can be added on the layout side
 * without any of this having to change. What IS already saved is the big one:
 * the cascade runs over a subtree instead of a document, and a change that
 * resolves to nothing costs no layout and no repaint at all. */
enum { INVAL_NONE = 0, INVAL_PAINT, INVAL_STYLE, INVAL_LAYOUT };

/* The highest tier reached since the last js_dom_clear_dirty(). */
int  js_dom_inval_level(void);
/* How many independent scopes were marked. ZERO means "the whole document" --
 * either nothing was marked, or the scopes stopped being worth tracking
 * separately (too many, or one of them was destroyed before we got here). */
int  js_dom_inval_roots(void);
/* Scope `i`: the element whose subtree must be re-styled. NULL if the node was
 * destroyed since it was marked, which the caller must read as "fall back to
 * the whole document". *siblings is set when the element's FOLLOWING siblings
 * must be re-styled too -- an `a + b` / `a ~ b` rule can key off a class the
 * mutation just changed. */
struct node *js_dom_inval_root(int i, int *siblings);

/* Number of registered event listeners across the whole document. */
int  js_dom_listener_count(void);

/* ---- viewport ----
 *
 * Where the page currently sits under the viewport, in document pixels. This is
 * what turns getBoundingClientRect's answer from DOCUMENT coordinates into
 * CLIENT coordinates, and it is the same origin the client_x/client_y of a
 * dispatched mouse event already uses -- so a page that measures an element and
 * compares the rect against event.clientY gets a consistent answer.
 *
 * The embedder owns the scroll offset (browser.c's `scroll`), so it has to push
 * it in whenever it changes. NOT calling this is safe and merely means rects
 * are reported in document coordinates; the two coincide at scroll 0, which is
 * where every page starts. js_dom_init resets it to 0 for the new page. */
void js_dom_set_scroll(int x, int y);

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
