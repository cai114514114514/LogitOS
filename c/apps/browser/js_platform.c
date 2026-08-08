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
#include <string.h>
#include <stdlib.h>

int printf(const char *, ...);

static int g_vw = 980, g_vh = 600;
void js_platform_set_viewport(int w, int h) { if (w > 0) g_vw = w; if (h > 0) g_vh = h; }

/* ---- entropy ------------------------------------------------------------
 * THIS IS NOT A CSPRNG AND THE NAME crypto.getRandomValues IS A LIE WE ARE
 * TELLING ON PURPOSE, so it is written down here rather than left for someone
 * to discover. LogitOS has no entropy source a ring-3 process can reach: no
 * /dev/urandom, no RDRAND wrapper, no kernel pool syscall. What pages actually
 * use getRandomValues for -- request ids, cache-busting nonces, React keys,
 * telemetry correlation ids -- needs unpredictability between calls, not
 * against an adversary, and a page that cannot get any at all simply throws.
 *
 * So: xorshift128+, seeded from the wall clock, the monotonic clock and two
 * heap addresses (the allocator's layout differs per run and per page). It is
 * good enough to keep ids distinct and nowhere near good enough to generate a
 * key with. If this browser ever grows WebCrypto, the fix is a kernel entropy
 * syscall feeding this seed, not a better shuffle here. */
static unsigned long long g_s0, g_s1;
static int g_seeded;

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
static JSValue js_random(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    int32_t n = 0;
    double hint = 0;
    if (argc > 0) JS_ToInt32(ctx, &n, argv[0]);
    if (argc > 1) JS_ToFloat64(ctx, &hint, argv[1]);
    if (n < 0) n = 0;
    if (n > 65536) n = 65536;
    if (!g_seeded) rng_seed((unsigned long long)hint);
    unsigned char *buf = malloc((size_t)n + 1);
    if (!buf) return JS_ThrowOutOfMemory(ctx);
    for (int i = 0; i < n; ) {
        unsigned long long r = rng_next();
        for (int k = 0; k < 8 && i < n; k++, i++) buf[i] = (unsigned char)(r >> (k * 8));
    }
    JSValue ab = JS_NewArrayBufferCopy(ctx, buf, (size_t)n);
    free(buf);
    return ab;
}

/* ---- unhandled promise rejections --------------------------------------
 * QuickJS reports these through a runtime hook rather than as an event, so the
 * bridge has to be in C. A page that installs window.onunhandledrejection
 * (bing does) gets called; a page that does not gets the message in the console
 * instead of silence, which is the difference between debugging a page and
 * guessing at it. */
static JSValue g_reject_hook = JS_UNDEFINED;
static JSContext *g_ctx;

static void rejection_tracker(JSContext *ctx, JSValueConst promise, JSValueConst reason,
                              int is_handled, void *opaque)
{
    (void)opaque;
    if (is_handled || !JS_IsFunction(ctx, g_reject_hook)) return;
    JSValue a[2];
    a[0] = JS_DupValue(ctx, promise);
    a[1] = JS_DupValue(ctx, reason);
    JSValue r = JS_Call(ctx, g_reject_hook, JS_UNDEFINED, 2, (JSValueConst *)a);
    if (JS_IsException(r)) JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, a[0]);
    JS_FreeValue(ctx, a[1]);
}

/* ---- the prelude -------------------------------------------------------- */
static const char *PLATFORM_PRELUDE =
"(function (__random, __vw, __vh) {\n"
"'use strict';\n"
"var G = globalThis;\n"
/* The house rule for this whole file. Three lines are adding to this runtime
   at once; whoever got there first keeps it. */
