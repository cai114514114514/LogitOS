/* The page's JavaScript runtime: lifetime, timers, the console, `window`.
 *
 * See js_page.h for the contract. The short version: the runtime is opened with
 * the document and closed on navigation, so a callback registered during load
 * is still callable when the user clicks a minute later. Everything in this
 * file exists because something has to survive `JS_Eval` returning. */
#include "quickjs.h"
#include "dom.h"
#include "js_dom.h"
#include "js_page.h"
/* fetch/Storage/history/URL live in js_webapi.c, which the browser links and
 * the host tests of THIS file do not. Every entry point is weak, so a build
 * without it links and simply has no network API -- see js_webapi.h. */
#define JS_WEBAPI_OPTIONAL
#include "js_webapi.h"
/* Timing, the document lifecycle, message queues, the observers -- js_platform.c,
 * weak for the same reason: the host tests of THIS file link without it. */
#define JS_PLATFORM_OPTIONAL
#include "js_platform.h"
/* MediaSource / SourceBuffer / HTMLMediaElement -- js_media.c, weak for the
 * same reason as the two above: the host tests of THIS file link without it and
 * simply come up with no media. */
#define JS_MEDIA_OPTIONAL
#include "js_media.h"
/* CSSOM-View geometry, document.styleSheets, the CSS namespace object --
 * js_cssom.c, weak for the same reason as the three above. */
#define JS_CSSOM_OPTIONAL
#include "js_cssom.h"
/* The Event constructor hierarchy, a constructible EventTarget and the rest of
 * addEventListener's options surface -- js_events.c, layered over the native
 * dispatcher js_dom.c owns. Weak for the same reason as the three above. */
#define JS_EVENTS_OPTIONAL
#include "js_events.h"
/* Dedicated Worker -- js_worker.c: a second JSRuntime per worker, run to
 * completion on THIS thread and folded into the three functions right below
 * (js_page_pending / js_page_next_due / js_page_run_due), per its own header:
 * "ONE JAR, ONE DOOR on scheduling." Weak for the same reason as the four
 * above -- the host tests of THIS file link without it and simply have no
 * Worker constructor. */
#define JS_WORKER_OPTIONAL
#include "js_worker.h"
/* `WebSocket` -- js_websocket.c, layered over G.EventTarget/CloseEvent/
 * MessageEvent from js_events.c above and G.TextEncoder/TextDecoder from
 * js_webapi.c's prelude. Weak for the same reason as the four above: the
 * host tests of THIS file link without it and simply have no WebSocket. */
#define JS_WEBSOCKET_OPTIONAL
#include "js_websocket.h"
#include "js_wasm.h"
/* `indexedDB` -- js_idb.c, layered over G.EventTarget from js_events.c above
 * and G.DOMException/G.structuredClone from js_platform.c. Weak for the same
 * reason as the five above: the host tests of THIS file link without it and
 * simply have no indexedDB, which is the correct feature-detect answer for a
 * build that does not link the store. See js_idb.h and js_idb.c's own header
 * for why "weak" here is safe rather than a corner cut -- js_idb.c's own
 * install guard additionally checks for EventTarget/DOMException/
 * structuredClone before doing anything, so a build with this TU linked but
 * missing one of those three dependencies also comes up with no indexedDB
 * instead of a half-built one. */
#define JS_IDB_OPTIONAL
#include "js_idb.h"
/* CacheStorage (`caches`/`Cache`) -- js_cache.c. Weak for the same reason as
 * every install above: a build without this TU keeps `typeof caches ===
 * 'undefined'`, the correct feature-detect answer. */
#define JS_CACHE_OPTIONAL
#include "js_cache.h"
/* navigator.serviceWorker -- js_swreg.c. register() always settles (see its
 * own header for why it always rejects rather than half-executing). Weak
 * for the same reason as every install above. */
#define JS_SWREG_OPTIONAL
#include "js_swreg.h"
/* The form controls + focus model -- js_forms.c. Declared here rather than
 * through a header because it is one symbol; weak for the same reason as the
 * five above. */
void js_forms_install(JSContext *ctx) LOGIT_WEAK;
/* The HTML element interfaces -- js_semantics.c: <dialog>'s showModal/close,
 * <table>'s rows/insertRow, <select>'s options, the popover and
 * command/commandfor invokers, and HTMLElement.prototype.click. Weak for the
 * same reason as the others; a build without that TU keeps today's behaviour
 * exactly. */
void js_semantics_install(JSContext *ctx) LOGIT_WEAK;
/* Element.prototype.animate and the computed-style overlay that makes it
 * observable -- js_anim.c. Weak like every install above, and here the
 * weakness is also the MEASUREMENT: with js_anim.c off the link line this call
 * is an exact no-op, so a control binary can be built from an identical tree
 * that differs only in whether the file is linked. That is what makes "2,202
 * subtests gained" attributable rather than asserted. */
void js_anim_install(JSContext *ctx) LOGIT_WEAK;
/* `new DOMParser().parseFromString(str, "text/html")` -- js_domparser.c, a
 * SEPARATE detached-document wrapper (see that file's header for why it is
 * not layered over js_dom.c's own, page-bound one). Weak like the rest: a
 * build without that TU links and `typeof DOMParser === 'undefined'`, which
 * is the correct feature-detect answer for a browser that doesn't have it. */
void js_domparser_install(JSContext *ctx) LOGIT_WEAK;
/* CanvasRenderingContext2D over c/lib/gfx -- js_canvas.c. Weak like the rest;
 * a build without that TU has canvases with no context, which is what this
 * browser was until the callee-naming instrument ranked `getContext` first
 * among every failed call in the site corpus. */
void js_canvas_install(JSContext *ctx) LOGIT_WEAK;
/* The Mach-O half of all five: see include/weaksym.h. */
LOGIT_WEAK_STUB(js_forms_install);
LOGIT_WEAK_STUB(js_semantics_install);
LOGIT_WEAK_STUB(js_anim_install);
LOGIT_WEAK_STUB(js_domparser_install);
LOGIT_WEAK_STUB(js_canvas_install);
/* js_wasm_install was CALLED through LOGIT_HAVE() below without ever being
 * declared here, and the two platforms disagree about what that means: on ELF
 * an undefined weak symbol resolves to NULL and the guard works, on Mach-O it
 * is a HARD LINK ERROR. So browser.aex built fine and FOURTEEN host gates went
 * red at once -- canvas, cssom, csstyle, currentscript, domiface, domparser,
 * domsub, events, loader, logreporter, selectors, webapi_globals,
 * webapi_platform, worker -- every one of them on `Undefined symbols:
 * _js_wasm_install`, none of them for a reason connected to the code under
 * test.
 *
 * That is shape #2 of CLAUDE.md's host-reality table, verbatim, and the tree
 * had already paid for it once and built include/weaksym.h to fix it. The
 * mechanism was there; the one line registering this symbol with it was not.
 * A new js_*_install must join this list in the same commit as its call. */
LOGIT_WEAK_STUB(js_wasm_install);
/* The WHATWG URL parser and URLSearchParams -- js_url.c. Weak for the same
 * reason as the six above. */
#define JS_URL_OPTIONAL
#include "js_url.h"
#include <string.h>
#include <stdlib.h>

int printf(const char *, ...);

/* ---- the console buffer ----
 * Bounded on purpose: a page in a console.log loop must not be able to grow the
 * browser's heap without limit, and only the first line is ever displayed. Once
 * full, output is dropped rather than rotated -- the FIRST messages are the ones
 * that explain a page, not the ten-thousandth. */
#define OUTCAP 4096
static char g_out[OUTCAP];
static int  g_outlen;

/* An optional observer on everything that goes into the buffer above.
 *
 * The buffer is capped at 4 KiB on purpose and that cap is right for the
 * browser -- only the first line is ever displayed, and a page in a
 * console.log loop must not be able to grow the heap. It is wrong for an
 * INSTRUMENT: tests/unit/webapi_probe.c counts a page's uncaught exceptions,
 * kimi produces more than 4 KiB of them, and the probe was silently reading a
 * truncated list and reporting the truncated count. Raising OUTCAP would fix
 * the measurement by changing the thing measured; a sink does not.
 *
 * Fragments, not lines: note() is called several times per message. The
 * observer assembles them, which keeps this end free of a line buffer. */
static void (*g_note_sink)(const char *frag);
void js_page_set_note_sink(void (*fn)(const char *frag)) { g_note_sink = fn; }

static void note(const char *s)
{
    if (!s) return;
    if (g_note_sink) g_note_sink(s);
    while (*s && g_outlen < OUTCAP - 1) g_out[g_outlen++] = *s++;
    g_out[g_outlen] = 0;
}

