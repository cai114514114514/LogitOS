/* The web platform outside the DOM tree and outside the network.
 *
 * See js_platform.h for what is here and, more importantly, WHY each thing is
 * here -- every entry below is either a name a page in tests/fixtures/webapi/
 * actually reached for (with the page named in the comment) or is marked as
 * requested-but-unmeasured. Nothing in this file is here because a browser is
 * "supposed to" have it.
 *
 * The shape follows js_webapi.c: one JS prelude evaluated as a function
 * expression so its C primitives arrive as ARGUMENTS rather than as globals a
 * page could reach or replace, plus the handful of things that genuinely need
 * C. Almost nothing does -- the only true C primitive here is entropy, because
 * script cannot produce any. */
#include "quickjs.h"
#include "js_platform.h"
#include "js_dom.h"        /* js_dom_has_activation -- the writeText activation gate below */
/* js_frame.c: the same-origin second browsing context an <iframe> below hands
 * its settled document to. OPTIONAL on purpose -- six host source lists name
 * js_platform.c and do not name js_frame.c (tests/domsub.mk, domiface.mk,
 * selectors.mk, webapi_platform.mk's PLATFORM_MOD, wpt.mk, iframe.mk), and a
 * hard reference here would break every one of them at link, which is exactly
 * the "hand-copied source lists" failure CLAUDE.md names. Same spelling
 * js_page.c uses for js_worker.h, including the weaksym.h Mach-O half. */
#define JS_FRAME_OPTIONAL
#include "js_frame.h"
#include "logit_abi.h"     /* CLIP_F_TEXT / CLIP_E_* -- needed in BOTH builds,
                             * see the clipboard section below for why the host
                             * stub still needs the real error codes. */
#include <string.h>
#include <stdlib.h>

int printf(const char *, ...);

static int g_vw = 980, g_vh = 600;
void js_platform_set_viewport(int w, int h) { if (w > 0) g_vw = w; if (h > 0) g_vh = h; }

/* ---- entropy ------------------------------------------------------------
 * crypto.getRandomValues IS NOW TRUE. This comment used to say, in capital
 * letters, that it was a lie: LogitOS had no entropy source a ring-3 process
 * could reach -- no /dev/urandom, no RDRAND wrapper, no kernel pool syscall --
 * so this was xorshift128+ seeded from the wall clock, the monotonic clock and
 * two heap addresses. Every CSRF token, UUID and WebCrypto shim on every page
 * was predictable from a handful of observations.
 *
 * SYS_GETRANDOM (include/abi/logit_abi.h) is that missing syscall: it is the
 * kernel's SHA-256 Hash_DRBG in c/kernel/core/rng.c, seeded from RDSEED/RDRAND
 * where the CPU has one, with state/output separation and periodic reseeding.
 * getrandom_bytes() in c/apps/logit.h loops over the per-call cap, so this path
 * either fills the whole buffer or fails.
 *
 * THE FALLBACK IS STILL HERE, and it is deliberate that it is now a FALLBACK
 * rather than the implementation: if the syscall ever fails (an older kernel, a
 * refused range) a page gets the old xorshift stream instead of an exception,
 * because a browser that throws out of getRandomValues does not render. The
 * distinction is observable -- __randomStrong() below reports which source is
 * live -- so "it silently degraded" is a thing a test can catch rather than a
 * thing someone discovers. */
#ifndef WEBAPI_HOST
#include "logit.h"
#else
/* Host test build (js_webapi.c's exact idiom, which this include missed when
 * the getrandom line landed -- it broke every test-platform* link with
 * "logit.h not found" until the webapi_platform units hit it): no kernel, so
 * the syscall "fails" and the code below takes the xorshift fallback it
 * already owns, reported honestly as __randomStrong()=0. */
static int getrandom_bytes(void *buf, unsigned long n) { (void)buf; (void)n; return -1; }
static int getrandom_strong(void) { return 1; } /* host OS entropy, below */
/* No kernel clipboard on the host either -- every host clipboard gate builds
 * its OWN clip_set/clip_get stub already (see webapi_probe.c); this one only
 * has to exist so js_platform.c links, and it always refuses so no host test
 * can mistake it for the real store. */
static int clip_set(int flavour, const void *buf, int len)
{ (void)flavour; (void)buf; (void)len; return CLIP_E_ARG; }
#endif
static unsigned long long g_s0, g_s1;
static int g_seeded;
/* -1 = no fill has happened yet, 1 = the last fill came from SYS_GETRANDOM,
 * 0 = it came from the xorshift fallback. Reported to script as
 * __randomStrong(), so a page (and tests/boot/run-entropy-test.sh) can tell
 * the two apart instead of trusting the name of the function. */
static int g_rng_kernel = -1;

static void rng_seed(unsigned long long clock_hint)
{
    void *a = malloc(1), *b = malloc(1);
    unsigned long long x = clock_hint * 0x9E3779B97F4A7C15ull;
    x ^= (unsigned long long)(unsigned long)a * 0xBF58476D1CE4E5B9ull;
    x ^= (unsigned long long)(unsigned long)b << 17;
    x ^= (unsigned long long)(unsigned long)&g_s0;
    free(a); free(b);
    /* splitmix64 twice, so a low-entropy seed still fills both words. */
    for (int i = 0; i < 2; i++) {
        x += 0x9E3779B97F4A7C15ull;
        unsigned long long z = x;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        if (i == 0) g_s0 = z | 1; else g_s1 = z | 1;
    }
    g_seeded = 1;
}

static unsigned long long rng_next(void)
{
    unsigned long long s1 = g_s0, s0 = g_s1;
    g_s0 = s0;
    s1 ^= s1 << 23;
    g_s1 = s1 ^ s0 ^ (s1 >> 18) ^ (s0 >> 5);
    return g_s1 + s0;
}

/* __random(n, clockHint) -> ArrayBuffer of n random bytes. Bounded at the
 * spec's own 65536-byte limit for getRandomValues, so a page cannot ask for a
 * gigabyte. */
#include "web_entropy.h"
static JSValue js_random(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    int32_t n = 0;
    double hint = 0;
    if (argc > 0) JS_ToInt32(ctx, &n, argv[0]);
    if (argc > 1) JS_ToFloat64(ctx, &hint, argv[1]);
    if (n < 0 || n > 65536) return JS_ThrowRangeError(ctx,"random request exceeds quota");
    unsigned char *buf = malloc((size_t)n + 1);
    if (!buf) return JS_ThrowOutOfMemory(ctx);
    /* Correction (2026-09-10): the historical fallback above is retired.
     * Clock-seeded output must not become a key or nonce on an error path. */
    if (web_entropy(buf,n) != 0) {
        g_rng_kernel=0; memset(buf,0,(size_t)n);free(buf);
        return JS_ThrowTypeError(ctx,"strong entropy unavailable");
    }
    g_rng_kernel=1;
    JSValue ab = JS_NewArrayBufferCopy(ctx, buf, (size_t)n);
    free(buf);
    return ab;
}

/* __randomStrong() -> which source the last fill actually used.
 *   -1 nothing generated yet
 *    0 the xorshift fallback ran (the syscall refused)
 *    1 SYS_GETRANDOM, DRBG seeded from rdtsc only (no RDSEED/RDRAND on this CPU)
 *    2 SYS_GETRANDOM, DRBG backed by a hardware entropy source
 * This exists so "it silently fell back" is observable. A page will not read
 * it; tests/boot/run-entropy-test.sh does, and so does anyone wondering
 * whether the name on the tin is true today. */
static JSValue js_random_strong(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t; (void)argc; (void)argv;
    if (g_rng_kernel <= 0) return JS_NewInt32(ctx, g_rng_kernel);
    return JS_NewInt32(ctx, getrandom_strong() ? 2 : 1);
}

/* ---- clipboard: writeText only -------------------------------------------
 *
 * MEASURED (188 bundles, 22 saved pages): 6 real writeText call sites in 5
 * bundles (baidu, kimi x2, nodejs, deepseek), ZERO readText/read() sites.
 * navigator.clipboard.readText and .write([ClipboardItem]) are therefore NOT
 * built -- readText because nothing asks for it and it is the one place a
 * page could read what the user copied out of a DIFFERENT application, and
 * write()/ClipboardItem because its one measured caller (kimi/s005.js)
 * already falls back to writeText in a catch. See the JS prelude below for
 * both refusals and why readText REJECTS rather than resolving "".
 *
 * __clipWriteText(s) -> the bytes stored, or a negative CLIP_E_* (see
 * include/abi/logit_abi.h). JS_ToCStringLen hands back the string's UTF-8
 * bytes directly -- clipboard.c validates them again on the way in, so a
 * lone surrogate the engine could not represent as UTF-8 is caught by the
 * SAME validator the keyboard shortcut and the address bar already go
 * through (clip_set_common, one call site for all three). There is
 * deliberately no second validator here: one jar, one door, this time by
 * construction rather than by discipline. */
static JSValue js_clip_write_text(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    /* THE GATE, AND IT IS IN C RATHER THAN IN THE PRELUDE ON PURPOSE. The
     * prelude below is ordinary JavaScript on the page's own global: a page
     * can replace navigator.clipboard, or reach the bound function through
     * any reference it kept, and be talking to this binding directly. A check
     * written up there guards the door and leaves the wall open. This is the
     * only place the syscall can be reached from, so this is where the
     * question has to be asked.
     *
     * WHAT IT PREVENTS, measured rather than imagined: without it any page
     * could call writeText from a timer and overwrite the clipboard EVERY
     * PROCESS ON THIS MACHINE reads -- whatever the user had copied out of
     * Terminal or TextEdit, replaced silently, with the browser in the
     * background. That shipped, and an adversarial review of the diff caught
     * it; no gate did, because none existed.
     *
     * CLIP_E_ARG rather than a new code: every measured call site treats a
     * negative return as failure and the prelude collapses everything but
     * CLIP_E_TOOBIG into one DOMException already. A page that is refused
     * gets the same rejected promise it gets when the clipboard is full,
     * which is a state its .catch() is already written for -- as opposed to a
     * resolved promise over a write that did not happen, which is the lie
     * this whole file's rule forbids. */
    if (!js_dom_has_activation()) return JS_NewInt32(ctx, CLIP_E_ARG);
    size_t len = 0;
    const char *s = argc > 0 ? JS_ToCStringLen(ctx, &len, argv[0]) : "";
    if (!s) return JS_NewInt32(ctx, CLIP_E_ARG);
    int r = clip_set(CLIP_F_TEXT, s, (int)len);
    if (argc > 0) JS_FreeCString(ctx, s);
    return JS_NewInt32(ctx, r);
}

/* ---- unhandled promise rejections --------------------------------------
 * QuickJS reports these through a runtime hook rather than as an event, so the
 * bridge has to be in C. A page that installs window.onunhandledrejection
 * (bing does) gets called; a page that does not gets the message in the console
 * instead of silence, which is the difference between debugging a page and
 * guessing at it. */
static JSValue g_reject_hook = JS_UNDEFINED;
static JSValue g_parsed_hook = JS_UNDEFINED;
static JSContext *g_ctx;
static int g_legacy_frames=1;
static struct js_platform_context *platform_current(void);

#include "js_rejections.inc"
#include "js_native_mo.inc"

/* ---- the prelude -------------------------------------------------------- */
#include "js_bootstrap_scan.inc"

extern int passive_frames_enabled(void) LOGIT_WEAK;
LOGIT_WEAK_STUB(passive_frames_enabled);
extern JSValue passive_frame_window(JSContext *,JSValueConst,int,JSValueConst *) LOGIT_WEAK;
LOGIT_WEAK_STUB(passive_frame_window);
static const char *PLATFORM_PRELUDE =
"(function (__random, __vw, __vh, __randomStrong, __clipWriteText, __nativeMOFlush, __initialIframes, __passiveFrames, __embeddedWindow, __legacyMarkup) {\n"
"'use strict';\n"
"var G = globalThis;\n"
/* The house rule for this whole file. Three lines are adding to this runtime
   at once; whoever got there first keeps it. */
"function def(o, k, v) { if (o && !(k in o)) { try { o[k] = v; } catch (e) {} } }\n"
"function defOwn(o, k, get) {\n"
"  if (!o || (k in o)) return;\n"
"  try { Object.defineProperty(o, k, { get: get, configurable: true }); } catch (e) {}\n"
"}\n"

/* ---- WRAPPING A DOM METHOD, and why it may not be done on "the element
 * prototype" any more.
 *
 * Until js_dom.c grew a real interface hierarchy there WAS one shared element
 * prototype, and `Object.getPrototypeOf(document.createElement('div'))` was the
 * only handle on it. It is now HTMLDivElement.prototype -- the bottom of a
 * chain -- and a wrapper installed there is an own property of <div> and of
 * nothing else. js_dom_iface.inc's iface_bridge() exists to rescue exactly
 * that: it relocates own properties from HTMLDivElement.prototype onto
 * Element.prototype after the installers have run.
 *
 * IT CANNOT RESCUE A WRAPPER OF A METHOD ELEMENT.PROTOTYPE ALREADY OWNS.
 * move_own_props moves only what the destination does not already have -- it
 * must, or it would clobber js_dom.c's own members with a probe element's
 * copies -- so the moved property is DROPPED when the name collides. And the
 * names that collide are precisely the ones worth wrapping:
 *
 *   appendChild  is owned by Node.prototype, so Element.prototype has no own
 *                copy, so the wrapper moved and childList records worked.
 *   setAttribute is owned by ELEMENT.prototype. The wrapper was dropped on
 *                the floor, silently, and MutationObserver produced no
 *                attribute record for anything -- for setAttribute, for
 *                className, and for classList.add/remove/replace, which
 *                js_tokenlist.c deliberately routes through setAttribute so
 *                the records could not disagree. The API constructed, observed
 *                and reported nothing; on a real page a render loop waiting on
 *                one never advances.
 *
 * So: find the prototype that OWNS the method and wrap it there. That is right
 * under any hierarchy, it needs no bridge, and it puts each wrapper where the
 * standard puts the method -- appendChild on Node.prototype (so a Text node's
 * insertion is seen too), setAttribute on Element.prototype.
 *
 * `tag` namespaces the once-only guard: two features here wrap the same
 * methods and both must get their turn, so the guard is per feature AND per
 * method rather than one flag on the object. Assignment rather than
 * defineProperty is deliberate -- writing to an existing own property keeps
 * its enumerability, and these are all non-enumerable natives. */
"function ownerOf(name) {\n"
"  var p = null;\n"
"  try { p = Object.getPrototypeOf(G.document.createElement('div')); } catch (e) { return null; }\n"
"  while (p) {\n"
"    if (Object.prototype.hasOwnProperty.call(p, name)) return p;\n"
"    p = Object.getPrototypeOf(p);\n"
"  }\n"
"  return null;\n"
"}\n"
"function wrapMethod(tag, name, make) {\n"
"  var P = ownerOf(name);\n"
"  if (!P) return;\n"
"  var orig = P[name];\n"
"  if (typeof orig !== 'function') return;\n"
"  var key = '__w_' + tag + '_' + name;\n"
"  if (P[key]) return;\n"
"  try {\n"
"    Object.defineProperty(P, key, { value: true, enumerable: false, configurable: true });\n"
"    P[name] = make(orig, name);\n"
"  } catch (e) {}\n"
"}\n"

/* ==== performance ========================================================
 * MEASURED: performance.mark on bing and wikipedia (2 of 7 pages);
 * performance.timing on bing, where <inline 8> -- its whole client-telemetry
 * bundle -- terminates on `performance.timing.navigationStart` and takes the
 * rest of the script with it.
 *
 * The User Timing half (mark/measure/getEntries*) is a real store, not stubs:
 * a page that marks and then measures gets the right duration out, because
 * pages BRANCH on these (bing's perf module compares durations to decide
 * whether to send a beacon) and a zero would be a different branch.
 *
 * performance.timing is the DEPRECATED Navigation Timing Level 1 interface,
 * and it is here for exactly the reason it is deprecated: everything shipped
 * before ~2019 still reads it. Its fields are wall-clock milliseconds, unlike
 * everything else here, which is why they are computed from timeOrigin. The
 * values we cannot honestly produce (domainLookupStart, connectEnd) are set to
 * navigationStart rather than to 0 -- a 0 means "did not happen" to these
 * pages, and produces negative durations that then get reported as errors. */
"(function () {\n"
"  var perf = G.performance;\n"
"  if (!perf) { perf = {}; G.performance = perf; }\n"
"  if (typeof perf.now !== 'function') perf.now = function () { return 0; };\n"
"  var origin = Date.now() - perf.now();\n"
"  def(perf, 'timeOrigin', origin);\n"
"  var entries = [];\n"
"  var PE = function PerformanceEntry() {};\n"
"  if (!G.PerformanceEntry) def(G, 'PerformanceEntry', PE);\n"
"  function ent(name, type, start, dur) {\n"
"    var e = Object.create(PE.prototype);\n"
"    e.name = String(name); e.entryType = type; e.startTime = start; e.duration = dur;\n"
"    e.toJSON = function () { return { name: this.name, entryType: this.entryType,\n"
"                                       startTime: this.startTime, duration: this.duration }; };\n"
"    return e;\n"
"  }\n"
   /* ==== PerformanceObserver ==============================================
    * MEASURED IN THE GUEST (rule 1 -- the host probe does not reach this):
    * tests/scoreboard/full-corpus/bing.serial.txt records a REAL
    * ReferenceError -- `PerformanceObserver` is not defined -- inside
    * bing.com's own inline script, un-guarded by any typeof check:
    *   `PerformanceObserver.supportedEntryTypes.indexOf("element")!==-1 &&
    *    (t=new PerformanceObserver(...), t.observe({type:"element",...}))`
    * The throw does not just skip that one line -- it takes the REST of the
    * enclosing IIFE with it: `window.iotdLiteCleanup` (removes the
    * image-of-the-day placeholder), a 2s fallback timer, and `c(2)` (the
    * indexedDB fetch that actually loads the image). Recorded as one of
    * bing's exactly 2 JS exceptions in the committed scoreboard baseline.
    *
    * SUPPORTED_TYPES IS THE ONLY LIST -- this is the ONE JAR, and
    * supportedEntryTypes plus the observe() filter below both read it, so
    * they cannot drift apart the way CLAUDE.md's cookie/ARG_MAX examples did.
    * It holds exactly 'mark' and 'measure' because those are the only entry
    * types this machine genuinely produces (the User Timing store two lines
    * up is real, not a stub) -- NOT 'resource', 'element', 'longtask',
    * 'paint', 'largest-contentful-paint' or 'navigation'. Every one of those
    * six is reached for by a page in the corpus (apple: resource,
    * bing-search/bing: element, jd/doubao's Slardar SDK + kimi's web-vitals:
    * longtask) and every one of those reaches has an honest false branch that
    * already does the right thing without ever constructing an observer --
    * listing the type here would silently break that branch by promising
    * entries that will never arrive. jd/doubao's SDK guards on the SEPARATE
    * global `PerformanceLongTaskTiming`, which this file does not define, so
    * their observer stays correctly unconstructed; deliberately NOT closing
    * that second door is what keeps them off. */
"  var SUPPORTED_TYPES = ['mark', 'measure'];\n"
"  var observers = [];\n"
"  var flushScheduled = false;\n"
"  function poelFor(list) {\n"
"    return { getEntries: function () { return list.slice(); },\n"
"             getEntriesByType: function (t) { return list.filter(function (x) { return x.entryType === t; }); },\n"
"             getEntriesByName: function (n, t) { return list.filter(function (x) { return x.name === n && (!t || x.entryType === t); }); } };\n"
"  }\n"
"  function scheduleFlush() {\n"
"    if (flushScheduled) return;\n"
"    flushScheduled = true;\n"
"    Promise.resolve().then(flushObservers);\n"
"  }\n"
   /* One flush per microtask tick, over EVERY observer with something pending,
    * so several marks/measures made in one script turn deliver as ONE
    * callback with all of them -- not one callback per entry, which is not
    * what the spec does and not what a page's own batching logic expects. */
"  function flushObservers() {\n"
"    flushScheduled = false;\n"
"    for (var i = 0; i < observers.length; i++) {\n"
"      var o = observers[i];\n"
"      if (!o._pending.length) continue;\n"
"      var list = o._pending; o._pending = [];\n"
"      try { o._cb(poelFor(list), o); } catch (e) { G.reportError(e); }\n"
"    }\n"
"  }\n"
"  function notifyObservers(e) {\n"
"    for (var i = 0; i < observers.length; i++) {\n"
"      var o = observers[i];\n"
"      if (o._types[e.entryType]) { o._pending.push(e); scheduleFlush(); }\n"
"    }\n"
"  }\n"
"  function PerformanceObserver(cb) {\n"
"    if (typeof cb !== 'function') throw new TypeError('PerformanceObserver requires a callback');\n"
"    this._cb = cb; this._types = {}; this._pending = []; this._active = false;\n"
"  }\n"
"  Object.defineProperty(PerformanceObserver, 'supportedEntryTypes',\n"
"    { value: Object.freeze(SUPPORTED_TYPES.slice()), enumerable: true });\n"
   /* observe() naming an unsupported type is a SILENT NO-OP, never a throw --
    * this is what bing's typeof-guarded 'resource'/'element' reaches and
    * kimi's TTI path all depend on: they check supportedEntryTypes THEMSELVES
    * before observing, and the ones that do not (bing's un-guarded 'element'
    * call above) must not be handed a second exception in place of the first
    * one this file exists to remove. */
"  PerformanceObserver.prototype.observe = function (opts) {\n"
"    opts = opts || {};\n"
"    var types = opts.entryTypes ? opts.entryTypes.slice() : (opts.type ? [opts.type] : null);\n"
"    if (!types) throw new TypeError(\"observe() requires 'type' or 'entryTypes'\");\n"
"    for (var i = 0; i < types.length; i++) {\n"
"      if (SUPPORTED_TYPES.indexOf(types[i]) < 0) continue;\n"
"      this._types[types[i]] = true;\n"
"    }\n"
"    if (!this._active) { observers.push(this); this._active = true; }\n"
"    if (opts.buffered) {\n"
"      var self = this;\n"
"      entries.forEach(function (e) { if (self._types[e.entryType]) self._pending.push(e); });\n"
"      if (self._pending.length) scheduleFlush();\n"
"    }\n"
"  };\n"
"  PerformanceObserver.prototype.disconnect = function () {\n"
"    var i = observers.indexOf(this);\n"
"    if (i >= 0) observers.splice(i, 1);\n"
"    this._active = false; this._types = {}; this._pending = [];\n"
"  };\n"
"  PerformanceObserver.prototype.takeRecords = function () {\n"
"    var r = this._pending; this._pending = []; return r;\n"
"  };\n"
#ifndef PLATFORM_NO_PERFORMANCE_OBSERVER
"  if (!G.PerformanceObserver) def(G, 'PerformanceObserver', PerformanceObserver);\n"
#endif
"  def(perf, 'mark', function (name, opts) {\n"
"    var t = (opts && typeof opts.startTime === 'number') ? opts.startTime : perf.now();\n"
"    var e = ent(name, 'mark', t, 0);\n"
"    if (opts && opts.detail !== undefined) e.detail = opts.detail;\n"
"    entries.push(e); notifyObservers(e); return e;\n"
"  });\n"
   /* User Timing L2, "convert a name to a timestamp": a name that is not a
      user mark is looked up in the PerformanceTiming interface BEFORE it is
      rejected. Only a name in neither is a SyntaxError.
    *
    * MEASURED on stripe.com, whose instrumentation calls
    * `performance.measure(x, 'navigationStart')` -- the most common form of
    * this call, because it is how a bundle measures anything against the
    * start of navigation. We threw `SyntaxError: mark 'navigationStart' does
    * not exist`, twice per load, from inside the Next.js hydration path.
    * perf.timing already carried the attribute; markTime never looked.
    *
    * timing is absolute wall-clock ms and a mark is relative to timeOrigin,
    * so the conversion is the subtraction. */
"  function markTime(n) {\n"
"    for (var i = entries.length - 1; i >= 0; i--)\n"
"      if (entries[i].entryType === 'mark' && entries[i].name === n) return entries[i].startTime;\n"
#ifndef PLATFORM_NO_TIMING_MARKS
"    var tim = perf.timing;\n"
"    if (tim && typeof tim[n] === 'number' && typeof tim.navigationStart === 'number')\n"
"      return tim[n] - tim.navigationStart;\n"
#endif
"    return null;\n"
"  }\n"
"  def(perf, 'measure', function (name, a, b) {\n"
"    var s = 0, e = perf.now();\n"
"    if (typeof a === 'string') { var m = markTime(a); if (m === null) throw new SyntaxError(\"mark '\" + a + \"' does not exist\"); s = m; }\n"
"    else if (a && typeof a === 'object') {\n"
"      if (typeof a.start === 'number') s = a.start;\n"
"      else if (typeof a.start === 'string') { var ms = markTime(a.start); if (ms !== null) s = ms; }\n"
"      if (typeof a.end === 'number') e = a.end;\n"
"      else if (typeof a.end === 'string') { var me = markTime(a.end); if (me !== null) e = me; }\n"
"      if (typeof a.duration === 'number' && typeof a.end !== 'number') e = s + a.duration;\n"
"    }\n"
"    if (typeof b === 'string') { var m2 = markTime(b); if (m2 === null) throw new SyntaxError(\"mark '\" + b + \"' does not exist\"); e = m2; }\n"
"    var r = ent(name, 'measure', s, e - s);\n"
"    entries.push(r); notifyObservers(r); return r;\n"
"  });\n"
"  def(perf, 'getEntries', function () { return entries.slice(); });\n"
"  def(perf, 'getEntriesByName', function (n, t) {\n"
"    return entries.filter(function (e) { return e.name === n && (!t || e.entryType === t); });\n"
"  });\n"
"  def(perf, 'getEntriesByType', function (t) {\n"
"    return entries.filter(function (e) { return e.entryType === t; });\n"
"  });\n"
"  def(perf, 'clearMarks', function (n) {\n"
"    entries = entries.filter(function (e) { return e.entryType !== 'mark' || (n !== undefined && e.name !== n); });\n"
"  });\n"
"  def(perf, 'clearMeasures', function (n) {\n"
"    entries = entries.filter(function (e) { return e.entryType !== 'measure' || (n !== undefined && e.name !== n); });\n"
"  });\n"
"  def(perf, 'clearResourceTimings', function () {});\n"
"  def(perf, 'setResourceTimingBufferSize', function () {});\n"
"  def(perf, 'toJSON', function () { return { timeOrigin: origin }; });\n"
   /* Navigation Timing 1. Absolute wall-clock ms; see the comment above on why
      the unknown phases are navigationStart and not 0. */
"  if (!perf.timing) {\n"
"    var t0 = Math.round(origin), tim = {};\n"
"    var same = ['navigationStart', 'fetchStart', 'domainLookupStart', 'domainLookupEnd',\n"
"                'connectStart', 'connectEnd', 'secureConnectionStart', 'requestStart',\n"
"                'responseStart', 'unloadEventStart', 'unloadEventEnd', 'redirectStart',\n"
"                'redirectEnd'];\n"
"    for (var i = 0; i < same.length; i++) tim[same[i]] = t0;\n"
"    var later = ['responseEnd', 'domLoading', 'domInteractive', 'domContentLoadedEventStart',\n"
"                 'domContentLoadedEventEnd', 'domComplete', 'loadEventStart', 'loadEventEnd'];\n"
"    for (var j = 0; j < later.length; j++) tim[later[j]] = t0;\n"
"    tim.toJSON = function () { var o = {}; for (var k in this) if (typeof this[k] === 'number') o[k] = this[k]; return o; };\n"
"    perf.timing = tim;\n"
       /* The lifecycle fills these in as it happens, so a page that subtracts
          domContentLoadedEventEnd - navigationStart gets a real number. */
"    G.__platMarkTiming = function (k) { if (tim[k] === t0 || tim[k] === undefined) tim[k] = Date.now(); };\n"
"  } else { G.__platMarkTiming = function () {}; }\n"
"  def(perf, 'navigation', { type: 0, redirectCount: 0, TYPE_NAVIGATE: 0, TYPE_RELOAD: 1,\n"
"                            TYPE_BACK_FORWARD: 2, TYPE_RESERVED: 255 });\n"
"})();\n"

/* ==== document lifecycle =================================================
 * MEASURED: document.readyState on bing and deepseek (2 of 7 pages) -- the
 * most-wanted single property in the corpus.
 *
 * It is a real state machine, not the constant 'complete' that would be the
 * cheap answer. A page that reads 'complete' during head parsing skips its own
 * DOMContentLoaded registration and then never initialises, which is a worse
 * failure than the one it replaces: silent instead of loud. browser.c already
 * dispatches DOMContentLoaded and load on the document, so the transitions
 * hang off those. Correction: waiting for DOMContentLoaded to become
 * interactive is too late for defer/module bodies. The loader now calls the
 * private parser-boundary hook before those bodies; DCL remains a fallback for
 * older embedders that only dispatch lifecycle events. */