"function def(o, k, v) { if (o && !(k in o)) { try { o[k] = v; } catch (e) {} } }\n"
"function defOwn(o, k, get) {\n"
"  if (!o || (k in o)) return;\n"
"  try { Object.defineProperty(o, k, { get: get, configurable: true }); } catch (e) {}\n"
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
"  function ent(name, type, start, dur) {\n"
"    return { name: String(name), entryType: type, startTime: start, duration: dur,\n"
"             toJSON: function () { return { name: this.name, entryType: this.entryType,\n"
"                                            startTime: this.startTime, duration: this.duration }; } };\n"
"  }\n"
"  def(perf, 'mark', function (name, opts) {\n"
"    var t = (opts && typeof opts.startTime === 'number') ? opts.startTime : perf.now();\n"
"    var e = ent(name, 'mark', t, 0);\n"
"    if (opts && opts.detail !== undefined) e.detail = opts.detail;\n"
"    entries.push(e); return e;\n"
"  });\n"
"  function markTime(n) {\n"
"    for (var i = entries.length - 1; i >= 0; i--)\n"
"      if (entries[i].entryType === 'mark' && entries[i].name === n) return entries[i].startTime;\n"
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
"    entries.push(r); return r;\n"
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
 * hang off those, and readystatechange fires with them as the spec requires. */
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
"  try {\n"
"    doc.addEventListener('DOMContentLoaded', function () {\n"
"      G.__platMarkTiming('domInteractive');\n"
"      G.__platMarkTiming('domContentLoadedEventStart');\n"
"      change('interactive');\n"
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
"    this.type = String(type); this.data = init.data;\n"
"    this.origin = init.origin || ''; this.lastEventId = init.lastEventId || '';\n"
"    this.source = init.source || null; this.ports = init.ports || [];\n"
"    this.bubbles = !!init.bubbles; this.cancelable = !!init.cancelable;\n"
"  };\n"
"}\n"
"if (!G.MessageChannel) {\n"
"  var Port = function MessagePort() {\n"
"    this._peer = null; this._l = []; this.onmessage = null; this._started = false;\n"
"    this._q = [];\n"
"  };\n"
"  Port.prototype = {\n"
"    constructor: Port,\n"
"    addEventListener: function (t, f) { if (t === 'message' && typeof f === 'function') this._l.push(f); },\n"
"    removeEventListener: function (t, f) {\n"
"      if (t !== 'message') return;\n"
"      var i = this._l.indexOf(f); if (i >= 0) this._l.splice(i, 1);\n"
"    },\n"
       /* start() is not decoration: a port with onmessage assigned is implicitly
          started, but one that only used addEventListener stays SILENT until
          start() is called, and code that forgets it is code we must not
          accidentally rescue -- it would behave differently here than in a
          browser. */
"    start: function () {\n"
"      if (this._started) return;\n"
"      this._started = true;\n"
"      var self = this, q = this._q; this._q = [];\n"
"      q.forEach(function (d) { self._deliver(d); });\n"
"    },\n"
"    close: function () { this._peer = null; this._l = []; this.onmessage = null; },\n"
"    _deliver: function (data) {\n"
"      var ev = new G.MessageEvent('message', { data: data, source: null });\n"
"      if (typeof this.onmessage === 'function') { try { this.onmessage(ev); } catch (e) { G.reportError(e); } }\n"
"      this._l.slice().forEach(function (f) { try { f(ev); } catch (e) { G.reportError(e); } });\n"
"    },\n"
"    postMessage: function (data) {\n"
"      var peer = this._peer;\n"
"      if (!peer) return;\n"
"      setTimeout(function () {\n"
"        if (peer._started || typeof peer.onmessage === 'function') peer._deliver(data);\n"
"        else peer._q.push(data);\n"
"      }, 0);\n"
"    }\n"
"  };\n"
"  G.MessagePort = Port;\n"
"  G.MessageChannel = function MessageChannel() {\n"
"    this.port1 = new Port(); this.port2 = new Port();\n"
"    this.port1._peer = this.port2; this.port2._peer = this.port1;\n"
"  };\n"
"}\n"
/* window.postMessage to ourselves. One window, so the only meaningful target is
   this one; delivery is async and the event carries our own origin. */