/* For the ONE writer that lives outside this file: js_module.c's top-level
 * module exception reporter. Everything else that appends here already lives
 * in this TU; a module page that dies on its first statement used to be a
 * SILENT white screen because report() printed to serial only -- the GUI
 * status bar (fed from this buffer) never heard about it. */
void js_page_note(const char *frag) { note(frag); }

const char *js_page_output(void)     { return g_out; }
int         js_page_output_len(void) { return g_outlen; }
void        js_page_output_clear(void) { g_outlen = 0; g_out[0] = 0; }

/* ---- the clock ---- */
static unsigned long long (*g_clock)(void);
void js_page_set_clock(unsigned long long (*fn)(void)) { g_clock = fn; }
static unsigned long long now_ms(void) { return g_clock ? g_clock() : 0; }
unsigned long long js_page_now_ms(void) { return now_ms(); }

/* ---- the CPU-slice watchdog --------------------------------------------
 * One synchronous entry into JS -- a script eval, a timer callback, a module
 * body -- gets a bounded CPU slice; past it, QuickJS's interrupt handler
 * fires and the entry unwinds as an uncatchable InternalError. Before this
 * existed, `while(1)` anywhere in a page owned the browser: run_collected_
 * scripts runs classic scripts synchronously, so one looping script wedged
 * the load pipeline forever. qwen.com was the wild specimen -- gdb on the
 * live wedge showed CPU#3 spinning at moving ring-3 addresses in the
 * browser image for 240 s, memory dripping from the loop's allocations,
 * everything else idle, nothing ever printed (the scoreboard's TIMEOUT
 * verdict, dbg4 run, 2026-08-16). The slice is deliberately huge -- 45 s at
 * TCG speed is several times any legitimate script measured (kimi's whole
 * 109-request load is 60 s) -- because the point is to convert "hung
 * forever" into "one loud line and the next script runs", not to police
 * slow-but-honest code. g_slice_due==0 disarms (input-event dispatch is
 * user-attended; it can be armed later if a wild page earns it). */
/* TWO RAILS, because the browser has two kinds of clock. On the device the
 * injected clock is a syscall that advances during a spin, so wall time is
 * the natural budget. In the host harnesses the clock is a FAKE stepped by
 * hand between pumps -- it is frozen for the whole of a synchronous eval, and
 * a time-only watchdog provably never fires there (the first draft hung
 * loader_test inside its own while(1) fixture). The fuel rail counts
 * interrupt-handler invocations, which QuickJS makes every
 * JS_INTERRUPT_COUNTER_INIT = 10,000 ~~bytecodes~~ -- AND THAT WORD WAS WRONG,
 * measured 2026-08-30 and corrected beside the original because somebody will
 * arrive holding it. `js_poll_interrupts` is not called per bytecode. Reading
 * third_party/quickjs/quickjs.c, it is called from OP_goto/goto8/goto16 and
 * from OP_if_true/if_false and their 8-bit forms (17629-17721) -- taken or
 * not -- and from JS_CallInternal's entry (16569). So the unit is a BRANCH OR
 * A CALL, not a bytecode, and 10,000 of those is far more work than 10,000
 * bytecodes. Confirmed against a known answer rather than by reading alone:
 * `for (i=0;i<N;i++) s+=i` costs exactly 2.00 polls per iteration -- the loop
 * test and the back edge -- measured at N = 200k/400k/800k by
 * `webapi_probe --prof-selftest`, which is the gate for this sentence.
 * The consequence for anyone sizing this rail: 2,000,000 calls is 2e10
 * BRANCHES AND CALLS, not 2e10 bytecodes, so the fuel budget is even further
 * above any honest script than the old wording claimed -- which is why it has
 * never been the rail that fires. Defaults: 45 s wall (several times any
 * honest script at TCG speed) and 2,000,000 calls, purely the frozen-clock
 * backstop.
 *
 * A SECOND CONSEQUENCE, and it is the one that bites: THE WATCHDOG CAN ONLY
 * FIRE AT A POLL POINT. A synchronous entry that executes fewer than 10,000
 * branches-or-calls is never checked at all, so the dog's resolution is coarse
 * and a small handler is invisible to it. */
#define JS_SLICE_MS_DEFAULT   45000
#define JS_SLICE_FUEL_DEFAULT 2000000
static long long g_slice_ms = JS_SLICE_MS_DEFAULT;
static long long g_slice_due;                 /* absolute ms; 0 = disarmed */
static long long g_slice_fuel_max = JS_SLICE_FUEL_DEFAULT;
static long long g_slice_fuel;                /* handler calls this slice */
static int g_slice_armed;
static int g_slice_hits;
void js_page_set_slice_ms(int ms) { g_slice_ms = ms > 0 ? ms : JS_SLICE_MS_DEFAULT; }
void js_page_set_slice_fuel(long long calls)
{ g_slice_fuel_max = calls > 0 ? calls : JS_SLICE_FUEL_DEFAULT; }
int  js_page_slice_hits(void) { return g_slice_hits; }
/* The WATCHDOG's own fuel counter for the slice that just ran -- deliberately a
 * different counter from js_prof's `fuel`, and exported for exactly one reason:
 * the observer-effect control. Comparing js_prof's count with the profiler on
 * against js_prof's count with it on again measures run-to-run determinism and
 * calls itself an observer-effect check, which is a control that cannot be
 * watched failing. This counter is incremented by slice_interrupt whether or
 * not g_prof_on, so it is the one number that can answer "does profiling change
 * the work?" -- see webapi_probe.c's check 5. */
long long js_page_slice_fuel_used(void) { return g_slice_fuel; }
void js_page_slice_begin(void);               /* defined after now_ms() */

static void prof_begin(long long t);          /* the profiler, defined below */

void js_page_slice_begin(void)
{
    long long t = g_clock ? (long long)now_ms() : 0;
    g_slice_due = g_clock ? t + g_slice_ms : 0;
    g_slice_fuel = 0;
    g_slice_armed = 1;
    prof_begin(t);
}

/* ---- js_prof: WHERE A SLICE'S TIME ACTUALLY GOES ------------------------
 *
 * THE QUESTION THIS EXISTS FOR. A page reports "[watchdog] script exceeded
 * its CPU slice" and nothing in this tree can say what the script was doing
 * for the budget. Raising the budget is not an answer; it is the same wait,
 * longer. The five things it could be need completely different work:
 *   (a) honest interpretation -- a megabyte of minified bundle really is a
 *       lot of bytecode on an interpreter under TCG;
 *   (b) the C boundary -- hundreds of thousands of DOM crossings;
 *   (c) something quadratic -- cost that grows faster than the input;
 *   (d) a spin -- forward progress zero, the watchdog SAVED us;
 *   (e) layout or paint re-entered from script, charged to the slice.
 * and a sixth this instrument found on its first run, which none of the five
 * covers: the budget being consumed by wall clock the script did not spend.
 *
 * THE CLOCK IS QUICKJS'S OWN WORK COUNTER, NOT THE HOST'S WALL CLOCK.
 * QuickJS calls slice_interrupt() every JS_INTERRUPT_COUNTER_INIT = 10,000
 * POLL EVENTS, and a poll event is a branch (OP_goto*, OP_if_true*,
 * OP_if_false*) or a function call -- NOT a bytecode; see the watchdog comment
 * above for the measurement that corrected that word. It is still what this
 * profiler needs: a known, uniform, machine-independent period, already in the
 * guest, immune to tools/perf/'s rule that "the host is contended, so host wall
 * clock is worthless here". `fuel` below counts those calls, so fuel*10,000 is
 * exactly the number of loop iterations and function calls the page performed.
 *
 * THE ONE WALL-CLOCK READ IS THE ONE THE WATCHDOG ALREADY DID. slice_interrupt
 * has always called now_ms() on every sample to test the time rail. This
 * profiler reuses that single read, so it adds no clock traffic at all -- which
 * matters, because on the device now_ms() is a syscall.
 *
 * WHAT A SAMPLE RECORDS, AND WHY THESE THREE NUMBERS SEPARATE THE HYPOTHESES.
 * Between two consecutive samples the page executed exactly 10,000 branches
 * and calls. The wall time that elapsed across them is therefore the cost of
 * that work PLUS everything the interpreter called out to and waited for. So:
 *   fuel    -- (branches+calls)/10,000. Rises only when interpreted code runs.
 *   js_ms   -- wall ms summed across samples taken INSIDE a JS entry.
 *   out_ms  -- wall ms that elapsed between one entry ending and the next
 *              sample arriving; i.e. time the browser spent NOT in JS.
 * and the discriminator is the ratio. js_ms/fuel near the interpreter's own
 * rate is (a). js_ms/fuel far above it is (b) or (e) -- time inside C called
 * from JS, which produces no poll events and so no samples. fuel large with the
 * page's DOM not growing is (d). And js_ms much SMALLER than the elapsed time
 * the watchdog is measuring means the budget is being spent by a clock rather
 * than by the script, which is the sixth case and is not a performance problem
 * at all.
 *
 * `gaps` is the coarse localiser: a single delta of JSPROF_GAP_MS or more
 * between two samples cannot be 10,000 branches of interpretation at any
 * plausible rate, so it is one long call out of the interpreter. The threshold
 * is four ticks of the device's 10 ms clock deliberately -- a granularity that
 * cannot manufacture a gap the way a 1-tick threshold would.
 *
 * WHAT IT CANNOT SEE, said rather than left to be discovered. It cannot name
 * the C function: a native call polls once on entry and then executes no
 * branches, so no sample lands inside it and
 * it, and the cost shows up only as the gap after it returns. It cannot name
 * the JS function either -- that needs the interpreter's current stack frame,
 * which is internal to third_party/quickjs and is another line's file. Both
 * are attributable to the SLICE, which is a script URL or a timer, and that is
 * the resolution this instrument claims. Nothing here is a per-function
 * profile and it must not be quoted as one. */