"var documentParsed = function () {};\n"
"(function () {\n"
"  var doc = G.document;\n"
"  if (!doc) return;\n"
"  var state = 'loading';\n"
"  defOwn(doc, 'readyState', function () { return state; });\n"
"  defOwn(doc, 'visibilityState', function () { return 'visible'; });\n"
"  defOwn(doc, 'hidden', function () { return false; });\n"
"  if (!('readyState' in doc)) return;\n"     /* defineProperty refused: leave it alone */
"  function change(s) {\n"
"    if (state === s) return;\n"
"    state = s;\n"
"    try {\n"
"      var e = new Event('readystatechange');\n"
"      if (doc.dispatchEvent) doc.dispatchEvent(e);\n"
"    } catch (x) {}\n"
"  }\n"
"  documentParsed = function () {\n"
"    if (state !== 'loading') return;\n"
"    G.__platMarkTiming('domInteractive');\n"
"    change('interactive');\n"
"  };\n"
"  try {\n"
"    doc.addEventListener('DOMContentLoaded', function () {\n"
"      documentParsed();\n"
"      G.__platMarkTiming('domContentLoadedEventStart');\n"
"      G.__platMarkTiming('domContentLoadedEventEnd');\n"
"    });\n"
"    doc.addEventListener('load', function () {\n"
"      G.__platMarkTiming('domComplete');\n"
"      G.__platMarkTiming('loadEventStart');\n"
"      change('complete');\n"
"      G.__platMarkTiming('loadEventEnd');\n"
"    });\n"
"  } catch (x) {}\n"
"})();\n"

/* ==== task and microtask queues ==========================================
 * MEASURED: queueMicrotask, postMessage and MessageChannel on deepseek --
 * which is React, and React's scheduler picks exactly one of MessageChannel /
 * setImmediate / setTimeout at module load. With none of the first two it falls
 * back to setTimeout, so this is not the difference between working and not;
 * it is the difference between a 0 ms task and a 4 ms clamp on every unit of
 * work React does, which on a page of any size is the whole frame budget.
 *
 * MessagePort delivery goes through setTimeout(0) because that is the only
 * macrotask source this runtime has. The ORDER is still right -- ports deliver
 * FIFO, and after the microtask queue -- which is the part scheduler code
 * depends on. */
"def(G, 'queueMicrotask', function (fn) {\n"
"  if (typeof fn !== 'function') throw new TypeError('queueMicrotask requires a function');\n"
"  Promise.resolve().then(function () { fn(); });\n"
"});\n"
"def(G, 'reportError', function (e) {\n"
"  try { console.error(e && e.stack ? e.stack : String(e)); } catch (x) {}\n"
"});\n"

"if (!G.MessageEvent) {\n"
"  G.MessageEvent = function MessageEvent(type, init) {\n"
"    init = init || {};\n"
"    var ev = new G.Event(String(type), init);\n"
"    Object.setPrototypeOf(ev, G.MessageEvent.prototype);\n"
"    Object.defineProperties(ev, {data:{value:init.data,enumerable:true},\n"
"      origin:{value:String(init.origin || ''),enumerable:true},\n"
"      lastEventId:{value:String(init.lastEventId || ''),enumerable:true},\n"
"      source:{value:init.source || null,enumerable:true},\n"
"      ports:{value:init.ports || [],enumerable:true}});\n"
"    return ev;\n"
"  };\n"
"  G.MessageEvent.prototype = Object.create(G.Event.prototype);\n"
"  Object.defineProperty(G.MessageEvent.prototype,'constructor',{value:G.MessageEvent,writable:true,configurable:true});\n"
"}\n"
#include "js_message_port.inc"
/* ==== BroadcastChannel ==================================================
 * MEASURED, 2026-08-30, in the guest, on the z.ai specimen (the zaiblank
 * package): the page's 3.2 MB entry module rejects at evaluation with
 *   [browser] module rejected .../index-B9hfiqvt.js:
 *     ReferenceError: 'BroadcastChannel' is not defined
 * and the SPA never mounts -- the whole BLANK. The exact usage in the
 * bundle, verbatim:
 *   const l = new BroadcastChannel("active-tab-channel"), ...
 *   l.onmessage = O => { O.data === "active" && kP.set(!1) };
 *   const w = () => { document.visibilityState === "visible" &&
 *                     (kP.set(!0), l.postMessage("active")) };
 * -- the multi-tab "which tab is active" lock idiom (post "active" on
 * visibilitychange; peers stand down). Any SPA that runs this idiom at
 * module init dies on statement one without this class.
 *
 * WHY THIS IS HERE AND NOT ON THE DELIBERATELY-ABSENT LIST: the API is
 * small, spec'd (HTML "The BroadcastChannel interface"), and builds on the
 * two things this file already owns -- MessageEvent and structuredClone.
 * "Absent beats present-and-wrong" applies when the wrong half is what a
 * stub would ship; everything below is implementable exactly, so absence
 * was costing a whole page for nothing. The doubao/weixin BLANK siblings
 * were checked and do NOT reference BroadcastChannel in the bundles their
 * shells ship today -- this fix is general in engine vocabulary (any page
 * using the idiom), not "the fix for the class".
 *
 * SEMANTICS, and the two that are choices rather than quotes:
 *   - Delivery targets are the OTHER same-name channels in THIS context.
 *     The spec broadcasts across an agent cluster; with one browsing
 *     context, the cluster's BroadcastChannel population IS this context's
 *     (two same-document channels of one name DO exchange messages in real
 *     browsers -- the cluster is not per-document). A lone channel's
 *     postMessage therefore delivers nowhere and throws nothing, which is
 *     exactly what a one-tab "active-tab" lock expects: no peers, no event.
 *   - The clone happens at CALL time, one independent copy per receiver,
 *     so a clone failure (function in the payload) throws out of
 *     postMessage itself -- same rule as window.postMessage above, for the
 *     same reason: a deferred clone failure is an unhandledrejection the
 *     page cannot attribute to anything.
 *   - Delivery is a setTimeout(0) task, like MessagePort's, because that
 *     is the only macrotask source this runtime has; FIFO after the
 *     microtask queue is the part message code depends on.
 *   - close() is idempotent and postMessage after close is a SILENT no-op
 *     (spec: "closed flag" then "return") -- a browser does not throw
 *     there, and code that posts after closing is common enough that
 *     throwing here would be a present-and-wrong of our own manufacture.
 *
 * WHAT IS DELIBERATELY NOT HERE: the worker global (js_worker.c's second
 * runtime has no BroadcastChannel -- `typeof BroadcastChannel ===
 * 'undefined'` inside a Worker is the correct feature-detect answer there,
 * same rule as indexedDB in that file's header, because a per-worker
 * channel registry that silently did not reach the page's channels would
 * be present-and-wrong); onmessageerror (structuredClone never fails
 * asynchronously here -- there is no deserialization step to fail late);
 * and EventTarget inheritance (addEventListener('message') is handled the
 * same way Port does it, which is observationally identical for this API:
 * 'message' is its only event). */
#ifndef ZAIBLANK_BC_ABSENT
/* ZAIBLANK_BC_ABSENT: test-zaiblank-bc-negctl's control build, the same idiom
 * as JS_DOCWRITE_NO_INSTALL / JS_IFRAME_NO_INSTALL further down. Compiling
 * the block out reproduces the pre-feature engine exactly -- BroadcastChannel
 * absent, everything else identical -- so the gates are known to measure THIS
 * feature and not some other reason a mount happens to succeed. */
"if (!G.BroadcastChannel) {\n"
"  var BC_REGISTRY = [];   /* live channels, registration order */\n"
"  G.BroadcastChannel = function BroadcastChannel(name) {\n"
"    if (arguments.length < 1)\n"
"      throw new TypeError(\"Failed to execute 'BroadcastChannel': 1 argument required, but only 0 present.\");\n"
"    this._name = String(name);\n"
"    this._l = []; this.onmessage = null; this._closed = false;\n"
"    BC_REGISTRY.push(this);\n"
"  };\n"
"  Object.defineProperty(G.BroadcastChannel.prototype, 'name', {\n"
"    configurable: true, enumerable: true,\n"
"    get: function () { return this._name; }\n"
"  });\n"
"  G.BroadcastChannel.prototype.addEventListener = function (t, f) {\n"
"    if (t === 'message' && typeof f === 'function' && this._l.indexOf(f) < 0) this._l.push(f);\n"
"  };\n"
"  G.BroadcastChannel.prototype.removeEventListener = function (t, f) {\n"
"    if (t !== 'message') return;\n"
"    var i = this._l.indexOf(f); if (i >= 0) this._l.splice(i, 1);\n"
"  };\n"
"  G.BroadcastChannel.prototype.postMessage = function (message) {\n"
"    if (this._closed) return;\n"
/* One clone per receiver, made NOW -- see the header. The sender's own
   object is never handed to a receiver, so a page that posts and mutates
   cannot observe its own mutation coming back. */
"    var peers = BC_REGISTRY.filter(function (c) {\n"
"      return c !== this && !c._closed && c._name === this._name;\n"
"    }, this);\n"
"    var cloned = peers.map(function () { return G.structuredClone(message); });\n"
"    var org = (G.location && G.location.origin) || '';\n"
"    var self = this;\n"
"    setTimeout(function () {\n"
"      for (var i = 0; i < peers.length; i++) {\n"
"        var c = peers[i];\n"
"        if (c._closed) continue;   /* closed between post and delivery */\n"
"        var ev = new G.MessageEvent('message', { data: cloned[i], origin: org, source: null });\n"
"        if (typeof c.onmessage === 'function')\n"
"          { try { c.onmessage(ev); } catch (e) { G.reportError(e); } }\n"
"        c._l.slice().forEach(function (f) { try { f(ev); } catch (e) { G.reportError(e); } });\n"
"      }\n"
"    }, 0);\n"
"  };\n"
"  G.BroadcastChannel.prototype.close = function () {\n"
"    if (this._closed) return;\n"
"    this._closed = true; this._l = []; this.onmessage = null;\n"
"    var i = BC_REGISTRY.indexOf(this); if (i >= 0) BC_REGISTRY.splice(i, 1);\n"
"  };\n"
"}\n"
#endif /* !ZAIBLANK_BC_ABSENT */
/* window.postMessage to ourselves. One window, so the only meaningful TARGET
 * is this one -- there is no second browsing context a message could reach
 * (js_dom.c's document/context statics are a hard singleton; see the comment
 * over the <iframe> feature below, and js_dom.c:4364's unguarded
 * JS_NewClassID for the measurement that makes a second live document unsafe
 * to build today). That is the one thing this function still cannot do.
 *
 * What it got WRONG even for a single window, fixed here: it accepted any
 * second argument and ignored it, and it delivered `data` BY REFERENCE. Both
 * are the same bug from two directions -- a page that relies on
 * postMessage's origin check (`event.origin === expectedOrigin`) to decide
 * whether to trust a message was, on this engine, never actually gated by
 * anything, and a page that mutates the object it just posted (a very common
 * pattern -- fire-and-forget, then reuse the buffer) would see that mutation
 * on the "received" side too, because there was only ever one object.
 *
 * TARGETORIGIN IS CHECKED FOR REAL, even with one window: 'the right target
 * window' collapses to G, but 'does the caller's asserted origin match', the
 * other half of the contract, does not collapse to a no-op just because
 * there is nowhere else to check it against -- skipping it would be the
 * exact silent-wrong-answer shape this whole file argues against elsewhere.
 * A mismatched, syntactically valid targetOrigin means the message is
 * silently NOT delivered (spec-correct: this is not a throw), watched by
 * test-platform's negative case rather than merely asserted.
 *
 * THE CLONE IS REAL AND SYNCHRONOUS. `G.structuredClone` (below in this same
 * file) is reused rather than a second clone implementation -- it is already
 * the one cycle-safe, Map/Set/TypedArray-aware clone this runtime has, and it
 * already throws DataCloneError exactly where the spec wants (functions,
 * symbols). The clone runs at CALL time, synchronously, so a clone failure
 * throws out of the postMessage() call itself -- not out of the deferred
 * task, where nothing could ever catch it and a page would see an
 * unhandledrejection-shaped mystery instead of the exception it threw.
 *
 * THERE ARE TWO OVERLOADS AND THIS FUNCTION KNEW ONLY ONE, WHICH IS WHY IT
 * THREW ON A CORRECT CALL. MEASURED IN THE GUEST, 2026-08-30, on
 * google.com/search?q=python -- the only JS exception on the page:
 *
 *   [browser] JS exception: SyntaxError: Failed to execute 'postMessage':
 *             Invalid target origin '[object Object]' in a call to 'postMessage'.
 *
 * `[object Object]` is `String({...})`. The caller had passed the MODERN form.
 * The IDL is two overloads, not one:
 *
 *   undefined postMessage(any message, USVString targetOrigin,
 *                         optional sequence<object> transfer = []);
 *   undefined postMessage(any message,
 *                         optional WindowPostMessageOptions options = {});
 *
 * so `postMessage(m)`, `postMessage(m, null)` and `postMessage(m, {targetOrigin:
 * '*'})` are all valid and all worked nowhere here: the first two died on the
 * "2 arguments required" TypeError this commit deletes, the third on the
 * SyntaxError above. Web IDL's overload resolution on argument 1 is the rule
 * implemented below and it is three lines: null/undefined -> dictionary, any
 * other object (including a function) -> dictionary, anything else -> USVString.
 * The dictionary's `targetOrigin` DEFAULTS TO '/' (same origin), which is why
 * the one-argument form delivers rather than throwing.
 *
 * AND THIS IS THE CLASS, NOT THE INSTANCE. `typeof window.postMessage ===
 * 'function'` answered true the whole time; a page feature-tests presence, gets
 * a truthy function, calls it the way every current browser accepts, and gets
 * an exception. Absence would have been survivable -- the page takes its
 * fallback. Present with a contract narrower than the one it advertises is not.
 * tests/fixtures/jstype/postmessage.html is the gate, host and guest.
 *
 * WHAT IS STILL NOT HERE, NAMED RATHER THAN FAKED: `transfer`. There is one
 * browsing context, so there is nowhere to transfer TO, and this runtime has no
 * detachable objects; the message is CLONED in every form. A page that posts
 * `{transfer: [buf]}` and then expects `buf.byteLength === 0` sees a length it
 * would not see in a browser. That is a behaviour difference and it is written
 * down here rather than papered over -- accepting the option and silently not
 * transferring is the lesser of the two wrongs only because the alternative
 * (throwing) breaks the call outright, which is the bug this comment opens on. */
"def(G, 'postMessage', function (data, targetOrigin) {\n"
"  var org = (G.location && G.location.origin) || '';\n"
"  if (arguments.length < 1) throw new TypeError(\"Failed to execute 'postMessage': 1 argument required, but only 0 present.\");\n"
"  var to;\n"
"  if (targetOrigin === null || targetOrigin === undefined) {\n"
"    to = '/';\n"
"  } else if (typeof targetOrigin === 'object' || typeof targetOrigin === 'function') {\n"
"    var _t = targetOrigin.targetOrigin;\n"
"    to = (_t === undefined) ? '/' : String(_t);\n"
"  } else {\n"
"    to = String(targetOrigin);\n"
"  }\n"
"  if (to !== '*' && to !== '/') {\n"
"    var validOrigin = false;\n"
"    try { validOrigin = (new G.URL(to)).origin === to; } catch (e) { validOrigin = false; }\n"
"    if (!validOrigin) throw new G.DOMException(\"Failed to execute 'postMessage': Invalid target origin '\" + to + \"' in a call to 'postMessage'.\", 'SyntaxError');\n"
"  }\n"
"  var cloned = G.structuredClone(data);\n"
"  if (to !== '*' && to !== '/' && to !== org) return;\n"
"  setTimeout(function () {\n"
"    var ev = new G.MessageEvent('message', { data: cloned, origin: org, source: G });\n"
"    if (typeof G.onmessage === 'function') { try { G.onmessage(ev); } catch (e) { G.reportError(e); } }\n"
"    try { if (G.dispatchEvent) G.dispatchEvent(ev); } catch (e) {}\n"
"  }, 0);\n"
"});\n"

/* MEASURED on bing: window.top (2 references) and window.parent. There is no
 * frame tree here -- no <iframe> support at all -- so a document is always the
 * top of its own. Pointing both at the window is not a stub: it is the correct
 * answer for an unframed document, and it is what `if (window.top !== window)
 * top.location = self.location` (bing's frame-buster, and half the web's) tests
 * for. Left undefined, that line throws and takes the script with it. */
"def(G, 'top', G);\n"
"def(G, 'parent', G);\n"
"def(G, 'frameElement', null);\n"
"def(G, 'frames', G);\n"
"def(G, 'length', 0);\n"
"def(G, 'closed', false);\n"
"def(G, 'name', '');\n"
"def(G, 'status', '');\n"
"def(G, 'isSecureContext', true);\n"

/* requestIdleCallback. MEASURED on wikipedia (window.requestIdleCallback), and
 * it is also on the owner's own list. There is no idle detection in this event
 * loop, so the callback is a timer and the deadline it reports is honest about
 * that: timeRemaining() returns a real countdown from a 50 ms budget, and
 * didTimeout is true when the page's own timeout forced the run. A page that
 * loops `while (deadline.timeRemaining() > 0)` therefore terminates, which is
 * the property that matters -- a constant 50 would hang the browser. */
"if (!G.requestIdleCallback) {\n"
"  G.requestIdleCallback = function (fn, opts) {\n"
"    var timeout = opts && opts.timeout;\n"
"    var delay = timeout ? Math.min(timeout, 50) : 1;\n"
"    return setTimeout(function () {\n"
"      var start = performance.now();\n"
"      fn({ didTimeout: !!timeout,\n"
"           timeRemaining: function () { return Math.max(0, 50 - (performance.now() - start)); } });\n"
"    }, delay);\n"
"  };\n"
"  G.cancelIdleCallback = function (id) { clearTimeout(id); };\n"
"}\n"

/* ==== errors =============================================================
 * MEASURED: window.DOMException on deepseek. It is what every abort and every
 * refused API throws, and `e instanceof DOMException` is how a page tells "the
 * user cancelled" from "the code is broken". js_webapi.c's AbortController and
 * its mkError() (the hook fetch_fail calls from C) both build AbortError /
 * NetworkError / TimeoutError through G.DOMException now, not a plain Error
 * wearing a `.name` property that only looked like one -- they used to fall
 * back to Error for want of this class, and the fallback is gone. */
"if (!G.DOMException) {\n"
"  var DE = function DOMException(message, name) {\n"
"    var e = Error.call(this, message);\n"
"    this.message = message === undefined ? '' : String(message);\n"
"    this.name = name === undefined ? 'Error' : String(name);\n"
"    if (e.stack) this.stack = e.stack;\n"
"  };\n"
"  DE.prototype = Object.create(Error.prototype);\n"
"  DE.prototype.constructor = DE;\n"
"  var codes = { IndexSizeError: 1, HierarchyRequestError: 3, WrongDocumentError: 4,\n"
"                InvalidCharacterError: 5, NotFoundError: 8, NotSupportedError: 9,\n"
"                InvalidStateError: 11, SyntaxError: 12, InvalidModificationError: 13,\n"
"                NamespaceError: 14, InvalidAccessError: 15, SecurityError: 18,\n"
"                NetworkError: 19, AbortError: 20, QuotaExceededError: 22,\n"
"                TimeoutError: 23, DataCloneError: 25 };\n"
"  Object.defineProperty(DE.prototype, 'code', { configurable: true,\n"
"    get: function () { return codes[this.name] || 0; } });\n"
"  for (var cn in codes) DE[cn] = codes[cn];\n"
"  G.DOMException = DE;\n"
"}\n"
/* PromiseRejectionEvent + the unhandledrejection path. MEASURED: deepseek asks
   for the constructor, bing assigns window.onunhandledrejection. The bridge
   from QuickJS's runtime-level tracker is the C hook installed below. */
"if (!G.PromiseRejectionEvent) {\n"
"  G.PromiseRejectionEvent = function PromiseRejectionEvent(type, init) {\n"
"    init = init || {};\n"
"    var ev = new G.Event(type, {bubbles:!!init.bubbles,cancelable:!!init.cancelable});\n"
"    Object.setPrototypeOf(ev, G.PromiseRejectionEvent.prototype);\n"
"    Object.defineProperties(ev, {promise:{value:init.promise,enumerable:true},reason:{value:init.reason,enumerable:true}});\n"
"    return ev;\n"
"  };\n"
"  G.PromiseRejectionEvent.prototype = Object.create(G.Event.prototype);\n"
"  G.PromiseRejectionEvent.prototype.constructor = G.PromiseRejectionEvent;\n"
"}\n"

/* ==== Storage named properties ==========================================
 * MEASURED on bing: `localStorage.eventLogQueue_Offline`. Storage is a legacy
 * platform object with named properties, so `localStorage.foo` is
 * `localStorage.getItem('foo')` and assigning to it stores. Pages written
 * before 2015 -- and bing's telemetry queue is one -- use only that form, and
 * against our object every read was undefined and every write vanished into a
 * JS property nothing persists.
 *
 * A Proxy rather than a C exotic handler, because the store is js_webapi.c's
 * and reaching into it from here would give one key two owners. Methods are
 * bound to the real object: its C implementations read an opaque pointer off
 * `this`, and a Proxy is not that pointer. */
"(function () {\n"
"  function wrapStorage(name) {\n"
"    var s = G[name];\n"
"    if (!s || typeof s.getItem !== 'function' || s.__named) return;\n"
"    var proxy = new Proxy(s, {\n"
"      get: function (t, k) {\n"
"        if (typeof k === 'symbol' || k === '__named') return Reflect.get(t, k, t);\n"
"        if (k in t) { var v = Reflect.get(t, k, t); return typeof v === 'function' ? v.bind(t) : v; }\n"
"        return t.getItem(k) === null ? undefined : t.getItem(k);\n"
"      },\n"
"      set: function (t, k, v) {\n"
"        if (typeof k === 'symbol' || (k in t)) { try { t[k] = v; } catch (e) {} return true; }\n"
"        t.setItem(k, v); return true;\n"
"      },\n"
"      has: function (t, k) {\n"
"        if (typeof k === 'symbol' || (k in t)) return true;\n"
"        return t.getItem(k) !== null;\n"
"      },\n"
"      deleteProperty: function (t, k) { t.removeItem(k); return true; },\n"
"      ownKeys: function (t) {\n"
"        var out = [];\n"
"        for (var i = 0; i < t.length; i++) { var k = t.key(i); if (k !== null) out.push(k); }\n"
"        return out;\n"
"      },\n"
"      getOwnPropertyDescriptor: function (t, k) {\n"
"        if (typeof k !== 'symbol' && t.getItem(k) !== null)\n"
"          return { value: t.getItem(k), writable: true, enumerable: true, configurable: true };\n"
"        return Reflect.getOwnPropertyDescriptor(t, k);\n"
"      }\n"
"    });\n"
"    try { s.__named = true; } catch (e) {}\n"
"    try { G[name] = proxy; } catch (e) {}\n"
"  }\n"
"  wrapStorage('localStorage');\n"
"  wrapStorage('sessionStorage');\n"
"})();\n"

/* ==== navigator gaps =====================================================
 * MEASURED: navigator.scheduling on deepseek -- React calls
 * navigator.scheduling.isInputPending() to decide whether to yield mid-render.
 * We have no input queue to inspect from here, and `false` is the answer that
 * keeps React rendering rather than yielding for ever. */
"(function () {\n"
"  var nav = G.navigator;\n"
"  if (!nav) return;\n"
"  def(nav, 'scheduling', { isInputPending: function () { return false; } });\n"
"  def(nav, 'doNotTrack', null);\n"
"  def(nav, 'webdriver', false);\n"
"  def(nav, 'sendBeacon', function () { return false; });\n"   /* honest: we send nothing */
"  def(nav, 'vendor', '');\n"
   /* There is no Java runtime or Java plug-in in this browser. This query is
    * an actual capability answer, like the empty installed-plugin list. */
"  def(nav, 'javaEnabled', function () { return false; });\n"
"  def(nav, 'product', 'Gecko');\n"
   /* navigator.mimeTypes / navigator.plugins.
    *
    * MEASURED, and it is the LAST uncaught exception on the real baidu page
    * once jQuery boots -- s006.js line 2128, inside baidu's browser sniffer:
    *
    *     !function () { if (navigator.mimeTypes.length > 0) { ... } }
    *
    * `cannot read property 'length' of undefined`. Chrome does not throw it.
    *
    * EMPTY IS THE TRUTHFUL ANSWER AND IS NOT A STUB. The question these two
    * collections answer is "which plugins are installed", and the answer here
    * is none -- so a page that tests `.length > 0` takes the no-plugin branch,
    * which is the branch that is correct for this browser. That is the
    * opposite of the crypto.subtle case: there, a stub would return something
    * a page believes is encryption; here, the empty collection IS the fact.
    * (Chrome ships a hardcoded five-entry PDF list for compatibility. Copying
    * that would be claiming a PDF plugin we do not have.) */
"  var emptyColl = function () {\n"
"    var c = [];\n"
"    c.item = function (i) { return this[i] || null; };\n"
"    c.namedItem = function () { return null; };\n"
"    c.refresh = function () {};\n"
"    return c;\n"
"  };\n"
"  def(nav, 'mimeTypes', emptyColl());\n"
"  def(nav, 'plugins', emptyColl());\n"
"})();\n"

/* ==== navigator.clipboard ================================================
 * MEASURED (188 bundles + 22 saved pages, tests/scoreboard/full-corpus): 6
 * real writeText call sites in 5 bundles -- baidu (also jsperf/baidu-async-
 * search.js), kimi s005 + s047 (Lexical), nodejs s017, deepseek s010. Every
 * one is a feature test with a document.execCommand('copy') fallback, but
 * that fallback is ALSO absent on this browser (js_forms.c's EDIT_CMDS never
 * registers 'copy'), so before this the measured behaviour split three ways:
 * baidu shows the user a literal "复制失败，请重试" error, nodejs/kimi leave
 * a Copy button that never flips to Copied, and kimi's Lexical site (whose
 * call is NOT inside a try) throws an uncaught TypeError because the
 * property access on undefined `navigator.clipboard` fails synchronously,
 * before .catch ever attaches.
 *
 * writeText -> the real store. SYS_CLIP_SET is already live in ring 3
 * (browser.c's own Ctrl+C/Ctrl+V on the address bar, psel_copy() on page
 * text) and /bin/clip is a second process that already reads it back, so
 * this is a fourth caller of a path with three working ones, not a new
 * capability.
 *
 * readText / read() are PRESENT AND REJECT, always, with a real
 * DOMException. Zero pages in the corpus call either, so refusing costs no
 * measured path, and rejecting is what a real browser does without a
 * permission grant -- every measured .catch() already handles it. This
 * MUST NOT resolve '' or []: a page that gets '' believes the clipboard is
 * genuinely empty (this item's own precedent, tests/fixtures/jsperf/baidu-
 * async-search.js's `getContext === i ? !1 : ...`, is exactly a plausible
 * wrong value walking a page further than absence would). THIS BROWSER
 * NEVER LETS A PAGE READ WHAT THE USER COPIED FROM ANOTHER APPLICATION --
 * there is no gesture or prompt that turns it on, because there is no UI on
 * this machine to ask the question with, and a permission prompt nobody can
 * answer is a control that cannot be watched failing (CLAUDE.md rule 5).
 *
 * write([ClipboardItem]) is left ABSENT, not stubbed. Its one measured
 * caller (kimi/s005.js's copyTextAndHtmlToClipboard) already catches and
 * falls back to writeText -- the working path -- and building it would mean
 * inventing a CLIP_F_HTML producer for a flavour the ABI declares and
 * nothing on this machine fills (logit_abi.h: "only CLIP_F_TEXT has a
 * producer today"), i.e. a brand-new category-(b) subsystem to serve a
 * call site that is already served. */
/* JS_CLIPBOARD_NO_INSTALL: the negative control, same shape as
 * JS_IFRAME_NO_INSTALL above -- compiling the installer out entirely (rather
 * than an in-page runtime flag) is what proves qmp_clipboard_js.py measures
 * THIS feature and not some other reason a page's writeText call happened to
 * resolve. See tests/clip.mk's test-clip-js-negctl. */
#ifndef JS_CLIPBOARD_NO_INSTALL
"(function () {\n"
"  var nav = G.navigator;\n"
"  if (!nav) return;\n"
"  var deny = function () {\n"
"    return Promise.reject(new G.DOMException(\n"
"      'Reading the clipboard is not permitted on this browser.', 'NotAllowedError'));\n"
"  };\n"
"  def(nav, 'clipboard', {\n"
"    writeText: function (text) {\n"
"      var s;\n"
"      try { s = String(text); }\n"
"      catch (e) { return Promise.reject(e); }\n"
"      return new Promise(function (resolve, reject) {\n"
"        var r = __clipWriteText(s);\n"
"        if (r >= 0) { resolve(); return; }\n"
          /* CLIP_E_TOOBIG (-2) is the one refusal a page's own error message
           * can be specific about without inventing anything: the store said
           * no, not "something went wrong". Every other code (-1 bad arg,
           * -3 no kernel memory, -5 not valid UTF-8) collapses to the same
           * NotAllowedError every measured .catch() already treats as
           * failure. */
"        var msg = (r === -2)\n"
"          ? 'The text is too large for the clipboard.'\n"
"          : 'Writing to the clipboard failed.';\n"
"        reject(new G.DOMException(msg, 'NotAllowedError'));\n"
"      });\n"
"    },\n"
"    readText: deny,\n"
"    read: deny\n"
"  });\n"
"})();\n"
#endif