"def(G, 'postMessage', function (data, origin) {\n"
"  var org = (G.location && G.location.origin) || '';\n"
"  setTimeout(function () {\n"
"    var ev = new G.MessageEvent('message', { data: data, origin: org, source: G });\n"
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
 * user cancelled" from "the code is broken". js_webapi.c's AbortController
 * throws a plain Error with .name = 'AbortError' for want of this class; it
 * keeps doing so, because changing it belongs to that file's own tests. */
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
"    this.type = String(type); this.promise = init.promise; this.reason = init.reason;\n"
"    this.bubbles = !!init.bubbles; this.cancelable = init.cancelable !== false;\n"
"    this._prevented = false;\n"
"    this.preventDefault = function () { this._prevented = true; };\n"
"  };\n"
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
"  def(nav, 'product', 'Gecko');\n"
"})();\n"

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
"    var bytes = new Uint8Array(__random(view.byteLength, Date.now()));\n"
"    var dst = new Uint8Array(view.buffer, view.byteOffset, view.byteLength);\n"
"    dst.set(bytes);\n"
"    return view;\n"
"  });\n"
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
"    for (var j = 0; j < ks.length; j++) out[ks[j]] = cl(x[ks[j]]);\n"
"    return out;\n"
"  }\n"
"  return cl(v);\n"
"});\n"

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