/* struct js_prof_slice is in js_page.h -- a harness reads the fields. */
#define JSPROF_SLICES   256
#define JSPROF_GAP_MS   40
static struct js_prof_slice g_prof[JSPROF_SLICES];
static int  g_prof_n;                 /* records in use */
static int  g_prof_over;              /* entries folded into the last record */
static int  g_prof_on;
static long long g_prof_last_ms;      /* clock at the previous sample/boundary */
static int  g_prof_in_js;             /* 1 between slice_begin and slice_end */
static const char *g_prof_label = "?";

/* A FREE-RUNNING POLL COUNTER, and it is deliberately not any of the three
 * counters above it. g_slice_fuel is zeroed by every slice_begin and
 * struct js_prof_slice::fuel is per record; neither can answer "how much work
 * did the four statements between these two lines of script cost", which is the
 * question a scaling scan asks -- cost against input size, the shape CLAUDE.md
 * says hides in an average. This one only ever increases and is reset by
 * js_prof_reset(), so a harness reads it before and after a benchmark and
 * subtracts. It is incremented whether or not the profiler is on, for the same
 * reason js_page_slice_fuel_used() is: a counter that exists only while being
 * observed cannot be the control for the observation. */
static long long g_polls;
long long js_prof_polls(void) { return g_polls; }

void js_prof_enable(int on) { g_prof_on = on ? 1 : 0; }
int  js_prof_enabled(void)  { return g_prof_on; }
void js_prof_label(const char *what) { g_prof_label = what ? what : "?"; }
void js_prof_reset(void)
{
    g_prof_n = 0; g_prof_over = 0; g_prof_last_ms = 0; g_prof_in_js = 0;
    g_polls = 0;
    for (int i = 0; i < JSPROF_SLICES; i++) {
        g_prof[i].what[0] = 0; g_prof[i].fuel = 0; g_prof[i].js_ms = 0;
        g_prof[i].out_ms = 0; g_prof[i].begin_ms = 0; g_prof[i].max_gap_ms = 0;
        g_prof[i].gap_ms = 0; g_prof[i].gaps = 0; g_prof[i].resumed = 0;
        g_prof[i].bitten = 0;
    }
}
int js_prof_count(void) { return g_prof_n; }
int js_prof_overflow(void) { return g_prof_over; }
const struct js_prof_slice *js_prof_at(int i)
{ return (i >= 0 && i < g_prof_n) ? &g_prof[i] : 0; }

static struct js_prof_slice *prof_cur(void)
{
    if (!g_prof_on) return 0;
    if (g_prof_n == 0) {            /* a sample before any slice_begin */
        g_prof_n = 1;
        g_prof[0].what[0] = '?'; g_prof[0].what[1] = 0;
    }
    return &g_prof[g_prof_n - 1];
}

static void prof_begin(long long t)
{
    if (!g_prof_on) { g_prof_in_js = 1; g_prof_last_ms = t; return; }
    struct js_prof_slice *s;
    if (g_prof_n < JSPROF_SLICES) {
        s = &g_prof[g_prof_n++];
        s->fuel = 0; s->js_ms = 0; s->out_ms = 0; s->max_gap_ms = 0;
        s->gap_ms = 0; s->gaps = 0; s->resumed = 0; s->bitten = 0;
    } else {
        /* Full. Fold the rest into the last record rather than dropping them:
         * a total that silently stops counting is the failure mode this tree
         * calls a silent cap. js_prof_overflow() reports how many. */
        g_prof_over++;
        s = &g_prof[JSPROF_SLICES - 1];
    }
    s->begin_ms = t;
    int i = 0;
    while (g_prof_label[i] && i < (int)sizeof s->what - 1) { s->what[i] = g_prof_label[i]; i++; }
    s->what[i] = 0;
    g_prof_in_js = 1;
    g_prof_last_ms = t;
}

/* Called when a synchronous JS entry returns, and from js_page_pending() -- the
 * browser's main loop -- for the entries that bracket nothing.
 *
 * IT MUST CHARGE THE INTERVAL SINCE THE LAST SAMPLE, and the first version did
 * not: it only reset the cursor, so the wall time between the final sample of
 * an entry and this boundary was charged to NOBODY. That is worse than the
 * misattribution it replaced, because a total that silently loses time reads as
 * a fast page. In the selftest it lost exactly the 5,000 ms the whole check
 * exists to see: js_ms fell from 5012 to 11 and out_ms stayed at 2.
 *
 * WHICH BUCKET, and this is the honest part. The interval ends at a boundary
 * the sampler did not observe, so it cannot be split between "the tail of the
 * handler" and "idle". The one thing that CAN be said about it is the thing
 * checks 4a/4b of the selftest validate: an interval of JSPROF_GAP_MS or more
 * cannot be 10,000 branches of interpretation at any plausible rate. So a long
 * interval goes to out_ms and is counted as a gap; a short one is charged to
 * js_ms, which is the conservative direction -- it bills the script.
 *
 * The consequence for the reader is a definition, and out_ms's is now "wall
 * time not attributable to interpretation" rather than "time with no JS
 * running". A long native call made FROM a script -- a forced layout, a decode
 * -- lands here too. That is a conflation and it is deliberate: separating it
 * needs the interpreter's stack frame, which is another line's file. `gaps`
 * counts both kinds and neither is script CPU, which is the question this
 * instrument was built to answer. */
void js_page_slice_end(void)
{
    if (g_prof_on) {
        long long t = (long long)now_ms();
        struct js_prof_slice *s = prof_cur();
        if (s && g_prof_last_ms) {
            long long d = t - g_prof_last_ms;
            if (d < 0) d = 0;
            if (d >= JSPROF_GAP_MS) {
                s->out_ms += d;
                s->gap_ms += d; s->gaps++;
                if (d > s->max_gap_ms) s->max_gap_ms = d;
            } else {
                s->js_ms += d;
            }
        }
        g_prof_last_ms = t;
    }
    g_prof_in_js = 0;
}

static void prof_sample(long long t)
{
    struct js_prof_slice *s = prof_cur();
    if (!s) return;
    s->fuel++;
    if (g_prof_last_ms) {
        long long d = t - g_prof_last_ms;
        if (d < 0) d = 0;
        if (g_prof_in_js) {
            s->js_ms += d;
            if (d >= JSPROF_GAP_MS) {
                s->gap_ms += d; s->gaps++;
                if (d > s->max_gap_ms) s->max_gap_ms = d;
            }
        } else {
            s->out_ms += d;
            s->resumed++;      /* a JS entry ran that never began a slice */
        }
    }
    g_prof_in_js = 1;
    g_prof_last_ms = t;
}

void js_prof_totals(long long *fuel_o, long long *js_ms_o, long long *out_ms_o,
                    int *gaps_o, int *resumed_o)
{
    long long fuel = 0, js_ms = 0, out_ms = 0;
    int gaps = 0, resumed = 0;
    for (int i = 0; i < g_prof_n; i++) {
        fuel += g_prof[i].fuel; js_ms += g_prof[i].js_ms;
        out_ms += g_prof[i].out_ms;
        gaps += g_prof[i].gaps; resumed += g_prof[i].resumed;
    }
    if (fuel_o) *fuel_o = fuel;
    if (js_ms_o) *js_ms_o = js_ms;
    if (out_ms_o) *out_ms_o = out_ms;
    if (gaps_o) *gaps_o = gaps;
    if (resumed_o) *resumed_o = resumed;
}

