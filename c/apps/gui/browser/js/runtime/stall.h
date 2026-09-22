#ifndef LOGIT_JS_STALL_H
#define LOGIT_JS_STALL_H

#include "quickjs.h"

/* THE SILENT-STALL INSTRUMENT: what is this page still WAITING for?
 *
 * WHY THIS EXISTS. Every instrument in this tree reports something that
 * HAPPENED. probe-webapi reports the globals a page reached for and missed;
 * the site scoreboard reports painted pixels and text runs; the console
 * reports exceptions. All of them are blind to the failure the browser
 * actually has on a modern application page: NOTHING GOES WRONG. There is no
 * exception, no failed request, no missing subresource -- the shell renders,
 * the content never arrives, and the page is simply parked forever on
 * something that will not happen.
 *
 * That class is invisible to a name-presence grep by construction. A grep says
 * `IntersectionObserver` is present; it cannot say the observer was
 * constructed, given three targets, and never called back. It says `fetch` is
 * present; it cannot say four fetches were started and one never settled. It
 * cannot see an `await` at all, because an await creates a promise the page
 * never named.
 *
 * TWO HALVES, AND THE SPLIT IS THE DESIGN.
 *
 *   1. THE NATIVE CENSUS (JS_StallCensus, third_party/quickjs). A read-only
 *      walk of the runtime's GC object list: every promise by state, every
 *      pending promise that has a reaction registered behind it, and every
 *      suspended async-function frame with the file:line of the await it is
 *      parked on. It takes no reference, calls nothing and allocates nothing,
 *      so IT CANNOT PERTURB WHAT IT MEASURES. This is the ground truth and it
 *      needs no cooperation from the page.
 *
 *   2. THE ARMED TRACKERS (js_stall_arm, below). A prelude that wraps the
 *      scheduling surface -- fetch, XHR, rAF, timers, every *Observer, the
 *      lifecycle listeners -- so a pending thing can be ATTRIBUTED to an
 *      origin, a target count or a callback that never ran. This half DOES
 *      perturb: attaching a settlement handler to a fetch's promise registers
 *      a reaction, so `promise_pending_awaited` rises by exactly one per
 *      in-flight fetch. That is stated rather than hidden, and the harness
 *      measures each page BOTH ways so the perturbation is a number on the
 *      report instead of a caveat in a comment.
 *
 * WHAT IS DELIBERATELY NOT HERE. No hostname, no URL, no bundle name and no
 * framework name appears in this file or in anything it decides. Trackers are
 * discovered BY SHAPE: anything on the global object whose name ends in
 * `Observer` and whose prototype has an `observe` method is wrapped, so an
 * observer added tomorrow is instrumented without editing this file. The one
 * fixed list is the document lifecycle event family, which is fixed in the
 * HTML standard rather than in any page.
 *
 * PENDING FETCHES ARE REPORTED BY ORIGIN, NOT BY URL. A diagnostic that prints
 * full request URLs is a request log; the origin is what tells you whether the
 * page is waiting on itself, on an API host or on a third party, which is the
 * whole diagnostic value, and it is where the line is drawn.
 *
 * ARMING IS OPT-IN AND THE BROWSER DOES NOT ARM ITSELF. Wrapping fetch changes
 * a page-visible function identity, and this tree's own evidence (the
 * getContext finding: a null-returning method is worse than an absent one)
 * says an instrument that alters the surface must not be on the path a user
 * takes. Harnesses arm it; the shipped browser does not, unless an embedder
 * calls js_stall_arm() explicitly.
 */

/* Install the trackers into a live page context. Call AFTER js_page_open() and
 * BEFORE the page's first script. Returns 1 if the prelude evaluated.
 * Idempotent: arming twice is a no-op, so a harness cannot double-count. */
int js_stall_arm(JSContext *ctx);

/* Write the report for the current runtime into `buf` (NUL-terminated, always
 * within `cap`). Returns the number of bytes written.
 *
 * SAFE ON AN UNARMED CONTEXT: the native census needs no prelude, so a caller
 * that never armed still gets the promise and async-frame half -- which is the
 * half with no observer effect. The armed sections are simply absent.
 *
 * CALL IT AT SETTLE, with no JS on the stack. A census taken from inside a
 * callback counts that callback's own frame as suspended, which is true and
 * useless. */
int js_stall_report(JSContext *ctx, char *buf, int cap);

/* 1 when the report has something in it that a page can be BLOCKED on:
 * an awaited pending promise, a suspended async frame, an unsettled fetch or
 * XHR, an outstanding rAF, or an observer that was given targets and never
 * called back. Distinct from "the report is non-empty" -- a page with 40
 * fulfilled promises and nothing outstanding is a page that finished. */
int js_stall_blocked(JSContext *ctx);

#endif /* LOGIT_JS_STALL_H */
