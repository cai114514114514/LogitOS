#ifndef LOGIT_JS_DEVTOOLS_H
#define LOGIT_JS_DEVTOOLS_H

/* DevTools: Sources + Network recording.
 *
 * THE OWNER'S ORDER was "add debug, fully abstracted to F12" -- and the
 * argument for why this is the right tool, not a nicer test harness, is in
 * CLAUDE.md's product-vs-sum note: a page paints as a PRODUCT of independent
 * capabilities, so one zero factor (a missing global, a script that 404s, a
 * module graph that dies) blanks the page no matter how many other things
 * work. The browser already computes the fact that would have named the zero
 * factor -- "scripts collected: N inline" and never "N executed" is the exact
 * gap that cost an afternoon on google.com's challenge page, five inline
 * scripts collected, ReferenceError with an EMPTY exceptions[] -- and prints
 * it once to a serial log nobody at the keyboard can see.
 *
 * THIS FILE IS A SECOND READER OF THOSE FACTS, NEVER A SECOND COUNTER
 * (CLAUDE.md rule 4, one jar two doors). Every dt_* call the caller makes
 * sits beside a printf that already existed in collect_scripts() /
 * run_collected_scripts() / res_fetch_all(), reading the SAME struct fields.
 * A bug that breaks the serial log line and a bug that breaks this panel are
 * meant to be the same bug -- and dt_sources_broken() exists specifically to
 * catch the case where they stop being the same bug (a hook edited on one
 * side and not the other): it compares the count THIS module was told to
 * expect (the exact xc+xm+in sum collect_scripts()'s own printf computes)
 * against the count it actually recorded, and the panel refuses to show a
 * list the moment those disagree. Rule 5: a control that cannot be watched
 * failing is worse than none. tests/unit/devtools_test.c is the host-side
 * gate that watches it fail on purpose -- see that file's header.
 *
 * NO GUI CALLS IN THIS FILE. Every function here is pure data bookkeeping
 * over static tables, host-testable without a compositor, a window, or
 * QEMU -- the browser (or a host harness) is the only thing that ever
 * decides what to draw with the numbers this module hands back. That is
 * also what keeps this file's presence in BROWSER_JS_SRC (globbed as
 * c/apps/browser/js_*.c, Makefile:944) free of the toolkit's own
 * SYS_GUI_* syscalls, so it links into a host test binary unmodified.
 *
 * MEMORY: every table below is a fixed-size static array, bounded and
 * capped rather than growing -- CLAUDE.md's constraint 3, a panel that
 * retains every request/script forever is the thing that makes the machine
 * worse in exactly the "it's slow and something is wrong" moment this tool
 * exists for. dt_reset() -- called once per navigation -- is what keeps it
 * bounded to the CURRENT page rather than the whole session. */

/* ---- Sources: what happened to every <script> the document named ---- */

enum { DT_KIND_CLASSIC = 0, DT_KIND_MODULE = 1 };
enum {
    DT_ST_COLLECTED  = 0,   /* res_add() queued it; outcome not known yet */
    DT_ST_EXECUTED   = 1,   /* js_page_eval / js_module_eval returned 1 */
    DT_ST_EXEC_ERROR = 2,   /* it ran and threw an uncaught exception */
    DT_ST_SKIPPED    = 3,   /* the DOM offered it, we chose not to fetch/run it */
    DT_ST_FAILED     = 4    /* fetched (or attempted) and it never ran */
};

#define DT_SCRIPT_MAX 192
#define DT_NAME_CAP   100
#define DT_REASON_CAP  72

/* Reset all recorded state for a fresh navigation. Call once, right after a
 * new document's DOM exists (dom_parse() succeeding is the right moment --
 * before that point there is no new page to record yet, and the OLD page's
 * table must not survive to be misread as the new page's). */
void dt_reset(void);

/* collect_scripts() calls this immediately after a successful res_add(),
 * with res_idx = g_nres - 1 at that point (the index the entry just took in
 * browser.c's own resource table). That index is the ONLY thing that lets
 * dt_script_mark() below find the same record later by position rather than
 * by matching strings -- two <script src> tags can share a URL. Pass -1 if
 * there is no res-table entry (there always is one when this is called from
 * the "successfully queued" branch; the parameter exists so this function
 * and dt_script_skip() below can share one obvious contract). */