void js_prof_dump(const char *tag)
{
    long long fuel = 0, js_ms = 0, out_ms = 0, gap = 0;
    int gaps = 0, resumed = 0;
    for (int i = 0; i < g_prof_n; i++) {
        fuel += g_prof[i].fuel; js_ms += g_prof[i].js_ms;
        out_ms += g_prof[i].out_ms; gap += g_prof[i].gap_ms;
        gaps += g_prof[i].gaps; resumed += g_prof[i].resumed;
    }
    printf("[jsprof] %s: slices=%d(+%d folded) fuel=%lld (=%lld branches+calls) "
           "js_ms=%lld out_ms=%lld gaps=%d/%lldms resumed=%d\n",
           tag ? tag : "", g_prof_n, g_prof_over, fuel, fuel * 10000,
           js_ms, out_ms, gaps, gap, resumed);
    printf("[jsprof] %-40s %10s %8s %8s %6s %8s %3s\n",
           "slice", "fuel", "js_ms", "out_ms", "gaps", "maxgap", "bit");
    for (int i = 0; i < g_prof_n; i++) {
        struct js_prof_slice *s = &g_prof[i];
        if (!s->fuel && !s->out_ms) continue;      /* under one poll period */
        printf("[jsprof] %-40s %10lld %8lld %8lld %6d %8lld %3d\n",
               s->what, s->fuel, s->js_ms, s->out_ms, s->gaps,
               s->max_gap_ms, s->bitten);
    }
}

/* QuickJS calls this every 10,000 branches-or-calls. Nonzero = interrupt: the
 * throws an uncatchable InternalError and the current synchronous entry
 * unwinds through the caller's normal exception printing. */
static int slice_interrupt(JSRuntime *rt, void *opaque)
{
    (void)rt; (void)opaque;
    /* ONE clock read, shared by the watchdog's time rail and the profiler.
     * On the device now_ms() is a syscall; a second one here would make the
     * instrument the largest thing it measures. */
    long long t = g_clock ? (long long)now_ms() : 0;
    g_polls++;
    if (g_prof_on) prof_sample(t);
    if (!g_slice_armed) return 0;
    int over_time = g_slice_due && g_clock && t > g_slice_due;
    int over_fuel = ++g_slice_fuel > g_slice_fuel_max;
    if (!over_time && !over_fuel) return 0;
    g_slice_hits++;
    g_slice_armed = 0;               /* one interrupt per slice, not a storm */
    note("[watchdog] script exceeded its CPU slice -- interrupted\n");
    printf("[js] watchdog: script exceeded its CPU slice (%s) -- interrupted\n",
           over_time ? "wall time" : "instruction fuel");
    /* AND WHAT THE BITTEN SLICE ACTUALLY CONSUMED, because the rail name alone
     * does not say whether the budget was spent or merely expired. `since` is
     * the interval the wall-time rail measured; js_ms is how much of it the
     * script was running in. When those two disagree the watchdog is not
     * reporting a slow script. */
    {
        struct js_prof_slice *s = g_prof_on ? prof_cur() : 0;
        if (s) s->bitten = over_time ? 1 : 2;
        printf("[js] watchdog: fuel=%lld (=%lld branches+calls) since_begin_ms=%lld"
               " js_ms=%lld out_ms=%lld resumed=%d slice=%s\n",
               g_slice_fuel, g_slice_fuel * 10000,
               g_slice_due ? t - (g_slice_due - g_slice_ms) : -1,
               s ? s->js_ms : -1, s ? s->out_ms : -1, s ? s->resumed : -1,
               s ? s->what : "<prof off>");
    }
    return 1;
}

static JSRuntime *g_rt;
static JSContext *g_ctx;
static unsigned long long g_t0;              /* page start, for performance.now() */
static char g_location[600] = "about:blank";

int  js_page_live(void) { return g_ctx != 0; }
JSContext *js_page_ctx(void) { return g_ctx; }
void js_page_set_location(const char *url)
{
    int i = 0;
    if (url) while (url[i] && i < (int)sizeof g_location - 1) { g_location[i] = url[i]; i++; }
    g_location[i] = 0;
}
const char *js_page_location(void) { return g_location; }

/* ---- timers ----
 *
 * One queue for setTimeout, setInterval and requestAnimationFrame, because they
 * differ only in when they are re-armed. It is a plain linked list scanned
 * linearly: a page holds a handful of timers, and a heap would cost more in
 * code than it saves in comparisons.
 *
 * `seq` is the tiebreaker. Two timers with the same deadline -- which is the
 * COMMON case here, since the clock only advances in 10 ms steps -- must fire in
 * the order they were created, or `setTimeout(a,0); setTimeout(b,0)` becomes a
 * coin flip. */
struct jstimer {
    struct jstimer *next;
    int id;
    int raf;                       /* requestAnimationFrame callback */
    int interval_ms;               /* >0: re-arm after firing (setInterval) */
    unsigned long long due;
    unsigned long long seq;
    JSValue fn;
    JSValue *argv; int argc;       /* extra setTimeout(fn, ms, ...args) arguments */
};

static struct jstimer *g_timers;
static int g_next_id = 1;                    /* setTimeout/setInterval handles */
static int g_next_raf_id = 1;                /* rAF has its own handle space, per spec */
static unsigned long long g_seq;
static unsigned long long g_frame_due;       /* the next animation-frame boundary */

/* 60 Hz is the target; the monotonic clock advances in 10 ms steps, so the real
 * cadence is 20 ms. Asking for 16 and letting the clock round up is honest --
 * claiming 16 would just mean firing every tick, i.e. a 100 Hz busy loop. */
#define FRAME_MS 16

static void timer_free(JSContext *ctx, struct jstimer *t)
{
    JS_FreeValue(ctx, t->fn);
    for (int i = 0; i < t->argc; i++) JS_FreeValue(ctx, t->argv[i]);
    free(t->argv);
    free(t);
}

static void timers_clear(JSContext *ctx)
{
    while (g_timers) { struct jstimer *t = g_timers; g_timers = t->next; timer_free(ctx, t); }
    g_frame_due = 0;
}

static void timer_unlink(struct jstimer *t)
{
    for (struct jstimer **pp = &g_timers; *pp; pp = &(*pp)->next)
        if (*pp == t) { *pp = t->next; return; }
}

int js_page_pending(void)
{
    /* THE JS/NOT-JS BOUNDARY THE PROFILER COULD NOT SEE, and it is here rather
     * than in js_page_slice_end() because slice_end has callers only for the
     * entries that ALSO call slice_begin -- script eval and timer callbacks.
     * Event dispatch does not: js_dom_dispatch() brackets nothing, so every
     * DOMContentLoaded, load, click and input handler ran with g_prof_in_js
     * left at 1 by the previous entry's last sample, and prof_sample charged
     * the idle BETWEEN two dispatches to js_ms -- script time the script never
     * spent. `resumed` was supposed to be the flag that said so and it only
     * fired for the FIRST unbracketed entry, for the same reason.
     *
     * This is the browser's main loop, which is by definition not inside a JS
     * entry, so clearing here is correct for every entry regardless of what it
     * brackets. Cost when profiling is off is one load and one branch; the
     * clock read is inside slice_end's own g_prof_on guard, which matters
     * because on the device now_ms() is a syscall and this runs every pass. */
    if (g_prof_on && g_prof_in_js) js_page_slice_end();
    if (g_timers) return 1;
    /* A fetch in flight also needs the loop to call js_page_run_due(), which is
     * where its socket is stepped. */
    if (LOGIT_HAVE(js_webapi_pending) && js_webapi_pending()) return 1;
    /* Same for a WebSocket that is CONNECTING, OPEN, or CLOSING -- see the long
     * comment on js_websocket_pump below. Missing this line is failure #5 of
     * this repository under a new name: the connection links, installs,
     * answers feature detection, and never progresses because nothing ever
     * calls js_websocket_pump again after the first frame. */
    if (LOGIT_HAVE(js_websocket_pending) && js_websocket_pending()) return 1;
    /* A live Worker with no timer of its own -- a dedicated worker sitting
     * idle after its startup fetch, waiting on a postMessage that has not
     * arrived yet -- must not read as an idle page either. See js_worker.h. */
    return LOGIT_HAVE(js_worker_pending) && js_worker_pending();
}

long long js_page_next_due(void)
{
    long long best = -1;
    for (struct jstimer *t = g_timers; t; t = t->next)
        if (best < 0 || (long long)t->due < best) best = (long long)t->due;
    if (LOGIT_HAVE(js_worker_next_due)) {
        long long wbest = js_worker_next_due();
        if (wbest >= 0 && (best < 0 || wbest < best)) best = wbest;
    }
    return best;
}

int js_page_pump(void) { return js_dom_run_jobs(g_ctx); }