/* ==== crypto =============================================================
 * REQUESTED, NOT MEASURED: no page in the corpus reached crypto.getRandomValues
 * -- they all died first. It is here because it was on the owner's list and
 * because a page that calls it and gets nothing throws immediately.
 * See the C comment on rng_seed for why the name overstates what this is. */
"(function () {\n"
"  var c = G.crypto;\n"
"  if (!c) { c = {}; try { G.crypto = c; } catch (e) { return; } }\n"
"  def(c, 'getRandomValues', function (view) {\n"
"    if (!view || view.BYTES_PER_ELEMENT === undefined)\n"
"      throw new TypeError('getRandomValues requires an integer TypedArray');\n"
"    if (view instanceof Float32Array || view instanceof Float64Array)\n"
"      throw new TypeError('getRandomValues does not accept a float array');\n"
"    if(view.byteLength>65536) throw new G.DOMException('random request exceeds quota','QuotaExceededError');\n"
"    var bytes;try{bytes=new Uint8Array(__random(view.byteLength,0))}catch(e){throw new G.DOMException('strong entropy unavailable','OperationError')}\n"
"    var dst = new Uint8Array(view.buffer, view.byteOffset, view.byteLength);\n"
"    dst.set(bytes);\n"
"    return view;\n"
"  });\n"
/* Which source the last getRandomValues actually used. Not a web API -- it is
   the observable that keeps "getRandomValues is real now" from being a claim
   nobody can check. See js_random_strong() in this file. */
"  def(G, '__logitEntropySource', __randomStrong);\n"
"  def(c, 'randomUUID', function () {\n"
"    var b = new Uint8Array(__random(16, Date.now()));\n"
"    b[6] = (b[6] & 0x0f) | 0x40; b[8] = (b[8] & 0x3f) | 0x80;\n"
"    var h = [];\n"
"    for (var i = 0; i < 16; i++) h.push((b[i] + 0x100).toString(16).slice(1));\n"
"    return h.slice(0, 4).join('') + '-' + h.slice(4, 6).join('') + '-' +\n"
"           h.slice(6, 8).join('') + '-' + h.slice(8, 10).join('') + '-' + h.slice(10).join('');\n"
"  });\n"
   /* crypto.subtle is deliberately NOT defined. It is the one place where a
      stub is worse than absence: a page that finds it assumes real WebCrypto
      and will encrypt something with whatever we return. */
"})();\n"

/* ==== base64 =============================================================
 * MEASURED, AND ONLY ON THE MACHINE. btoa does not appear in the host probe's
 * table at all, because the probe evaluates scripts and stops; bing reaches
 * btoa from a setTimeout callback, which only runs once there is an event loop
 * turning. tests/qmp/qmp_bing.py found it in the serial log as
 * `[js] uncaught in timer: ReferenceError: 'btoa' is not defined`, and the
 * probe now pumps its timers for exactly that reason. A host instrument that
 * does not run the event loop cannot see a third of what a page does.
 *
 * These are the LATIN-1 pair, not the UTF-8 one: btoa throws on any code unit
 * above 255, which is the behaviour every `btoa(unescape(encodeURIComponent(s)))`
 * idiom on the web is written around. Encoding UTF-8 silently instead would
 * make those idioms produce double-encoded output that decodes to mojibake. */
"(function () {\n"
"  var T = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';\n"
"  def(G, 'btoa', function (s) {\n"
"    s = String(s);\n"
"    var out = '', i, a, b, c;\n"
"    for (i = 0; i < s.length; i++)\n"
"      if (s.charCodeAt(i) > 255)\n"
"        throw new G.DOMException('The string contains characters outside of the "
"Latin1 range.', 'InvalidCharacterError');\n"
"    for (i = 0; i < s.length; i += 3) {\n"
"      a = s.charCodeAt(i);\n"
"      b = i + 1 < s.length ? s.charCodeAt(i + 1) : 0;\n"
"      c = i + 2 < s.length ? s.charCodeAt(i + 2) : 0;\n"
"      var n = (a << 16) | (b << 8) | c;\n"
"      out += T[(n >> 18) & 63] + T[(n >> 12) & 63];\n"
"      out += i + 1 < s.length ? T[(n >> 6) & 63] : '=';\n"
"      out += i + 2 < s.length ? T[n & 63] : '=';\n"
"    }\n"
"    return out;\n"
"  });\n"
"  def(G, 'atob', function (s) {\n"
"    s = String(s).replace(/[ \\t\\n\\f\\r]/g, '');\n"
"    if (s.length % 4 === 0) s = s.replace(/==?$/, '');\n"
"    if (s.length % 4 === 1 || /[^A-Za-z0-9+/]/.test(s))\n"
"      throw new G.DOMException('The string to be decoded is not correctly encoded.',\n"
"                               'InvalidCharacterError');\n"
"    var out = '', bits = 0, n = 0;\n"
"    for (var i = 0; i < s.length; i++) {\n"
"      n = (n << 6) | T.indexOf(s[i]); bits += 6;\n"
"      if (bits >= 8) { bits -= 8; out += String.fromCharCode((n >> bits) & 255); }\n"
"    }\n"
"    return out;\n"
"  });\n"
"})();\n"

/* ==== structuredClone ====================================================
 * REQUESTED, NOT MEASURED. A real deep clone with cycle handling, because the
 * one-line JSON round trip that usually stands in for it silently drops Map,
 * Set, Date, TypedArray and every cycle, and a page that clones its state with
 * it gets corrupted state rather than an error. Functions throw
 * DataCloneError, as the spec says -- that is how a page finds its own bug. */
"def(G, 'structuredClone', function (v) {\n"
"  var seen = new Map();\n"
"  function cl(x) {\n"
"    if (x === null || typeof x !== 'object') {\n"
"      if (typeof x === 'function') throw new G.DOMException('could not be cloned', 'DataCloneError');\n"
"      if (typeof x === 'symbol') throw new G.DOMException('could not be cloned', 'DataCloneError');\n"
"      return x;\n"
"    }\n"
"    if (seen.has(x)) return seen.get(x);\n"
"    if ((portIs && portIs(x)) || (G.MessagePort && x instanceof G.MessagePort) || (G.Node && x instanceof G.Node) ||\n"
"        (typeof Promise!=='undefined' && x instanceof Promise) ||\n"
"        (typeof WeakMap!=='undefined' && x instanceof WeakMap) ||\n"
"        (typeof WeakSet!=='undefined' && x instanceof WeakSet) ||\n"
"        (typeof WeakRef!=='undefined' && x instanceof WeakRef) ||\n"
"        (typeof SharedArrayBuffer!=='undefined' && x instanceof SharedArrayBuffer))\n"
"      throw new G.DOMException('could not be cloned','DataCloneError');\n"
"    var out;\n"
"    if (x instanceof Date) { out = new Date(x.getTime()); seen.set(x, out); return out; }\n"
"    if (x instanceof RegExp) { out = new RegExp(x.source, x.flags); seen.set(x, out); return out; }\n"
"    if (x instanceof ArrayBuffer) { out = x.slice(0); seen.set(x, out); return out; }\n"
"    if (ArrayBuffer.isView(x)) {\n"
"      out = new x.constructor(cl(x.buffer), x.byteOffset, x.length !== undefined ? x.length : undefined);\n"
"      seen.set(x, out); return out;\n"
"    }\n"
"    if (x instanceof Map) { out = new Map(); seen.set(x, out);\n"
"      x.forEach(function (v2, k2) { out.set(cl(k2), cl(v2)); }); return out; }\n"
"    if (x instanceof Set) { out = new Set(); seen.set(x, out);\n"
"      x.forEach(function (v2) { out.add(cl(v2)); }); return out; }\n"
"    if (Array.isArray(x)) { out = new Array(x.length); seen.set(x, out);\n"
"      for (var i = 0; i < x.length; i++) if (i in x) out[i] = cl(x[i]); return out; }\n"
"    if (x instanceof Error) { out = new x.constructor(x.message); seen.set(x, out);\n"
"      out.name = x.name; return out; }\n"
"    out = {}; seen.set(x, out);\n"
"    var ks = Object.keys(x);\n"
"    for (var j = 0; j < ks.length; j++) Object.defineProperty(out,ks[j],{value:cl(x[ks[j]]),writable:true,enumerable:true,configurable:true});\n"
"    return out;\n"
"  }\n"
"  return cl(v);\n"
"});\n"
"portClone=G.structuredClone;\n"

/* ==== Blob / File / FormData ============================================
 * REQUESTED, NOT MEASURED. Blob stores its parts as bytes rather than as the
 * strings it was given, because the moment a page does `new Blob([u8])` and
 * asks for .size, a string-backed Blob answers with a character count and every
 * upload boundary computed from it is wrong.
 *
 * FormData's iteration order is insertion order INCLUDING duplicates -- a form
 * with three checkboxes of the same name is the case that made the spec say so,
 * and a Map-backed implementation loses two of them. */
"(function () {\n"
"  function enc(s) {\n"
"    if (G.TextEncoder) return new G.TextEncoder().encode(s);\n"
"    var out = [], i, c;\n"
"    for (i = 0; i < s.length; i++) {\n"
"      c = s.charCodeAt(i);\n"
"      if (c < 0x80) out.push(c);\n"
"      else if (c < 0x800) { out.push(0xc0 | (c >> 6), 0x80 | (c & 63)); }\n"
"      else { out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63)); }\n"
"    }\n"
"    return new Uint8Array(out);\n"
"  }\n"
"  function dec(u8) {\n"
"    if (G.TextDecoder) return new G.TextDecoder().decode(u8);\n"
"    var s = '';\n"
"    for (var i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]);\n"
"    return s;\n"
"  }\n"
"  if (!G.Blob) {\n"
"    var Blob = function Blob(parts, opts) {\n"
"      var chunks = [], total = 0, i;\n"
"      parts = parts || [];\n"
"      for (i = 0; i < parts.length; i++) {\n"
"        var p = parts[i], u;\n"
"        if (p instanceof Blob) u = p._b;\n"
"        else if (p instanceof ArrayBuffer) u = new Uint8Array(p.slice(0));\n"
"        else if (ArrayBuffer.isView(p)) u = new Uint8Array(p.buffer.slice(p.byteOffset, p.byteOffset + p.byteLength));\n"
"        else u = enc(String(p));\n"
"        chunks.push(u); total += u.length;\n"
"      }\n"
"      var all = new Uint8Array(total), o = 0;\n"
"      for (i = 0; i < chunks.length; i++) { all.set(chunks[i], o); o += chunks[i].length; }\n"
"      this._b = all;\n"
"      this.size = total;\n"
"      this.type = (opts && opts.type) ? String(opts.type).toLowerCase() : '';\n"
"    };\n"
"    Blob.prototype = {\n"
"      constructor: Blob,\n"
"      slice: function (s, e, t) {\n"
"        var b = new Blob([], { type: t || '' });\n"
"        b._b = this._b.slice(s === undefined ? 0 : s, e === undefined ? this._b.length : e);\n"
"        b.size = b._b.length;\n"
"        return b;\n"
"      },\n"
"      text: function () { var self = this; return Promise.resolve().then(function () { return dec(self._b); }); },\n"
"      arrayBuffer: function () { var self = this; return Promise.resolve().then(function () { return self._b.buffer.slice(self._b.byteOffset, self._b.byteOffset + self._b.length); }); },\n"
"      bytes: function () { var self = this; return Promise.resolve().then(function () { return self._b.slice(); }); }\n"
"    };\n"
"    G.Blob = Blob;\n"
"    G.File = function File(parts, name, opts) {\n"
"      G.Blob.call(this, parts, opts);\n"
"      this.name = String(name);\n"
"      this.lastModified = (opts && opts.lastModified) || Date.now();\n"
"    };\n"
"    G.File.prototype = Object.create(Blob.prototype);\n"
"    G.File.prototype.constructor = G.File;\n"
"  }\n"
"  if (!G.FormData) {\n"
"    var FD = function FormData() { this._e = []; };\n"
"    FD.prototype = {\n"
"      constructor: FD,\n"
"      append: function (n, v, fn) { this._e.push([String(n), (v instanceof G.Blob) ? v : String(v), fn]); },\n"
"      set: function (n, v, fn) {\n"
"        var k = String(n), done = false;\n"
"        this._e = this._e.filter(function (p) {\n"
"          if (p[0] !== k) return true;\n"
"          if (done) return false;\n"
"          done = true; p[1] = (v instanceof G.Blob) ? v : String(v); p[2] = fn; return true;\n"
"        });\n"
"        if (!done) this.append(n, v, fn);\n"
"      },\n"
"      get: function (n) { var k = String(n);\n"
"        for (var i = 0; i < this._e.length; i++) if (this._e[i][0] === k) return this._e[i][1];\n"
"        return null; },\n"
"      getAll: function (n) { var k = String(n);\n"
"        return this._e.filter(function (p) { return p[0] === k; }).map(function (p) { return p[1]; }); },\n"
"      has: function (n) { var k = String(n);\n"
"        return this._e.some(function (p) { return p[0] === k; }); },\n"
"      delete: function (n) { var k = String(n);\n"
"        this._e = this._e.filter(function (p) { return p[0] !== k; }); },\n"
"      forEach: function (fn, t) { this._e.slice().forEach(function (p) { fn.call(t, p[1], p[0], this); }, this); },\n"
"      keys: function () { return this._e.map(function (p) { return p[0]; })[Symbol.iterator](); },\n"
"      values: function () { return this._e.map(function (p) { return p[1]; })[Symbol.iterator](); },\n"
"      entries: function () { return this._e.map(function (p) { return [p[0], p[1]]; })[Symbol.iterator](); }\n"
"    };\n"
"    FD.prototype[Symbol.iterator] = FD.prototype.entries;\n"
"    G.FormData = FD;\n"
"  }\n"
   /* createObjectURL returns a data: URL rather than a blob: one, and that is a
      decision, not a shortcut: fetch() understands data: (js_webapi.c), so an
      <img src=URL.createObjectURL(blob)> and a fetch of the same string both
      resolve, where a blob: scheme nothing can dereference would resolve
      neither. The cost is that the URL is not revocable and is large; both are
      visible rather than silent. */
"  if (G.URL && !G.URL.createObjectURL) {\n"
"    var b64 = function (u8) {\n"
"      var t = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/', o = '', i;\n"
"      for (i = 0; i + 2 < u8.length; i += 3) {\n"
"        var n = (u8[i] << 16) | (u8[i + 1] << 8) | u8[i + 2];\n"
"        o += t[n >> 18] + t[(n >> 12) & 63] + t[(n >> 6) & 63] + t[n & 63];\n"
"      }\n"
"      if (i < u8.length) {\n"
"        var r = u8.length - i;\n"
"        var m = (u8[i] << 16) | ((r > 1 ? u8[i + 1] : 0) << 8);\n"
"        o += t[m >> 18] + t[(m >> 12) & 63] + (r > 1 ? t[(m >> 6) & 63] : '=') + '=';\n"
"      }\n"
"      return o;\n"
"    };\n"
"    G.URL.createObjectURL = function (b) {\n"
"      if (!b || !b._b) return 'data:,';\n"
"      return 'data:' + (b.type || 'application/octet-stream') + ';base64,' + b64(b._b);\n"
"    };\n"
"    G.URL.revokeObjectURL = function () {};\n"
"  }\n"
"})();\n"

/* ==== canvas.getContext IS NOT HERE, AND IS NOT ABSENT EITHER ============
 *
 * This block used to argue at length that the method should stay away,
 * because the canonical feature test is `!!canvas.getContext` and a
 * null-returning stub flips it true. The argument was right about the stub and
 * wrong about the conclusion: the exit is a REAL context, and it landed --
 * `c/apps/browser/js_canvas.c`, over c/lib/gfx, installed onto
 * HTMLCanvasElement.prototype.
 *
 * The concern that motivated the old note is answered by WHERE it installs
 * rather than by staying away: HTMLCanvasElement.prototype is a real link in a
 * <canvas>'s chain (js_dom_iface.inc:214), so `div.getContext` is still
 * undefined and a probe aimed at the wrong element still gets the right
 * answer. tests/unit/canvas_test.c asserts that, so a later move to
 * Element.prototype cannot pass unnoticed.
 *
 * The corpus evidence is kept because it is the reason the stub was refused
 * and would be the reason again. tests/fixtures/jsperf/baidu-async-search.js:
 *
 *     var o = a.getContext === i ? !1 : a.getContext("2d");
 *     if (o === !1) return !1;
 *     a.width = a.height = 10; ...
 *
 * `i` is undefined and the early return compares STRICTLY against false. A
 * getContext returning null slips past that guard and the page walks on
 * holding null -- a clean fallback turned into a crash further from its cause.
 * ======================================================================== */

/* ==== the observers ======================================================
 * REQUESTED, NOT MEASURED: no page in the corpus reached one, because they all
 * died earlier. They are here because their ABSENCE is load-bearing in a way
 * the others' is not -- a page that constructs an IntersectionObserver and gets
 * a ReferenceError never shows its lazily-loaded content at all, and that is
 * most images on most modern pages.
 *
 * WHAT THEY HONESTLY DO, UPDATED TWICE NOW. First: browser.c grew real
 * `scroll`/`resize` dispatch (sync_scroll / browser_resize), so IO_LIVE below
 * re-measures every observer with a live target on both, which is what makes
 * below-the-fold content that only reveals itself when the user scrolls arrive
 * at all. The bounded RECHECKS timer stays as a fallback for content that
 * never needs a scroll or resize to be seen. Second, found the same day: the
 * events firing did not mean the ANSWER was right. `rootMargin` and
 * `threshold` were accepted, stored, and read back correctly by their own
 * getters, and never once consulted by the code computing an intersection --
 * the single most dangerous shape in this file, because every property a page
 * can read to check "did my options take" answered correctly while the
 * geometry underneath was wrong. Both are applied now: rootMargin expands (or
 * shrinks) the viewport rect before intersecting, and a threshold crossing is
 * what triggers delivery, not a raw non-empty record set -- the old code
 * pushed one record per target on every run whether or not anything had
 * changed, which is not what a page that toggles state per callback without
 * unobserving can survive. `root` as a scrolling ELEMENT stays genuinely
 * unsupported -- there is exactly one root in this browser, the viewport --
 * `opts.root` is stored only so `observer.root` reads back what was passed.
 *
 * MutationObserver is different in kind: it observes mutations, and mutations
 * are things script does, so it can be exact. The DOM's mutating methods are
 * wrapped here rather than hooked in js_dom.c -- that file belongs to another
 * line -- and each wrapper reports the same records a browser would. */
"var RECHECKS = 12;\n"
"if (!G.IntersectionObserver) {\n"
"  var vp = function () {\n"
"    return { w: G.innerWidth || __vw, h: G.innerHeight || __vh };\n"
"  };\n"
"  var rect = function (el) {\n"
"    try { return el.getBoundingClientRect(); } catch (e) { return null; }\n"
"  };\n"
   /* rootMargin is a 1-to-4-value CSS margin shorthand, each value px or a
      percentage -- and the percentage is of the ROOT's own dimension on ITS
      OWN axis (width for left/right, height for top/bottom), not one number
      applied to both. A margin that fails to parse is not silently dropped:
      observer-exceptions.html constructs `{rootMargin: 'auto'}` and asserts
      the constructor throws, so this returns null on failure and the caller
      turns that into a real SyntaxError rather than falling back to 0px. */
"  var parseRootMargin = function (s, root) {\n"
"    var trimmed = String(s).trim();\n"
   /* An empty string is not "1 unparseable part" -- empty-root-margin.html
      asserts it means a margin of size zero, and String.split on '' returns
      [''] (one empty element), which the px/% regex below would otherwise
      reject as a syntax error. */
"    if (trimmed === '') return { top: 0, right: 0, bottom: 0, left: 0 };\n"
"    var parts = trimmed.split(/\\s+/);\n"
"    if (!parts.length || parts.length > 4) return null;\n"
"    var vals = [];\n"
"    for (var i = 0; i < parts.length; i++) {\n"
"      var m = /^(-?[0-9]*\\.?[0-9]+)(px|%)$/.exec(parts[i]);\n"
"      if (!m) return null;\n"
"      vals.push({ n: parseFloat(m[1]), pct: m[2] === '%' });\n"
"    }\n"
"    if (vals.length === 1) vals = [vals[0], vals[0], vals[0], vals[0]];\n"
"    else if (vals.length === 2) vals = [vals[0], vals[1], vals[0], vals[1]];\n"
"    else if (vals.length === 3) vals = [vals[0], vals[1], vals[2], vals[1]];\n"
"    var px = function (v, dim) { return v.pct ? v.n / 100 * dim : v.n; };\n"
"    return { top: px(vals[0], root.h), right: px(vals[1], root.w),\n"
"             bottom: px(vals[2], root.h), left: px(vals[3], root.w) };\n"
"  };\n"
"  var IOEntry = function IntersectionObserverEntry() {};\n"
   /* Data properties, not accessors -- what this buys is
      `'isIntersecting' in IntersectionObserverEntry.prototype`, which is the
      guard the w3c/IntersectionObserver polyfill (and everything that bundles
      it) uses to decide whether a real implementation is already present.
      Before this the prototype was empty, the guard read false, and a page
      installed a POLYFILL on top of a working shim -- one written against
      real scroll events firing a synchronous callback, which is not this. */
"  IOEntry.prototype = { constructor: IOEntry, target: null, time: 0, rootBounds: null,\n"
"                        boundingClientRect: null, intersectionRect: null,\n"
"                        intersectionRatio: 0, isIntersecting: false };\n"
"  G.IntersectionObserverEntry = IOEntry;\n"
"  var IO_LIVE = [];\n"      /* every IntersectionObserver with >=1 observed target */
"  var IO = function IntersectionObserver(cb, opts) {\n"
"    if (typeof cb !== 'function')\n"
"      throw new TypeError('IntersectionObserver: callback is not a function');\n"
"    opts = opts || {};\n"
"    var margin = parseRootMargin(opts.rootMargin != null ? opts.rootMargin : '0px', vp());\n"
"    if (!margin)\n"
"      throw new SyntaxError('IntersectionObserver: rootMargin must be 1-4 px/% values');\n"
"    var th = opts.threshold === undefined ? [0]\n"
"           : Array.isArray(opts.threshold) ? opts.threshold.slice() : [opts.threshold];\n"
"    for (var i = 0; i < th.length; i++)\n"
"      if (!(th[i] >= 0 && th[i] <= 1))\n"
"        throw new RangeError('IntersectionObserver: threshold must be within [0, 1]');\n"
"    th.sort(function (a, b) { return a - b; });\n"
"    this._cb = cb; this._t = []; this._n = 0; this._margin = margin; this._pending = [];\n"
"    this.root = opts.root || null;\n"
"    this.rootMargin = opts.rootMargin != null ? String(opts.rootMargin) : '0px';\n"
"    this.thresholds = th;\n"
"  };\n"
"  IO.prototype = {\n"
"    constructor: IO,\n"
   /* `idx: -1` is a sentinel meaning "never evaluated", not "evaluated at
      threshold 0" -- the spec requires an initial record for every newly
      observed target regardless of whether it intersects, and 0 would collide
      with a real threshold-0 non-intersecting state and suppress it. */
"    observe: function (el) {\n"
"      if (!el) return;\n"
"      for (var i = 0; i < this._t.length; i++) if (this._t[i].el === el) return;\n"
"      this._t.push({ el: el, idx: -1 });\n"
"      if (IO_LIVE.indexOf(this) < 0) IO_LIVE.push(this);\n"
"      this._schedule();\n"
"    },\n"
"    unobserve: function (el) {\n"
"      for (var i = 0; i < this._t.length; i++)\n"
"        if (this._t[i].el === el) { this._t.splice(i, 1); break; }\n"
"      if (!this._t.length) { var j = IO_LIVE.indexOf(this); if (j >= 0) IO_LIVE.splice(j, 1); }\n"
"    },\n"
"    disconnect: function () {\n"
"      this._t = [];\n"
"      var j = IO_LIVE.indexOf(this); if (j >= 0) IO_LIVE.splice(j, 1);\n"
"    },\n"
"    takeRecords: function () { var r = this._pending; this._pending = []; return r; },\n"
"    _schedule: function () {\n"
"      if (this._armed) return;\n"
"      this._armed = true;\n"
"      var self = this;\n"
"      setTimeout(function () { self._armed = false; self._run(); }, 0);\n"
"    },\n"
   /* Deliver only on a THRESHOLD CROSSING (or a target's first evaluation),
      never unconditionally -- entries.forEach(e => if (e.isIntersecting)
      load()) is the near-universal idiom and survives repeat delivery by
      luck; the equally common idiom that toggles state on every callback
      without unobserving does not, and would re-render on every scroll pixel
      if this delivered every run regardless of whether anything crossed. */
"    _run: function () {\n"
"      var v = vp(), m = this._margin, self = this;\n"
   /* `0 - m.top`, not `-m.top` -- unary negation of a literal +0 margin
      produces IEEE754 negative zero, and empty-root-margin.html's
      assert_equals distinguishes it from positive 0 and fails on the sign
      alone, never mind the value. */
"      var root = { top: 0 - m.top, left: 0 - m.left, right: v.w + m.right, bottom: v.h + m.bottom };\n"
"      root.width = root.right - root.left; root.height = root.bottom - root.top;\n"
"      var recs = [];\n"
"      this._t.forEach(function (e) {\n"
"        var r = rect(e.el);\n"
"        if (!r) return;\n"
   /* isIntersecting is an EDGE-INCLUSIVE rectangle overlap test, computed on
      the UNCLAMPED overlap (ixRaw/iyRaw can go negative -- that is what "no
      overlap" looks like), not "ratio > 0". A root shrunk by a negative
      rootMargin to exactly zero height still touches a target that spans it,
      and root-margin-rounding.html exists because that zero can come out
      -0.0000001 by floating point, which the 0.01px slop absorbs without
      ever letting two genuinely separate rects claim to touch. */
"        var ixRaw = Math.min(r.right, root.right) - Math.max(r.left, root.left);\n"
"        var iyRaw = Math.min(r.bottom, root.bottom) - Math.max(r.top, root.top);\n"
"        var intersects = ixRaw >= -0.01 && iyRaw >= -0.01;\n"
"        var ix = Math.max(0, ixRaw), iy = Math.max(0, iyRaw);\n"
"        var area = (r.width || 0) * (r.height || 0);\n"
"        var ratio = area > 0 ? (ix * iy) / area : (intersects ? 1 : 0);\n"
"        var idx = 0;\n"
"        for (var i = 0; i < self.thresholds.length; i++) if (ratio >= self.thresholds[i]) idx = i + 1;\n"
"        if (idx === e.idx) return;\n"
"        e.idx = idx;\n"
"        var entry = Object.create(IOEntry.prototype);\n"
"        entry.target = e.el; entry.time = performance.now();\n"
"        entry.isIntersecting = intersects; entry.intersectionRatio = ratio;\n"
"        entry.boundingClientRect = r;\n"
"        entry.rootBounds = { top: root.top, left: root.left, right: root.right, bottom: root.bottom,\n"
"                             width: root.width, height: root.height, x: root.left, y: root.top };\n"
"        entry.intersectionRect = { top: Math.max(r.top, root.top), left: Math.max(r.left, root.left),\n"
"                                   width: ix, height: iy };\n"
"        recs.push(entry);\n"
"      });\n"
       /* Delivery is a microtask, as the spec says, and it is what makes
          takeRecords() meaningful: called between a run and the microtask
          draining _pending, it takes those records itself and the queued
          microtask then finds nothing left to deliver. */
"      if (recs.length) {\n"
"        self._pending = self._pending.concat(recs);\n"
"        if (!self._qd) {\n"
"          self._qd = true;\n"
"          Promise.resolve().then(function () {\n"
"            self._qd = false;\n"
"            var r = self._pending; self._pending = [];\n"
"            if (r.length) { try { self._cb(r, self); } catch (e) { G.reportError(e); } }\n"
"          });\n"
"        }\n"
"      }\n"
"      if (++this._n < RECHECKS && this._t.length)\n"
"        setTimeout(function () { self._run(); }, 100);\n"
"    }\n"
"  };\n"
"  var reschedule_live = function () {\n"
"    for (var i = 0; i < IO_LIVE.length; i++) IO_LIVE[i]._schedule();\n"
"  };\n"
"  G.addEventListener('scroll', reschedule_live);\n"
"  G.addEventListener('resize', reschedule_live);\n"
"  G.IntersectionObserver = IO;\n"
"}\n"
"if (!G.ResizeObserver) {\n"
"  var ROEntry = function ResizeObserverEntry() {};\n"
"  ROEntry.prototype = { constructor: ROEntry, target: null, contentRect: null,\n"
"                        borderBoxSize: null, contentBoxSize: null, devicePixelContentBoxSize: null };\n"
"  G.ResizeObserverEntry = ROEntry;\n"
"  var RO = function ResizeObserver(cb) {\n"
"    if (typeof cb !== 'function')\n"
"      throw new TypeError('ResizeObserver: callback is not a function');\n"
"    this._cb = cb; this._t = []; this._n = 0; this._pending = [];\n"
"  };\n"
"  RO.prototype = {\n"
"    constructor: RO,\n"
   /* `w: -1, h: -1` is the same never-evaluated sentinel as IO's `idx: -1` --
      a real box can be exactly 0x0 (a collapsed element) and that must still
      produce one initial record. */