/* ==== the observers ======================================================
 * REQUESTED, NOT MEASURED: no page in the corpus reached one, because they all
 * died earlier. They are here because their ABSENCE is load-bearing in a way
 * the others' is not -- a page that constructs an IntersectionObserver and gets
 * a ReferenceError never shows its lazily-loaded content at all, and that is
 * most images on most modern pages.
 *
 * WHAT THEY HONESTLY DO. There are no scroll or resize events in this browser
 * (browser.c owns the scroll offset and dispatches neither), so an observer
 * cannot be continuous. Each one delivers its initial records -- which is a
 * real measurement, from getBoundingClientRect against the viewport, not a
 * fabricated isIntersecting:true -- and then re-evaluates a BOUNDED number of
 * times on a timer, so a page that lazy-loads on intersection loads the content
 * that is actually on screen and stops. It is not the real thing and the limit
 * is stated in RECHECKS below rather than left for someone to find.
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
"  var IO = function IntersectionObserver(cb, opts) {\n"
"    this._cb = cb; this._t = []; this._n = 0;\n"
"    opts = opts || {};\n"
"    this.root = opts.root || null;\n"
"    this.rootMargin = opts.rootMargin || '0px';\n"
"    this.thresholds = Array.isArray(opts.threshold) ? opts.threshold.slice()\n"
"                    : [typeof opts.threshold === 'number' ? opts.threshold : 0];\n"
"  };\n"
"  IO.prototype = {\n"
"    constructor: IO,\n"
"    observe: function (el) { if (el && this._t.indexOf(el) < 0) { this._t.push(el); this._schedule(); } },\n"
"    unobserve: function (el) { var i = this._t.indexOf(el); if (i >= 0) this._t.splice(i, 1); },\n"
"    disconnect: function () { this._t = []; },\n"
"    takeRecords: function () { return []; },\n"
"    _schedule: function () {\n"
"      if (this._armed) return;\n"
"      this._armed = true;\n"
"      var self = this;\n"
"      setTimeout(function () { self._armed = false; self._run(); }, 0);\n"
"    },\n"
"    _run: function () {\n"
"      var v = vp(), recs = [], self = this;\n"
"      this._t.forEach(function (el) {\n"
"        var r = rect(el);\n"
"        if (!r) return;\n"
"        var ix = Math.max(0, Math.min(r.right, v.w) - Math.max(r.left, 0));\n"
"        var iy = Math.max(0, Math.min(r.bottom, v.h) - Math.max(r.top, 0));\n"
"        var area = (r.width || 0) * (r.height || 0);\n"
"        var ratio = area > 0 ? (ix * iy) / area : 0;\n"
"        recs.push({ target: el, isIntersecting: ix > 0 && iy > 0, intersectionRatio: ratio,\n"
"                    boundingClientRect: r, rootBounds: { top: 0, left: 0, right: v.w, bottom: v.h,\n"
"                                                         width: v.w, height: v.h, x: 0, y: 0 },\n"
"                    intersectionRect: { top: Math.max(r.top, 0), left: Math.max(r.left, 0),\n"
"                                        width: ix, height: iy },\n"
"                    time: performance.now() });\n"
"      });\n"
"      if (recs.length) { try { this._cb(recs, this); } catch (e) { G.reportError(e); } }\n"
"      if (++this._n < RECHECKS && this._t.length)\n"
"        setTimeout(function () { self._run(); }, 100);\n"
"    }\n"
"  };\n"
"  G.IntersectionObserver = IO;\n"
"  G.IntersectionObserverEntry = function IntersectionObserverEntry() {};\n"
"}\n"
"if (!G.ResizeObserver) {\n"
"  var RO = function ResizeObserver(cb) { this._cb = cb; this._t = []; this._n = 0; };\n"
"  RO.prototype = {\n"
"    constructor: RO,\n"
"    observe: function (el) { if (el && this._t.indexOf(el) < 0) { this._t.push(el); this._schedule(); } },\n"
"    unobserve: function (el) { var i = this._t.indexOf(el); if (i >= 0) this._t.splice(i, 1); },\n"
"    disconnect: function () { this._t = []; },\n"
"    _schedule: function () {\n"
"      if (this._armed) return;\n"
"      this._armed = true;\n"
"      var self = this;\n"
"      setTimeout(function () { self._armed = false; self._run(); }, 0);\n"
"    },\n"
"    _run: function () {\n"
"      var recs = [], self = this;\n"
"      this._t.forEach(function (el) {\n"
"        var r; try { r = el.getBoundingClientRect(); } catch (e) { return; }\n"
"        var box = [{ inlineSize: r.width, blockSize: r.height }];\n"
"        recs.push({ target: el, contentRect: r, borderBoxSize: box, contentBoxSize: box,\n"
"                    devicePixelContentBoxSize: box });\n"
"      });\n"
"      if (recs.length) { try { this._cb(recs, this); } catch (e) { G.reportError(e); } }\n"
"      if (++this._n < RECHECKS && this._t.length)\n"
"        setTimeout(function () { self._run(); }, 100);\n"
"    }\n"
"  };\n"
"  G.ResizeObserver = RO;\n"
"}\n"
"if (!G.MutationObserver) {\n"
"  var mos = [];\n"
"  var MO = function MutationObserver(cb) { this._cb = cb; this._recs = []; this._t = []; };\n"
"  MO.prototype = {\n"
"    constructor: MO,\n"
"    observe: function (el, opts) {\n"
"      if (!el) return;\n"
"      this._t.push({ node: el, opts: opts || { childList: true } });\n"
"      if (mos.indexOf(this) < 0) mos.push(this);\n"
"    },\n"
"    disconnect: function () { var i = mos.indexOf(this); if (i >= 0) mos.splice(i, 1); this._t = []; },\n"
"    takeRecords: function () { var r = this._recs; this._recs = []; return r; }\n"
"  };\n"
"  G.MutationObserver = MO;\n"
"  G.MutationRecord = function MutationRecord() {};\n"
   /* Does `node` lie inside anything this observer watches? `subtree` is the
      difference between an observer on document.body seeing every change on the
      page and seeing none of them, so it is walked, not assumed. */