int js_page_run_due(void)
{
    if (!g_ctx) return 0;
    int ran = 0;

    /* In-flight fetches first: they are the thing that must make progress on
     * EVERY pass of the loop, timers or no timers. A resolved promise queues
     * its reactions, so drain the microtask queue before returning -- the
     * embedder repaints on a non-zero return and the .then() that writes the
     * DOM has to have run by then. */
    if (LOGIT_HAVE(js_webapi_pump)) {
        int n = js_webapi_pump(g_ctx);
        if (n > 0) { js_dom_run_jobs(g_ctx); ran += n; }
    }
    /* WebSocket connections next, same reasoning: a socket that only makes
     * progress when a timer happens to be pending is a socket that hangs on
     * a page with none -- which describes most of the WPT websockets/ corpus,
     * whose async_test()s have no timer at all. */
    if (LOGIT_HAVE(js_websocket_pump)) {
        int n = js_websocket_pump(g_ctx);
        if (n > 0) { js_dom_run_jobs(g_ctx); ran += n; }
    }
    /* Every worker task due on this pass: a queued startup, a delivered
     * message in either direction, a worker's own timer. Unconditional, like
     * the two pumps above -- a page with zero timers of its own but a live
     * worker must still be driven every pass, which is why this runs before
     * the `!g_timers` early return below. */
    if (LOGIT_HAVE(js_worker_run_due)) ran += js_worker_run_due();
    if (!g_timers) return ran;

    unsigned long long now = now_ms();
    /* Snapshot the sequence counter: a callback that schedules another timer for
     * "now" must wait for the next pass. Without this, `setTimeout(f, 0)` calling
     * itself would spin inside this loop and the browser would never repaint.
     * (A re-armed setInterval takes a fresh seq for the same reason.) */
    unsigned long long limit = g_seq;

    for (;;) {
        struct jstimer *best = 0;
        for (struct jstimer *t = g_timers; t; t = t->next) {
            if (t->due > now || t->seq > limit) continue;
            if (!best || t->due < best->due || (t->due == best->due && t->seq < best->seq))
                best = t;
        }
        if (!best) break;

        /* Take our own references before calling: the callback may clear this
         * very timer (clearInterval(self) is idiomatic), which would otherwise
         * free the JSValue we are about to invoke. */
        JSValue fn = JS_DupValue(g_ctx, best->fn);
        int nargs = best->argc;
        JSValue *args = 0;
        if (best->raf) {
            nargs = 1;
            args = malloc(sizeof *args);
            if (args) args[0] = JS_NewFloat64(g_ctx, (double)(now - g_t0));
            else nargs = 0;
        } else if (nargs > 0) {
            args = malloc((size_t)nargs * sizeof *args);
            if (args) for (int i = 0; i < nargs; i++) args[i] = JS_DupValue(g_ctx, best->argv[i]);
            else nargs = 0;
        }

        if (best->interval_ms > 0) {
            /* Re-arm from NOW, not from the old deadline: catching up on missed
             * ticks after a slow page load would fire a burst of callbacks the
             * page never asked for. */
            best->due = now + (unsigned long long)best->interval_ms;
            best->seq = ++g_seq;
        } else {
            timer_unlink(best);
            timer_free(g_ctx, best);
        }

        js_prof_label(best->raf ? "<rAF callback>" : "<timer callback>");
        js_page_slice_begin();       /* each timer callback is its own slice */
        JSValue r = JS_Call(g_ctx, fn, JS_UNDEFINED, nargs, (JSValueConst *)args);
        if (JS_IsException(r)) {
            JSValue e = JS_GetException(g_ctx);
            const char *m = JS_ToCString(g_ctx, e);
            printf("[js] uncaught in timer: %s\n", m ? m : "?");
            note("[exception] "); if (m) note(m); note("\n");
            if (m) JS_FreeCString(g_ctx, m);
            JS_FreeValue(g_ctx, e);
        }
        JS_FreeValue(g_ctx, r);
        JS_FreeValue(g_ctx, fn);
        for (int i = 0; i < nargs; i++) JS_FreeValue(g_ctx, args[i]);
        free(args);
        js_dom_run_jobs(g_ctx);          /* a timer that resolves a promise: run its reactions now */
        js_page_slice_end();
        ran++;
    }

    /* The frame boundary has passed; the next rAF starts a new frame. */
    if (g_frame_due && g_frame_due <= now) g_frame_due = 0;
    return ran;
}

static int timer_add(JSContext *ctx, JSValueConst fn, long delay, int interval,
                     int raf, int argc, JSValueConst *argv)
{
    struct jstimer *t = calloc(1, sizeof *t);
    if (!t) return 0;
    unsigned long long now = now_ms();
    if (raf) {
        /* Every rAF callback registered before the boundary runs in the same
         * frame with the same timestamp -- that is what lets a page schedule
         * several animations and have them stay in step. */
        if (!g_frame_due || g_frame_due <= now) g_frame_due = now + FRAME_MS;
        t->due = g_frame_due;
        t->id = g_next_raf_id++;
        t->raf = 1;
    } else {
        if (delay < 0) delay = 0;
        t->due = now + (unsigned long long)delay;
        t->id = g_next_id++;
        t->interval_ms = interval ? (delay > 0 ? (int)delay : 1) : 0;
    }
    t->seq = ++g_seq;
    t->fn = JS_DupValue(ctx, fn);
    if (argc > 0) {
        t->argv = malloc((size_t)argc * sizeof *t->argv);
        if (t->argv) { for (int i = 0; i < argc; i++) t->argv[i] = JS_DupValue(ctx, argv[i]); t->argc = argc; }
    }
    t->next = g_timers;
    g_timers = t;
    return t->id;
}

static void timer_cancel(JSContext *ctx, int id, int raf)
{
    for (struct jstimer **pp = &g_timers; *pp; pp = &(*pp)->next)
        if ((*pp)->id == id && (*pp)->raf == raf) {
            struct jstimer *t = *pp;
            *pp = t->next;
            timer_free(ctx, t);
            return;
        }
}

/* setTimeout(fn, delay, ...args). A string first argument (the eval form) is
 * deliberately not supported -- it is deprecated, and supporting it would mean
 * carrying a second code path that can only ever compile untrusted text. */
static JSValue js_setTimeout(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_NewInt32(ctx, 0);
    int32_t ms = 0;
    if (argc > 1) JS_ToInt32(ctx, &ms, argv[1]);
    return JS_NewInt32(ctx, timer_add(ctx, argv[0], ms, 0, 0,
                                      argc > 2 ? argc - 2 : 0, argc > 2 ? argv + 2 : 0));
}
static JSValue js_setInterval(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_NewInt32(ctx, 0);
    int32_t ms = 0;
    if (argc > 1) JS_ToInt32(ctx, &ms, argv[1]);
    return JS_NewInt32(ctx, timer_add(ctx, argv[0], ms, 1, 0,
                                      argc > 2 ? argc - 2 : 0, argc > 2 ? argv + 2 : 0));
}
static JSValue js_clearTimer(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    int32_t id = 0;
    if (argc > 0) JS_ToInt32(ctx, &id, argv[0]);
    timer_cancel(ctx, id, 0);
    return JS_UNDEFINED;
}
static JSValue js_raf(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_NewInt32(ctx, 0);
    return JS_NewInt32(ctx, timer_add(ctx, argv[0], 0, 0, 1, 0, 0));
}
static JSValue js_caf(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    int32_t id = 0;
    if (argc > 0) JS_ToInt32(ctx, &id, argv[0]);
    timer_cancel(ctx, id, 1);
    return JS_UNDEFINED;
}
static JSValue js_perf_now(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t; (void)argc; (void)argv;
    return JS_NewFloat64(ctx, (double)(now_ms() - g_t0));
}

/* ---- console ----
 * Richer than js_dom.c's fallback: this one also captures into the buffer the
 * browser's status bar reads, which is the only place a headless screenshot
 * test can see that a script ran. */
/* An Error logged BY THE PAGE is the only window we have into a bundle's own
 * failure handling, and JS_ToCString on it yields the message alone. MEASURED
 * on stripe.com: six consecutive `[error] TypeError: not a function` out of
 * React's error path, identical, naming nothing -- six different bugs and one
 * function called six times look exactly the same in that log, and the whole
 * page renders empty behind them.
 *
 * QuickJS's .stack holds the frames WITHOUT the message header (unlike V8),
 * which is why the uncaught-exception printer already emits message-then-stack
 * and why this can simply append. The trailing newline is trimmed so the
 * caller's terminator stays the only one. */
static void con_stack(JSContext *ctx, JSValueConst v)
{
    if (!JS_IsError(ctx, v)) return;
    JSValue st = JS_GetPropertyStr(ctx, v, "stack");
    if (!JS_IsString(st)) { JS_FreeValue(ctx, st); return; }
    const char *s = JS_ToCString(ctx, st);
    if (s && *s) {
        int n = 0;
        while (s[n]) n++;
        while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) n--;
        /* Serial only. note() feeds the status bar, which is one line wide;
         * a stack there would push out the message it belongs to. */
        if (n > 0) printf("\n%.*s", n, s);
    }
    if (s) JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, st);
}