void dt_script_add(int res_idx, const char *name, int kind);

/* The DOM offered a <script> that never became a browser.c resource-table
 * entry at all: wrong type, nomodule fallback, unsupported scheme (data:,
 * javascript:), an empty inline body, or res_add() itself failing. There is
 * no res_idx to remember -- this state is final the moment it is recorded. */
void dt_script_skip(const char *name, int kind, const char *reason);

/* run_collected_scripts() (or the inserted-script drain) calls this with the
 * SAME res_idx dt_script_add() was given, to move a COLLECTED record to its
 * final EXECUTED / EXEC_ERROR / FAILED state. An index nothing recorded (a
 * stale value, or DT_SCRIPT_MAX already hit) is a silent no-op -- this must
 * never be where a crash gets introduced into page load. */
void dt_script_mark(int res_idx, int state, const char *reason);

/* Set once, right beside collect_scripts()'s own "scripts collected: %d
 * external classic, %d external module, %d inline" printf, to exactly
 * xc + xm + in from THAT call site. This is the number dt_sources_broken()
 * checks dt_script_collected_count() against. */
void dt_script_set_expect(int n);

int dt_script_count(void);              /* collected + skipped, this page */
int dt_script_collected_count(void);    /* only what dt_script_add() recorded */
int dt_sources_broken(void);            /* 1 if expect was set and disagrees */
int dt_script_expect(void);             /* -1 if not set this page */

/* Read-only accessors for the panel renderer (kept out of this file so it
 * stays free of gui_* calls -- see the file header). All four `*cap`
 * buffers are NUL-terminated on return; a too-small buffer truncates rather
 * than overflows. Returns 0 (and leaves outputs empty) for i out of range. */
int dt_script_get(int i, char *name, int namecap, int *kind, int *state,
                   char *reason, int reasoncap);

/* ---- Network: what actually got requested, and what did not ---- */

#define DT_NET_MAX  384
#define DT_URL_CAP  112
#define DT_KIND_CAP  10
#define DT_ERR_CAP   56

/* res_fetch_all() calls this once per entry, right after its own
 * fetch-succeeded / fetch-failed branch already decided the outcome --
 * reading e->url/e->ref, e->status, e->data/e->len and e->err, the SAME
 * fields its "fetch failed" printf reads. `got`: 1 arrived with a body, 0
 * requested but failed (status/err explain how). */
void dt_net_record(const char *kind, const char *url, int got, int status,
                    const char *err);

/* A resource the document names that the browser chose NEVER TO REQUEST --
 * the shape behind github.com asking for 28 of its 94 required subresources
 * and painting only its accessibility skeleton. `reason` is why (e.g. "data:
 * URI, unsupported", "accessibility theme variant, not applied"). Recorded
 * with got = -1, distinct from a request that was made and failed. */
void dt_net_required_not_requested(const char *kind, const char *url, const char *reason);

int dt_net_count(void);
int dt_net_get(int i, char *kind, int kindcap, char *url, int urlcap,
                int *got, int *status, char *err, int errcap);

/* ---- text rendering, shared by the panel and the host gate ----
 *
 * These build the EXACT sentence a person reads -- once, here -- so
 * tests/unit/devtools_test.c and the panel's gui_text() calls are provably
 * showing the same words, not two independent phrasings of the same fact
 * that could drift (rule 4 again, one level up: not just the same COUNT,
 * the same SENTENCE). "A panel is only as good as the sentence it puts in
 * front of a person at 2am." */

/* "N collected, N executed, N error, N skipped, N failed" -- or, when
 * dt_sources_broken() is true, a banner naming the disagreement instead of a
 * count that cannot be trusted. Never silently shows a clean-looking summary
 * over broken bookkeeping (rule 3, never stub to success). */
void dt_summary_line(char *out, int cap);

/* One row: "[classic] app.js -- EXECUTED" / "[module] (inline) -- FAILED:
 * uncaught exception". */
void dt_script_line(int i, char *out, int cap);

/* One Network row: "[script] 200 OK https://…/app.js" /
 * "[style] NEVER REQUESTED (data: URI, unsupported) https://…" */
void dt_net_line(int i, char *out, int cap);

#endif /* LOGIT_JS_DEVTOOLS_H */