"    observe: function (el) {\n"
"      if (!el) return;\n"
"      for (var i = 0; i < this._t.length; i++) if (this._t[i].el === el) return;\n"
"      this._t.push({ el: el, w: -1, h: -1 });\n"
"      this._schedule();\n"
"    },\n"
"    unobserve: function (el) {\n"
"      for (var i = 0; i < this._t.length; i++)\n"
"        if (this._t[i].el === el) { this._t.splice(i, 1); return; }\n"
"    },\n"
"    disconnect: function () { this._t = []; },\n"
"    takeRecords: function () { var r = this._pending; this._pending = []; return r; },\n"
"    _schedule: function () {\n"
"      if (this._armed) return;\n"
"      this._armed = true;\n"
"      var self = this;\n"
"      setTimeout(function () { self._armed = false; self._run(); }, 0);\n"
"    },\n"
   /* Same crossing discipline as IO's, over box size instead of a ratio:\n"
      deliver only when the measured box actually changed, or 60 identical\n"
      callbacks a second replace the old bounded-death bug with a livelock. */
"    _run: function () {\n"
"      var recs = [], self = this;\n"
"      this._t.forEach(function (e) {\n"
"        var r; try { r = e.el.getBoundingClientRect(); } catch (ex) { return; }\n"
"        if (r.width === e.w && r.height === e.h) return;\n"
"        e.w = r.width; e.h = r.height;\n"
"        var box = [{ inlineSize: r.width, blockSize: r.height }];\n"
"        var entry = Object.create(ROEntry.prototype);\n"
"        entry.target = e.el; entry.contentRect = r; entry.borderBoxSize = box;\n"
"        entry.contentBoxSize = box; entry.devicePixelContentBoxSize = box;\n"
"        recs.push(entry);\n"
"      });\n"
"      if (recs.length) {\n"
"        self._pending = self._pending.concat(recs);\n"
"        if (!self._qd) {\n"
"          self._qd = true;\n"
"          Promise.resolve().then(function () {\n"
"            self._qd = false;\n"
"            var r = self._pending; self._pending = [];\n"
"            if (r.length) { try { self._cb(r, self); } catch (ex) { G.reportError(ex); } }\n"
"          });\n"
"        }\n"
"      }\n"
"      if (++this._n < RECHECKS && this._t.length)\n"
"        setTimeout(function () { self._run(); }, 100);\n"
"    }\n"
"  };\n"
"  G.ResizeObserver = RO;\n"
"}\n"
"if (!G.MutationObserver) {\n"
"  var mos = [];\n"
   /* IO and RO both throw TypeError on a non-function callback at
      construction; MO did not -- it stored the bad value and would only fail
      later, inside emit()'s own try/catch, reported through
      G.reportError/console.error rather than at the caller's own line. Not a
      hang (that catch still lets delivery continue for every OTHER observer),
      but a page that does `new MutationObserver(cb)` where cb is undefined
      because of a typo got a working-looking observer object instead of the
      construction-time error every other observer constructor gives it. */
"  var refreshNativeWatches=function(){\n"
"    var list=[];mos.forEach(function(m){m._t.forEach(function(t){list.push([t.node,!!t.opts.childList,!!t.opts.characterData,!!t.opts.subtree]);});});\n"
"    __nativeMOFlush(mos.length,false,list);\n"
"  };\n"
"  var MO = function MutationObserver(cb) {\n"
"    if (typeof cb !== 'function')\n"
"      throw new TypeError('MutationObserver: callback is not a function');\n"
"    this._cb = cb; this._recs = []; this._t = [];\n"
"  };\n"
"  MO.prototype = {\n"
"    constructor: MO,\n"
"    observe: function (el, opts) {\n"
"      __nativeMOFlush();\n"
"      if (!el || typeof el.nodeType !== 'number') throw new TypeError('observe needs a Node');\n"
"      opts = Object.assign({}, opts || {});\n"
"      if (opts.characterData === undefined && opts.characterDataOldValue) opts.characterData=true;\n"
"      if (opts.attributes === undefined && (opts.attributeOldValue || opts.attributeFilter)) opts.attributes=true;\n"
"      if ((!opts.childList && !opts.attributes && !opts.characterData) || (!opts.characterData && opts.characterDataOldValue) || (!opts.attributes && (opts.attributeOldValue || opts.attributeFilter))) throw new TypeError('invalid MutationObserver options');\n"
"      var prior=this._t.find(function(t){return t.node===el});if(prior)prior.opts=opts;else this._t.push({node:el,opts:opts});\n"
"      if (mos.indexOf(this) < 0) mos.push(this);\n"
"      refreshNativeWatches();\n"
"    },\n"
"    disconnect: function () { __nativeMOFlush(); var i = mos.indexOf(this); if (i >= 0) mos.splice(i, 1); this._t = []; this._recs = []; refreshNativeWatches(); },\n"
"    takeRecords: function () { __nativeMOFlush(); var r = this._recs; this._recs = []; return r; }\n"
"  };\n"
"  G.MutationObserver = MO;\n"
"  G.MutationRecord = function MutationRecord() {};\n"
   /* Does `node` lie inside anything this observer watches? `subtree` is the
      difference between an observer on document.body seeing every change on the
      page and seeing none of them, so it is walked, not assumed. */
/* A single observer can register both a parent and its child with different
 * types. Returning the first ancestral match discarded a later eligible text
 * registration; filter each registration first, then union oldValue requests. */
"  var watches = function (mo, node, rec, chain) {\n"
"    var result=null;\n"
"    for (var i=0;i<mo._t.length;i++) {\n"
"      var e=mo._t[i],o=e.opts,match=e.node===node;\n"
"      if(!match&&o.subtree){\n"
"        if(chain)match=chain.indexOf(e.node)>=0;\n"
"        else for(var p=node;p;p=p.parentNode){if(p===e.node){match=true;break;}}\n"
"      }\n"
"      if(!match || !o[rec.type])continue;\n"
"      if(rec.type==='attributes'&&o.attributeFilter&&o.attributeFilter.indexOf(rec.attributeName)<0)continue;\n"
"      if(!result)result={};\n"
"      result[rec.type]=true;\n"
"      if(o.characterDataOldValue)result.characterDataOldValue=true;\n"
"      if(o.attributeOldValue)result.attributeOldValue=true;\n"
"    }\n"
"    return result;\n"
"  };\n"
"  var emit = function (target, rec, chain) {\n"
   /* __domQuiet: a tree move that is an IMPLEMENTATION DETAIL rather than
      something the page did. installCloneNode moves a node into a scratch
      container to read its markup and puts it straight back; a page must not
      see that as a removal and an insertion, because a virtual-DOM diff
      watching the subtree would act on it. */
"    if (G.__domQuiet) return;\n"
"    if (!chain) __nativeMOFlush();\n"
"    var notified=false;\n"
"    for (var i = 0; i < mos.length; i++) {\n"
"      var o = watches(mos[i], target, rec, chain);\n"
"      if (!o) continue;\n"
"      if (rec.type === 'childList' && !o.childList) continue;\n"
"      if (rec.type === 'attributes' && !o.attributes) continue;\n"
"      if (rec.type === 'characterData' && !o.characterData) continue;\n"
"      var copy=Object.assign(Object.create(G.MutationRecord.prototype),rec);copy.target=target;\n"
"      if((rec.type==='characterData'&&!o.characterDataOldValue)||(rec.type==='attributes'&&!o.attributeOldValue))copy.oldValue=null;\n"
"      mos[i]._recs.push(copy);notified=true;\n"
"    }\n"
"    if(notified)__nativeMOFlush(undefined, true);\n"
"  };\n"
"  var deliverMutations = function () {\n"
"    mos.slice().forEach(function (m) {\n"
"      if (!m._recs.length) return;\n"
"      var r = m._recs; m._recs = [];\n"
"      try { m._cb(r, m); } catch (e) { G.reportError(e); }\n"
"    });\n"
"  };\n"
   /* Each method is wrapped on the prototype that OWNS it -- see wrapMethod at
      the top of this file for what went wrong when they were all wrapped on
      "the element prototype", and for why setAttribute was the one that
      silently disappeared while appendChild survived. */
   /* Every record carries all SEVEN spec fields now, not just the ones this
      mutation touches. mutationobservers.js -- the shared helper the real
      WPT MutationObserver suite is built on, not a page this tree invented --
      compares a MISSING key as `null` (its own checkField: a `field ===
      undefined` default is null) but assert_equals is `===`, and a key this
      object never set reads back `undefined`, not `null`. Every record below
      used to omit previousSibling/nextSibling/attributeNamespace entirely, so
      `undefined !== null` failed attributes, characterData AND childList
      records alike, on every WPT file built on that helper, independent of
      whether the mutation itself was reported correctly -- the shape was
      wrong even when the content was right. This is the general form of the
      Node.isEqualNode trap: a record that arrives, on time, with the right
      TYPE, still reads as "nothing happened" to code that walks its full
      shape.
      previousSibling/nextSibling ARE computed for childList, not left null:
      for an insertion the node is in its final tree position by the time
      orig.apply returns, so its own previousSibling/nextSibling are exactly
      the spec's definition; for a plain removeChild the node is detached by
      then, so those two are read BEFORE orig.apply runs instead. */
"  ['appendChild', 'insertBefore', 'replaceChild', 'removeChild'].forEach(function (m) {\n"
"    wrapMethod('mo', m, function (orig, name) {\n"
"      return function () {\n"
"        var added = [], removed = [], prevSib = null, nextSib = null;\n"
"        if (name === 'removeChild') {\n"
"          removed = [arguments[0]];\n"
"          if (arguments[0]) { prevSib = arguments[0].previousSibling; nextSib = arguments[0].nextSibling; }\n"
"        } else if (name === 'replaceChild') { added = [arguments[0]]; removed = [arguments[1]]; }\n"
"        else added = [arguments[0]];\n"
"        var r = orig.apply(this, arguments);\n"
"        if (added.length && added[0]) { prevSib = added[0].previousSibling; nextSib = added[0].nextSibling; }\n"
"        if (mos.length) emit(this, { type: 'childList', addedNodes: added, removedNodes: removed,\n"
"                                     previousSibling: prevSib, nextSibling: nextSib,\n"
"                                     attributeName: null, attributeNamespace: null, oldValue: null });\n"
"        return r;\n"
"      };\n"
"    });\n"
"  });\n"
"  ['setAttribute', 'removeAttribute'].forEach(function (m) {\n"
"    wrapMethod('mo', m, function (orig) {\n"
"      return function (name) {\n"
"        var old = null;\n"
"        try { old = this.getAttribute(name); } catch (e) {}\n"
"        var r = orig.apply(this, arguments);\n"
"        if (mos.length) emit(this, { type: 'attributes', attributeName: String(name),\n"
"                                     attributeNamespace: null, oldValue: old,\n"
"                                     addedNodes: [], removedNodes: [],\n"
"                                     previousSibling: null, nextSibling: null });\n"
"        return r;\n"
"      };\n"
"    });\n"
"  });\n"
/* CharacterData now arrives through native dom subscriptions, including
 * keyboard edits and native replaceData. Do not also wrap the setters. */
"}\n"


/* ==== HTMLElement.dataset ================================================
 * THE TOP OF THE CHROME DIFFERENTIAL, and the only entry on it that two
 * different pages hit for two entirely different reasons:
 *
 *   deepseek  React 19's stylesheet hoisting reads `o.dataset.precedence` on
 *             every <link data-precedence> it finds, and dies with
 *             `cannot read property 'precedence' of undefined` inside its own
 *             commit phase -- so the page's React never finishes mounting.
 *   mdn       `document.documentElement.dataset.theme = ...` in the very
 *             first inline script, which the page wraps in a try/catch and
 *             reports as `Unable to set theme`. No exception escapes, which is
 *             exactly why a probe that counts only uncaught exceptions could
 *             not see it: the page degrades silently and renders in the wrong
 *             theme for ever.
 *
 * Real headless Chrome throws neither on the same committed bytes.
 *
 * A Proxy rather than a snapshot object, because dataset is LIVE in both
 * directions: `el.dataset.x = 1` must write the attribute, `delete
 * el.dataset.x` must remove it, and a setAttribute done elsewhere must show
 * through. A plain object built at first access would satisfy the read in
 * deepseek and silently drop the write in mdn.
 *
 * The name mapping is the spec's: data-foo-bar <-> fooBar, and an uppercase
 * letter in the JS name is illegal rather than silently lowercased. */
"function installDataset(proto) {\n"
"  if (!proto || proto.__dsWrapped) return;\n"
"  try { Object.defineProperty(proto, '__dsWrapped', { value: true, enumerable: false }); } catch (e) { return; }\n"
"  var toAttr = function (k) {\n"
"    if (/-[a-z]/.test(k)) return null;\n"        /* data-foo is not reachable as .data-foo */
"    return 'data-' + k.replace(/[A-Z]/g, function (c) { return '-' + c.toLowerCase(); });\n"
"  };\n"
"  var toProp = function (a) {\n"
"    return a.slice(5).replace(/-([a-z])/g, function (m, c) { return c.toUpperCase(); });\n"
"  };\n"
"  try {\n"
"    Object.defineProperty(proto, 'dataset', {\n"
"      configurable: true,\n"
"      get: function () {\n"
"        var el = this;\n"
"        if (el.__ds) return el.__ds;\n"
"        var d = new Proxy({}, {\n"
"          get: function (t, k) {\n"
"            if (typeof k !== 'string') return undefined;\n"
"            var a = toAttr(k); if (!a) return undefined;\n"
"            var v = el.getAttribute(a);\n"
"            return v === null ? undefined : v;\n"
"          },\n"
"          set: function (t, k, v) {\n"
"            if (typeof k !== 'string') return true;\n"
"            var a = toAttr(k);\n"
"            if (a) el.setAttribute(a, String(v));\n"
"            return true;\n"
"          },\n"
"          has: function (t, k) {\n"
"            if (typeof k !== 'string') return false;\n"
"            var a = toAttr(k);\n"
"            return !!a && el.getAttribute(a) !== null;\n"
"          },\n"
"          deleteProperty: function (t, k) {\n"
"            var a = typeof k === 'string' ? toAttr(k) : null;\n"
"            if (a && el.removeAttribute) el.removeAttribute(a);\n"
"            return true;\n"
"          },\n"
             /* ENUMERATION IS A NAMED GAP, not an oversight. Object.keys(
                el.dataset) needs the element's attribute NAMES, and js_dom.c
                publishes getAttribute/setAttribute/hasAttribute and no way to
                list them -- no `attributes`, no `getAttributeNames`. That file
                belongs to the DOM line, so the primitive is an ask, not an
                edit: one getAttributeNames() and this starts working, which is
                why the call is already here rather than the list being
                hardcoded empty. Neither page on the differential enumerates;
                both read and write by name, which does work. */
"          ownKeys: function () {\n"
"            var out = [];\n"
"            try {\n"
"              var names = el.getAttributeNames ? el.getAttributeNames() : [];\n"
"              for (var i = 0; i < names.length; i++)\n"
"                if (names[i].indexOf('data-') === 0) out.push(toProp(names[i]));\n"
"            } catch (e) {}\n"
"            return out;\n"
"          },\n"
"          getOwnPropertyDescriptor: function (t, k) {\n"
"            if (typeof k !== 'string') return undefined;\n"
"            var a = toAttr(k); if (!a) return undefined;\n"
"            var v = el.getAttribute(a);\n"
"            if (v === null) return undefined;\n"
"            return { value: v, writable: true, enumerable: true, configurable: true };\n"
"          }\n"
"        });\n"
"        try { Object.defineProperty(el, '__ds', { value: d, enumerable: false }); } catch (e) {}\n"
"        return d;\n"
"      }\n"
"    });\n"
"  } catch (e) {}\n"
"}\n"

/* ==== NodeFilter + document.createTreeWalker =============================
 * lit-html. MEASURED on the MDN fixture, whose four modules are a lit
 * application: `new TreeWalker` is created once at module scope
 * (`p.createTreeWalker(p, 129)`) and again for every template instantiation,
 * and with createTreeWalker absent the call returns undefined and the next
 * line -- `d.nextNode()` -- is `TypeError: not a function`. Chrome throws
 * nothing. One missing method takes out every page built on lit or on any
 * other library that walks the tree that way, which is most Web Components.
 *
 * Implemented over firstChild/nextSibling/parentNode/nodeType, which js_dom.c
 * already publishes, so this is a real pre-order walk and not a flattened
 * snapshot: a filter that rejects a node must still descend into it, and code
 * that mutates the tree between nextNode() calls must see the new shape.
 * currentNode is writable, because lit sets it. */
"function installTreeWalker() {\n"
"  if (G.NodeFilter) return;\n"
"  var NF = {\n"
"    FILTER_ACCEPT: 1, FILTER_REJECT: 2, FILTER_SKIP: 3,\n"
"    SHOW_ALL: 0xFFFFFFFF, SHOW_ELEMENT: 1, SHOW_ATTRIBUTE: 2, SHOW_TEXT: 4,\n"
"    SHOW_CDATA_SECTION: 8, SHOW_PROCESSING_INSTRUCTION: 64, SHOW_COMMENT: 128,\n"
"    SHOW_DOCUMENT: 256, SHOW_DOCUMENT_TYPE: 512, SHOW_DOCUMENT_FRAGMENT: 1024\n"
"  };\n"
"  def(G, 'NodeFilter', NF);\n"
"  var shows = function (n, what) {\n"
"    var t = n.nodeType;\n"
"    if (!t) return false;\n"
       /* whatToShow is a bitmask over (nodeType - 1), which is the detail that
          makes SHOW_COMMENT 128 line up with nodeType 8. */
"    return !!(what & (1 << (t - 1)));\n"
"  };\n"
"  var accept = function (w, n) {\n"
"    if (!shows(n, w.whatToShow)) return NF.FILTER_SKIP;\n"
"    var f = w.filter;\n"
"    if (!f) return NF.FILTER_ACCEPT;\n"
"    var r;\n"
"    try { r = typeof f === 'function' ? f(n) : f.acceptNode(n); } catch (e) { throw e; }\n"
"    return r === undefined ? NF.FILTER_ACCEPT : r;\n"
"  };\n"
"  function TreeWalker(root, whatToShow, filter) {\n"
"    this.root = root; this.currentNode = root;\n"
"    this.whatToShow = whatToShow === undefined ? NF.SHOW_ALL : (whatToShow >>> 0);\n"
"    this.filter = filter || null;\n"
"  }\n"
   /* Pre-order, with the one distinction that is easy to get backwards and
      changes the answer completely: FILTER_REJECT prunes the whole subtree,
      FILTER_SKIP rejects only the node and still descends into its children.
      A comment walker (lit's, whatToShow=128) SKIPS every element, so a
      version that treated skip as reject would return nothing at all and lit
      would render an empty template with no error. */
"  TreeWalker.prototype.nextNode = function () {\n"
"    var n = this.currentNode, res = NF.FILTER_ACCEPT;\n"
"    for (;;) {\n"
"      while (res !== NF.FILTER_REJECT && n.firstChild) {\n"
"        n = n.firstChild;\n"
"        res = accept(this, n);\n"
"        if (res === NF.FILTER_ACCEPT) { this.currentNode = n; return n; }\n"
"      }\n"
"      var sib = null, up = n;\n"
"      while (up && up !== this.root) { sib = up.nextSibling; if (sib) break; up = up.parentNode; }\n"
"      if (!sib) return null;\n"
"      n = sib;\n"
"      res = accept(this, n);\n"
"      if (res === NF.FILTER_ACCEPT) { this.currentNode = n; return n; }\n"
"    }\n"
"  };\n"
"  TreeWalker.prototype.parentNode = function () {\n"
"    var n = this.currentNode;\n"
"    while (n && n !== this.root) {\n"
"      n = n.parentNode;\n"
"      if (n && accept(this, n) === NF.FILTER_ACCEPT) { this.currentNode = n; return n; }\n"
"    }\n"
"    return null;\n"
"  };\n"
"  TreeWalker.prototype.firstChild = function () {\n"
"    var c = this.currentNode && this.currentNode.firstChild;\n"
"    while (c) {\n"
"      if (accept(this, c) === NF.FILTER_ACCEPT) { this.currentNode = c; return c; }\n"
"      c = c.nextSibling;\n"
"    }\n"
"    return null;\n"
"  };\n"
"  TreeWalker.prototype.nextSibling = function () {\n"
"    var s = this.currentNode && this.currentNode.nextSibling;\n"
"    while (s) {\n"
"      if (accept(this, s) === NF.FILTER_ACCEPT) { this.currentNode = s; return s; }\n"
"      s = s.nextSibling;\n"
"    }\n"
"    return null;\n"
"  };\n"
"  def(G, 'TreeWalker', TreeWalker);\n"
"  if (G.document && !G.document.createTreeWalker) {\n"
"    try {\n"
"      G.document.createTreeWalker = function (root, whatToShow, filter) {\n"
"        return new TreeWalker(root, whatToShow, filter);\n"
"      };\n"
"    } catch (e) {}\n"
"  }\n"
"}\n"
/* ==== the interface objects =============================================
 * `Node`, `Element`, `HTMLElement`, `HTMLDialogElement` and the rest exist on
 * the platform as NAMES as much as as types, and a page that reaches one it
 * cannot find stops there. This is the general case of the same bug that took
 * kimi out over `Storage`:
 *
 *   mdn      `'closedBy' in HTMLDialogElement.prototype` -- a feature test in
 *            a module's top-level body, so a ReferenceError rejects the whole
 *            module and the application does not mount. It appeared only AFTER
 *            createTreeWalker landed, because before that the module died
 *            earlier. Chrome throws nothing.
 *   corpus   HTMLElement is referenced 47 times across the seven fixtures,
 *            more than any other interface name.
 *
 * THIS WHOLE FUNCTION IS NOW A NO-OP ON A COMPLETE BUILD, and that is the
 * outcome it asked for. Everything below is guarded by `if (name in G)
 * return`, and js_dom.c's js_dom_iface.inc publishes all 68 interfaces as real
 * constructors over a real prototype chain -- so every `mk()` here finds the
 * name already taken and steps aside. It stays as the fallback for a link
 * without that file, and because the guard is what makes it harmless.
 *
 * WHAT IT USED TO SAY, kept because the correction is the point: "js_dom.c has
 * ONE element class with ONE shared prototype -- there is no per-tag class to
 * hand out", so Element.prototype and HTMLElement.prototype WERE that one
 * object and each per-tag name got a fresh empty prototype plus a
 * Symbol.hasInstance that string-compared tagName. That has been false since
 * the hierarchy landed, and believing it is how a wrapper installed on
 * `Object.getPrototypeOf(createElement('div'))` -- which is HTMLDivElement's
 * prototype now, not a shared one -- gets to reach <div> and nothing else.
 * See wrapMethod at the top of this file for what that cost. */
"function installInterfaces() {\n"
"  var EP = null;\n"
"  try { EP = Object.getPrototypeOf(G.document.createElement('div')); } catch (e) {}\n"
"  if (!EP) return;\n"
"  var mk = function (name, proto, tags) {\n"
"    if (name in G) return G[name];\n"
"    var C = function () { throw new TypeError('Illegal constructor'); };\n"
"    try {\n"
"      Object.defineProperty(C, 'name', { value: name, configurable: true });\n"
"      C.prototype = proto;\n"
"      Object.defineProperty(proto, 'constructor',\n"
"        { value: C, writable: true, configurable: true });\n"
"      if (tags) Object.defineProperty(C, Symbol.hasInstance, {\n"
"        value: function (o) {\n"
"          if (!o || o.nodeType !== 1 || !o.tagName) return false;\n"
"          return tags.indexOf(String(o.tagName).toLowerCase()) >= 0;\n"
"        }, configurable: true });\n"
"      G[name] = C;\n"
"    } catch (e) {}\n"
"    return G[name];\n"
"  };\n"
   /* The shared chain. Node and Element and HTMLElement all resolve to the one
      prototype our DOM has; that is the truthful mapping, not a shortcut. */
"  mk('EventTarget', EP, null);\n"
"  mk('Node', EP, null);\n"
"  mk('Element', EP, null);\n"
"  mk('CharacterData', EP, null);\n"
"  mk('Text', EP, null);\n"
"  mk('Comment', EP, null);\n"
"  mk('DocumentFragment', EP, null);\n"
   /* Document has its OWN prototype -- js_dom.c gives the document object a
      different class from an element -- so it must not be published over EP,
      or `document instanceof Document` would be false while every <div> was
      true. MEASURED: after navigator.mimeTypes landed, baidu's s006.js moved
      straight on to `ReferenceError: 'Document' is not defined`; its sniffer
      walks a list of interface names testing which exist. */
   /* The guard is load-bearing, and the assertion `an element is NOT a
      Document` is what caught its absence: if the object's prototype turns
      out to be Object.prototype -- which it is whenever the class puts its
      methods on the instance rather than on a shared prototype -- then
      publishing an interface over it makes EVERYTHING an instance of it, and
      `div instanceof Document` comes back true. A distinct prototype object,
      with hasInstance doing the real test, is the only safe shape. */
"  var mkOver = function (name, obj, alt) {\n"
"    if (name in G) return;\n"
"    var P = null;\n"
"    try { P = Object.getPrototypeOf(obj); } catch (e) { return; }\n"
"    if (P && P !== Object.prototype && P !== EP) { mk(name, P, null); return; }\n"
"    var C = function () { throw new TypeError('Illegal constructor'); };\n"
"    try {\n"
"      Object.defineProperty(C, 'name', { value: name, configurable: true });\n"
"      C.prototype = Object.create(P || Object.prototype);\n"
"      Object.defineProperty(C, Symbol.hasInstance,\n"
"        { value: function (o) { return o === obj || (!!alt && o === alt); }, configurable: true });\n"
"      G[name] = C;\n"
"    } catch (e) {}\n"
"  };\n"
"  mkOver('Document', G.document, null);\n"
"  mkOver('HTMLDocument', G.document, null);\n"
"  mkOver('Window', G, null);\n"
   /* HTMLElement is NOT a plain throwing constructor, because it is the one a
      custom element's `class X extends HTMLElement` calls through super().
      See installCustomElements: during an upgrade it returns the element being
      upgraded, and a base constructor that returns an object makes that object
      the derived constructor's `this`. That single language rule is what makes
      a real upgrade possible from JS. */
"  if (!('HTMLElement' in G)) {\n"
"    var HE = function () {\n"
"      if (G.__ceUpgrading) { var e = G.__ceUpgrading; G.__ceUpgrading = null; return e; }\n"
"      throw new TypeError('Illegal constructor');\n"
"    };\n"
"    try {\n"
"      Object.defineProperty(HE, 'name', { value: 'HTMLElement', configurable: true });\n"
"      HE.prototype = EP;\n"
"      Object.defineProperty(EP, 'constructor', { value: HE, writable: true, configurable: true });\n"
"      G.HTMLElement = HE;\n"
"    } catch (e) {}\n"
"  }\n"
"  var per = {\n"
"    HTMLAnchorElement: ['a'], HTMLAreaElement: ['area'], HTMLBRElement: ['br'],\n"
"    HTMLButtonElement: ['button'], HTMLCanvasElement: ['canvas'],\n"
"    HTMLDataListElement: ['datalist'], HTMLDetailsElement: ['details'],\n"
"    HTMLDialogElement: ['dialog'], HTMLDivElement: ['div'],\n"
"    HTMLEmbedElement: ['embed'], HTMLFormElement: ['form'],\n"
"    HTMLHeadingElement: ['h1','h2','h3','h4','h5','h6'],\n"
"    HTMLIFrameElement: ['iframe'], HTMLImageElement: ['img'],\n"
"    HTMLInputElement: ['input'], HTMLLabelElement: ['label'],\n"
"    HTMLLIElement: ['li'], HTMLLinkElement: ['link'], HTMLMetaElement: ['meta'],\n"
"    HTMLObjectElement: ['object'], HTMLOListElement: ['ol'],\n"
"    HTMLOptionElement: ['option'], HTMLParagraphElement: ['p'],\n"
"    HTMLPreElement: ['pre'], HTMLScriptElement: ['script'],\n"
"    HTMLSelectElement: ['select'], HTMLSlotElement: ['slot'],\n"
"    HTMLSpanElement: ['span'], HTMLStyleElement: ['style'],\n"
"    HTMLTableElement: ['table'], HTMLTemplateElement: ['template'],\n"
"    HTMLTextAreaElement: ['textarea'], HTMLUListElement: ['ul'],\n"
"    HTMLVideoElement: ['video'], HTMLAudioElement: ['audio'],\n"
"    HTMLMediaElement: ['video','audio'], HTMLUnknownElement: []\n"
"  };\n"
"  for (var k in per) {\n"
"    if (k in G) continue;\n"
"    var p = Object.create(EP);\n"
"    mk(k, p, per[k]);\n"
"  }\n"
"}\n"
/* ==== customElements =====================================================
 * MEASURED: after dataset, createTreeWalker, the interface objects and the
 * non-special URL landed, ELEVEN of MDN's twelve remaining exceptions were
 * this one name, once per Web Component on the page -- `couldn't load code
 * for <switch>: ReferenceError: 'customElements' is not defined`. Chrome
 * throws none of them. It is also the whole Web Components web: lit, Stencil,
 * FAST, and every design system built on them.
 *
 * THIS IS A REAL UPGRADE, NOT A REGISTRY THAT REMEMBERS NAMES. That
 * distinction is the reason this took thought rather than ten lines, and a
 * `define()` that recorded the class and did nothing else would have been the
 * `crypto.subtle` mistake in a new place: every page would believe its
 * components were registered and render nothing, silently, with no error to
 * find.
 *
 * How an upgrade is possible at all from JS. A custom element is
 * `class X extends HTMLElement`, and the element the browser upgrades has to
 * BECOME the `this` inside X's constructor. `super()` performs
 * Construct(HTMLElement, [], X), and a BASE constructor that returns an object
 * has that object become the derived constructor's `this` -- so HTMLElement
 * above returns the element currently being upgraded, and X's constructor then
 * initialises the real node. Reflect.construct(X, [], X) drives it. Nothing is
 * copied and no wrapper is interposed: the node in the tree is the node the
 * component's code holds.
 *
 * The prototype is swapped first (Object.setPrototypeOf(el, X.prototype)),
 * which is what makes the component's methods reachable on the node and what
 * `instanceof X` answers on.
 *
 * WHEN UPGRADES HAPPEN. On define(), over every matching element already in
 * the document -- which is the case that matters for server-rendered markup,
 * and MDN's entire page is that. And on insertion, through the same
 * appendChild/insertBefore/replaceChild wrappers the MutationObserver support
 * already installs, so a component created after its definition also upgrades.
 *
 * WHAT IS NOT HERE, named rather than approximated:
 *   - attachShadow. There is no shadow tree in js_dom.c and inventing one from
 *     JS would produce encapsulation that does not encapsulate -- styles would
 *     leak and a page would be wrong in a way nothing reports. A component
 *     that calls it still fails, loudly, and the DOM line owns the primitive.
 *   - `is=` customised built-ins, which no engine but Chrome ships.
 *   - disconnectedCallback fires from removeChild only; a node dropped by an
 *     innerHTML rewrite of its parent does not get one. */
