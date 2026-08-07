#ifndef LOGIT_JS_PAGE_H
#define LOGIT_JS_PAGE_H

#include "quickjs.h"

struct node;

/* The page's JavaScript runtime -- the thing that makes a loaded page LIVE.
 *
 * Before this existed the runtime was created and destroyed inside the function
 * that ran a page's <script>s, so nothing a script registered could ever be
 * called again: addEventListener recorded handlers into a runtime that was
 * freed three lines later, and setTimeout could not exist at all. Here the
 * runtime is opened when a document is parsed and closed when the browser
 * navigates away, and everything that outlives one script evaluation --
 * listeners, timers, promise reactions -- lives in between.
 *
 * Exactly one page runtime exists at a time. js_page_open() on a live one
 * closes it first.
 *
 * ORDERING RULE, and it is the trap this whole module exists to avoid: the DOM
 * must outlive the runtime. Every JS wrapper holds a {node, serial} handle and
 * the node holds a weak pointer back to the wrapper, so the runtime has to be
 * torn down (which clears every wrapper slot) BEFORE dom_free() runs. Call
 * js_page_close() first, always. */

/* Create the runtime + context and bind them to `root`. 1 on success. */
int  js_page_open(struct node *root);
/* Tear it down: cancel timers, release listeners, clear every wrapper slot,
 * free the context and the runtime. Safe to call when nothing is open. */
void js_page_close(void);
int  js_page_live(void);
JSContext *js_page_ctx(void);

/* Evaluate page script. Drains the microtask queue afterwards, so a script that
 * ends in `await` or `Promise.then` has actually run by the time this returns
 * (up to its first real suspension on a timer). 1 if it completed without an
 * uncaught exception. */
int  js_page_eval(const char *src, int len, const char *filename);

/* Drain the microtask queue; returns the number of jobs run. */
int  js_page_pump(void);

/* ---- timers ----
 * setTimeout / setInterval / requestAnimationFrame all land in one queue keyed
 * by a monotonic deadline. The main loop asks js_page_pending() (a pointer
 * test, no syscall) and only reads the clock when something is actually
 * scheduled -- an idle page must not cost more than it did before timers
 * existed. */
int  js_page_pending(void);
/* Deadline of the earliest scheduled callback in monotonic ms, or -1. */
long long js_page_next_due(void);
/* Run every callback whose deadline has passed, in deadline order. Returns how
 * many ran. Callbacks scheduled BY a callback wait for the next call, so a
 * setTimeout(f, 0) loop cannot starve the main loop. */
int  js_page_run_due(void);

/* The monotonic clock, injected by the embedder: the browser passes
 * monotonic_ms() (a syscall that only exists in the OS build) and the host test
 * passes a fake it steps by hand -- which is what makes timer ordering testable
 * without sleeping through it. Must be set before js_page_open(). */
void js_page_set_clock(unsigned long long (*fn)(void));

/* window.location.href, for pages that read it. Purely informational: assigning
 * to it does not navigate (navigation is driven by the browser's event loop,
 * and re-entering a page load from inside a JS callback would free the DOM the
 * caller is standing on). */
void js_page_set_location(const char *url);

/* Console output captured from the page (console.log/warn/error + uncaught
 * exceptions), NUL-terminated and bounded. The browser shows the first line in
 * its status bar. */
const char *js_page_output(void);
int         js_page_output_len(void);
void        js_page_output_clear(void);

#endif /* LOGIT_JS_PAGE_H */