"  var watches = function (mo, node) {\n"
"    for (var i = 0; i < mo._t.length; i++) {\n"
"      var e = mo._t[i];\n"
"      if (e.node === node) return e.opts;\n"
"      if (e.opts.subtree) {\n"
"        var p = node;\n"
"        while (p) { if (p === e.node) return e.opts; p = p.parentNode; }\n"
"      }\n"
"    }\n"
"    return null;\n"
"  };\n"
"  var queued = false;\n"
"  var emit = function (target, rec) {\n"
"    for (var i = 0; i < mos.length; i++) {\n"
"      var o = watches(mos[i], target);\n"
"      if (!o) continue;\n"
"      if (rec.type === 'childList' && !o.childList) continue;\n"
"      if (rec.type === 'attributes' && !o.attributes) continue;\n"
"      if (rec.type === 'characterData' && !o.characterData) continue;\n"
"      rec.target = target;\n"
"      mos[i]._recs.push(rec);\n"
"    }\n"
"    if (queued) return;\n"
"    queued = true;\n"
       /* Records are delivered on a microtask, as the spec says: a page that
          mutates ten nodes in a loop must get ONE callback with ten records,
          not ten callbacks, or every virtual-DOM diff runs ten times. */
"    Promise.resolve().then(function () {\n"
"      queued = false;\n"
"      mos.slice().forEach(function (m) {\n"
"        if (!m._recs.length) return;\n"
"        var r = m._recs; m._recs = [];\n"
"        try { m._cb(r, m); } catch (e) { G.reportError(e); }\n"
"      });\n"
"    });\n"
"  };\n"
   /* The element prototype, not `Element.prototype`: js_dom.c registers the
      class without publishing a constructor, so the only handle on the shared
      prototype is an element. (js_select.c publishes a `Element` façade, but
      only if it ran first, and this must not depend on that.) */
"  var proto = null;\n"
"  try { proto = Object.getPrototypeOf(G.document.createElement('div')); } catch (e) {}\n"
"  if (proto && !proto.__moWrapped) {\n"
"    try {\n"
"      Object.defineProperty(proto, '__moWrapped', { value: true, enumerable: false });\n"
"      ['appendChild', 'insertBefore', 'replaceChild', 'removeChild'].forEach(function (m) {\n"
"        var orig = proto[m];\n"
"        if (typeof orig !== 'function') return;\n"
"        proto[m] = function () {\n"
"          var r = orig.apply(this, arguments);\n"
"          var added = [], removed = [];\n"
"          if (m === 'removeChild') removed = [arguments[0]];\n"
"          else if (m === 'replaceChild') { added = [arguments[0]]; removed = [arguments[1]]; }\n"
"          else added = [arguments[0]];\n"
"          if (mos.length) emit(this, { type: 'childList', addedNodes: added, removedNodes: removed,\n"
"                                       attributeName: null, oldValue: null });\n"
"          return r;\n"
"        };\n"
"      });\n"
"      ['setAttribute', 'removeAttribute'].forEach(function (m) {\n"
"        var orig = proto[m];\n"
"        if (typeof orig !== 'function') return;\n"
"        proto[m] = function (name) {\n"
"          var old = null;\n"
"          try { old = this.getAttribute(name); } catch (e) {}\n"
"          var r = orig.apply(this, arguments);\n"
"          if (mos.length) emit(this, { type: 'attributes', attributeName: String(name),\n"
"                                       oldValue: old, addedNodes: [], removedNodes: [] });\n"
"          return r;\n"
"        };\n"
"      });\n"
"    } catch (e) {}\n"
"  }\n"
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
 * WHAT THE PROTOTYPES ACTUALLY ARE, stated because it is a real deviation.
 * js_dom.c has ONE element class with ONE shared prototype -- there is no
 * per-tag class to hand out. So Element.prototype and HTMLElement.prototype
 * ARE that shared object (which makes `el instanceof HTMLElement` correctly
 * true for every element), and each per-tag interface gets a FRESH prototype
 * object inheriting from it. Two consequences, both deliberate:
 *
 *   - `el instanceof HTMLInputElement` must not be true for a <div>, and with
 *     a shared prototype it would be. So each per-tag interface carries a
 *     Symbol.hasInstance that tests tagName. Publishing them all over one
 *     prototype would answer `instanceof` wrongly, which is worse than not
 *     publishing them: a page would take a branch it must not take.
 *   - a page that PATCHES `HTMLDialogElement.prototype.showModal` patches an
 *     object no live element has in its chain, so the patch does not take
 *     effect. Feature DETECTION -- which is what the corpus does, and what the
 *     name is overwhelmingly used for -- is answered correctly; monkey
 *     patching a per-tag prototype is not. Fixing that properly needs per-tag
 *     classes in js_dom.c, which is the DOM line's file. */
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
"    if (typeof el.connectedCallback === 'function' && inDocument(el))\n"
"      { try { el.connectedCallback(); } catch (e) { G.reportError(e); } }\n"
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
"    walk(root, function (el) {\n"
"      var d = defs[String(el.tagName || '').toLowerCase()];\n"
"      if (d) upgradeOne(el, d);\n"
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
   /* Insertion upgrades. The same three methods the MutationObserver support
      wraps, wrapped once more here -- order does not matter because each
      wrapper calls through, and doing it here rather than there keeps the two
      features independent. */