"function installCustomElements() {\n"
"  if (G.customElements) return;\n"
"  var defs = {}, waiting = {};\n"
#ifndef CE_LEGACY_CREATION_LIFECYCLE
"  var hasDefs = false, rawCreate = G.document.createElement;\n"
"  var connectOne = function(el) {\n"
"    if (el.__ceState !== 'upgraded' || el.__ceConnected || !inDocument(el)) return;\n"
"    el.__ceConnected = true;\n"
"    if (typeof el.connectedCallback === 'function')\n"
"      try { el.connectedCallback(); } catch(e) { G.reportError(e); }\n"
"  };\n"
"  var disconnectOne = function(el) {\n"
"    if (el.__ceState !== 'upgraded' || !el.__ceConnected) return;\n"
"    el.__ceConnected = false;\n"
"    if (typeof el.disconnectedCallback === 'function')\n"
"      try { el.disconnectedCallback(); } catch(e) { G.reportError(e); }\n"
"  };\n"
#endif
"  var validName = function (n) {\n"
"    return typeof n === 'string' && /^[a-z][a-z0-9._]*-[a-z0-9._-]*$/.test(n);\n"
"  };\n"
"  var upgradeOne = function (el, d) {\n"
"    if (!el || el.__ceState) return;\n"
"    el.__ceState = 'upgrading';\n"
"    try {\n"
"      Object.setPrototypeOf(el, d.ctor.prototype);\n"
"      G.__ceUpgrading = el;\n"
"      Reflect.construct(d.ctor, [], d.ctor);\n"
"    } catch (e) { G.__ceUpgrading = null; el.__ceState = 'failed'; G.reportError(e); return; }\n"
"    G.__ceUpgrading = null;\n"
"    el.__ceState = 'upgraded';\n"
       /* Observed attributes already present are delivered before
          connectedCallback, as the spec orders them: a component that reads a
          value in attributeChangedCallback must not see connectedCallback
          first and render with a default. */
"    var obs = d.ctor.observedAttributes;\n"
"    if (obs && obs.length && typeof el.attributeChangedCallback === 'function') {\n"
"      for (var i = 0; i < obs.length; i++) {\n"
"        var v = null;\n"
"        try { v = el.getAttribute(obs[i]); } catch (e) {}\n"
"        if (v !== null) { try { el.attributeChangedCallback(obs[i], null, v); } catch (e) { G.reportError(e); } }\n"
"      }\n"
"    }\n"
#ifdef CE_LEGACY_CREATION_LIFECYCLE
"    if (typeof el.connectedCallback === 'function' && inDocument(el))\n"
"      { try { el.connectedCallback(); } catch (e) { G.reportError(e); } }\n"
#else
"    connectOne(el);\n"
#endif
"  };\n"
"  var inDocument = function (n) {\n"
"    for (var p = n; p; p = p.parentNode) if (p === G.document || p === G.document.documentElement) return true;\n"
"    return false;\n"
"  };\n"
"  var walk = function (root, fn) {\n"
"    if (!root) return;\n"
"    if (root.nodeType === 1) fn(root);\n"
"    var c = root.firstChild;\n"
"    while (c) { walk(c, fn); c = c.nextSibling; }\n"
"  };\n"
"  var upgradeTree = function (root) {\n"
#ifndef CE_LEGACY_CREATION_LIFECYCLE
       /* An empty registry has no possible candidate: do not enumerate every
          descendant on ordinary appendChild (framework detached-tree builds). */
"    if (!hasDefs) return;\n"
#endif
"    walk(root, function (el) {\n"
"      var d = defs[String(el.tagName || '').toLowerCase()];\n"
"      if (d) upgradeOne(el, d);\n"
#ifndef CE_LEGACY_CREATION_LIFECYCLE
"      connectOne(el);\n"
#endif
"    });\n"
"  };\n"
"  var CE = {\n"
"    define: function (name, ctor, options) {\n"
"      if (typeof ctor !== 'function')\n"
"        throw new TypeError('customElements.define: constructor is not a function');\n"
"      if (!validName(name))\n"
"        throw new G.DOMException(\"'\" + name + \"' is not a valid custom element name\",\n"
"                                 'SyntaxError');\n"
"      name = String(name).toLowerCase();\n"
"      if (defs[name])\n"
"        throw new G.DOMException(\"'\" + name + \"' has already been defined\", 'NotSupportedError');\n"
"      if (options && options.extends)\n"
"        throw new G.DOMException('customised built-in elements are not supported',\n"
"                                 'NotSupportedError');\n"
"      defs[name] = { ctor: ctor, name: name };\n"
#ifndef CE_LEGACY_CREATION_LIFECYCLE
"      hasDefs = true;\n"
#endif
"      try { upgradeTree(G.document.documentElement || G.document); } catch (e) {}\n"
"      var w = waiting[name];\n"
"      if (w) { delete waiting[name]; w.forEach(function (r) { try { r(ctor); } catch (e) {} }); }\n"
"    },\n"
"    get: function (name) { var d = defs[String(name).toLowerCase()]; return d ? d.ctor : undefined; },\n"
"    getName: function (ctor) {\n"
"      for (var k in defs) if (defs[k].ctor === ctor) return k;\n"
"      return null;\n"
"    },\n"
"    upgrade: function (root) { try { upgradeTree(root); } catch (e) {} },\n"
"    whenDefined: function (name) {\n"
"      name = String(name).toLowerCase();\n"
"      if (defs[name]) return Promise.resolve(defs[name].ctor);\n"
"      if (!validName(name))\n"
"        return Promise.reject(new G.DOMException(\"'\" + name + \"' is not a valid custom element name\",\n"
"                                                 'SyntaxError'));\n"
"      return new Promise(function (res) {\n"
"        (waiting[name] = waiting[name] || []).push(res);\n"
"      });\n"
"    }\n"
"  };\n"
"  def(G, 'customElements', CE);\n"
   /* ==== DIRECT CONSTRUCTION: `new MyElement()`, not just parser upgrade ====
    *
    * MEASURED, jsfb: `class Nt extends Tt {}` (solarite) where Tt is a Proxy
    * whose construct trap does `Reflect.construct(t, e, i)` -- so `new Nt()`
    * reaches the native HTMLElement constructor with NO node being upgraded,
    * and js_dom_iface.inc's iface_ctor (correctly) throws 'Illegal
    * constructor' for that case, because the ONLY legitimate direct call it
    * knows about is the upgrade handoff via __ceUpgrading. That is one of the
    * spec's TWO ways to get a custom element, and only one was built: the
    * other -- `new MyElement()` constructing a BRAND NEW element, the same
    * thing `document.createElement` + upgrade produces, just author-invoked
    * -- had no path at all. Two independent jsfb implementations stopped on
    * exactly this line.
    *
    * WHY THIS IS DONE WITH A JS PROXY RATHER THAN A THIRD C CASE. The
    * information needed to answer it -- "is newTarget a registered custom
    * element, and under what tag" -- lives entirely in `defs`, a JS closure
    * with no C-visible form, and duplicating a registry across the JS/C
    * boundary is the one-jar-two-doors mistake CLAUDE.md names. A `construct`
    * trap on the global `HTMLElement` binding sees the same (target, args,
    * newTarget) triple the native call would have received, and a trap with
    * no other handler forwards every other property (`.prototype`, so
    * `instanceof HTMLElement` and `HTMLDivElement.prototype instanceof
    * HTMLElement` are unaffected) and every other call (`Reflect.construct`
    * back to the ORIGINAL native constructor for the two cases that already
    * work: __ceUpgrading in flight, and the genuinely illegal `new
    * HTMLElement()` with no registration, which must keep throwing).
    *
    * ORDERING IS THE WHOLE CONTRACT. This runs inside the prelude, before any
    * page <script> or module evaluates, so `class X extends HTMLElement`
    * anywhere in page code reads the WRAPPED binding -- `extends` evaluates
    * its operand at class-definition time, not at `new`-time. A page that
    * captured the native constructor before this ran (impossible from page
    * code, since nothing runs before the prelude) would fall outside it; nothing
    * in this tree does. */
"  (function () {\n"
"    var Raw = G.HTMLElement;\n"
"    if (typeof Raw !== 'function' || Raw.__ceWrapped) return;\n"
"    var P = new Proxy(Raw, {\n"
"      construct: function (target, args, newTarget) {\n"
          /* The upgrade handoff (existing behaviour, moved up front so it
             costs nothing extra: an upgrade in flight never needs the
             registry lookup below). */
"        if (G.__ceUpgrading) {\n"
"          var up = G.__ceUpgrading; G.__ceUpgrading = null; return up;\n"
"        }\n"
"        var tag = null;\n"
"        for (var k in defs) if (defs[k].ctor === newTarget) { tag = k; break; }\n"
          /* Not a registered custom element -- fall through to the REAL
             constructor so `new HTMLElement()` and every other caller of this
             trap keeps throwing (or working) exactly as before this file. */
"        if (tag === null) return Reflect.construct(target, args, newTarget);\n"
"        if (!G.document || typeof G.document.createElement !== 'function')\n"
"          throw new TypeError('HTMLElement: no document to construct into');\n"
#ifdef CE_LEGACY_CREATION_LIFECYCLE
"        var el = G.document.createElement(tag);\n"
#else
       /* The original document method is essential: calling the newly wrapped
          createElement below from HTMLElement's super() constructs the same
          class recursively, rather than supplying its one native node. */
"        var el = rawCreate.call(G.document, tag);\n"
#endif
          /* Same swap upgradeOne does for the parser-upgrade case: the
             prototype makes the node instanceof the leaf class and reaches
             its methods; __ceState makes it a no-op for a LATER upgrade pass
             (define() or an insertion) that would otherwise try to construct
             it a second time. connectedCallback is deliberately NOT fired
             here -- the node is not in a document yet, and the insertion
             wrappers below fire it the moment it actually is one, same as
             any other already-upgraded custom element that moves. */
"        try { Object.setPrototypeOf(el, newTarget.prototype); } catch (e) {}\n"
"        el.__ceState = 'upgraded';\n"
"        return el;\n"
"      }\n"
"    });\n"
"    try { Object.defineProperty(P, '__ceWrapped', { value: 1 }); } catch (e) {}\n"
"    try { Object.defineProperty(G, 'HTMLElement', { value: P, writable: true, configurable: true }); }\n"
"    catch (e) { G.HTMLElement = P; }\n"
"  })();\n"
   /* Correction 2026-09-09, kept beside the earlier claim above: insertion
    * wrappers did NOT connect already-upgraded elements; upgradeOne returned
    * early. And createElement did not upgrade at all until append. The real
    * GitHub behaviors module read getPrototypeOf(createElement('turbo-frame')
    * .delegate) before append and rejected with TypeError: not an object.
    * tests/unit/custom_elements_test.c consumes a detached controller through
    * that operation and also exercises new, reconnect, fragment and replace.
    *
    * Native creation is captured once, so the public creation door and direct
    * construction share one registry without recursively invoking each other.
    * Snapshot insertion candidates BEFORE native insertion: fragments are empty
    * afterwards, and a moved node's old connection is otherwise irrecoverable.
    * Call reactions only after the native mutation succeeds, so a failed
    * insertBefore does not manufacture a disconnect. The native bindings still
    * return null for some refusals rather than throw; check the returned node
    * as well as exceptions before issuing any reaction. This remains a synchronous
    * subset: parser/innerHTML removal reactions and shadow-including traversal
    * are deliberately not claimed here (their native mutation doors need work).
    */
#ifndef CE_LEGACY_CREATION_LIFECYCLE
"  ['createElement', 'createElementNS'].forEach(function(m) {\n"
"    var owner = G.document;\n"
"    while (owner && !Object.prototype.hasOwnProperty.call(owner,m)) owner=Object.getPrototypeOf(owner);\n"
"    if (!owner || typeof owner[m] !== 'function') return;\n"
"    var orig=owner[m];\n"
"    owner[m]=function() {\n"
"      var el=orig.apply(this,arguments);\n"
"      if (this===G.document && !G.__domQuiet && (m==='createElement' || arguments[0]==='http://www.w3.org/1999/xhtml')) {\n"
"        var d=defs[String(el.tagName||'').toLowerCase()];\n"
"        if(d) upgradeOne(el,d);\n"
"      }\n"
"      return el;\n"
"    };\n"
"  });\n"
"  var snapshot = function(root) {\n"
"    var a=[]; walk(root,function(el){a.push({el:el,connected:inDocument(el)});}); return a;\n"
"  };\n"
"  ['appendChild','insertBefore','replaceChild'].forEach(function(m) {\n"
"    wrapMethod('ce',m,function(orig) { return function() {\n"
"      if (G.__domQuiet || !hasDefs) return orig.apply(this,arguments);\n"
"      var incoming=snapshot(arguments[0]);\n"
"      var outgoing=m==='replaceChild' ? snapshot(arguments[1]) : [];\n"
"      var r=orig.apply(this,arguments);\n"
"      if (r !== arguments[m==='replaceChild' ? 1 : 0]) return r;\n"
"      outgoing.forEach(function(x){if(x.connected) disconnectOne(x.el);});\n"
"      incoming.forEach(function(x){if(x.connected) disconnectOne(x.el);});\n"
"      incoming.forEach(function(x){var d=defs[String(x.el.tagName||'').toLowerCase()];if(d) upgradeOne(x.el,d);connectOne(x.el);});\n"
"      return r;\n"
"    }; });\n"
"  });\n"
"  wrapMethod('ce','removeChild',function(orig) { return function(n) {\n"
"    if (G.__domQuiet || !hasDefs) return orig.apply(this,arguments);\n"
"    var outgoing=snapshot(n), r=orig.apply(this,arguments);\n"
"    if (r !== n) return r;\n"
"    outgoing.forEach(function(x){if(x.connected) disconnectOne(x.el);});\n"
"    return r;\n"
"  }; });\n"
#else
   /* Insertion upgrades. The same three methods the MutationObserver support
      wraps, wrapped once more here -- order does not matter because each
      wrapper calls through, and doing it here rather than there keeps the two
      features independent. Both go through wrapMethod (see the top of the
      file), which is what puts each wrapper on the prototype that owns the
      method instead of on <div>'s. */
"  ['appendChild', 'insertBefore', 'replaceChild'].forEach(function (m) {\n"
"    wrapMethod('ce', m, function (orig) {\n"
"      return function () {\n"
"        var r = orig.apply(this, arguments);\n"
           /* Same reason as the MutationObserver guard: a node parked in
              cloneNode's scratch container is not being inserted into the
              document and must not be upgraded there. */
"        if (!G.__domQuiet) { try { upgradeTree(arguments[0]); } catch (e) {} }\n"
"        return r;\n"
"      };\n"
"    });\n"
"  });\n"
"  wrapMethod('ce', 'removeChild', function (orig) {\n"
"    return function (n) {\n"
"      var r = orig.apply(this, arguments);\n"
"      if (G.__domQuiet) return r;\n"
"      try {\n"
"        walk(n, function (el) {\n"
"          if (el.__ceState === 'upgraded' && typeof el.disconnectedCallback === 'function')\n"
"            { try { el.disconnectedCallback(); } catch (e) { G.reportError(e); } }\n"
"        });\n"
"      } catch (e) {}\n"
"      return r;\n"
"    };\n"
"  });\n"
#endif
"}\n"
/* ==== Node.cloneNode =====================================================
 * THE SINGLE FAILURE THAT TAKES BAIDU DOWN, and it is one call.
 *
 * The real baidu document -- the 694 KB page the browser lands on after
 * following baidu's UA-sniffed redirect, tests/fixtures/webapi/baidureal --
 * loads jQuery 1.10.2 as its first script. jQuery.support runs at load and
 * line 97 of that file is
 *
 *     support.html5Clone =
 *       document.createElement("nav").cloneNode(true).outerHTML !== "<:nav></:nav>"
 *
 * cloneNode does not exist, so that is `TypeError: not a function`, jQuery
 * never finishes initialising, and FOURTEEN OF TWENTY-EIGHT scripts on the
 * page then die with `ReferenceError: '$' is not defined`. The probe's miss
 * table reported `$` (9 refs, DIES) and `F` (3 refs) at the top, and neither
 * is an API -- they are the wreckage. One missing method, thirteen derived
 * failures. jQuery 1.x is on an enormous fraction of the web.
 *
 * HOW IT IS DONE HERE, AND WHAT THAT COSTS. js_dom.c publishes
 * getAttribute/setAttribute/hasAttribute and NO WAY TO LIST ATTRIBUTES -- no
 * `attributes`, no getAttributeNames -- so a clone cannot be built by copying
 * the properties one by one. What it does publish is innerHTML in both
 * directions, and a serialize/re-parse round trip through it reproduces
 * attributes, children and text exactly, because the same tokenizer that built
 * the original builds the copy.
 *
 * To get ONE node's markup out of an innerHTML that only speaks about a
 * parent's children, the node is moved into a scratch <div>, read, and put
 * back at its exact old position (parent + nextSibling, both captured first,
 * restored in a finally). Mutation records for that move are suppressed with
 * __domQuiet -- a page must not observe a clone as two tree mutations.
 *
 * THREE FIDELITY LIMITS, stated rather than discovered later:
 *   - it is a SERIALIZATION, so anything the serializer cannot express does
 *     not survive: event listeners (correct -- the spec drops them too), and
 *     JS properties set on the node (the spec drops those too).
 *   - re-parsing happens in a <div> context, so a bare <td> or <option>
 *     cloned outside its table or select is subject to the HTML parser's
 *     foster-parenting rules and can come back empty. Cloning a <template>'s
 *     content, or a whole <table>, is fine; cloning a lone row is not.
 *   - the clone's parentNode is the scratch container rather than null, and a
 *     node that had NO parent when it was cloned keeps the scratch container
 *     as its parent afterwards. Both follow from the same constraint: this DOM
 *     cannot express "detached" without destroying. Every normal use --
 *     clone, then insert somewhere -- moves it out on the next appendChild and
 *     never notices. An ATTACHED node is put back at its exact old position,
 *     which is asserted.
 *
 * The right home for this is js_dom.c, where it is a walk of the C node tree
 * and has neither limit. That file belongs to the DOM line: the ask is
 * cloneNode, and getAttributeNames() with it, which would also complete
 * dataset's enumeration. Until then a page that could not run jQuery at all
 * runs it. */
"function installCloneNode() {\n"
"  var EP = null;\n"
"  try { EP = Object.getPrototypeOf(G.document.createElement('div')); } catch (e) {}\n"
"  if (!EP) return;\n"
"  var D = G.document;\n"
   /* The serialization primitive both cloneNode and the attribute enumeration
      below are built on: an element's own markup, obtained by parking it in a
      scratch container (a MOVE, never a detach) and reading innerHTML, then
      putting it back exactly. */
"  var markupOf = function (node) {\n"
"    var host = D.createElement('div');\n"
"    var parent = node.parentNode, next = node.nextSibling, markup = '';\n"
"    var q = G.__domQuiet;\n"
"    G.__domQuiet = true;\n"
"    try { host.appendChild(node); markup = host.innerHTML; } catch (e) { markup = ''; }\n"
"    try { if (parent) { if (next) parent.insertBefore(node, next); else parent.appendChild(node); } }\n"
"    catch (e) {}\n"
"    G.__domQuiet = q;\n"
"    return markup;\n"
"  };\n"
   /* ---- Element.getAttributeNames / Element.attributes ----
    *
    * js_dom.c can answer "what is the value of this attribute" and cannot
    * answer "which attributes are there". That single gap is why dataset
    * cannot enumerate, why cloneNode cannot copy attribute by attribute, and
    * why jQuery 1.10.2 stops -- jQuery.support line 99 is
    *
    *     for (i in {submit:true, change:true, focusin:true}) {
    *       div.setAttribute(eventName = "on" + i, "t");
    *       support[i+"Bubbles"] = eventName in window ||
    *                              div.attributes[eventName].expando === false;
    *     }
    *
    * and `div.attributes` is undefined, so that is
    * `cannot read property 'onfocusin' of undefined` and jQuery never
    * finishes. The names are recoverable from the element's own serialization,
    * which is what the parser wrote and therefore includes attributes no
    * script ever set. Parsing a start tag with a regular expression is
    * exactly as bad as it sounds in general and exactly right here: the input
    * is not arbitrary HTML, it is dom_serialize.c's output, whose attribute
    * values are always double-quoted and escaped.
    *
    * It is O(subtree) per call because the serialization is. That is the cost
    * of not having the primitive; the correct fix is getAttributeNames() in
    * js_dom.c, which is the DOM line's file and would make this three lines. */
"  var ATTR_RE = /([^\\s=\"'>\\/]+)(?:\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)'|([^\\s\">]*)))?/g;\n"
"  var attrPairs = function (el) {\n"
"    var m = markupOf(el);\n"
"    var gt = -1, q = 0;\n"
"    for (var i = 0; i < m.length; i++) {\n"
"      var c = m.charAt(i);\n"
"      if (q) { if (c === q) q = 0; continue; }\n"
"      if (c === '\"' || c === \"'\") { q = c; continue; }\n"
"      if (c === '>') { gt = i; break; }\n"
"    }\n"
"    if (gt < 0) return [];\n"
"    var tag = m.slice(1, gt);\n"
"    var sp = tag.search(/[\\s\\/]/);\n"
"    if (sp < 0) return [];\n"
"    var rest = tag.slice(sp);\n"
"    var out = [], g;\n"
"    ATTR_RE.lastIndex = 0;\n"
"    while ((g = ATTR_RE.exec(rest))) {\n"
"      var name = g[1];\n"
"      if (!name || name === '/') continue;\n"
"      var v = g[2] !== undefined ? g[2] : (g[3] !== undefined ? g[3] : (g[4] !== undefined ? g[4] : ''));\n"
       /* The serializer escapes; undo the two that matter for a value. */
"      v = v.replace(/&quot;/g, '\"').replace(/&amp;/g, '&').replace(/&lt;/g, '<').replace(/&gt;/g, '>');\n"
"      out.push({ name: name, value: v });\n"
"    }\n"
"    return out;\n"
"  };\n"
"  if (typeof EP.getAttributeNames !== 'function') {\n"
"    try {\n"
"      Object.defineProperty(EP, 'getAttributeNames', {\n"
"        value: function () { return attrPairs(this).map(function (a) { return a.name; }); },\n"
"        writable: true, configurable: true, enumerable: false });\n"
"    } catch (e) {}\n"
"  }\n"
"  if (!('attributes' in EP)) {\n"
"    try {\n"
"      Object.defineProperty(EP, 'attributes', {\n"
"        configurable: true,\n"
"        get: function () {\n"
"          var el = this, pairs = attrPairs(el), map = [];\n"
"          for (var i = 0; i < pairs.length; i++) {\n"
"            var a = { name: pairs[i].name, localName: pairs[i].name, value: pairs[i].value,\n"
"                      specified: true, nodeType: 2, nodeName: pairs[i].name,\n"
"                      nodeValue: pairs[i].value, ownerElement: el };\n"
"            map[i] = a;\n"
"            map[pairs[i].name] = a;\n"
"          }\n"
"          map.getNamedItem = function (n) { return this[String(n)] || null; };\n"
"          map.item = function (i) { return this[i] || null; };\n"
"          return map;\n"
"        }\n"
"      });\n"
"    } catch (e) {}\n"
"  }\n"
"  if (typeof EP.cloneNode === 'function') return;\n"
"  var cloneVia = function (node, deep) {\n"
"    if (!node) return null;\n"
"    var t = node.nodeType;\n"
"    if (t === 3) return D.createTextNode(node.nodeValue == null ? '' : String(node.nodeValue));\n"
"    if (t === 8) return D.createComment ? D.createComment(node.data == null ? '' : String(node.data))\n"
"                                        : D.createTextNode('');\n"
"    if (t === 11) {\n"
"      var f = D.createDocumentFragment();\n"
"      if (deep) for (var c = node.firstChild; c; c = c.nextSibling) {\n"
"        var cc = cloneVia(c, true);\n"
"        if (cc) f.appendChild(cc);\n"
"      }\n"
"      return f;\n"
"    }\n"
"    if (t !== 1) return null;\n"
"    if (node === D.documentElement) return null;\n"
   /* NOTHING IS EVER DETACHED, and that constraint is not stylistic.
      js_dom.c's removeChild does not orphan a node, it RECYCLES it
      (dom_destroy_subtree -- see the comment above el_replaceChild, where the
      trade is argued). So the obvious "take it out, read it, put it back"
      destroys the caller's node: the first version of this did exactly that
      and quietly emptied the test document's #wrap subtree. insert_run has
      MOVE semantics, so appendChild/insertBefore alone are enough to park the
      node in a scratch container, read the markup, and put it back where it
      was -- with no destroy anywhere on the path. */
"    var markup = markupOf(node);\n"
   /* A FRESH container for the re-parse. Reusing `host` gave back its stale
      leftover child rather than the re-parsed copy, so every clone came out
      as an element wrapper with no nodeType at all. */
"    var sink = D.createElement('div');\n"
"    try { sink.innerHTML = markup; } catch (e) { return null; }\n"
"    var out = sink.firstChild;\n"
"    if (out && !deep) { try { out.innerHTML = ''; } catch (e) {} }\n"
"    return out;\n"
"  };\n"
"  try {\n"
"    Object.defineProperty(EP, 'cloneNode', {\n"
"      value: function (deep) { return cloneVia(this, deep === true); },\n"
"      writable: true, configurable: true, enumerable: false\n"
"    });\n"
"  } catch (e) {}\n"
   /* A DocumentFragment is not an element and does not share EP, so it needs
      its own -- jQuery clones one twice in a row during feature detection. */
