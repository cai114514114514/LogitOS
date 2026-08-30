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
 * uncaught exception.
 *
 * `filename` is the script's URL -- the base a dynamic import() inside it
 * resolves against, and what appears in a stack trace. `node` is the <script>
 * element these bytes came from, or NULL when there is not one (a host test's
 * synthetic snippet, an injected driver); it becomes document.currentScript
 * for the duration and NOTHING derives it from `filename`. The two used to be
 * one argument and the runtime pattern-matched the string to recover the node
 * -- see the currentScript comment in js_page.c for what that cost. */
int  js_page_eval(const char *src, int len, const char *filename, struct node *node);

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
/* The same clock, for a second consumer that needs the identical notion of
 * "now" -- js_worker.c's own per-runtime watchdog and its task queue. Reading
 * two different clocks for "the page" and "its workers" would let a worker's
 * due time and the page's agree only by coincidence. */
unsigned long long js_page_now_ms(void);

/* window.location.href, for pages that read it. Purely informational: assigning
 * to it does not navigate (navigation is driven by the browser's event loop,
 * and re-entering a page load from inside a JS callback would free the DOM the
 * caller is standing on). */
void js_page_set_location(const char *url);
/* The same string back out, NUL-terminated. js_worker.c's same-origin check
 * (a classic worker's script fetch is same-origin-only, RFC-style: refuse by
 * name, not by network accident) reads it to compare against a worker's
 * resolved script URL. */
const char *js_page_location(void);

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
 * branches-or-calls -- NOT per 10,000 bytecodes; see js_page.c). What host
 * harnesses use, and the backstop everywhere else. */
void js_page_set_slice_fuel(long long calls);
int  js_page_slice_hits(void);
/* The watchdog's OWN fuel count for the current/last slice. Counted whether or
 * not js_prof is enabled, which is what makes it the one honest input to an
 * observer-effect control: js_prof's own counter cannot measure whether js_prof
 * changes the work. */
long long js_page_slice_fuel_used(void);
void js_page_slice_begin(void);
/* The other end of one synchronous JS entry. Call it when the entry returns
 * (js_page_eval and js_page_run_due do). It arms nothing and disarms nothing:
 * its only job is to tell js_prof that the wall clock from here to the next
 * bytecode belongs to the browser, not to the script. An embedder that never
 * calls it loses only the js_ms/out_ms split, and gains a `resumed` count that
 * says so. */
void js_page_slice_end(void);

/* ---- js_prof: where a slice's time goes ----
 *
 * A sampling profiler on QuickJS's own work counter. The engine calls the
 * interrupt handler every 10,000 POLL EVENTS -- and a poll event is a branch
 * (OP_goto*, OP_if_true*, OP_if_false*) or a function call, NOT a bytecode;
 * js_page.c carries the measurement that corrected that word, and the number
 * is 2.00 polls per `for (i=0;i<N;i++) s+=i` iteration. This counts those
 * calls and, at each one, reads the wall clock the watchdog was reading
 * anyway: one array index and some arithmetic per 10,000 branches, no extra
 * syscall.
 *
 * The unit of attribution is a SLICE -- one synchronous entry into JS, named
 * by the script URL or "<timer callback>". It is not a per-function profile
 * and must not be quoted as one: a native call polls once on entry and then
 * runs no branches, so no sample lands inside it, and QuickJS's current stack
 * frame is internal to third_party/quickjs.
 *
 * READING IT.  fuel*10,000 is exactly the branches and calls executed. js_ms is
 * wall time with a script running; out_ms is wall time with none.  js_ms/fuel
 * far above the interpreter's own rate means time inside C called from JS.
 * `resumed` counts JS entries that ran without a slice_begin -- an embedder
 * gap, and the reason a stale deadline can bite a script that did no work. */
struct js_prof_slice {
    char      what[48];      /* the script URL, or "<timer callback>" */
    long long fuel;          /* interrupt calls = (branches+calls)/10,000 */
    long long js_ms;         /* wall ms between samples inside a JS entry */
    long long out_ms;        /* wall ms that passed with no JS running */
    long long begin_ms;      /* when js_page_slice_begin() armed this slice */
    long long max_gap_ms;    /* the largest single inter-sample delta */
    long long gap_ms;        /* sum of deltas >= 40 ms */
    int       gaps;
    int       resumed;       /* entries that ran WITHOUT a slice_begin */
    int       bitten;        /* 0 none, 1 wall-time rail, 2 fuel rail */
};
/* Free-running poll-event counter (branches+calls / 10,000), reset only by
 * js_prof_reset(). Read before and after a block to cost that block. */
long long js_prof_polls(void);
void js_prof_enable(int on);
int  js_prof_enabled(void);
void js_prof_reset(void);
/* Names the NEXT slice. Call immediately before js_page_slice_begin(). */
void js_prof_label(const char *what);
int  js_prof_count(void);
int  js_prof_overflow(void);
const struct js_prof_slice *js_prof_at(int i);
void js_prof_dump(const char *tag);
/* The totals, for a harness that wants the numbers rather than the table. */
void js_prof_totals(long long *fuel, long long *js_ms, long long *out_ms,
                    int *gaps, int *resumed);

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
 * rather than through js_page_eval (which calls these for you) -- the WPT
 * runner and tests/unit/webapi_probe.c, both of which need the exception
 * OBJECT and so cannot go through js_page_eval.
 *
 * Pass the <script> node whose bytes are about to be evaluated; NULL means
 * "not a script element", which is currentScript === null. There is no
 * filename here and no matching: an instrument that had to describe the
 * running script in a string was an instrument measuring its own description.
 *
 * Always call the end half, and call it AFTER draining the microtask queue
 * (js_page_pump), not before. HTML performs the checkpoint inside "run a
 * classic script" and restores currentScript afterwards, so a promise reaction
 * the script queued still sees it -- which is what every turbopack-built site
 * on the web reads. js_page_eval does it in that order; an embedder that
 * drains after this call reports null to every reaction on every page. */
void js_page_begin_script(struct node *node);
void js_page_end_script(void);

#endif /* LOGIT_JS_PAGE_H */
