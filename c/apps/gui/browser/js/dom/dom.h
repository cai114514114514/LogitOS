#ifndef LOGIT_JS_DOM_H
#define LOGIT_JS_DOM_H

#include "quickjs.h"

struct node;

/* Document-owned DOM binding state. NULL selects the legacy top-level slot.
 * Each live slot must use its OWN JSRuntime and native DOM arena. This is a
 * native scheduling boundary, not a WindowProxy or an origin permission:
 * activate only between JS entries, and keep it active through evaluation,
 * microtasks, cleanup and runtime destruction. Other JS modules and layout
 * have separate ownership and are NOT switched by this API.
 *
 * A slot remains allocated until cleanup + JS_FreeContext/JS_FreeRuntime have
 * released every wrapper. destroy refuses a live slot. Native DOM mutation
 * notifications temporarily enter their owner without executing page script;
 * finalizers account to their owner even when another slot is selected. */
struct js_dom_context;
struct js_dom_context *js_dom_context_create(void);
int js_dom_context_activate(struct js_dom_context *next,
                            struct js_dom_context **previous);
int js_dom_context_destroy(struct js_dom_context *state);

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
/* Advances for every attached invalidation, including an already dirty scope. */
unsigned long long js_dom_mutation_generation(void);

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
/* A native control's live state changed without changing its HTML attributes.
 * Schedule its repaint; detached controls wait for the insertion invalidation. */
void js_dom_control_changed(struct node *n);
/* Layer membership requires layout even when computed CSS compares equal. */
void js_dom_top_layer_changed(struct node *n);

/* Number of registered event listeners across the whole document. */
int  js_dom_listener_count(void);

/* Whether the parsed event path carries an inline on<type> content attribute.
 * This is a native DOM query: it never compiles or dispatches the handler. */
int  js_dom_event_has_inline_handler(struct node *target, const char *type);

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
/* CSSOM and the embedder read the SAME viewport origin. Nullable outputs. */
void js_dom_get_scroll(int *x, int *y);

/* ---- what js_reflect.c reaches into this file for ----
 *
 * Four operations and one throw, which is the whole surface IDL attribute
 * reflection needs: resolve a wrapper to its node, read an attribute WITH ITS
 * LENGTH (a value may contain U+0000 and dom.c stores it; a `const char *`
 * alone cannot say so), write one, remove one. Writing and removing go through
 * the same path setAttribute()/removeAttribute() use, so a reflected setter and
 * an explicit setAttribute cannot disagree about the id index, the class list
 * or the invalidation tier. */
struct node *js_dom_node_from(JSValueConst v);
const char  *js_dom_attr_len(const struct node *n, const char *name, int *len);
void         js_dom_attr_write(JSContext *ctx, struct node *n, const char *name,
                               const char *val, int vlen);
int          js_dom_attr_erase(JSContext *ctx, struct node *n, const char *name);
JSValue      js_dom_throw_dom(JSContext *ctx, const char *name, const char *msg);

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
/* Native window-message delivery, not a JS-exposed dispatch shortcut.
 * The embedder must validate the recipient/source/origin and select/bracket
 * the owning runtime first. Does not grant transient user activation. */
JSValue js_dom_deliver_window_message(JSValueConst event);

/* Actual platform/chrome keyboard ownership, independent of activeElement.
 * An absent/unknown query means no focus. The embedder sets this before page
 * creation; sync at event boundaries emits transitions through the existing
 * window event-target binding. Querying hasFocus itself never dispatches JS. */
void js_dom_set_focus_query(int (*query)(void));
int  js_dom_has_focus(void);
int  js_dom_sync_focus(void);

/* TRANSIENT ACTIVATION -- "did the user do something in the last five
 * seconds", the gate a capability that acts outside the page must ask before
 * acting. js_dom_dispatch() stamps it for the activation event types only,
 * because everything reaching that function came from real input (it is the
 * same place that sets isTrusted); dispatchEvent() does not, so a page cannot
 * manufacture activation by synthesising a click.
 *
 * THE ONE CALLER TODAY is navigator.clipboard.writeText (js_platform.c),
 * which writes the clipboard EVERY PROCESS ON THIS MACHINE reads. It shipped
 * without this and a review caught it, not a gate. Anything else that reaches
 * outside the page -- a download, a notification, a window-level action --
 * belongs behind the same call rather than behind a second idea of what a
 * gesture is. See js_dom.c for the window, why it is a duration and not a
 * flag, and what it deliberately does not model. */
void js_dom_note_activation(void);
int  js_dom_has_activation(void);

/* THE node object for a node -- the same cached wrapper every other binding in
 * this file hands out, so `js_dom_node_value(ctx, n) === document.scripts[i]`
 * for the same element rather than being a second object that merely looks
 * like it.
 *
 * Exported for exactly one caller, and the reason is the defect it closed.
 * js_page.c knows which <script> is executing and could not say so in a node,
 * because this file's wrap() is static -- so document.currentScript was
 * published as an INDEX into document.scripts and js_platform.c turned it back
 * into an element. That indirection is what made an inline classic script's
 * currentScript null for the entire life of the feature: the index had to be
 * recovered by pattern-matching a filename string, and the string the browser
 * passes for an inline script (the page URL + "#inline-script-N", which it
 * needs as an import() base) never matched the pattern. It also could not
 * survive the commonest use of the property -- `document.currentScript
 * .remove()` -- because removing the element renumbers document.scripts under
 * the index that was still standing on it. A node in, the same node out, no
 * string in between. */
JSValue js_dom_node_value(JSContext *ctx, struct node *n);

/* Where diagnostics (uncaught handler exceptions) go, in addition to printf.
 * js_page.c points this at its console buffer so the status bar shows them. */
void js_dom_set_note(void (*fn)(const char *));

/* Register a sink called with a <script> node that just entered the document
 * by DOM insertion (appendChild etc.) after parse. The sink must only ENQUEUE
 * the node for later execution on the browser's frame loop, never run it
 * synchronously -- the inserting script is still on the stack. NULL to unhook
 * (host DOM tests with no page runtime). See
 * docs/superpowers/specs/2026-08-16-inserted-script-execution.md. */
void js_dom_set_script_sink(void (*fn)(struct node *));
/* Native document policy, checked before compiling handler attributes.
 * NULL retains the default behavior; the callback must not enter JS. */
void js_dom_set_inline_handler_policy(int (*fn)(void));
/* Source mutation discovery includes detached Image elements. No IO or JS may
 * run from this sink; the embedder drains it after the current script unwinds. */
void js_dom_set_image_sink(void (*fn)(struct node *));

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

void js_dom_text_changed(struct node *n);
JSValue js_dom_wrap_node(JSContext *, struct node *);
/* Private selector installer argument, never a property on globalThis.
 * Originally only validated single-simple ASTs entered this callback; the
 * same literals now also prefilter complex queries before full JS matching. */
JSValue js_dom_simple_query(JSContext *, JSValueConst, int, JSValueConst *);

/* Live Element-wrapper handles, including externally retained detached ones.
 * Diagnostic counter for ownership/GC gates; does not create a JS root. */
unsigned js_dom_wrapper_count(void);

#endif /* LOGIT_JS_DOM_H */