"  try {\n"
"    var FP = Object.getPrototypeOf(D.createDocumentFragment());\n"
"    if (FP && FP !== EP && typeof FP.cloneNode !== 'function')\n"
"      Object.defineProperty(FP, 'cloneNode', {\n"
"        value: function (deep) { return cloneVia(this, deep === true); },\n"
"        writable: true, configurable: true, enumerable: false });\n"
"  } catch (e) {}\n"
"}\n"
/* ==== Document.importNode / Document.adoptNode / document.implementation ==
 * MEASURED: jsfb_matrix's work-order table over the 221-implementation
 * js-framework-benchmark corpus groups these three under "Document interface
 * members absent (importNode/adoptNode/createDocument)", 7 independent
 * implementations stopped. The general shape behind the name, not a site
 * quirk: a page builds a piece of tree off to the side -- a <template>'s
 * .content, a fresh document.createDocumentFragment(), a detached tree built
 * with document.createElement -- and wants it moved or copied INTO the live
 * document. importNode/adoptNode are the standard's names for that, and every
 * templating helper that predates or avoids innerHTML (most Web Component
 * base classes, lit-html on <template> content) reaches for them.
 *
 * WHAT THIS DOES, FOR REAL: same-document import/adopt -- which is what the
 * corpus actually exercises, since this engine has exactly one live document
 * -- is implemented on the existing primitives, both already correct:
 * cloneNode (installCloneNode, above: a lossless serialize/reparse round
 * trip) for importNode, and removeChild (detach-only since 2026-08-28, see
 * el_removeChild's comment in js_dom.c) for adoptNode's "take it out of
 * wherever it was" step.
 *
 * CROSS-DOCUMENT NODES ARE NATIVE, NOT SERIALIZED. DOMParser nodes use a
 * separate wrapper class and arena, but js_domparser.c exposes one hidden
 * native transfer that calls dom_import_node into the live document. That
 * copies attributes, namespaces, text and the requested descendants without
 * a lossy JavaScript reconstruction. adoptNode uses the same copy and detaches
 * the old parsed node. It cannot preserve JS object identity across QuickJS
 * native classes; the returned live wrapper is authoritative. This bounded
 * deviation replaced a measured Bilibili player stop: the public page loaded
 * and negotiated H.264/AAC, then rejected its DOMParser-built template before
 * it could create MediaSource.
 *
 * ONE JAR, NOT TWO: `document.implementation` (createHTMLDocument,
 * createDocumentType, createDocument, hasFeature) is NOT built here. It is
 * already real, at the C level, in js_dom_iface.inc's impl_funcs -- and its
 * createHTMLDocument is a BETTER answer than a DOMParser-backed one would
 * have been: it shares the live document's own arena on purpose (its own
 * comment: "sharing the arena costs a synthetic node kind and buys the one
 * behaviour the caller exists for"), so a node from it is NOT a foreign
 * cross-document node at all -- `ownerDocument` resolves the same as any
 * other node in this engine, and importNode/adoptNode on it Just Work through
 * the ordinary cloneNode/removeChild path below, no special case needed. The
 * first version of this file's comment assumed createHTMLDocument was
 * DOMParser-backed and duplicated it in JS on that wrong assumption; caught
 * by testing against the real thing (its ownerDocument came back === document,
 * not a foreign object) before it shipped. The genuinely foreign case this
 * genuinely foreign case is `new DOMParser().parseFromString()`; that case is
 * routed through the native transfer below. */
"function installImportAdopt() {\n"
"  var D = G.document;\n"
"  if (!D || typeof D.importNode === 'function') return;\n"
"  var checkNode = function (node, verb) {\n"
"    if (!node || typeof node.nodeType !== 'number')\n"
"      throw new TypeError(\"Failed to execute '\" + verb + \"Node' on 'Document': \" +\n"
"                          'parameter 1 is not of type \\'Node\\'.');\n"
"    if (node.nodeType === 9)\n"
"      throw new G.DOMException('A Document node may not be ' + verb + 'ed', 'NotSupportedError');\n"
"    return node.ownerDocument !== undefined && node.ownerDocument !== null &&\n"
"           node.ownerDocument !== D;\n"
"  };\n"
"  def(D, 'importNode', function (node, deep) {\n"
"    var foreign = checkNode(node, 'import');\n"
"    if (foreign) {\n"
"      if (typeof G.__domParserTransfer !== 'function')\n"
"        throw new G.DOMException('foreign document transfer is unavailable', 'NotSupportedError');\n"
"      return G.__domParserTransfer(node, deep === true, false);\n"
"    }\n"
   /* Same-document nodes always have cloneNode by this point (EP/FP both got
      it above, in installCloneNode -- called before this function, see the
      install sequence at the bottom of this file). Refuse an unknown wrapper
      rather than let a missing method read as a successful import. */
"    if (typeof node.cloneNode !== 'function')\n"
"      throw new G.DOMException('node cannot be cloned', 'NotSupportedError');\n"
"    return node.cloneNode(deep === true);\n"
"  });\n"
"  def(D, 'adoptNode', function (node) {\n"
"    var foreign = checkNode(node, 'adopt');\n"
"    if (foreign) {\n"
"      if (typeof G.__domParserTransfer !== 'function')\n"
"        throw new G.DOMException('foreign document transfer is unavailable', 'NotSupportedError');\n"
"      return G.__domParserTransfer(node, true, true);\n"
"    }\n"
"    if (node.parentNode) { try { node.parentNode.removeChild(node); } catch (e) {} }\n"
"    return node;\n"
"  });\n"
"}\n"
/* document.currentScript. js_page.c is handed the <script> NODE by whoever is
 * running it and hands that same node straight back out through
 * js_dom_node_value -- so this is a one-line forward, with no lookup and
 * nothing that can go stale.
 *
 * IT USED TO BE AN INDEX INTO document.scripts and this getter was the second
 * half of that: js_page.c published __currentScriptIndex() and the element was
 * recovered as document.scripts[i]. Two things were wrong with it and only the
 * first was ever visible. The index was derived by matching the filename
 * string the embedder passed, and for an inline classic script in the shipped
 * browser it never matched, so this getter returned null for every inline
 * script on every page (see js_page.c). And `document.currentScript.remove()`
 * -- the commonest thing pages do with the property -- renumbers
 * document.scripts underneath an index that is still standing on it, so even a
 * correct index would name the wrong element on the next read.
 *
 * Null outside a classic script's own synchronous execution, which is what the
 * spec says and what js_page.c enforces by clearing the node before the
 * microtask drain. */
"function installCurrentScript() {\n"
"  if (!G.document || ('currentScript' in G.document)) return;\n"
"  if (typeof G.__currentScriptNode !== 'function') return;\n"
"  try {\n"
"    Object.defineProperty(G.document, 'currentScript', {\n"
"      configurable: true,\n"
"      get: function () { return G.__currentScriptNode() || null; }\n"
"    });\n"
"  } catch (e) {}\n"
"}\n"
/* ==== the reflected URL properties: .src and .href =======================
 * document.currentScript alone is necessary and not sufficient. Webpack's
 * chunk loader reads
 *
 *     var s = document.currentScript; ... if (s) { u = s.src; }
 *     if (!u || !/^http(s?):/.test(u)) throw ...
 *
 * and js_dom.c publishes getAttribute("src") and no `src` PROPERTY at all, so
 * `s.src` is undefined and the loader throws even with currentScript working.
 * The two are one fix from the page's point of view.
 *
 * THE PROPERTY IS ABSOLUTE, THE ATTRIBUTE IS NOT, and that is the whole
 * difference between them: `<script src="/static/x.js">` has an attribute of
 * "/static/x.js" and a .src of "https://host/static/x.js". Returning the
 * attribute would satisfy `typeof s.src === 'string'` and still fail the
 * /^http(s?):/ test, which is a worse outcome than absence because it looks
 * like it worked.
 *
 * Only tags that HAVE the attribute in HTML get the property -- `div.src` is
 * undefined in a browser and must stay undefined here, or feature detection
 * that asks "is this an image" by testing for .src gets a yes for everything.
 * Writing goes back through setAttribute, so the two stay one value. */
"function installReflectedURLs() {\n"
"  var EP = null;\n"
"  try { EP = Object.getPrototypeOf(G.document.createElement('div')); } catch (e) {}\n"
"  if (!EP) return;\n"
"  var abs = function (v) {\n"
"    if (v === null || v === undefined) return '';\n"
"    if (v === '') return '';\n"
"    try { return new G.URL(String(v), G.document.baseURI || G.location.href).href; }\n"
"    catch (e) { return String(v); }\n"
"  };\n"
"  var reflect = function (prop, attr, tags) {\n"
"    if (prop in EP) return;\n"
"    try {\n"
"      Object.defineProperty(EP, prop, {\n"
"        configurable: true,\n"
"        get: function () {\n"
"          var t = String(this.tagName || '').toLowerCase();\n"
"          if (tags.indexOf(t) < 0) return undefined;\n"
"          return abs(this.getAttribute(attr));\n"
"        },\n"
"        set: function (v) { try { this.setAttribute(attr, String(v)); } catch (e) {} }\n"
"      });\n"
"    } catch (e) {}\n"
"  };\n"
"  reflect('src', 'src', ['script','img','iframe','source','video','audio',\n"
"                         'embed','track','input','frame']);\n"
"  reflect('href', 'href', ['a','link','area','base']);\n"
"  reflect('action', 'action', ['form']);\n"
"}\n"

/* ==== document.write / document.writeln ===================================
 * THE SPEC, CONDENSED TO THE TWO SENTENCES THIS ENGINE MUST ANSWER (WHATWG
 * "document write steps", dynamic-markup-insertion): while a script inserted
 * by the parser is executing, the string goes into the input stream JUST
 * BEFORE THE INSERTION POINT -- the tokenizer's current position, which for
 * an executing script is immediately after its own end tag -- and parsing
 * continues from there; and writeln is write plus U+000A.
 *
 * WHY THE SPLICE IS AT TREE LEVEL AND NOT IN THE TOKENIZER. This engine does
 * not parse and script concurrently: browser.c runs dom_parse() to EOF first
 * (html_tree.c's DEVIATION 3 note says so -- "there is no script execution
 * point in this parser"), and only then executes the collected scripts in
 * document order. By the time any script runs, html_tok_free() has already
 * released the input buffer; there is no insertion point to splice into. A
 * spec-literal splice would mean moving script execution INSIDE the parse
 * loop, which is browser.c's and js_page.c's plumbing, not this file's. What
 * is emulated instead is the OBSERVABLE of the splice: the inserted stream
 * position "just after </script>" has a tree address -- immediately after the
 * script element -- and everything the source puts after the script is
 * already sitting there. So a write inserts its parsed nodes after the
 * WRITING script element, a per-script cursor advancing across that script's
 * successive writes so they land in call order. The resulting document order
 * is the order a real browser's parser produces for the same page; what
 * differs is timing, and every timing difference is named below.
 *
 * THE WRAP-LIST RULE, PAID IN THIS COMMIT: insertion goes through
 * parent.insertBefore() -- a prototype method installIframes wraps -- so a
 * written <iframe> reaches onInsert()/initFrame, and at C level the call
 * lands in insert_run(), where js_dom.c's offer_scripts() enqueue picks up
 * a written <script> -- provided the installer revived it first (trap 7:
 * the parse stamps fragment scripts done, and only a rebuilt, unstamped
 * script is offered). So the door-eight rule's "call through
 * insert_run/insert_markup OR be on the wrap list" is satisfied by both
 * halves at once.
 *
 * THE SPEC'S TRAPS, EACH WITH WHAT THIS DOES INSTEAD AND WHY:
 *
 *   1. A write after the parser is done. The modern spec's answer is an
 *      IMPLICIT document.open() -- which WIPES the tree and every listener
 *      on it -- unless the ignore-destructive-writes counter is set (which
 *      is how real browsers answer writes from asynchronously-loaded
 *      scripts, the python.org case in collapse-recount.md: an event
 *      listener's write is refused, not honoured). That wipe is precisely
 *      the half-built shape this tree refuses to ship behind a silent
 *      default, so this implements the counter's behaviour for EVERY
 *      after-load write: contribute nothing, throw nothing, keep the page.
 *      The gate's negative control exists to watch that refusal hold
 *      (tests/qmp/qmp_docwrite.py, DW-DOM-ORDER2 / no-orange assertions).
 *      document.open() itself stays ABSENT: the only thing it would add
 *      here is the wipe-plus-resurrection path, and without a streaming
 *      parser to hand the new document to, resurrected writes would be
 *      present-and-wrong rather than absent.
 *
 *   2. "After the parser is done" has to be DETECTED, not assumed. The
 *      gate is document.currentScript's node bridge: js_page.c sets it for
 *      exactly the synchronous extent of a classic script's execution and
 *      clears it before timers or event handlers run, so currentScript ==
 *      null is this engine's "no insertion point". A write from a timer
 *      (the fixture's setTimeout), an event handler, or a module (modules
 *      never get currentScript, by spec) is ignored by the same test. A
 *      script that REMOVED itself (the x.com currentScript.remove() idiom)
 *      and then writes is ignored too -- parentNode null -- because the
 *      insertion point this implementation can name was just destroyed.
 *
 *   3. The insertion point is the SCRIPT'S POSITION, not "append". The
 *      wrong-but-tempting implementation is body.appendChild, which puts a
 *      head script's write AFTER page content it precedes in the source.
 *      A real browser parses a head-positioned write with the "in head"
 *      insertion mode, which pops to body for body-flow content, so such
 *      writes land at the TOP of body, before source body content, in write
 *      order across MULTIPLE head scripts. Hence the head rule below: head
 *      writes insert before body.firstChild, and the body cursor (bodyAt)
 *      is GLOBAL across scripts rather than per-script -- the second head
 *      script's write must land after the first's, and per-script cursors
 *      would both aim at body.firstChild and land in reverse execution
 *      order. Head-ALLOWED content written from head (a <style>, a
 *      <script src>) lands in body instead of head: visually
 *      indistinguishable (CSS and script execution do not care where in the
 *      tree they sit, and document order among WRITTEN nodes is preserved),
 *      and stated here rather than fixed with a per-token classifier, which
 *      would guess the parser's "left head and cannot go back" state that
 *      only the real mode machine knows.
 *
 *   4. Nested writes. The spec ignores a write whose entry's script nesting
 *      level exceeds 1. That state is unreachable here: js_dom.c's script
 *      sink ENQUEUES rather than executing on the inserting stack (the
 *      recursion trap its own comment names), so no classic script ever
 *      executes synchronously beneath another. Stated rather than handled;
 *      if synchronous script-in-script execution ever lands, this is the
 *      line that needs a nesting counter.
 *
 *   5. Execution order of a WRITTEN <script>. In a spec browser a written
 *      blocking script executes when the parser reaches it -- before the
 *      SOURCE content that follows, so before a later source script. Here
 *      all parse-collected scripts run first (browser.c collected them
 *      before the first write happened), then the drain runs written ones
 *      in tree order. Position is exact, timing is one drain late; the
 *      python.org shape -- write, then a LATER script reads the written
 *      node back -- works because later source scripts run after the
 *      writing script's nodes are already in the tree, synchronously.
 *
 *   6. Synchronous visibility. The spec defers parsing written input until
 *      the executing script yields, so in a spec browser the write's own
 *      script cannot see its nodes on the next statement. Here insertion is
 *      immediate, so it can. The observable difference is confined to the
 *      writing script itself (every later observer sees the same tree),
 *      and immediate is strictly the more useful choice for pages that
 *      write-then-query.
 *
 *   7. A written <script> must EXECUTE, and the parse door above is built
 *      to stop exactly that. el_set_html stamps every fragment-parsed
 *      script "already started" (js_dom.c's mark_fragment_scripts_started,
 *      the 2026-08-30 data-block fix) -- CORRECT for innerHTML, where
 *      Chrome executes an injected script never, and wrong here:
 *      document.write input goes through the MAIN parser in a real
 *      browser, and the main parser does not set that flag. The stamp is
 *      why the installer REVIVES each written script by rebuilding the
 *      element -- createElement('script') + copied attributes + copied
 *      text, swapped in inside the detached scratch -- before the subtree
 *      is inserted into the connected document, where offer_scripts()
 *      finds it unstamped, enqueues it, and the drain runs it. That is the
 *      createElement('script')+appendChild path the corpus already
 *      exercises, not a new execution door. A C-level un-stamp API was
 *      rejected: it would live in dom.c (not this file) and any caller
 *      could use it to resurrect a genuine innerHTML script, undoing the
 *      data-block fix from the outside. The rebuild changes the script
 *      element's identity, which no page can observe: it happens
 *      synchronously inside the write call, before any handle to either
 *      the old or the new node has escaped to page code.
 *      [The claim this corrects -- "a written <script> reaches
 *      offer_scripts()' enqueue" by insertion alone -- was true when this
 *      section was written (2026-08-30 13:14) and was falsified forty
 *      minutes later by the stamp above landing in js_dom.c; it is kept
 *      here because the git history will happily show it both ways.]
 *
 * WHAT ELSE IS DELIBERATELY NOT HERE: document.open()/document.close() (see
 * trap 1); writes into a DOMParser document or a frame document (this
 * installs on the PAGE document only -- `G.document` -- and js_domparser.c's
 * documents never pass the `('write' in D)` absence test anyway); entity
 * re-processing differences that only a streaming tokenizer positioned
 * mid-token would show. Parsing of the written string itself IS the real
 * spec machinery, not a scanner: it rides el_set_html's
 * html_parse_fragment() with a <body> context element, the WHATWG fragment
 * parsing algorithm this tree already scores against html5lib. */
"function installDocWrite() {\n"
   /* The legacy tree-splice helper revives scripts as dynamic nodes. An
    * active CSP-owned child requires real parser provenance and insertion
    * timing, so this helper must stay absent there until that is implemented. */
"  if (!__legacyMarkup) return;\n"
"  var D = G.document;\n"
"  if (!D || ('write' in D) || typeof D.createElement !== 'function') return;\n"
"  if (typeof G.__currentScriptNode !== 'function') return;\n"
   /* at: the in-place cursor -- the script element on its first write, the
      last node IT wrote afterwards; bodyAt: the head-written position in
      body, deliberately GLOBAL (trap 3). forScript keys the reset. */
"  var forScript = null, at = null, bodyAt = null;\n"
"  var tagOf = function (n) {\n"
"    return (n && n.tagName) ? String(n.tagName).toLowerCase() : '';\n"
"  };\n"
"  var inHead = function (n) {\n"
"    for (var p = n; p; p = p.parentNode) {\n"
"      var t = tagOf(p);\n"
"      if (t === 'body') return false;\n"
"      if (t === 'head') return true;\n"
"    }\n"
"    return false;\n"
"  };\n"
"  var make = function (lineFeed) {\n"
"    return function () {\n"
"      var s = '';\n"
"      for (var i = 0; i < arguments.length; i++) s += String(arguments[i]);\n"
"      if (lineFeed) s += '\\n';\n"
"      var cs = G.__currentScriptNode();\n"
#ifdef JS_DOCWRITE_AFTERLOAD
      /* THE NEGATIVE CONTROL'S WRONG IMPLEMENTATION, compiled in only for
       * the control build (make test-docwrite-afterload-red). This is the
       * tempting shape trap 1 refuses: after-load writes appended at body
       * end. It exists so the gate's DW-DOM-ORDER2 / no-orange assertions
       * can be WATCHED FAILING -- a control that has never failed is not
       * evidence -- and compiles away entirely in the shipped browser. */
"      if (!cs || !cs.parentNode) {\n"
"        if (D.body && s !== '') {\n"
"          var sc = D.createElement('body');\n"
"          sc.innerHTML = s;\n"
"          for (var c = sc.firstChild; c; ) {\n"
"            var nx = c.nextSibling;\n"
"            D.body.appendChild(c);\n"
"            c = nx;\n"
"          }\n"
"        }\n"
"        return;\n"
"      }\n"
#else
      /* Trap 1 + trap 2: no current script (or it removed itself) means no
       * insertion point -- contribute nothing, throw nothing, keep the
       * page. */
"      if (!cs || !cs.parentNode) return;\n"
#endif
"      if (s === '') return;\n"
"      if (forScript !== cs) { forScript = cs; at = cs; }\n"
"      var parent, ref;\n"
"      if (inHead(cs)) {\n"
"        if (!D.body) return;\n"
"        if (!bodyAt || bodyAt.parentNode !== D.body) bodyAt = D.body.firstChild;\n"
"        parent = D.body; ref = bodyAt;\n"
"      } else {\n"
"        parent = at.parentNode;\n"
"        if (!parent) return;\n"
"        ref = at.nextSibling;\n"
"      }\n"
      /* The real parser, by the only door that gives the fragment
       * algorithm with a body context: innerHTML on a scratch <body>. The
       * scratch is never connected, so its own mutations mark nothing; the
       * insertBefore moves below mark the REAL destination and that is what
       * invalidates layout. */
"      var scratch = D.createElement('body');\n"
"      scratch.innerHTML = s;\n"
      /* Then revive every <script> the parse just killed (trap 7 below).
       * next is captured BEFORE reviving a child because replaceChild
       * detaches the old node and a detached node's nextSibling is null --
       * without it the walk would silently stop at the first written
       * script, skipping any markup after it. */
"      var revive = function (n) {\n"
"        if (!n || n.nodeType !== 1) return;\n"
"        if (tagOf(n) === 'script') {\n"
"          try {\n"
"            var f = D.createElement('script');\n"
"            var aa = n.attributes;\n"
"            for (var q = 0; q < aa.length; q++)\n"
"              f.setAttribute(aa[q].name, aa[q].value);\n"
"            f.textContent = n.textContent;\n"
"            if (n.parentNode) n.parentNode.replaceChild(f, n);\n"
"          } catch (e) { try { G.reportError(e); } catch (e2) {} }\n"
"        }\n"
"        for (var c = n.firstChild; c; ) {\n"
"          var nx = c.nextSibling;\n"
"          revive(c);\n"
"          c = nx;\n"
"        }\n"
"      };\n"
"      revive(scratch);\n"
"      var kids = [], k = scratch.firstChild;\n"
"      while (k) { kids.push(k); k = k.nextSibling; }\n"
"      for (var j = 0; j < kids.length; j++) {\n"
"        parent.insertBefore(kids[j], ref);\n"
"        ref = kids[j];\n"
"      }\n"
"      if (kids.length) { if (inHead(cs)) bodyAt = ref; else at = ref; }\n"
"    };\n"
"  };\n"
"  def(D, 'write', make(false));\n"
"  def(D, 'writeln', make(true));\n"
"}\n"

/* ==== <iframe>: a nested browsing context =================================
 * THE TERMINATION QUESTION FOR THIS FEATURE IS THE LOAD EVENT. A page that
 * creates an iframe and waits for its onload must get exactly one, on every
 * path -- success, failure, refusal -- or it waits forever. Every branch below
 * ends in settle(), and settle() always schedules exactly one 'load'.
 *
 * WHAT THIS DELIBERATELY IS NOT: a second JSRuntime. ~~There is exactly one
 * JSContext for the whole page (js_page.h), so no script ever runs "as" a
 * frame -- a frame's document is DATA, not a second program.~~ THE SECOND
 * HALF OF THAT IS NO LONGER TRUE AND THE CORRECTION IS KEPT BESIDE IT,
 * because it is the sentence someone will arrive holding. A frame's document
 * now gets a second JSCONTEXT (js_frame.c, reached from settle() below), on
 * the page's ONE runtime -- which is the opposite of js_worker.c's second
 * RUNTIME and for the opposite reason: worker values must not cross, and
 * same-origin frame values MUST (`frame.contentDocument.body` and the
 * parent's handle to that node have to be the same object). A second context
 * also delivers, by mechanism rather than simulation, the one property the
 * measured specimen is actually reaching for -- fresh intrinsics the page has
 * not monkey-patched, which is precisely what an about:blank detection frame
 * is built to obtain. What a frame still does NOT get is a DOM of its own:
 * `document` inside a frame script is a ReferenceError, stated, and the
 * measured reason is in settle()'s own comment (js_dom.c:4364).
 *
 * The rest of this paragraph stands unchanged and is now load-bearing for a
 * second reason: contentDocument is built through `new DOMParser().parseFromString(...)`
 * rather than through js_dom.c's own document: js_dom.c is hardwired to ONE
 * live document via file statics (g_root/g_document and everything the
 * mutation-invalidation record touches), and reaching into that for a second,
 * navigable document is a real de-singleton project this pass does not take
 * on. DOMParser's `dp_cid` wrapper already IS a second, independent document
 * representation with its own recycle-safe {node,serial} handles -- reusing
 * it here costs nothing new and, critically, cannot cross-contaminate
 * js_dom.c's dirty-scope tracking: a frame document has none to leak into,
 * and that survives the frame document becoming MUTABLE (js_domparser.c grew
 * createElement/createTextNode/appendChild/insertBefore/removeChild/
 * setAttribute/innerHTML=/getElementsByTagName for js_frame.c) precisely
 * because those mutations are dp_arena's, not js_dom.c's -- the two document
 * representations share no statics at all, which is the property that made
 * dp_cid the right thing to reuse and would not have held for any shortcut
 * through js_dom.c.
 * The honest cost, restated for what it is TODAY: contentDocument has
 * DOMParser's surface (getElementById, querySelector(All), traversal,
 * textContent, plus the mutation methods above) and not the full Document
 * interface -- no live NodeLists, no events, no DocumentFragment, no
 * innerHTML getter. ~~and nothing inside a frame document ever runs a
 * <script> of its own -- refused by construction~~ -- that clause is retired:
 * an inline <script> in a frame document now runs, in the frame's own
 * JSContext, whether it arrived by insertion or was already in the parsed
 * markup. A <script src=...> is still refused, BY NAME on the console rather
 * than by construction (js_frame.c's cut list, item 6).
 *
 * SAME-ORIGIN IS THE GATE, NOT A COURTESY. A cross-origin src is refused
 * before any fetch is attempted (checked against the parsed URL's origin, not
 * against a substring of it), contentWindow/contentDocument throw
 * SecurityError afterwards, and `load` still fires once on the element --
 * the page's own handler must see the refusal, because a silent blank iframe
 * is indistinguishable from a slow one and is exactly the shape that makes a
 * page wait forever on a promise that was never listening to `load` at all.
 *
 * XML/XHTML/SVG/binary responses are answered THE SAME WAY js_domparser.c
 * already answers them for DOMParser directly, not refused into a silent
 * blank: this tree has no XML parser, so `parseDocAs()` hands the response's
 * real MIME type to `new DOMParser().parseFromString(...)`, which returns the
 * same HTML-shaped <parsererror> document js_domparser.c has always produced
 * for identical bytes. contentDocument is that document, readable, same
 * origin; `load` fires once. Before this pass the two doors disagreed: the
 * iframe door refused into `contentDocument === null` with no throw, which is
 * exactly the silent-blank-same-origin-looking shape this file's own header
 * comment warns makes a page wait forever.
 *
 * `sandbox` is refused by name for the same reason js_domparser.c refuses
 * XML by name: honouring the attribute's PRESENCE while ignoring its
 * RESTRICTIONS is a security contract answered wrongly, in a process that
 * runs adversary code with no ASLR and no stack canaries. Absent is safer
 * than present-and-wrong, so an iframe carrying `sandbox` navigates nowhere
 * -- and, unlike the XML case, contentWindow/contentDocument THROW a
 * SecurityError naming sandbox rather than answering null: a silently-null
 * same-origin-shaped document is indistinguishable from "not loaded yet",
 * the one shape this whole feature is measured against.
 *
 * INSERTION USED TO BE CAUGHT FOUR WAYS AND THAT WAS NOT ENOUGH. The original
 * shape here wrapped appendChild/insertBefore/replaceChild/setAttribute plus
 * innerHTML and called that "every DOM-mutation path a real page uses to
 * bring an iframe into being". It was refuted with a concrete hang:
 * ParentNode.append/prepend, ChildNode.before/after/replaceWith,
 * Element.insertAdjacentHTML and the outerHTML setter are SEPARATE C entry
 * points in this engine (js_dom_iface.inc: el_insert_variadic /
 * el_set_outerHTML / el_insertAdjacentHTML) that were never wrapped, so
 * `new Promise(res => { f.onload = res; container.append(f); })` never
 * settled -- parentNode was set, the element was really in the document, and
 * __frameInit was never created, because nothing had ever called
 * onInsert()/initFrame() for that path.
 *
 * THE CHOKE-POINT QUESTION, ANSWERED HONESTLY RATHER THAN ASSUMED: is there
 * one C place to hook instead of seven more JS wraps? Almost.
 * el_insert_variadic (js_dom_iface.inc) is ALREADY the single implementation
 * behind append/prepend/before/after/replaceWith/replaceChildren, and it
 * calls the SAME insert_run() that appendChild/insertBefore/replaceChild use
 * (js_dom.c) -- eight of the ten JS-reachable insertion surfaces already
 * share one C function. But insert_run() is deep in js_dom.c's tree-mutation
 * core, mid-refactor by another line of work as this was written, and
 * bridging a C-level "an iframe just became reachable" signal back into this
 * file's JS closures (gen counters, fetch, srcdoc, the once-ever guard) means
 * either a new C->JS callback registration (this file already reads that
 * pattern is fragile: js_dom_set_script_sink has to be called from BOTH
 * browser.c and js_page.c or the WPT runner sees no script sink at all) or
 * moving JS-owned iframe state into C. Either is a real project, not a
 * one-file fix, and outerHTML/insertAdjacentHTML don't even go through
 * insert_run -- they share a SECOND C path (insert_markup) that inserts
 * parsed markup directly via dom_insert_before(), so "the" choke point is
 * really two, not one. So: wrap the seven doors, in THIS file only, using
 * the exact wrapMethod('ifr', ...) mechanism already proven for the first
 * four -- it operates on the JS-visible property regardless of whether the
 * native implementation is one C function or three, so it needs no change to
 * js_dom.c/js_dom_iface.inc and cannot collide with work in progress there.
 *
 * DOOR NUMBER EIGHT, NAMED RATHER THAN DISCOVERED LATER: grep for every
 * JS-reachable call site of dom_insert_before() found exactly two --
 * insert_run() (js_dom.c, magic-dispatched by both the old wraps and the new
 * ones below) and insert_markup() (js_dom_iface.inc, behind outerHTML= and
 * insertAdjacentHTML, both wrapped below). The remaining dom_insert_before()
 * callers are html_tree.c's parser (the initial parse, already covered by
 * the existing_iframes pass at the bottom of this function) and forms.c's
 * native text-editing splices (contenteditable caret/line-break handling --
 * not a script-reachable insertion primitive). Two APIs were deliberately
 * ABSENT rather than half-built and would be door eight if either is ever
 * added: Range.insertNode/surroundContents (js_forms.c names them as
 * withheld) and ~~document.write/writeln (not implemented at all, zero
 * hits)~~.
 *
 * THE STRUCK CLAIM IS KEPT BESIDE ITS CORRECTION because somebody is going
 * to arrive holding it. "Zero hits" was true of the webapi probe's seven
 * pages and was refuted the moment the scoreboard measured real navigation:
 * tests/scoreboard/collapse-recount.md records `TypeError: write is not a
 * function (it is undefined)` from www.2345.com/#inline-script-4:7 (page
 * painted minus its written half, 2 exceptions) and from python.org inside
 * an event listener (1 exception), and 0820-g4/qwen.json catches an
 * iconfont injector calling document.write('<style>...') in a try block.
 * With the method ABSENT each of those pages took a hard TypeError that
 * killed its script -- the same trap shape as a null-returning
 * getContext, except here absence was NOT the safe side. document.write and
 * document.writeln are implemented now -- installDocWrite(), the section
 * above this one, with the spec mapping and the deviation list in its own
 * comment.
 *
 * THE RULE, AND HOW THE NEW DOOR PAID IT: "it must call through
 * insert_run/insert_markup or be added to this file's wrap list in the SAME
 * commit, not after." installDocWrite does BOTH, in the commit that added
 * it: it inserts by calling parent.insertBefore() -- the prototype method
 * installIframes wraps -- so an <iframe> inside written markup reaches
 * onInsert()/initFrame through the wrap, and at C level the call lands in
 * insert_run(), where js_dom.c's offer_scripts() enqueue picks up a
 * written <script> for run_pending_inserted_scripts() -- once the
 * installer has revived it, because the innerHTML parse stamps fragment
 * scripts dead and only the rebuilt element is enqueueable (trap 7 in the
 * section above). Nothing inserts written nodes behind those doors.
 * Range.insertNode/surroundContents remain absent, still js_forms.c's to
 * name.
 *
 * wrapMethod('ifr', ...) (see its own comment above for why each method is
 * wrapped where it is OWNED, not on a shared prototype the interface
 * hierarchy since gave up) now covers every JS-reachable DOM-mutation path a
 * real page uses to bring an iframe into being, synchronously -- which is
 * what makes `frame.contentWindow.DOMException` readable on the very next
 * statement after `body.appendChild(frame)`, the exact shape url/failure.html
 * exercises. The contentWindow/contentDocument GETTERS ALSO call initFrame()
 * before reading state, so even an iframe reached through some path this
 * file still did not think to wrap would still answer correctly the instant
 * something asks -- the getters are the backstop, the method wraps are what
 * let `onload` alone (no property ever read) still fire without the page
 * reaching in first. Every one of the wraps funnels through onInsert(),
 * not initFrame() directly, so a RE-insertion of an element already
 * navigated once (remove() then appendChild() of the same node -- probe A6)
 * re-navigates instead of hitting initFrame's once-ever guard and going
 * silent. And `iframe.src = url` / `iframe.srcdoc = html` (probes A3/A4) are
 * not a fifth path at all: the property setters below are defined in terms
 * of `this.setAttribute(...)`, so they run through the setAttribute wrap
 * rather than deciding navigation a second way -- see the comment at that
 * redefinition for why a second, independent setter would have been the
 * wrong fix. */