static JSValue con_out(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    for (int i = 0; i < argc; i++) {
        const char *s = JS_ToCString(ctx, argv[i]);
        if (!s) continue;
        if (i) { note(" "); printf(" "); }
        note(s); printf("%s", s);
        JS_FreeCString(ctx, s);
        con_stack(ctx, argv[i]);
    }
    note("\n"); printf("\n");
    return JS_UNDEFINED;
}
static JSValue con_warn(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{ note("[warn] "); printf("[warn] "); return con_out(ctx, t, argc, argv); }
static JSValue con_error(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{ note("[error] "); printf("[error] "); return con_out(ctx, t, argc, argv); }

/* ---- document.currentScript -------------------------------------------
 *
 * MEASURED on two unrelated sites, and it is not a page quirk -- it is the
 * mechanism bundlers use to find their own chunks and the mechanism inline
 * scripts use to find themselves:
 *
 *   nodejs.org   the webpack/Next.js chunk loader derives its base URL from
 *                document.currentScript.src, finds nothing, checks whether it
 *                is in a Worker, and throws its OWN error: "chunk path empty
 *                but not in a worker". Every script after it on the page then
 *                fails too -- one undefined property, the whole page.
 *   x.com        four uncaught exceptions, all `cannot read property 'remove'
 *                of undefined`, from the standard document.currentScript
 *                .remove() idiom where an inline script deletes its own tag.
 *
 * THE NODE, NOT A NAME FOR IT, AND THAT IS THE WHOLE FIX. Until 2026-08-29
 * this held an INDEX into document.scripts and worked out which script was
 * running by pattern-matching the filename the embedder passed:
 *
 *     match = !filename || !strchr(filename, ':');   / * an inline one * /
 *
 * Every page URL contains "https:", so for an inline classic script in the
 * SHIPPED BROWSER that test was false every time, the index stayed -1, and
 * document.currentScript was null for the entire life of the feature. The two
 * halves were each right on their own and landed the same day, 25 commits
 * apart: browser.c had just started passing the page URL + "#inline-script-N"
 * so that `import('./x.js')` from an inline <script> has a base to resolve
 * against, and this matcher was written for the "<inline>" that used to be
 * there. The src branch had the mirror-image defect -- it paired the attribute
 * to the filename with a SUFFIX test, so src="./x.js" against the absolute URL
 * it resolved to (".../x.js") did not match, because the literal "./" is in
 * the attribute and not in the URL.
 *
 * A string that has to be pattern-matched to recover information the caller
 * already had is the defect, not the pattern -- so the caller passes the node.
 * There is nothing left to match, no cursor to keep aligned, and no filename
 * shape a future embedder can break by changing. js_dom.c grew ONE export
 * (js_dom_node_value) so the node can be handed to JavaScript as itself; the
 * index indirection through document.scripts is gone with it, which is also
 * what makes `document.currentScript.remove()` -- the x.com idiom above --
 * behave: removing the element used to renumber the collection the index was
 * standing in.
 *
 * NULL EVERYWHERE ELSE, which is the spec and is also rule 2 of this tree: a
 * currentScript that names the wrong script is worse than one that names
 * none. "Everywhere else" has a precise edge and it is NOT where this file
 * first put it -- the script's own MICROTASK CHECKPOINT is inside the script,
 * not after it, so a `.then()` the script queued sees the script and a
 * setTimeout callback does not. The full argument, and the production code
 * that depends on it, is on js_page_eval below. js_module_eval never sets it
 * at all: currentScript is null during a module by definition. */
static struct node *g_cur_script;

static JSValue js_cur_script_node(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t; (void)argc; (void)argv;
    return g_cur_script ? js_dom_node_value(ctx, g_cur_script) : JS_NULL;
}

/* Exported because js_page_eval is not the only caller that runs a page's
 * classic script: tests/unit/webapi_probe.c and the WPT runner evaluate each
 * one themselves so they can keep the exception object rather than the printed
 * message. They pass the same node browser.c does, through the same door --
 * before this took a node they passed a filename string, which is how the
 * probe came to be measuring a currentScript the browser could never produce.
 * A NULL node means "no script is running", i.e. currentScript === null. */
void js_page_begin_script(struct node *node)
{
#ifdef JS_CURRENTSCRIPT_NOTOLD
    /* THE NEGATIVE CONTROL, and it is the defect itself on a switch rather
     * than a lookalike: the runtime is not told which node is running, so
     * document.currentScript is null. That is EXACTLY the state the shipped
     * browser was in for every inline classic script on every page -- not
     * because anyone chose it, but because the only channel was a filename
     * string and the string could not carry the answer.
     *
     * It is here so `make test-currentscript-negctl` can be WATCHED FAILING
     * the positive gate's assertions on the real machine. A control that
     * cannot be watched failing is worse than no control (rule 5), and this
     * one is cheap: one -D, one link, the same driver, the same page. */
    (void)node;
    g_cur_script = 0;
#else
    g_cur_script = node;
#endif
}
void js_page_end_script(void) { g_cur_script = 0; }

int js_page_open(struct node *root)
{
    js_page_close();
    g_rt = JS_NewRuntime();
    if (!g_rt) return 0;
    /* The browser's ring-3 user stack is 8 MiB (wm.c gives it double the normal
     * app stack). Bound QuickJS's overflow guard well under that so a deeply
     * recursive script throws a catchable "stack overflow" RangeError instead of
     * overrunning the real stack -- and, critically, so the THROW itself
     * (JS_ThrowError2 builds an Error + backtrace, ~2 MiB) has ample room below
     * the guard. 2 MiB limit -> the guard fires with ~6 MiB still free. */
    JS_SetMaxStackSize(g_rt, 2 * 1024 * 1024);
    JS_SetInterruptHandler(g_rt, slice_interrupt, 0);   /* the CPU-slice watchdog */
    g_ctx = JS_NewContext(g_rt);
    if (!g_ctx) { JS_FreeRuntime(g_rt); g_rt = 0; return 0; }

    g_t0 = now_ms();
    g_seq = 0; g_next_id = 1; g_next_raf_id = 1; g_frame_due = 0;
    g_cur_script = 0;

    JSValue g = JS_GetGlobalObject(g_ctx);
    JSValue con = JS_NewObject(g_ctx);
    JS_SetPropertyStr(g_ctx, con, "log",   JS_NewCFunction(g_ctx, con_out, "log", 1));
    JS_SetPropertyStr(g_ctx, con, "info",  JS_NewCFunction(g_ctx, con_out, "info", 1));
    JS_SetPropertyStr(g_ctx, con, "debug", JS_NewCFunction(g_ctx, con_out, "debug", 1));
    JS_SetPropertyStr(g_ctx, con, "warn",  JS_NewCFunction(g_ctx, con_warn, "warn", 1));
    JS_SetPropertyStr(g_ctx, con, "error", JS_NewCFunction(g_ctx, con_error, "error", 1));
    JS_SetPropertyStr(g_ctx, g, "console", con);
    JS_SetPropertyStr(g_ctx, g, "print", JS_NewCFunction(g_ctx, con_out, "print", 1));
    JS_SetPropertyStr(g_ctx, g, "alert", JS_NewCFunction(g_ctx, con_out, "alert", 1));

    JS_SetPropertyStr(g_ctx, g, "setTimeout",  JS_NewCFunction(g_ctx, js_setTimeout, "setTimeout", 2));
    JS_SetPropertyStr(g_ctx, g, "setInterval", JS_NewCFunction(g_ctx, js_setInterval, "setInterval", 2));
    JS_SetPropertyStr(g_ctx, g, "clearTimeout",  JS_NewCFunction(g_ctx, js_clearTimer, "clearTimeout", 1));
    JS_SetPropertyStr(g_ctx, g, "clearInterval", JS_NewCFunction(g_ctx, js_clearTimer, "clearInterval", 1));
    JS_SetPropertyStr(g_ctx, g, "requestAnimationFrame",
                      JS_NewCFunction(g_ctx, js_raf, "requestAnimationFrame", 1));
    JS_SetPropertyStr(g_ctx, g, "cancelAnimationFrame",
                      JS_NewCFunction(g_ctx, js_caf, "cancelAnimationFrame", 1));

    JSValue perf = JS_NewObject(g_ctx);
    JS_SetPropertyStr(g_ctx, perf, "now", JS_NewCFunction(g_ctx, js_perf_now, "now", 0));
    JS_SetPropertyStr(g_ctx, g, "performance", perf);

    /* The bridge js_platform.c turns into document.currentScript. Not a
     * property of `document` here because js_dom.c owns that object and
     * installs it below; see the comment above js_cur_script_node. */
    JS_SetPropertyStr(g_ctx, g, "__currentScriptNode",
                      JS_NewCFunction(g_ctx, js_cur_script_node, "__currentScriptNode", 0));

    /* `location` normally comes from js_webapi_install below, parsed into
     * components and writable. This href-only stand-in is what a build without
     * js_webapi.c gets -- the surface it had before -- so dropping that file
     * from a link takes away fetch, not the page's own address. */
    if (!LOGIT_HAVE(js_webapi_install)) {
        JSValue loc = JS_NewObject(g_ctx);
        JS_SetPropertyStr(g_ctx, loc, "href", JS_NewString(g_ctx, g_location));
        JS_SetPropertyStr(g_ctx, g, "location", loc);
    }
    JSValue nav = JS_NewObject(g_ctx);
    /* A Mozilla/5.0 prefix because a startling amount of shipped JS branches on
     * it before it will render anything at all. The rest is truthful. */
    JS_SetPropertyStr(g_ctx, nav, "userAgent",
                      JS_NewString(g_ctx, "Mozilla/5.0 (LogitOS; x86_64) Logit/1.0"));
    JS_SetPropertyStr(g_ctx, nav, "appName", JS_NewString(g_ctx, "Netscape"));
    JS_SetPropertyStr(g_ctx, nav, "platform", JS_NewString(g_ctx, "LogitOS x86_64"));
    JS_SetPropertyStr(g_ctx, nav, "language", JS_NewString(g_ctx, "en-US"));
    { JSValue langs = JS_NewArray(g_ctx);
      JS_SetPropertyUint32(g_ctx, langs, 0, JS_NewString(g_ctx, "en-US"));
      JS_SetPropertyStr(g_ctx, nav, "languages", langs); }
    JS_SetPropertyStr(g_ctx, nav, "onLine", JS_NewBool(g_ctx, 1));
    JS_SetPropertyStr(g_ctx, nav, "cookieEnabled", JS_NewBool(g_ctx, 0));   /* no jar on this path */
    JS_SetPropertyStr(g_ctx, nav, "hardwareConcurrency", JS_NewInt32(g_ctx, 1));
    JS_SetPropertyStr(g_ctx, nav, "maxTouchPoints", JS_NewInt32(g_ctx, 0));
    JS_SetPropertyStr(g_ctx, g, "navigator", nav);

    /* `window === globalThis`, so `window.foo = 1; foo` works and the mountain
     * of feature detection that starts with `typeof window` sees a browser. */
    JS_SetPropertyStr(g_ctx, g, "window", JS_DupValue(g_ctx, g));
    JS_SetPropertyStr(g_ctx, g, "self", JS_DupValue(g_ctx, g));

    js_dom_set_note(note);
    js_dom_init(g_ctx, root);            /* installs document + Element + Event */
    /* AFTER js_dom_init: no dependency on it (DOMParser's documents are
     * detached, never on `root`), placed here only to keep the DOM-adjacent
     * installers together rather than scattered above it. See js_domparser.c's
     * header for the lifetime scheme -- no close hook is registered because
     * none is needed (ordinary QuickJS object teardown is sufficient). */
    if (LOGIT_HAVE(js_domparser_install)) js_domparser_install(g_ctx);
    js_dom_bind_event_target(g_ctx, g);  /* window.addEventListener + window.on* */
    /* AFTER the DOM: js_webapi publishes document.location and dispatches
     * popstate through window.dispatchEvent, both of which js_dom.c owns. */
    if (LOGIT_HAVE(js_webapi_install)) js_webapi_install(g_ctx, g_location);
    /* LAST. js_platform.c fills gaps in what the two above publish (document,
     * navigator, performance, localStorage) and every one of its installs is
     * conditional on the property being absent -- which only means anything
     * once everyone who owns one has had their turn. */
    if (LOGIT_HAVE(js_select_install)) js_select_install(g_ctx);
    if (LOGIT_HAVE(js_intl_install)) js_intl_install(g_ctx);
    /* AFTER js_dom_init (it takes the Element prototype) and after the platform
     * fills in `document`; MediaSource has no dependency on either, but the
     * HTMLMediaElement members are installed on the element prototype. */
    if (LOGIT_HAVE(js_media_install)) js_media_install(g_ctx);
    if (LOGIT_HAVE(js_platform_install)) js_platform_install(g_ctx);
    /* AFTER js_platform_install: it is what creates `crypto` in the first
     * place (getRandomValues, randomUUID). js_subtle_install only fills in
     * `.subtle` on whatever `crypto` object already exists. */
    if (LOGIT_HAVE(js_subtle_install)) js_subtle_install(g_ctx);
    /* LAST of the last. The event layer needs js_dom.c's native Event classes
     * to wrap, js_webapi.c's AbortSignal for the `signal` option, and it
     * deliberately REPLACES two placeholders js_platform.c installs when
     * nobody better has (EventTarget, PromiseRejectionEvent) -- so unlike
     * js_platform.c's "only if absent" rule, this one has to run after the
     * placeholder exists in order to take it over. */
    if (LOGIT_HAVE(js_events_install)) js_events_install(g_ctx);
    /* AFTER js_events_install (needs the real, constructible G.EventTarget --
     * IDBRequest/IDBTransaction/IDBDatabase all extend it) and after
     * js_platform_install above (needs G.DOMException and G.structuredClone).
     * js_idb.c's own install guard re-checks all three and is a silent no-op
     * if any is missing, so this ordering is a courtesy, not a requirement --
     * see js_idb.c's header for the termination invariant every path here is
     * built around. Weak, like every install above: a build without that TU
     * keeps `typeof indexedDB === 'undefined'`, the correct feature-detect
     * answer for a browser that does not have it. */
    if (LOGIT_HAVE(js_idb_install)) js_idb_install(g_ctx);
    /* AFTER js_webapi_install (needs the real, singleton-bound G.fetch/
     * Request/Response/Headers -- js_cache.c is deliberately NOT a second
     * implementation of any of the four) and after js_platform_install
     * (needs G.DOMException). Weak like every install above: a build
     * without js_cache.c keeps `typeof caches === 'undefined'`. */
    if (LOGIT_HAVE(js_cache_install)) js_cache_install(g_ctx);
    /* AFTER js_platform_install (needs G.DOMException) and after `navigator`
     * already exists -- js_page.c creates that object directly, well before
     * this line, so the ordering requirement is trivially satisfied. Weak
     * like every install above: a build without js_swreg.c keeps
     * `'serviceWorker' in navigator === false`. See js_swreg.c's header for
     * why register() always rejects rather than half-executing. */
    if (LOGIT_HAVE(js_swreg_install)) js_swreg_install(g_ctx);
    /* AFTER all of the above, and the ordering is not a preference. js_cssom.c
     * takes the Element prototype js_dom.c published, and it deliberately
     * REPLACES two bindings older files install: getBoundingClientRect (its
     * version flushes a pending layout first) and matchMedia (its version is
     * the cascade's own media evaluator, which closes the divergence css.h
     * names). Installing it earlier means those two get overwritten again. */
    if (LOGIT_HAVE(js_cssom_install)) js_cssom_install(g_ctx);
    /* The form controls and the focus model -- js_forms.c: element.value,
     * .checked, .focus(), document.activeElement, form.submit(). LAST, and
     * "only if absent" like js_platform.c: every property it defines is one an
     * earlier file may already own, and a definition here that shadowed one of
     * theirs would turn a working property into undefined for EVERY element
     * rather than only for controls. Weak, like every other install above, so a
     * build without that object (the focus negative control, and the host tests
     * of this file) links and simply has no form bindings. */
    if (LOGIT_HAVE(js_forms_install)) js_forms_install(g_ctx);
    /* AFTER js_webapi_install, and that ordering is the point. js_webapi.c
     * ships a URL / URLSearchParams built as a JS prelude over
     * c/net/http/url.c -- the four-field parser the fetch needs, which has no
     * userinfo, no non-special scheme and no percent-encoding. js_url.c
     * REPLACES both globals with the standard's algorithm, and replacing
     * something means running after the thing that installed it. Weak, like
     * every install above, so a build without that TU keeps the old pair. */
    if (LOGIT_HAVE(js_url_install)) js_url_install(g_ctx);
    /* AFTER js_events_install (needs G.EventTarget/CloseEvent/MessageEvent)
     * and js_webapi_install (needs G.TextEncoder/TextDecoder); AFTER
     * js_url_install so a `new WebSocket(url)` validates its argument with
     * the real WHATWG URL parser rather than js_webapi.c's four-field one.
     * Weak, like every install above: a build without js_websocket.c keeps
     * `typeof WebSocket === 'undefined'`, the correct feature-detect answer
     * for a browser that does not have it. */
    if (LOGIT_HAVE(js_websocket_install)) js_websocket_install(g_ctx);
    /* AFTER js_platform_install and js_events_install: the parent-side event
     * delivery a Worker fires (`onerror`/`onmessage`) reaches for
     * G.DOMException / G.MessageEvent / G.ErrorEvent when they exist and
     * falls back to a plain object shape when they do not, so running after
     * them is strictly better and not a hard requirement -- see js_worker.h.
     * Weak like every install above: a build without js_worker.c keeps
     * `typeof Worker === 'undefined'`. */
    if (LOGIT_HAVE(js_worker_install)) js_worker_install(g_ctx);
    /* AFTER js_webapi_install, and that ordering is the one thing js_wasm.c
     * asks for: WebAssembly.instantiateStreaming takes a Response and reads it
     * with .arrayBuffer(), so it needs the real G.Response.  It degrades the
     * right way rather than half-installing -- with no Response the streaming
     * pair is simply not defined, which is the correct feature-detect answer,
     * while the rest of the namespace works.  ("Streaming" is also the honest
     * word for it: Response.body is undefined in this browser, so it awaits
     * the whole body, which the specification explicitly permits.)
     * Weak like every install above: a build without js_wasm.c keeps
     * `typeof WebAssembly === 'undefined'`, and that matters more here than
     * anywhere else in this list -- a page feature-tests the constructor and
     * then TRUSTS what it gets, so a half-built WebAssembly is worse than
     * none. */
    if (LOGIT_HAVE(js_wasm_install)) js_wasm_install(g_ctx);
    /* AFTER js_forms_install, and that ordering is load-bearing in one place:
     * js_forms.c installs focus()/blur() on HTMLInputElement.prototype only
     * (its `Object.getPrototypeOf(createElement('input'))` was the ONE shared
     * element prototype before 7fc2bec), and js_semantics.c copies that
     * descriptor up to HTMLElement.prototype so a <button> or <dialog> can be
     * focused. It can only copy a descriptor that already exists. */
    if (LOGIT_HAVE(js_semantics_install)) js_semantics_install(g_ctx);
    /* After the interface objects exist, because it asks for
     * HTMLCanvasElement.prototype BY NAME and installs nothing if it is
     * absent -- saying so out loud rather than leaving the page to rediscover
     * it as `getContext is not a function`. */
    if (LOGIT_HAVE(js_canvas_install)) js_canvas_install(g_ctx);
    /* LAST, after everyone who owns a piece of what it composes with has had
     * their turn, and the ordering is load-bearing twice over. It takes
     * Element.prototype, which js_dom.c publishes. And it REPLACES
     * window.getComputedStyle with a wrapper that delegates to whatever is
     * installed at the time -- so it has to run after js_cssom.c and
     * js_platform.c, or it wraps a placeholder and the real one overwrites the
     * wrapper afterwards, leaving animate() present and unobservable. */
    if (LOGIT_HAVE(js_anim_install)) js_anim_install(g_ctx);
    JS_FreeValue(g_ctx, g);
    return 1;
}

void js_page_close(void)
{
    if (!g_ctx) { g_rt = 0; return; }
    /* Order is load-bearing. Everything holding a JSValue must let go before
     * JS_FreeRuntime, which asserts on live GC objects -- and js_dom_cleanup
     * also clears the DOM's weak wrapper slots, so no node is left pointing at
     * a JSObject in a runtime that is about to stop existing.
     *
     * js_worker_close_all() FIRST, before anything else: every live worker
     * holds a JSValue reference INTO g_ctx (the Worker instance's own
     * __deliverMessage/__deliverError calls resolve back into this page's
     * global), so those references have to be released before this context
     * is torn down, not after. It also frees each worker's own JSRuntime --
     * a second live runtime outliving the page it belongs to is exactly the
     * kind of thing nothing downstream would notice until it crashed. */
    if (LOGIT_HAVE(js_worker_close_all)) js_worker_close_all();
    timers_clear(g_ctx);
    if (LOGIT_HAVE(js_platform_close)) js_platform_close(g_ctx);  /* unhooks the rejection tracker */
    if (LOGIT_HAVE(js_webapi_close)) js_webapi_close(g_ctx);   /* aborts fetches, drops promise resolvers */
    if (LOGIT_HAVE(js_websocket_close)) js_websocket_close(g_ctx); /* closes sockets, drops self refs */
    if (LOGIT_HAVE(js_media_close)) js_media_close(g_ctx);     /* stops playback, frees the DPBs */
    if (LOGIT_HAVE(js_cssom_close)) js_cssom_close(g_ctx);     /* drops the node lookup cache */
    js_dom_cleanup(g_ctx);
    js_dom_set_note(0);
    JS_FreeContext(g_ctx);
    JS_FreeRuntime(g_rt);
    g_ctx = 0; g_rt = 0;
}

int js_page_eval(const char *src, int len, const char *filename, struct node *node)
{
    if (!g_ctx || !src) return 0;
    js_prof_label(filename ? filename : "<page>");
    js_page_slice_begin();
    js_page_begin_script(node);
    JSValue v = JS_Eval(g_ctx, src, (size_t)len, filename ? filename : "<page>",
                        JS_EVAL_TYPE_GLOBAL);
    int ok = !JS_IsException(v);
    if (!ok) {
        JSValue e = JS_GetException(g_ctx);
        const char *m = JS_ToCString(g_ctx, e);
        printf("[browser] JS exception: %s\n", m ? m : "?");
        /* AND THE STACK, because the message alone is not a diagnosis.
         *
         * `TypeError: cannot read property 'charAt' of undefined` names the
         * operation and nothing else -- not the function, not the file, not
         * which of a bundle's thousands of call sites. Every real page that
         * failed today failed with a line of exactly that shape, and each one
         * cost a separate investigation to place.
         *
         * The stack was always here; this printed the message and dropped it.
         * That is the same defect the engine had until a QuickJS patch landed
         * today -- build_backtrace() emitted frames with no message header, so
         * every reporter on every page printed a stack with no error in it.
         * Fixing one half and leaving the other prints two useless halves.
         *
         * Defensive on purpose: a thrown value need not be an Error, `stack`
         * may be absent or a getter, and this runs while an exception is
         * already in flight -- so an accessor that threw would have nowhere to
         * go. Read it only when it is a plain string. */
        if (JS_IsObject(e)) {
            JSValue st = JS_GetPropertyStr(g_ctx, e, "stack");
            if (JS_IsString(st)) {
                const char *s = JS_ToCString(g_ctx, st);
                if (s && *s) printf("%s", s);
                if (s) JS_FreeCString(g_ctx, s);
            }
            JS_FreeValue(g_ctx, st);
        }
        note("[exception] "); if (m) note(m); note("\n");
        if (m) JS_FreeCString(g_ctx, m);
        JS_FreeValue(g_ctx, e);
    }
    JS_FreeValue(g_ctx, v);
    /* THE MICROTASK CHECKPOINT, AND IT RUNS WHILE currentScript IS STILL THIS
     * SCRIPT. That ordering is not a detail and it is not ours -- it is what
     * HTML says, and getting it backwards is what this file did until
     * 2026-08-29.
     *
     * "Execute the script element" sets document's currentScript to el, calls
     * "run a classic script", and only THEN restores the old value. The
     * microtask checkpoint lives inside "run a classic script" ("clean up
     * after running script": pop the execution context, and if the stack is
     * now empty, perform a microtask checkpoint) -- so it happens BEFORE the
     * restore. A promise reaction queued by a classic script therefore sees
     * that script as document.currentScript in every real browser.
     *
     * THE CORPUS SAYS SO, WHICH IS WORTH MORE THAN THE SPEC CITATION. Next.js's
     * turbopack runtime (tests/fixtures/frameworks/next, s011.js) registers a
     * chunk from an `async` function that AWAITS the sibling chunks and then
     * calls getAssetPrefix(), which is
     *
     *     let e = document.currentScript;
     *     if (!(e instanceof HTMLScriptElement)) throw new InvariantError(...)
     *
     * -- production code on every turbopack-built site on the web. It runs as
     * a promise reaction, never synchronously. If currentScript were null at a
     * post-script checkpoint, that invariant would fire on every Next.js page
     * load for everybody; it does not. It fired HERE, twice per run, and that
     * exception is what killed the client render.
     *
     * The restore is BELOW this line for exactly that reason. Note what does
     * NOT change: a setTimeout callback still sees null (js_page_run_due drains
     * jobs with no script in scope), and so does anything a later task queues
     * -- currentScript is bounded by the script's own checkpoint, not left
     * lying around. */
    js_dom_run_jobs(g_ctx);
    js_page_end_script();
    js_page_slice_end();
    return ok;
}