"  var EP2 = null;\n"
"  try { EP2 = Object.getPrototypeOf(G.document.createElement('div')); } catch (e) {}\n"
"  if (EP2 && !EP2.__ceWrapped) {\n"
"    try {\n"
"      Object.defineProperty(EP2, '__ceWrapped', { value: true, enumerable: false });\n"
"      ['appendChild', 'insertBefore', 'replaceChild'].forEach(function (m) {\n"
"        var orig = EP2[m];\n"
"        if (typeof orig !== 'function') return;\n"
"        EP2[m] = function () {\n"
"          var r = orig.apply(this, arguments);\n"
"          try { upgradeTree(arguments[0]); } catch (e) {}\n"
"          return r;\n"
"        };\n"
"      });\n"
"      var rm = EP2.removeChild;\n"
"      if (typeof rm === 'function') {\n"
"        EP2.removeChild = function (n) {\n"
"          var r = rm.apply(this, arguments);\n"
"          try {\n"
"            walk(n, function (el) {\n"
"              if (el.__ceState === 'upgraded' && typeof el.disconnectedCallback === 'function')\n"
"                { try { el.disconnectedCallback(); } catch (e) { G.reportError(e); } }\n"
"            });\n"
"          } catch (e) {}\n"
"          return r;\n"
"        };\n"
"      }\n"
"    } catch (e) {}\n"
"  }\n"
"}\n"
"installTreeWalker();\n"
"installInterfaces();\n"
"installCustomElements();\n"
"try { installDataset(Object.getPrototypeOf(G.document.createElement('div'))); } catch (e) {}\n"

/* The hook the C rejection tracker calls. It is returned rather than published
 * as a global, so a page cannot fake an unhandled rejection. */
"return {\n"
"  onReject: function (promise, reason) {\n"
"    var ev = new G.PromiseRejectionEvent('unhandledrejection',\n"
"                                         { promise: promise, reason: reason });\n"
"    if (typeof G.onunhandledrejection === 'function') {\n"
"      try { G.onunhandledrejection(ev); } catch (e) {}\n"
"    }\n"
"    try { if (G.dispatchEvent) G.dispatchEvent(ev); } catch (e) {}\n"
"    if (!ev._prevented) {\n"
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
    if (!ctx) return;
    g_ctx = ctx;
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
    JSValue args[3];
    args[0] = JS_NewCFunction(ctx, js_random, "__random", 2);
    args[1] = JS_NewInt32(ctx, g_vw);
    args[2] = JS_NewInt32(ctx, g_vh);
    JSValue hooks = JS_Call(ctx, fn, JS_UNDEFINED, 3, (JSValueConst *)args);
    for (int i = 0; i < 3; i++) JS_FreeValue(ctx, args[i]);
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
    JS_FreeValue(ctx, hooks);
    JS_SetHostPromiseRejectionTracker(JS_GetRuntime(ctx), rejection_tracker, 0);
}

void js_platform_close(JSContext *ctx)
{
    if (ctx) {
        JS_SetHostPromiseRejectionTracker(JS_GetRuntime(ctx), 0, 0);
        JS_FreeValue(ctx, g_reject_hook);
    }
    g_reject_hook = JS_UNDEFINED;
    g_ctx = 0;
}