"function installIframes() {\n"
"  var D = G.document;\n"
"  if (!D || typeof D.createElement !== 'function') return;\n"
"  var IFP = null;\n"
"  try { IFP = Object.getPrototypeOf(D.createElement('iframe')); } catch (e) {}\n"
"  if (!IFP || Object.prototype.hasOwnProperty.call(IFP, 'contentWindow')) return;\n"
   /* One record per element, an own non-enumerable expando -- lives exactly as
      long as the element does. ~~It needs no C-side lifetime hook.~~ That old
      claim was refuted by WPT creating and removing more than eight sequential
      iframes: the record itself keeps `doc` reachable, so DOMParser's finalizer
      cannot free js_frame.c's JSContext and its fixed 8-slot table fills. The
      release helpers below explicitly end that context at removal/navigation;
      js_page_close remains the backstop. `gen` is the in-flight-load fence:
      startLoad bumps it before
      it does anything async, and every continuation checks its own captured
      gen against the current one before touching anything -- a src changed or
      an element removed mid-fetch is a stale continuation that settles into
      a no-op, not a race. */
"  var rec = function (el) {\n"
"    if (!Object.prototype.hasOwnProperty.call(el, '__frame')) {\n"
"      try {\n"
"        Object.defineProperty(el, '__frame', { value: { doc: null, win: null, blocked: null, gen: 0 },\n"
"                                                enumerable: false, configurable: false, writable: false });\n"
"      } catch (e) { return null; }\n"
"    }\n"
"    return el.__frame;\n"
"  };\n"
   /* Connectedness by walking parentNode to the Document, not
      Node.prototype.contains/isConnected -- neither is guaranteed present,
      and parentNode is the one traversal every node answers. */
"  var connected = function (el) {\n"
"    for (var n = el; n; n = n.parentNode) if (n === D) return true;\n"
"    return false;\n"
"  };\n"
   /* One authoritative context-release door. It deliberately does not clear
      r.win: committed navigation keeps WindowProxy identity while its
      document changes. A DOM removal clears r.win separately because that
      browsing context is destroyed; reinsertion creates a new one. The C
      call is idempotent, so a later DOMParser finalizer is a harmless no-op. */
"  var releaseDoc = function (r) {\n"
"    if (!r || !r.doc) return;\n"
"    var old = r.doc;\n"
"    r.doc = null;\n"
"    if (typeof G.__frameRelease === 'function') {\n"
"      try { G.__frameRelease(old); } catch (e) {}\n"
"    }\n"
"  };\n"
"  var fireLoad = function (el) {\n"
"    setTimeout(function () { try { el.dispatchEvent(new G.Event('load')); } catch (e) {} }, 0);\n"
"  };\n"
"  var parseDoc = function (html) {\n"
"    try { return new G.DOMParser().parseFromString(html == null ? '' : String(html), 'text/html'); }\n"
"    catch (e) { return null; }\n"
"  };\n"
   /* Same door as parseDoc, but told the real MIME type, so a non-HTML
      response gets the SAME <parsererror> document js_domparser.c already
      hands back to `new DOMParser().parseFromString(sameBytes, sameMime)` --
      the two doors are made to agree rather than the iframe door refusing
      into a silent-blank shape the DOMParser door has never produced. */
"  var parseDocAs = function (text, mime) {\n"
"    try { return new G.DOMParser().parseFromString(text == null ? '' : String(text), mime || 'text/html'); }\n"
"    catch (e) { return null; }\n"
"  };\n"
   /* `blocked` names WHY contentWindow/contentDocument must throw, and is the
      only thing the getters and makeWindow() need to build the right
      SecurityError message -- 'cross-origin' for a real navigation refusal,
      'sandbox' because this engine does not honour the attribute's
      allowances and a silent same-origin-shaped blank would be the exact
      hang shape the whole feature is measured against. */
"  var blockedErr = function (r) {\n"
"    if (r.blocked === 'passive') return new G.DOMException('Passive embedded documents have no script-visible browsing context.', 'NotSupportedError');\n"
"    if (r.blocked === 'sandbox')\n"
"      return new G.DOMException('Blocked access to a sandboxed frame: this engine does not honour the sandbox attribute, so access is refused rather than silently granted.', 'SecurityError');\n"
"    return new G.DOMException('Blocked a frame from accessing a cross-origin frame.', 'SecurityError');\n"
"  };\n"
   /* The one exit every load path funnels through. `doc` set + `blocked` null
      is a real, readable document (about:blank, srcdoc, a same-origin fetch,
      a failed fetch's empty stand-in, or a non-HTML response answered the
      way DOMParser would answer it -- all spec-adjacent "this frame has a
      document now", not five different answers); `blocked` non-null is the
      one that makes contentWindow/contentDocument throw, doc is irrelevant
      and left null. Whichever it is, exactly one load. */
"  var settle = function (el, r, gen, doc, blocked) {\n"
"    if (r.gen !== gen) return;\n"
       /* A committed navigation replaces the browsing context. Keeping the
          old context until page close was not a cache: after eight src/srcdoc
          changes the ninth document had no JS realm. Release only at commit,
          not at fetch start, so the old document remains active while an
          asynchronous navigation is still in flight. */
"    if (r.doc && r.doc !== doc) releaseDoc(r);\n"
"    r.doc = doc; r.blocked = blocked || null;\n"
       /* ==== the frame becomes a SCRIPT HOST here, and only here ==========
          __frameAdopt (js_frame.c) gives this document a JSContext of its own
          on the page's existing runtime, so a <script> inside it runs instead
          of being data. Everything about WHICH documents may do that is
          already decided ABOVE this line and is unchanged: the cross-origin
          refusal and the sandbox refusal both `settle(el, r, gen, null,
          'cross-origin'|'sandbox')` and so arrive here with doc === null AND
          blocked set, failing this guard twice over. This call adds no origin
          logic of its own -- deliberately, because it MUST NOT become a
          second place where "may this run" is decided (one jar, two doors);
          js_platform.c's existing gate stays the only door.

          AND THE REFUSAL AT THAT GATE STAYS A REFUSAL, with a measurement
          behind it now rather than a preference: a real cross-origin frame
          would need a second DOM, and js_dom.c:4364 calls
          `JS_NewClassID(&elem_cid)` UNGUARDED inside js_dom_init, so a second
          js_dom_init does not leak the parent's g_root/g_ctx/g_document -- it
          OVERWRITES them, and the parent page's own document.getElementById
          would then search the untrusted child's tree. That failure runs in
          the direction that makes it a vulnerability, not merely a bug, so
          the boundary cannot be made real on today's DOM and the honest
          answer is to keep refusing. Said plainly, because it is the thing
          most likely to be misread as done: this closes the frame BOOTSTRAP
          that a same-origin about:blank frame is, and NOT any cross-origin
          embed -- no third-party widget, player, card field or OAuth frame
          becomes reachable through this line.

          Guarded by `typeof` rather than a C-side link edge on purpose: the
          six host source lists that build js_platform.c without js_frame.c
          (see the include) then take a JS branch that does not exist, which
          is the same "absent dependency is inert" contract the weak symbol
          gives the C half. */
"      if (doc && !blocked && typeof G.__frameAdopt === 'function') {\n"
"        try { G.__frameAdopt(doc); } catch (eAdopt) {}\n"
"      }\n"
       /* ONE JAR, TWO DOORS, closed: contentDocument.defaultView must be the
          SAME object as contentWindow (probe B1 -- WPT's own idiom, e.g.
          `doc.defaultView.DOMException`), not a second window built lazily
          and differently by whichever door gets read first. Defining it as a
          getter that shares makeWindow()'s r.win cache, rather than eagerly
          building a window every settle(), keeps a document nobody ever
          reads .contentWindow on exactly as cheap as before. */
"      if (doc) {\n"
"        try {\n"
       /* This wrapper is live only while the current connected frame owns it.
        * The parent's hasFocus includes the WM/chrome state; activeElement
        * adds the descendant condition. A sibling frame must stay false.
        * Nested frame navigation/input remains outside this frame backend. */
"          Object.defineProperty(doc, 'hasFocus', { configurable: true, writable: true,\n"
"            value: function () {\n"
"              if (this !== doc) throw new TypeError('Document.hasFocus requires its Document');\n"
"              return !!(!r.blocked && r.doc === doc && el.isConnected &&\n"
"                typeof D.hasFocus === 'function' && D.hasFocus() && D.activeElement === el);\n"
"            } });\n"
"          Object.defineProperty(doc, 'defaultView', { configurable: true, enumerable: true,\n"
"            get: function () {\n"
"              if (r.blocked) return null;\n"
"              if (!r.win) r.win = makeWindow(el, r);\n"
"              return r.win;\n"
"            } });\n"
"        } catch (e5) {}\n"
"      }\n"
       /* Named refusals, not silent absence -- and the FIRST of the two is no
          longer a refusal at all when js_frame.c is linked, which is why the
          line is split rather than left standing. A console line that says a
          script "will never run" while it is at that moment running is worse
          than no line: it is an instrument that lies, and it would be read by
          exactly the person trying to work out why their frame behaved
          unexpectedly. So the message is decided by the same `typeof` the
          adopt call above uses, not by belief about what is linked.
          The nested-<iframe> half is unchanged and still a true refusal:
          js_frame.c does not re-scan a frame document for nested browsing
          contexts (its own cut list, item 3). Neither branch is a termination
          concern -- settle() has already committed to firing exactly one
          `load` below whichever way this goes. */
"      if (doc && !blocked) {\n"
"        try { if (doc.querySelector && doc.querySelector('script'))\n"
"          console.log(typeof G.__frameAdopt === 'function'\n"
"            ? '[iframe] a <script> inside this frame document runs in the frame\\'s OWN JSContext ' +\n"
"              '(fresh intrinsics, no document/DOM of its own) -- see js_frame.c'\n"
"            : '[iframe] a <script> inside a frame document will never run: ' +\n"
"              'frame documents are parsed data (DOMParser-backed), not a second program'); } catch (e3) {}\n"
"        try { if (doc.querySelector && doc.querySelector('iframe'))\n"
"          console.log('[iframe] a nested <iframe> inside a frame document will not be navigated: ' +\n"
"                      'frame documents are not re-scanned for nested browsing contexts'); } catch (e4) {}\n"
"      }\n"
"    fireLoad(el);\n"
"  };\n"
"  var startLoad = function (el, url) {\n"
"    var r = rec(el);\n"
"    if (!r) return;\n"
"    var gen = ++r.gen;\n"
#ifdef FRAME_SANDBOX_URL_BYPASS
/* Negative control: restore the old malformed-URL return before refusal. */
"    try { new G.URL(String(url), D.baseURI || (G.location && G.location.href) || ''); }\n"
"    catch (badURL) { settle(el, r, gen, parseDoc(''), null); return; }\n"
#endif
/* Attribute setters enter startLoad directly. Refuse before invalid-URL
 * fallback too, otherwise a sandboxed frame can acquire an unrestricted blank
 * document by assigning malformed src after insertion. */
"    if (el.hasAttribute && el.hasAttribute('sandbox')) {\n"
"      console.log('[iframe] refused: sandbox attribute is not honoured by this engine, src=' + String(url));\n"
"      settle(el, r, gen, null, 'sandbox');\n"
"      return;\n"
"    }\n"
"    var base = D.baseURI || (G.location && G.location.href) || '';\n"
"    var u = null;\n"
"    try { u = new G.URL(String(url), base); } catch (e) {}\n"
       /* An unparseable src is not a navigation at all in real browsers --
          treated here the same as no src: about:blank. */
"    if (!u) { settle(el, r, gen, parseDoc(''), null); return; }\n"
/* Network frames now have a separate native document owner. Do not ALSO fetch
 * their bytes through parent fetch/CORS, expose a DOMParser copy, execute child
 * scripts in the compatibility realm, or fabricate a load event before pixels
 * exist. The boolean is a native prelude argument captured before author JS. */
"    if (__passiveFrames && (u.protocol === 'http:' || u.protocol === 'https:')) {\n"
"      if (r.doc) releaseDoc(r); r.doc = null; r.blocked = 'passive'; return;\n"
"    }\n"
"    var top = (G.location && G.location.origin) || '';\n"
"    if (u.origin !== top) {\n"
"      console.log('[iframe] cross-origin navigation refused: ' + u.origin + ' (top is ' + top + ')');\n"
"      settle(el, r, gen, null, 'cross-origin');\n"
"      return;\n"
"    }\n"
"    if (typeof G.fetch !== 'function') { settle(el, r, gen, parseDoc(''), null); return; }\n"
"    var alive = function () { return r.gen === gen && connected(el); };\n"
"    G.fetch(u.href).then(function (resp) {\n"
"      if (!alive()) return;\n"
"      if (!resp.ok) { settle(el, r, gen, parseDoc(''), null); return; }\n"
"      var ct = '';\n"
"      try { ct = (resp.headers && typeof resp.headers.get === 'function') ? (resp.headers.get('content-type') || '') : ''; } catch (e2) {}\n"
"      var kind = ct.split(';')[0].replace(/^\\s+|\\s+$/g, '').toLowerCase();\n"
"      var mime = kind || 'text/html';\n"
"      if (kind && kind !== 'text/html')\n"
"        console.log('[iframe] content-type \\'' + kind + '\\' is not text/html -- this engine has no parser for it, ' +\n"
"                    'answering the same way DOMParser answers identical bytes');\n"
"      return resp.text().then(function (t) { if (alive()) settle(el, r, gen, parseDocAs(t, mime), null); });\n"
"    }).catch(function () { if (alive()) settle(el, r, gen, parseDoc(''), null); });\n"
"  };\n"
"  var loadSrcdoc = function (el, html) {\n"
"    var r = rec(el);\n"
"    if (!r) return;\n"
"    var gen = ++r.gen;\n"
"    if (el.hasAttribute && el.hasAttribute('sandbox')) {\n"
"      console.log('[iframe] refused: sandbox attribute is not honoured by this engine (srcdoc)');\n"
"      settle(el, r, gen, null, 'sandbox');\n"
"      return;\n"
"    }\n"
"    settle(el, r, gen, parseDoc(html), null);\n"
"  };\n"
   /* The navigation DECISION, factored out of the once-ever guard below so it
      can be run again on a RE-insertion (remove() then appendChild() of the
      SAME element -- probe A6) without re-reading the guard. Real browsers
      run the iframe/frame insertion steps on every insertion, not only the
      first; the current content attributes are the source of truth each
      time, exactly as they are the first time. */
"  var navigate = function (el) {\n"
#ifndef FRAME_SANDBOX_BLANK_BYPASS
/* An empty/missing src never reached startLoad/loadSrcdoc's sandbox guards.
 * That granted a readable document and ran injected child scripts while the
 * element advertised sandbox. Refuse before choosing ANY source, including
 * about:blank; this does not claim to implement sandbox token permissions. */
"    if (el.hasAttribute && el.hasAttribute('sandbox')) {\n"
"      var blockedRec = rec(el);\n"
"      if (blockedRec) settle(el, blockedRec, ++blockedRec.gen, null, 'sandbox');\n"
"      return;\n"
"    }\n"
#endif
"    var sd = el.getAttribute ? el.getAttribute('srcdoc') : null;\n"
"    var sr = el.getAttribute ? el.getAttribute('src') : null;\n"
"    if (sd !== null && sd !== '') { loadSrcdoc(el, sd); return; }\n"
"    if (sr !== null && sr !== '') { startLoad(el, sr); return; }\n"
"    var r = rec(el);\n"
"    if (r) settle(el, r, r.gen, parseDoc(''), null);\n"
"  };\n"
   /* Runs once per element, ever -- the '__frameInit' expando is the guard,
      same shape as '__frame' above. Both the mutation wraps and the
      contentWindow/contentDocument getters call this before doing anything
      else, so it is idempotent by construction rather than by convention. */
"  var initFrame = function (el) {\n"
"    if (!el || Object.prototype.hasOwnProperty.call(el, '__frameInit')) return;\n"
"    try {\n"
"      Object.defineProperty(el, '__frameInit', { value: true, enumerable: false, configurable: false, writable: false });\n"
"    } catch (e) { return; }\n"
"    navigate(el);\n"
"  };\n"
   /* Called from every INSERTION path (parser-built, appendChild family,
      innerHTML). First insertion ever: same as initFrame. Re-insertion of an
      element this file has already navigated once: initFrame's guard would
      make that a no-op, which is probe A6's hang -- an iframe removed and put
      back never fires load again. So a re-insertion re-runs navigate()
      directly, using whatever src/srcdoc the element carries right now. */
"  var onInsert = function (el) {\n"
"    if (Object.prototype.hasOwnProperty.call(el, '__frameInit')) navigate(el);\n"
"    else initFrame(el);\n"
"  };\n"
   /* Not a Window: a plain object in the ONE realm this page has. document/
      location are accessors so the cross-origin throw happens at read time,
      matching the termination bar's row 6 -- the throw is what a page's own
      try/catch can observe, where a silently-null property cannot be told
      apart from "not loaded yet". */
"  var makeWindow = function (el, r) {\n"
"    var win = {};\n"
"    try {\n"
"      Object.defineProperty(win, 'document', { enumerable: true,\n"
"        get: function () { if (r.blocked) throw blockedErr(r); return r.doc || null; } });\n"
"      Object.defineProperty(win, 'location', { enumerable: true,\n"
"        get: function () { if (r.blocked) throw blockedErr(r); return G.location; },\n"
"        set: function (v) {\n"
"          var base = D.baseURI || (G.location && G.location.href) || '';\n"
"          try { new G.URL(String(v), base); }\n"
             /* This is the synchronous throw url/failure.html's 188 cases
                assert: an unparseable assignment to contentWindow.location
                never reaches startLoad at all. */
"          catch (e) { throw new G.DOMException(\"'\" + v + \"' is not a valid URL.\", 'SyntaxError'); }\n"
"          startLoad(el, String(v));\n"
"        } });\n"
"    } catch (e) {}\n"
"    win.parent = G; win.top = G; win.self = win; win.frameElement = el;\n"
"    win.DOMException = G.DOMException; win.Node = G.Node; win.Element = G.Element; win.Document = G.Document;\n"
       /* Still a terminating no-op, but the REASON changed and the old one is
          now false, so it is restated rather than left to rot: it used to be
          "no script ever runs as this window", and with js_frame.c linked a
          script does run for this frame. What remains true is narrower and is
          the honest statement of a gap: `win` is a plain object built in the
          PARENT's realm, and js_frame.c's frame context has no reference to
          it, so nothing inside the frame can ever addEventListener('message')
          on this object. Cross-context message delivery therefore does not
          exist in either direction and this stays a no-op rather than a queue
          that fills forever. Closing it means contentWindow BEING the frame's
          own global object instead of this stand-in -- a real change to what
          this getter returns, and the next thing to build here, not something
          to half-wire by making postMessage enqueue to nobody. */
"    win.postMessage = function () {};\n"
"    return win;\n"
"  };\n"
"  Object.defineProperty(IFP, 'contentWindow', { configurable: true, get: function () {\n"
"    initFrame(this);\n"
"    var r = rec(this);\n"
"    if (!r) return null;\n"
"    if (r.blocked === 'passive' && __embeddedWindow) return __embeddedWindow(this);\n"
"    if (r.blocked) throw blockedErr(r);\n"
"    if (!r.win) r.win = makeWindow(this, r);\n"
"    return r.win;\n"
"  } });\n"
"  Object.defineProperty(IFP, 'contentDocument', { configurable: true, get: function () {\n"
"    initFrame(this);\n"
"    var r = rec(this);\n"
"    if (!r) return null;\n"
"    if (r.blocked === 'passive') return null;\n"
"    if (r.blocked) throw blockedErr(r);\n"
"    return r.doc || null;\n"
"  } });\n"
   /* Shared by every insertion path below: the element itself (if it is an
      iframe) plus every iframe descendant, each handed to onInsert() so a
      first insertion navigates and a RE-insertion (probe A6) re-navigates. */
"  var scan = function (node) {\n"
"    if (!node || node.nodeType !== 1) return;\n"
"    try {\n"
"      if (String(node.tagName || '').toLowerCase() === 'iframe') onInsert(node);\n"
"      if (typeof node.querySelectorAll === 'function') {\n"
"        var list = node.querySelectorAll('iframe');\n"
"        for (var i = 0; i < list.length; i++) onInsert(list[i]);\n"
"      }\n"
"    } catch (e) {}\n"
"  };\n"
"  ['appendChild', 'insertBefore', 'replaceChild'].forEach(function (m) {\n"
"    wrapMethod('ifr', m, function (orig, name) {\n"
"      return function () {\n"
"        var removed = name === 'replaceChild' ? listIframes(arguments[1]) : [];\n"
"        var r = orig.apply(this, arguments);\n"
"        releaseDetachedList(removed);\n"
"        scan(arguments[0]);\n"
"        return r;\n"
"      };\n"
"    });\n"
"  });\n"
"  wrapMethod('ifr', 'removeChild', function (orig) {\n"
"    return function (node) {\n"
"      var removed = listIframes(node);\n"
"      var r = orig.apply(this, arguments);\n"
"      releaseDetachedList(removed);\n"
"      return r;\n"
"    };\n"
"  });\n"
   /* REFUTED 2026-08-28: these four methods, plus setAttribute/innerHTML
      below, do NOT cover every insertion path. ParentNode.append/prepend/
      replaceChildren and ChildNode.before/after/replaceWith are separate,
      VARIADIC C entry points (el_insert_variadic in js_dom_iface.inc -- one
      C function behind all six, magic-dispatched, but six distinct
      JS-visible properties on two different prototypes, each needing its own
      wrap). Unlike appendChild/insertBefore/replaceChild, the new node(s) are
      not always arguments[0]: append/prepend take any number of (Node or
      string) arguments and before/after/replaceWith take the SAME variadic
      shape for the new siblings, so every argument is scanned, not just the
      first -- a string argument is a text node and scan() no-ops on it
      (nodeType check), so this costs nothing on the common one-node call. */
   /* insertAdjacentElement joins this loop and NOT the one above, because the
      node it inserts is arguments[1] -- arguments[0] is the position string.
      That is exactly the shape this loop already handles: it scans EVERY
      argument, and scan() no-ops on a non-node (the nodeType check), which is
      how append/prepend already tolerate their string arguments. Landing it
      here in the same commit as the method itself is the rule this file states
      thirty lines above: a new insertion door must call through
      insert_run/insert_markup OR be added to this list, "not after". It does
      both -- js_dom_iface.inc routes it through insert_run -- because the
      C-level choke point does not signal this file, so the C routing buys the
      cascade-refusal and cycle checks and the wrap buys the iframe init. */
"  ['append', 'prepend', 'replaceChildren', 'before', 'after', 'replaceWith',\n"
"   'insertAdjacentElement'].forEach(function (m) {\n"
"    wrapMethod('ifr', m, function (orig, name) {\n"
"      return function () {\n"
"        var removed = (name === 'replaceChildren' || name === 'replaceWith')\n"
"          ? listIframes(this) : [];\n"
"        var r = orig.apply(this, arguments);\n"
"        releaseDetachedList(removed);\n"
"        for (var i = 0; i < arguments.length; i++) scan(arguments[i]);\n"
"        return r;\n"
"      };\n"
"    });\n"
"  });\n"
"  wrapMethod('ifr', 'remove', function (orig) {\n"
"    return function () {\n"
"      var removed = listIframes(this);\n"
"      var r = orig.apply(this, arguments);\n"
"      releaseDetachedList(removed);\n"
"      return r;\n"
"    };\n"
"  });\n"
   /* innerHTML is not the only C-parser insertion door. innerHTML replaces
      ALL of `this`'s children, so scan(this) after the call only ever sees
      NEW content -- nothing pre-existing to accidentally re-navigate.
      outerHTML= and insertAdjacentHTML() do not have that property: they
      insert new markup ALONGSIDE nodes that are already there (siblings for
      outerHTML/beforebegin/afterend, existing children for afterbegin/
      beforeend), so a blind scan(parent) would find already-initialized
      iframes elsewhere in that parent and onInsert() would RE-navigate them
      -- a spurious reload of a frame this call never touched. snapshotScope
      takes a before/after snapshot of the scope's iframes and onInsert()s
      only the ones that are NEW, by object identity, so a sibling iframe
      that was already loaded is left alone. */
"  var listIframes = function (node) {\n"
"    var out = [];\n"
"    try {\n"
"      if (String(node.tagName || '').toLowerCase() === 'iframe') out.push(node);\n"
"      if (typeof node.querySelectorAll === 'function') {\n"
"        var list = node.querySelectorAll('iframe');\n"
"        for (var i = 0; i < list.length; i++) out.push(list[i]);\n"
"      }\n"
"    } catch (e) {}\n"
"    return out;\n"
"  };\n"
   /* Release only after the native mutation and only if the frame really is
      detached. That connectedness check distinguishes removal from a move:
      appendChild can detach from one parent and attach to another inside one
      native call, and destroying the context in the middle would turn a move
      into a reload. Lists are captured before destructive setters because
      replaceChildren/innerHTML may recycle their C nodes while the wrappers
      still carry the __frame record needed to release the JS context. */
"  var releaseDetachedList = function (frames) {\n"
"    for (var i = 0; i < frames.length; i++) {\n"
"      var el = frames[i], live = false;\n"
"      try { live = connected(el); } catch (e) {}\n"
"      if (live || !Object.prototype.hasOwnProperty.call(el, '__frame')) continue;\n"
"      var r = el.__frame;\n"
"      ++r.gen;\n"
"      releaseDoc(r);\n"
"      r.win = null; r.blocked = null;\n"
"    }\n"
"  };\n"
"  var scanNew = function (scope, before) {\n"
"    if (!scope) return;\n"
"    var after = listIframes(scope);\n"
"    for (var i = 0; i < after.length; i++)\n"
"      if (before.indexOf(after[i]) === -1) onInsert(after[i]);\n"
"  };\n"
"  (function () {\n"
"    var EP = ownerOf('innerHTML');\n"
"    if (!EP) return;\n"
"    var desc = Object.getOwnPropertyDescriptor(EP, 'innerHTML');\n"
"    if (!desc || typeof desc.set !== 'function') return;\n"
"    var key = '__w_ifr_innerHTML';\n"
"    if (EP[key]) return;\n"
"    try {\n"
"      Object.defineProperty(EP, key, { value: true, enumerable: false, configurable: true });\n"
"      var origSet = desc.set, origGet = desc.get;\n"
"      Object.defineProperty(EP, 'innerHTML', { configurable: true, enumerable: desc.enumerable,\n"
"        get: origGet,\n"
"        set: function (v) {\n"
"          var removed = listIframes(this);\n"
"          origSet.call(this, v);\n"
"          releaseDetachedList(removed);\n"
"          scan(this);\n"
"        } });\n"
"    } catch (e) {}\n"
"  })();\n"
   /* textContent= is another subtree-replacement door. It can insert only a
      text node, so there is nothing to scan afterwards, but every former
      iframe descendant still needs its context released. */
