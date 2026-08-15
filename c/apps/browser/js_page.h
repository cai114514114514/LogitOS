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

/* ---- the CPU-slice watchdog ----
 * One synchronous entry into JS (script eval / timer callback / module body)
 * gets a bounded CPU slice; past it the engine interrupts and the entry
 * unwinds as an uncatchable error, loudly. Default 45 s -- far above any
 * honest script at TCG speed; the point is that `while(1)` in a page stops
 * owning the browser (qwen was the wild specimen). set_slice_ms for tests;
 * slice_hits so a harness can assert the dog actually bit; slice_begin is
 * exported for the one out-of-file sync entry, js_module_eval. */
void js_page_set_slice_ms(int ms);
/* The frozen-clock rail: budget in interrupt-handler calls (one per 10,000
 * bytecodes). What host harnesses use, and the backstop everywhere else. */
void js_page_set_slice_fuel(long long calls);
int  js_page_slice_hits(void);
void js_page_slice_begin(void);

/* Append a fragment to the console buffer from outside js_page.c. Exists for
 * exactly one caller -- js_module.c's module-exception reporter -- so a module
 * that throws at top level surfaces in the status bar like every other
 * uncaught exception, instead of dying serial-only (the "silent white page"
 * failure R3 named). */
void js_page_note(const char *frag);

/* Observe every fragment as it is appended, BEFORE the bound above applies.
 * For an instrument that must not miss a message: the buffer is capped at
 * 4 KiB, which is right for a status bar and wrong for counting a real
 * application's exceptions -- kimi.com emits more than that and the probe was
 * reading a truncated list. Fragments arrive as console.log passes them, so
 * the observer assembles lines itself. NULL to unhook. */
void js_page_set_note_sink(void (*fn)(const char *frag));

/* document.currentScript, for an embedder that runs a classic script itself
 * rather than through js_page_eval (which calls these for you). `filename` is
 * the script's URL, or anything without a ':' for an inline one; the pairing
 * with the document's <script> elements is done inside. Always call the end
 * half: currentScript must be null everywhere except a classic script's own
 * synchronous execution. */
void js_page_begin_script(const char *filename);
void js_page_end_script(void);

#endif /* LOGIT_JS_PAGE_H */