"  (function () {\n"
"    var NP = ownerOf('textContent');\n"
"    if (!NP) return;\n"
"    var desc = Object.getOwnPropertyDescriptor(NP, 'textContent');\n"
"    if (!desc || typeof desc.set !== 'function') return;\n"
"    var key = '__w_ifr_textContent';\n"
"    if (NP[key]) return;\n"
"    try {\n"
"      Object.defineProperty(NP, key, { value: true, enumerable: false, configurable: true });\n"
"      var origSet = desc.set, origGet = desc.get;\n"
"      Object.defineProperty(NP, 'textContent', { configurable: true, enumerable: desc.enumerable,\n"
"        get: origGet,\n"
"        set: function (v) {\n"
"          var removed = listIframes(this);\n"
"          origSet.call(this, v);\n"
"          releaseDetachedList(removed);\n"
"        } });\n"
"    } catch (e) {}\n"
"  })();\n"
   /* insertAdjacentHTML(position, html): beforebegin/afterend land among
      `this`'s SIBLINGS (scope = this.parentNode), afterbegin/beforeend land
      inside `this` (scope = this). Either way the scope may already contain
      other, already-navigated iframes this call did not touch, hence
      scanNew's before/after diff rather than scan()'s blind sweep. */
"  wrapMethod('ifr', 'insertAdjacentHTML', function (orig) {\n"
"    return function (pos) {\n"
"      var p = String(pos || '').toLowerCase();\n"
"      var scope = (p === 'beforebegin' || p === 'afterend') ? this.parentNode : this;\n"
"      var before = scope ? listIframes(scope) : [];\n"
"      var r = orig.apply(this, arguments);\n"
"      scanNew(scope, before);\n"
"      return r;\n"
"    };\n"
"  });\n"
   /* outerHTML= replaces `this` itself with parsed markup, so `this` (and any
      __frameInit it already carries) is destroyed by the call -- the scope to
      diff is `this.parentNode`, captured BEFORE the call since afterward
      `this` no longer has a live parentNode to read. Same accessor-wrap shape
      as innerHTML above (outerHTML is CGETSET_DEF, not a plain method, so
      wrapMethod's plain-assignment form does not apply). */
"  (function () {\n"
"    var EP = ownerOf('outerHTML');\n"
"    if (!EP) return;\n"
"    var desc = Object.getOwnPropertyDescriptor(EP, 'outerHTML');\n"
"    if (!desc || typeof desc.set !== 'function') return;\n"
"    var key = '__w_ifr_outerHTML';\n"
"    if (EP[key]) return;\n"
"    try {\n"
"      Object.defineProperty(EP, key, { value: true, enumerable: false, configurable: true });\n"
"      var origSet = desc.set, origGet = desc.get;\n"
"      Object.defineProperty(EP, 'outerHTML', { configurable: true, enumerable: desc.enumerable,\n"
"        get: origGet,\n"
"        set: function (v) {\n"
"          var scope = this.parentNode;\n"
"          var before = scope ? listIframes(scope) : [];\n"
"          origSet.call(this, v);\n"
"          releaseDetachedList(before);\n"
"          scanNew(scope, before);\n"
"        } });\n"
"    } catch (e) {}\n"
"  })();\n"
"  wrapMethod('ifr', 'setAttribute', function (orig) {\n"
"    return function (name) {\n"
"      var r = orig.apply(this, arguments);\n"
"      try {\n"
"        if (String(this.tagName || '').toLowerCase() === 'iframe' &&\n"
"            Object.prototype.hasOwnProperty.call(this, '__frameInit')) {\n"
"          var lname = String(name).toLowerCase();\n"
"          if (lname === 'src') startLoad(this, this.getAttribute('src') || '');\n"
"          else if (lname === 'srcdoc') loadSrcdoc(this, this.getAttribute('srcdoc') || '');\n"
"        }\n"
"      } catch (e) {}\n"
"      return r;\n"
"    };\n"
"  });\n"
   /* THE SEAM: `iframe.src = url` and `iframe.srcdoc = html` are C-level IDL
      accessors installed by js_reflect.c's refl_set(), which writes the
      content attribute through js_dom_attr_write() directly -- a SECOND door
      onto the same attribute that never passes through the setAttribute wrap
      above (probes A3/A4: property assignment fired no load, ever). The
      obvious fix -- give these two properties their OWN setter that calls
      startLoad/loadSrcdoc -- would be a THIRD door: setAttribute('src', x),
      removeAttribute('src') and a hypothetical direct-property path would
      each decide independently whether a navigation happened, the exact
      one-jar-two-doors shape this tree has paid for three times. Closing it
      for real means making property assignment BE a setAttribute call, not a
      second implementation of "did src change": the setter below is defined
      in terms of `this.setAttribute(...)`, i.e. it runs through the SAME
      wrapped method above, so there is exactly one place that ever decides a
      navigation should start. The getter is untouched (keeps refl_get's
      existing resolved-URL semantics for src). */
"  ['src', 'srcdoc'].forEach(function (attr) {\n"
"    var d = Object.getOwnPropertyDescriptor(IFP, attr);\n"
"    if (!d || typeof d.get !== 'function' || !d.configurable) return;\n"
"    var origGet = d.get;\n"
"    try {\n"
"      Object.defineProperty(IFP, attr, { configurable: true, enumerable: d.enumerable,\n"
"        get: function () { return origGet.call(this); },\n"
"        set: function (v) { this.setAttribute(attr, String(v)); } });\n"
"    } catch (e) {}\n"
"  });\n"
   /* ONE JAR, TWO DOORS: window.length used to be a constant 0 no matter how
      many <iframe>s the document held, which is the "frames" door disagreeing
      with the document's own tree, the second door. It is derived now. (Not
      done: window.frames[i] indexed access, which would need globalThis
      itself wrapped in a Proxy -- too large a blast radius for what this pass
      is worth; window.frames stays the pre-existing `=== window` alias.) */
"  try {\n"
"    Object.defineProperty(G, 'length', { configurable: true, get: function () {\n"
"      try { return D.querySelectorAll('iframe').length; } catch (e) { return 0; }\n"
"    } });\n"
"  } catch (e) {}\n"
   /* Parser-built iframes: already in the tree by the time this runs
      (js_dom_init, which builds `document` from the parsed tree, runs before
      js_platform_install -- see js_page.c's install sequence), so a page that
      never calls appendChild at all (every <iframe src=...> in the source
      HTML) still gets its load fired without the mutation wraps' help. */
"  try {\n"
"    var existing = __initialIframes === undefined ? D.querySelectorAll('iframe') : __initialIframes;\n"
"    for (var i = 0; i < existing.length; i++) initFrame(existing[i]);\n"
"  } catch (e) {}\n"
"}\n"

/* ==== ElementInternals: Element.attachInternals =============================
 * MEASURED: jsfb_matrix, "throws: TypeError: attachInternals is not a
 * function" -- keyed/plaited, keyed/ui5-webcomponents, non-keyed/
 * ui5-webcomponents, all three dying inside their base custom-element class's
 * CONSTRUCTOR (plaited: `this.#n = this.attachInternals()`; ui5-webcomponents:
 * `this._internals = this.attachInternals()`), called UNCONDITIONALLY for
 * EVERY element the framework defines -- not just form controls. A missing
 * attachInternals fails every component on the page at construction, before
 * a single row renders.
 *
 * REAL STATE, NOT A STUB THAT DOES NOTHING (rule 3): setValidity/
 * checkValidity/validity/validationMessage form a closed loop over real
 * per-element state -- set flags, read them back, dispatch a real 'invalid'
 * Event the way the spec says. setFormValue/setFormState record what was
 * set, readable by a component's own later code. shadowRoot is the SAME
 * accessor Element.prototype.shadowRoot uses (js_dom.c's el_get_shadowRoot),
 * not a second copy that could disagree with it -- one jar, one door. states
 * is a real Set.
 *
 * WHAT THIS DOES NOT DO, named rather than hidden, per the same rule:
 *   - no real <form> participation. setFormValue's value is stored, not
 *     submitted: this engine's <form> has no FormData walk that visits a
 *     form-associated custom element, so wiring submission would be building
 *     a feature the rest of the browser does not have yet, not closing a gap
 *     in this one.
 *   - no closed-mode bypass. Real ElementInternals.shadowRoot differs from
 *     the public property exactly once: a CLOSED shadow root is still
 *     reachable through internals, which is the whole point of closed mode
 *     for a component's own code. Reusing el_get_shadowRoot here means a
 *     closed root reads null through internals too. Nothing in the measured
 *     corpus attaches a closed shadow root (lit, plaited and ui5-webcomponents
 *     all default to 'open'), so this is a disclosed gap, not a measured one.
 *   - ARIA reflection (ariaLabel, role, ...) is a plain per-instance data
 *     property, not wired to anything. There is no accessibility tree on this
 *     engine for it to be right or wrong ABOUT -- a plain store that reads
 *     back what was written is the honest answer, not a fabricated one.
 *   - the ONE-attachInternals-per-element throw is real (NotSupportedError,
 *     matching the spec). The spec's OTHER throw condition -- attachInternals
 *     called on an element that is not a defined, non-built-in custom
 *     element -- is deliberately NOT checked: every real caller in this
 *     corpus calls it from inside a genuine custom-element constructor, and
 *     guessing wrong here would throw on a legitimate call, which is worse
 *     than the spec gap of not throwing on an illegitimate one (rule 3's
 *     asymmetry: absent is safer than present-and-wrong, and here a false
 *     throw IS the present-and-wrong case). */
"function installElementInternals() {\n"
"  var EP = null;\n"
"  try { EP = Object.getPrototypeOf(G.document.createElement('div')); } catch (e) {}\n"
"  if (!EP || typeof EP.attachInternals === 'function') return;\n"
"  var attached = (typeof WeakSet === 'function') ? new WeakSet() : null;\n"
"  var ARIA = ['ariaAtomic', 'ariaAutoComplete', 'ariaBusy', 'ariaChecked', 'ariaColCount',\n"
"    'ariaColIndex', 'ariaColSpan', 'ariaCurrent', 'ariaDisabled', 'ariaExpanded',\n"
"    'ariaHasPopup', 'ariaHidden', 'ariaKeyShortcuts', 'ariaLabel', 'ariaLevel', 'ariaLive',\n"
"    'ariaModal', 'ariaMultiLine', 'ariaMultiSelectable', 'ariaOrientation', 'ariaPlaceholder',\n"
"    'ariaPosInSet', 'ariaPressed', 'ariaReadOnly', 'ariaRequired', 'ariaRoleDescription',\n"
"    'ariaRowCount', 'ariaRowIndex', 'ariaRowSpan', 'ariaSelected', 'ariaSetSize', 'ariaSort',\n"
"    'ariaValueMax', 'ariaValueMin', 'ariaValueNow', 'ariaValueText', 'role'];\n"
"  var VALIDITY_KEYS = ['valueMissing', 'typeMismatch', 'patternMismatch', 'tooLong',\n"
"    'tooShort', 'rangeUnderflow', 'rangeOverflow', 'stepMismatch', 'badInput', 'customError'];\n"
"  EP.attachInternals = function () {\n"
"    var host = this;\n"
"    if (attached) {\n"
"      if (attached.has(host))\n"
"        throw new G.DOMException(\n"
"          \"Failed to execute 'attachInternals' on 'HTMLElement': ElementInternals for \" +\n"
"          'the specified element was already attached.', 'NotSupportedError');\n"
"      attached.add(host);\n"
"    }\n"
"    var validity = {}, k;\n"
"    for (k = 0; k < VALIDITY_KEYS.length; k++) validity[VALIDITY_KEYS[k]] = false;\n"
"    var message = '';\n"
"    var valid = true;\n"
"    var formValue = null, formState = null;\n"
"    var states = (typeof Set === 'function') ? new Set()\n"
"      : { add: function () {}, delete: function () { return false; }, has: function () { return false; } };\n"
"    var it = {};\n"
"    Object.defineProperty(it, 'shadowRoot', { enumerable: true,\n"
"      get: function () { return host.shadowRoot || null; } });\n"
"    Object.defineProperty(it, 'form', { enumerable: true, get: function () { return null; } });\n"
"    Object.defineProperty(it, 'labels', { enumerable: true, get: function () { return []; } });\n"
"    Object.defineProperty(it, 'willValidate', { enumerable: true, get: function () {\n"
"      return !!(host.constructor && host.constructor.formAssociated);\n"
"    } });\n"
"    Object.defineProperty(it, 'validity', { enumerable: true, get: function () {\n"
"      var v = {}, i;\n"
"      for (i = 0; i < VALIDITY_KEYS.length; i++) v[VALIDITY_KEYS[i]] = validity[VALIDITY_KEYS[i]];\n"
"      v.valid = valid;\n"
"      return v;\n"
"    } });\n"
"    Object.defineProperty(it, 'validationMessage', { enumerable: true,\n"
"      get: function () { return message; } });\n"
"    Object.defineProperty(it, 'states', { enumerable: true, get: function () { return states; } });\n"
"    it.setValidity = function (flags, msg, anchor) {\n"
"      var i, any = false;\n"
"      for (i = 0; i < VALIDITY_KEYS.length; i++) validity[VALIDITY_KEYS[i]] = false;\n"
"      if (flags) {\n"
"        for (i = 0; i < VALIDITY_KEYS.length; i++) {\n"
"          if (flags[VALIDITY_KEYS[i]]) { validity[VALIDITY_KEYS[i]] = true; any = true; }\n"
"        }\n"
"      }\n"
"      valid = !any;\n"
"      message = any ? String(msg || '') : '';\n"
"    };\n"
"    it.checkValidity = function () {\n"
"      if (valid) return true;\n"
"      try { host.dispatchEvent(new G.Event('invalid', { cancelable: true })); } catch (e) {}\n"
"      return false;\n"
"    };\n"
"    it.reportValidity = it.checkValidity;\n"
"    it.setFormValue = function (value, state) {\n"
"      formValue = value; formState = (state === undefined) ? value : state;\n"
"    };\n"
"    ARIA.forEach(function (name) {\n"
"      var v = null;\n"
"      Object.defineProperty(it, name, { enumerable: true,\n"
"        get: function () { return v; }, set: function (x) { v = x; } });\n"
"    });\n"
"    return it;\n"
"  };\n"
"}\n"
/* A minimal `ShadowRoot` global for `instanceof` -- MEASURED as load-bearing,
 * not decorative: keyed/plaited's event-retargeting helper does
 * `e.composedPath().find(n => n instanceof ShadowRoot) === t.getRootNode()`
 * inside the delegated click handler EVERY row-level interaction (select,
 * delete) goes through, and a bare `ShadowRoot` identifier with no global at
 * all is a ReferenceError, not a false comparison -- it would take the whole
 * handler down, not just this one check. Identity is decided by
 * el_isShadowRootNode (js_dom.c's __ldom_isShadowRoot), the same primitive
 * dom_is_shadow_root the DOM layer itself uses, so this cannot disagree with
 * "is this actually the node dom_attach_shadow built". */
"if (!('ShadowRoot' in G)) {\n"
"  var SR = function ShadowRoot() { throw new TypeError('Illegal constructor'); };\n"
"  try {\n"
"    Object.defineProperty(SR, Symbol.hasInstance, { configurable: true, value: function (o) {\n"
"      try { return !!(o && typeof o.__ldom_isShadowRoot === 'function' && o.__ldom_isShadowRoot()); }\n"
"      catch (e) { return false; }\n"
"    } });\n"
"  } catch (e) {}\n"
"  G.ShadowRoot = SR;\n"
"}\n"

"installTreeWalker();\n"
"installInterfaces();\n"
/* NamedNodeMap is spec'd `iterable<Attr>` (Symbol.iterator === values()), but
 * js_dom_iface.inc builds its prototype as a plain object (JS_NewObject),
 * not over Array.prototype the way NodeList/HTMLCollection are -- so it has
 * `.length` and indexed access but no iterator. MEASURED: this is the SECOND
 * failure behind importNode for uhtml/lit-html/cydon, reached only once
 * importNode itself works -- `for (const {name, value} of el.attributes)`
 * (uhtml's clone-and-patch path) threw 'value is not iterable' on the very
 * next line. Array.prototype.values is generic over any array-like `this`
 * (length + numeric indices), which NamedNodeMap already has, so borrowing
 * it is exact, not an approximation -- the same technique js_tokenlist.c
 * already uses for DOMTokenList's Symbol.iterator. */
"if (G.NamedNodeMap && G.NamedNodeMap.prototype && G.Symbol && G.Symbol.iterator) {\n"
"  try { def(G.NamedNodeMap.prototype, G.Symbol.iterator, Array.prototype.values); } catch (e) {}\n"
"}\n"
"installCloneNode();\n"
"try { installImportAdopt(); } catch (e) {}\n"
"installCurrentScript();\n"
"installReflectedURLs();\n"
/* JS_DOCWRITE_NO_INSTALL: test-docwrite-negctl's control (the same idiom as
 * the JS_IFRAME_NO_INSTALL guard ten lines below). Compiling the installer
 * out is what reproduces the pre-feature build exactly -- document.write
 * absent, pages still render -- so the positive gate is known to measure
 * THIS feature and not some other reason written blocks happen to appear. */
#ifndef JS_DOCWRITE_NO_INSTALL
"try { installDocWrite(); } catch (e) {}\n"
#endif
"installCustomElements();\n"
"try { installElementInternals(); } catch (e) {}\n"
"try { installDataset(Object.getPrototypeOf(G.document.createElement('div'))); } catch (e) {}\n"
/* JS_IFRAME_NO_INSTALL: test-iframe's negative control. Compiling the
 * installer out entirely (rather than, say, disabling it with a runtime
 * flag) is what test-iframe-negctl links against, so the control is proof
 * that test-iframe is measuring THIS feature and not some other reason the
 * probe's 'load' events happen to fire. */
#ifndef JS_IFRAME_NO_INSTALL
"try { installIframes(); } catch (e) {}\n"
#endif

/* The hook the C rejection tracker calls. It is returned rather than published
 * as a global, so a page cannot fake an unhandled rejection. */
"return {\n"
"  onParsed: function () { documentParsed(); },\n"
"  onNativeDelivery: function(){ if(typeof deliverMutations==='function')deliverMutations(); },\n"
"  onNativeText: function(target,oldValue,chain,added,previous,following,split){if(typeof emit==='function')emit(target,{type:split?'childList':'characterData',attributeName:null,attributeNamespace:null,oldValue:split?null:oldValue,addedNodes:split?[added]:[],removedNodes:[],previousSibling:split?previous:null,nextSibling:split?following:null},chain)},\n"
"  onReject: function (promise, reason, handled) {\n"
"    var type = handled ? 'rejectionhandled' : 'unhandledrejection';\n"
"    var ev = new G.PromiseRejectionEvent(type,\n"
"                                         { promise: promise, reason: reason, cancelable: !handled });\n"
"    if (typeof G['on'+type] === 'function') {\n"
"      try { G['on'+type](ev); } catch (e) {}\n"
"    }\n"
"    try { if (G.dispatchEvent) G.dispatchEvent(ev); } catch (e) {}\n"
"    if (!handled && !ev.defaultPrevented && !ev._prevented) {\n"
       /* String(reason) FIRST, then the stack.
        *
        * This used to print `reason.stack` alone, which is right in V8 --
        * there the stack's first line IS "TypeError: message". QuickJS omits
        * it, so every unhandled rejection on every page arrived as a bare
        * backtrace with no error in it: deepseek's read as
        *   [error]     at cv (s001.js)
        * and the actual failure, `cannot read property 'precedence' of
        * undefined`, was nowhere on the console. That one line of formatting
        * is the difference between a diagnosable page and a guess. The stack
        * is still appended, and the prefix test means this stays correct if
        * the engine's .stack ever grows the message line. */
"      try {\n"
"        var s = reason && reason.stack ? String(reason.stack) : '';\n"
"        var head = String(reason);\n"
"        console.error('Uncaught (in promise) ' +\n"
"                      (s && s.indexOf(head) === 0 ? s : (s ? head + '\\n' + s : head)));\n"
"      } catch (e) {}\n"
"    }\n"
"  }\n"
"};\n"
"})\n";

void js_platform_install(JSContext *ctx)
{
    if (!ctx || g_ctx) return;
    g_ctx = ctx;
#ifndef FRAME_BOOTSTRAP_LATE_INSTALL
    /* installIframes does MORE than define getters: its initial native
     * snapshot synchronously navigates parser-built srcdoc/about:blank frames.
     * Their settle() must already see __frameAdopt. The old AFTER-prelude
     * ordering below ran only the dynamic half of the identical-srcdoc guest
     * fixture (1 child console side effect instead of 2). DOMParser has been
     * installed by js_page_open before this entry; frame_install itself only
     * registers C sinks/functions and has no platform-prelude dependency. */
    if (g_legacy_frames && LOGIT_HAVE(js_frame_install)) js_frame_install(ctx);
#endif
    JSValue fn = JS_Eval(ctx, PLATFORM_PRELUDE, strlen(PLATFORM_PRELUDE), "<platform>",
                         JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(fn)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        printf("[platform] prelude failed: %s\n", m ? m : "?");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
        JS_FreeValue(ctx, fn);
        return;
    }
    JSValue args[10];
    args[0] = JS_NewCFunction(ctx, js_random, "__random", 2);
    args[1] = JS_NewInt32(ctx, g_vw);
    args[2] = JS_NewInt32(ctx, g_vh);
    args[3] = JS_NewCFunction(ctx, js_random_strong, "__randomStrong", 0);
    args[4] = JS_NewCFunction(ctx, js_clip_write_text, "__clipWriteText", 1);
    args[5] = JS_NewCFunction(ctx, native_mo_drain, "drainMutations", 0);
    args[6] = js_bootstrap_snapshot(ctx,"iframe",0);
    args[7] = JS_NewBool(ctx,LOGIT_HAVE(passive_frames_enabled)&&passive_frames_enabled());
    args[8] = LOGIT_HAVE(passive_frame_window)?JS_NewCFunction(ctx,passive_frame_window,"embeddedWindow",1):JS_UNDEFINED;
    args[9] = JS_NewBool(ctx,g_legacy_frames);
    if(JS_IsException(args[6])) {
        for(int i=0;i<10;i++)JS_FreeValue(ctx,args[i]);
        JS_FreeValue(ctx,fn);return;
    }
    JSValue hooks = JS_Call(ctx, fn, JS_UNDEFINED, 10, (JSValueConst *)args);
    for (int i = 0; i < 10; i++) JS_FreeValue(ctx, args[i]);
    JS_FreeValue(ctx, fn);
    if (JS_IsException(hooks)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        printf("[platform] prelude call failed: %s\n", m ? m : "?");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
        JS_FreeValue(ctx, hooks);
        return;
    }
    JS_FreeValue(ctx, g_reject_hook);
    g_reject_hook = JS_GetPropertyStr(ctx, hooks, "onReject");
    JS_FreeValue(ctx, g_parsed_hook);
    g_parsed_hook = JS_GetPropertyStr(ctx, hooks, "onParsed");
    g_native_mo_hook = JS_GetPropertyStr(ctx, hooks, "onNativeText");
    g_native_mo_delivery = JS_GetPropertyStr(ctx, hooks, "onNativeDelivery");
    struct node *mo_root = js_dom_root();
    if (mo_root) dom_subscribe(mo_root->doc, mo_subscription, native_mo_notify, platform_current());
    JS_FreeValue(ctx, hooks);
    JS_SetHostPromiseRejectionTracker(JS_GetRuntime(ctx), rejection_tracker, 0);
    /* OLD CLAIM, corrected by the entry-time registration above:
     * AFTER the prelude, not before: js_frame_install defines __frameAdopt on
     * globalThis and the prelude's settle() reads it by `typeof` at call time,
     * so ordering only has to put it before the first navigation -- which is
     * any time after installIframes() merely DEFINED the getters. Installed
     * from here rather than from js_page.c because js_page_close() already
     * calls js_platform_close() at exactly the point js_frame_close_all needs
     * (before JS_FreeContext/JS_FreeRuntime), so this needs no new hook cut
     * into a file another line of work is actively editing. */
#ifdef FRAME_BOOTSTRAP_LATE_INSTALL
    if (g_legacy_frames && LOGIT_HAVE(js_frame_install)) js_frame_install(ctx);
#endif
}

void js_platform_document_parsed(JSContext *ctx)
{
    if(!ctx || ctx!=g_ctx || !JS_IsFunction(ctx,g_parsed_hook))return;
    JSValue result=JS_Call(ctx,g_parsed_hook,JS_UNDEFINED,0,NULL);
    if(JS_IsException(result)) {
        JSValue error=JS_GetException(ctx);
        const char *message=JS_ToCString(ctx,error);
        printf("[platform] parser lifecycle failed: %s\n",message?message:"?");
        if(message)JS_FreeCString(ctx,message);
        JS_FreeValue(ctx,error);
    }
    JS_FreeValue(ctx,result);
}

void js_platform_close(JSContext *ctx)
{
    if (!ctx || ctx!=g_ctx) return;
    /* FIRST, and the ordering is the same invariant js_page.c states above its
     * js_worker_close_all() call: every live frame holds a JSContext on this
     * page's runtime, and JS_FreeRuntime asserts on live GC objects. This
     * function is itself called from js_page_close() before
     * JS_FreeContext(g_ctx)/JS_FreeRuntime(g_rt), which is the whole reason
     * the hook lives here. */
    if (g_legacy_frames && LOGIT_HAVE(js_frame_close_all)) js_frame_close_all();
    if (ctx) {
        JS_SetHostPromiseRejectionTracker(JS_GetRuntime(ctx), 0, 0);
        rejections_close(ctx);
        native_mo_close(ctx);
        JS_FreeValue(ctx, g_reject_hook);
        JS_FreeValue(ctx, g_parsed_hook);
    }
    g_reject_hook = JS_UNDEFINED;
    g_parsed_hook = JS_UNDEFINED;
    g_ctx = 0;
}

/* Native subscriptions escape into the DOM list: keep the subscription in
 * stable owner storage and switch a pointer, never memcpy its linked node.
 * Entropy remains a shared OS service, not a duplicated PRNG stream. */
#define PLATFORM_CONTEXT_FIELDS(X) \
    X(g_vw) X(g_vh) X(g_ctx) X(g_legacy_frames) \
    X(g_reject_hook) X(g_parsed_hook) \
    X(rejections) X(rejection_seq) X(rejection_overflow) \
    X(mo_subscription) X(mo_head) X(mo_tail) X(mo_count) \
    X(mo_flushing) X(mo_enabled) X(mo_scheduled) X(mo_watches) X(mo_watch_count) \
    X(g_native_mo_hook) X(g_native_mo_delivery)

struct js_platform_context {
    struct dom_subscription subscription;
#define PLATFORM_FIELD(n) __typeof__(n) n;
    PLATFORM_CONTEXT_FIELDS(PLATFORM_FIELD)
#undef PLATFORM_FIELD
};
static struct js_platform_context platform_default_context;
static struct js_platform_context *platform_active_context=&platform_default_context;
static struct js_platform_context *platform_current(void)
{ return platform_active_context; }

struct js_platform_context *js_platform_context_create(void)
{
    struct js_platform_context *s=calloc(1,sizeof *s);
    if (!s) return NULL;
    s->g_vw=980;s->g_vh=600;
    s->g_reject_hook=s->g_parsed_hook=JS_UNDEFINED;
    s->g_native_mo_hook=s->g_native_mo_delivery=JS_UNDEFINED;
    s->mo_subscription=&s->subscription;
    return s;
}

void js_platform_context_activate(struct js_platform_context *next)
{
    if (!next) next=&platform_default_context;
    if (next==platform_active_context) return;
#define PLATFORM_SAVE(n) memcpy(&platform_active_context->n,&n,sizeof n);
    PLATFORM_CONTEXT_FIELDS(PLATFORM_SAVE)
#undef PLATFORM_SAVE
#define PLATFORM_LOAD(n) memcpy(&n,&next->n,sizeof n);
    PLATFORM_CONTEXT_FIELDS(PLATFORM_LOAD)
#undef PLATFORM_LOAD
    platform_active_context=next;
}

int js_platform_context_destroy(struct js_platform_context *s)
{
    if (!s || s==&platform_default_context) return 0;
    struct js_platform_context *old=platform_active_context;
    js_platform_context_activate(s);
    int busy=g_ctx || mo_subscription->doc || mo_head || mo_watches;
    js_platform_context_activate(old==s && !busy ? NULL : old);
    if (busy) return 0;
    free(s);
    return 1;
}
