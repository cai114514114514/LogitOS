/* The silent-stall instrument. See js_stall.h for what it is and why the two
 * halves are split the way they are.
 *
 * This file adds ZERO behaviour to the browser unless an embedder calls
 * js_stall_arm(). js_stall_report() works on an unarmed context and is the
 * half with no observer effect at all, so the cheapest honest measurement is
 * available without touching the page.
 */
#include "js_stall.h"
#include <string.h>

/* ==== the armed trackers ================================================
 *
 * DISCOVERY BY SHAPE, NOT BY NAME. The observer section walks the global
 * object and wraps every constructor whose name ends in `Observer` and whose
 * prototype carries an `observe` method. Nothing here knows that
 * IntersectionObserver exists; an observer added to js_platform.c tomorrow is
 * instrumented the day it lands, and one removed stops appearing. The same
 * argument the site scoreboard makes about ranking applies to instruments: a
 * hand-written list is a list of what its author remembered.
 *
 * A CONSTRUCTOR IS WRAPPED WITH A Proxy, NOT REPLACED. Replacing `G.X` with a
 * plain function breaks `instanceof`, subclassing, `.prototype` identity and
 * `.name` -- every one of which a framework checks. A `construct` trap
 * preserves all of them and only substitutes the callback argument.
 *
 * THE PERTURBATIONS, NAMED. Two, both in the fetch/stream trackers, both
 * measured by running each page armed and unarmed:
 *   - attaching a settlement handler registers a reaction, so every in-flight
 *     fetch adds one to `promise_pending_awaited`;
 *   - that handler's rejection arm marks a rejected fetch HANDLED, so
 *     `promise_rejected_unhandled` falls by the number of failed fetches.
 * Neither changes what the page computes. Both change a number this
 * instrument also prints, which is why the harness prints both runs.
 */
static const char STALL_PRELUDE[] =
"(function () {\n"
"  var G = globalThis;\n"
"  if (G.__stall) return 1;\n"
"  var S = {\n"
"    fetch:  { started: 0, settled: 0, failed: 0, org: {} },\n"
"    xhr:    { started: 0, settled: 0, org: {} },\n"
"    read:   { issued: 0, settled: 0 },\n"
"    raf:    { queued: 0, ran: 0, cancelled: 0, live: {} },\n"
"    to:     { armed: 0, fired: 0, cleared: 0, live: {} },\n"
"    iv:     { armed: 0, fired: 0, cleared: 0, live: {} },\n"
"    obs:    {},\n"
"    life:   {},\n"
"    lsn:    {},\n"
"    ce:     { defined: 0, connected: 0, disconnected: 0, attrchanged: 0, adopted: 0, shadow: 0 }\n"
"  };\n"
"  G.__stall = S;\n"
"  var bump = function (m, k, d) { m[k] = (m[k] || 0) + d; };\n"
   /* An ORIGIN, never a URL. A relative or scheme-less reference is the
      document's own origin by definition, and saying so is more useful than
      re-deriving the base here -- the question this answers is "is the page
      waiting on itself, on an API host, or on a third party". */
"  var org = function (u) {\n"
"    try {\n"
"      u = String(u == null ? '' : u);\n"
"      var m = /^([a-zA-Z][a-zA-Z0-9+.-]*:)\\/\\/([^\\/?#]*)/.exec(u);\n"
"      if (m) return m[1] + '//' + m[2];\n"
"      if (u.charAt(0) === '/' || u.indexOf(':') < 0) return '(document)';\n"
"      return '(' + u.split(':')[0] + ':)';\n"
"    } catch (e) { return '(unparsed)'; }\n"
"  };\n"
   /* ---- fetch. Counted by origin, decremented on settle, so what is left in
      `org` at the end is exactly the set still in flight. */
"  if (typeof G.fetch === 'function') {\n"
"    var of = G.fetch;\n"
"    G.fetch = function (input) {\n"
"      var u = input, o;\n"
"      try { if (input && typeof input === 'object' && input.url) u = input.url; } catch (e) {}\n"
"      o = org(u);\n"
"      S.fetch.started++; bump(S.fetch.org, o, 1);\n"
"      var p;\n"
"      try { p = of.apply(this, arguments); }\n"
"      catch (e) { S.fetch.settled++; bump(S.fetch.org, o, -1); throw e; }\n"
"      try {\n"
"        if (p && typeof p.then === 'function')\n"
   /* The rejection arm does NOT rethrow. Rethrowing would create a fresh
      unhandled rejection of the instrument's own making, and the page would
      see a console error that its own code did not produce. The page still
      holds `p` with whatever handlers it attached; this derived promise is
      discarded. */
"          p.then(function () { S.fetch.settled++; bump(S.fetch.org, o, -1); },\n"
"                 function () { S.fetch.settled++; S.fetch.failed++; bump(S.fetch.org, o, -1); });\n"
"      } catch (e) {}\n"
"      return p;\n"
"    };\n"
"    try { G.fetch.name; } catch (e) {}\n"
"  }\n"
   /* ---- the response body. A fetch whose PROMISE settles and whose BODY
      never completes is a stall the fetch counter above cannot see, and it is
      the shape a streaming endpoint has. Counted as reads issued vs reads
      settled: a reader parked on read() forever is issued > settled. */
"  if (G.ReadableStream && G.ReadableStream.prototype &&\n"
"      typeof G.ReadableStream.prototype.getReader === 'function') {\n"
"    var ogr = G.ReadableStream.prototype.getReader;\n"
"    G.ReadableStream.prototype.getReader = function () {\n"
"      var rd = ogr.apply(this, arguments);\n"
"      try {\n"
"        if (rd && typeof rd.read === 'function' && !rd.__stallWrapped) {\n"
"          var orr = rd.read;\n"
"          rd.read = function () {\n"
"            S.read.issued++;\n"
"            var q;\n"
"            try { q = orr.apply(this, arguments); }\n"
"            catch (e) { S.read.settled++; throw e; }\n"
"            try {\n"
"              if (q && typeof q.then === 'function')\n"
"                q.then(function () { S.read.settled++; }, function () { S.read.settled++; });\n"
"            } catch (e) {}\n"
"            return q;\n"
"          };\n"
"          rd.__stallWrapped = 1;\n"
"        }\n"
"      } catch (e) {}\n"
"      return rd;\n"
"    };\n"
"  }\n"
   /* ---- XHR. `send` is the start; readyState 4 is the end. Wrapping
      onreadystatechange would fight the page for the slot, so a listener is
      added instead -- addEventListener is additive and the page keeps its own. */
"  if (G.XMLHttpRequest && G.XMLHttpRequest.prototype) {\n"
"    var XP = G.XMLHttpRequest.prototype;\n"
"    var oo = XP.open, os = XP.send;\n"
"    if (typeof oo === 'function')\n"
"      XP.open = function (m, u) { try { this.__stallOrg = org(u); } catch (e) {} return oo.apply(this, arguments); };\n"
"    if (typeof os === 'function')\n"
"      XP.send = function () {\n"
"        var o = this.__stallOrg || '(document)', done = 0, self = this;\n"
"        S.xhr.started++; bump(S.xhr.org, o, 1);\n"
"        var fin = function () { if (done) return; done = 1; S.xhr.settled++; bump(S.xhr.org, o, -1); };\n"
"        try {\n"
"          if (typeof this.addEventListener === 'function') {\n"
"            this.addEventListener('load', fin); this.addEventListener('error', fin);\n"
"            this.addEventListener('abort', fin); this.addEventListener('timeout', fin);\n"
"            this.addEventListener('readystatechange', function () { if (self.readyState === 4) fin(); });\n"
"          }\n"
"        } catch (e) {}\n"
"        try { return os.apply(this, arguments); }\n"
"        catch (e) { fin(); throw e; }\n"
"      };\n"
"  }\n"
   /* ---- rAF and the timers. `queued - ran` is the number of callbacks the
      page is still expecting the frame loop to deliver, and a browser whose
      frame loop never turns reports every rAF it was ever given. */
   /* The live-id helpers, shared by rAF, setTimeout and setInterval. Defined
    * before their first use so the reading order matches the calling order.
    * See LIVE IDS below for why a live SET and not a subtraction. */
"  var live = function (rec, id) { if (id !== undefined && id !== null) rec.live[id] = 1; return id; };\n"
"  var dead = function (rec, id) { try { delete rec.live[id]; } catch (e) {} };\n"
   /* cancelAnimationFrame IS WRAPPED, and it is not a detail: without it the
    * first version of this instrument reported `raf queued=2 ran=0` on a
    * framework whose scheduler cancels its own frame requests as a matter of
    * course, which reads as "the browser never turns the frame loop" and is
    * the exact false positive this instrument would be worthless for
    * producing. A cancelled callback is not an unmet obligation. */
"  if (typeof G.requestAnimationFrame === 'function') {\n"
"    var ora = G.requestAnimationFrame;\n"
"    G.requestAnimationFrame = function (cb) {\n"
"      S.raf.queued++;\n"
"      var id = ora.call(this, function () { S.raf.ran++; dead(S.raf, id); return cb.apply(this, arguments); });\n"
"      return live(S.raf, id);\n"
"    };\n"
"  }\n"
"  if (typeof G.cancelAnimationFrame === 'function') {\n"
"    var oca = G.cancelAnimationFrame;\n"
"    G.cancelAnimationFrame = function (id) { S.raf.cancelled++; dead(S.raf, id); return oca.apply(this, arguments); };\n"
"  }\n"
   /* LIVE IDS, NOT armed-minus-fired-minus-cleared. The first version of this
    * subtracted counters and reported `outstanding=-10`, because a page is
    * entitled to call clearTimeout on a timer that has already fired and on
    * one that never existed -- so `cleared` is not a partition of `armed`. A
    * negative outstanding is not a small error, it is a number that tells the
    * reader to stop believing the report. The live set is exact: an id enters
    * on arm and leaves when it fires or is cleared, whichever happens. */
"  if (typeof G.setTimeout === 'function') {\n"
"    var ost = G.setTimeout;\n"
"    G.setTimeout = function (cb) {\n"
"      S.to.armed++;\n"
"      var a = Array.prototype.slice.call(arguments), id;\n"
"      if (typeof cb === 'function')\n"
"        a[0] = function () { S.to.fired++; dead(S.to, id); return cb.apply(this, arguments); };\n"
"      else S.to.fired++;\n"
"      id = ost.apply(this, a);\n"
"      return live(S.to, id);\n"
"    };\n"
"  }\n"
"  if (typeof G.setInterval === 'function') {\n"
"    var osi = G.setInterval;\n"
"    G.setInterval = function (cb) {\n"
"      S.iv.armed++;\n"
"      var a = Array.prototype.slice.call(arguments);\n"
"      if (typeof cb === 'function')\n"
"        a[0] = function () { S.iv.fired++; return cb.apply(this, arguments); };\n"
   /* An interval stays live until it is cleared -- firing does not retire it.
    * That asymmetry with setTimeout is the whole difference between the two
    * APIs and the report would be wrong if it were folded away. */
"      return live(S.iv, osi.apply(this, a));\n"
"    };\n"
"  }\n"
"  if (typeof G.clearTimeout === 'function') {\n"
"    var oct = G.clearTimeout;\n"
"    G.clearTimeout = function (id) { S.to.cleared++; dead(S.to, id); return oct.apply(this, arguments); };\n"
"  }\n"
"  if (typeof G.clearInterval === 'function') {\n"
"    var oci = G.clearInterval;\n"
"    G.clearInterval = function (id) { S.iv.cleared++; dead(S.iv, id); return oci.apply(this, arguments); };\n"
"  }\n"
   /* ---- CUSTOM ELEMENT REACTIONS. A definition that is never followed by a
    * connectedCallback is a component that will never initialise, and it is
    * silent by construction: `customElements.define` returns, the tag parses,
    * the element is in the DOM and its constructor may even have run. Every
    * component framework built on custom elements gates its first render on
    * that callback, so `defined=N connected=0` is a whole page's worth of
    * content that never arrives with nothing in the console.
    *
    * The three reactions are wrapped on the class's own prototype at define
    * time. The lookup walks the prototype chain (a base class supplies them
    * far more often than the leaf), and the assignment shadows on the leaf, so
    * the original is still called and no other subclass is affected. */
"  if (G.customElements && typeof G.customElements.define === 'function') {\n"
"    var REACT = [['connectedCallback', 'connected'], ['disconnectedCallback', 'disconnected'],\n"
"                 ['attributeChangedCallback', 'attrchanged'], ['adoptedCallback', 'adopted']];\n"
"    var ocd = G.customElements.define;\n"
"    G.customElements.define = function (name, ctor) {\n"
"      S.ce.defined++;\n"
"      try {\n"
"        var p = ctor && ctor.prototype;\n"
"        if (p) REACT.forEach(function (pair) {\n"
"          var f = p[pair[0]];\n"
"          if (typeof f !== 'function' || f.__stallCE) return;\n"
"          var w = function () { S.ce[pair[1]]++; return f.apply(this, arguments); };\n"
"          w.__stallCE = 1;\n"
"          p[pair[0]] = w;\n"
"        });\n"
"      } catch (e) {}\n"
"      return ocd.apply(this, arguments);\n"
"    };\n"
"  }\n"
"  try {\n"
"    var pe = G.document && G.document.createElement ? G.document.createElement('div') : null;\n"
"    var po = pe;\n"
"    while (po && !Object.prototype.hasOwnProperty.call(po, 'attachShadow')) po = Object.getPrototypeOf(po);\n"
"    if (po) {\n"
"      var oas = po.attachShadow;\n"
"      po.attachShadow = function () { S.ce.shadow++; return oas.apply(this, arguments); };\n"
"    }\n"
"  } catch (e) {}\n"
   /* ---- the observers, discovered by shape. See the header comment. */
"  try {\n"
"    Object.getOwnPropertyNames(G).forEach(function (n) {\n"
"      if (n.length < 9 || n.slice(-8) !== 'Observer') return;\n"
"      var C;\n"
"      try { C = G[n]; } catch (e) { return; }\n"
"      if (typeof C !== 'function' || !C.prototype) return;\n"
"      if (typeof C.prototype.observe !== 'function') return;\n"
"      var rec = S.obs[n] = { made: 0, observed: 0, fired: 0, records: 0 };\n"
"      var op = C.prototype.observe;\n"
"      C.prototype.observe = function () { rec.observed++; return op.apply(this, arguments); };\n"
"      try {\n"
"        G[n] = new Proxy(C, {\n"
"          construct: function (T, args, NT) {\n"
"            rec.made++;\n"
"            var a = Array.prototype.slice.call(args);\n"
"            if (typeof a[0] === 'function') {\n"
"              var cb = a[0];\n"
"              a[0] = function (recs) {\n"
"                rec.fired++;\n"
"                try { if (recs && recs.length) rec.records += recs.length; } catch (e) {}\n"
"                return cb.apply(this, arguments);\n"
"              };\n"
"            }\n"
"            return Reflect.construct(T, a, NT === undefined ? T : NT);\n"
"          }\n"
"        });\n"
"      } catch (e) {}\n"
"    });\n"
"  } catch (e) {}\n"
   /* ---- LISTENERS THAT ARE WAITING FOR AN EVENT THAT NEVER COMES.
    *
    * NO FIXED LIST OF EVENT TYPES. The first version of this section carried
    * one -- DOMContentLoaded, load, pageshow, popstate -- and a fixed list is
    * exactly the mistake probe-webapi was built to stop making: it can only
    * ever report what its author already suspected. Instead, EVERY type a page
    * registers for is recorded, and at the moment it registers, a witness
    * listener is added alongside it. So the question answered is the general
    * one: of everything this page is listening for, what did this browser
    * never fire?
    *
    * `seen` IS A FLAG, NOT A COUNT, and that is deliberate. `window` and
    * `document` are two JS objects over ONE underlying node in this browser
    * (js_dom.c: doc_addEventListener binds both to g_root), so a witness
    * registered through each would be invoked twice per dispatch and the first
    * version of this file duly reported `dispatched=2` for a single event. A
    * flag cannot be double-counted, and "did it ever fire" is the entire
    * question. The listener count is per registration and is genuine.
    *
    * The witness is registered in the CAPTURE phase so it runs even if a page
    * handler calls stopPropagation, and through the ORIGINAL method so it is
    * not counted as one of the page's own registrations. */
"  var rec = function (t) { return S.life[t] || (S.life[t] = { lsn: 0, seen: 0 }); };\n"
"  var wrapAEL = function (obj) {\n"
"    if (!obj) return;\n"
"    var oa;\n"
"    try { oa = obj.addEventListener; } catch (e) { return; }\n"
"    if (typeof oa !== 'function' || obj.__stallAEL) return;\n"
"    obj.addEventListener = function (t) {\n"
"      try {\n"
"        t = String(t);\n"
"        var r = rec(t); r.lsn++; bump(S.lsn, t, 1);\n"
"        var key = '__stallSaw$' + t;\n"
"        if (!this[key]) {\n"
"          try { Object.defineProperty(this, key, { value: 1, enumerable: false, configurable: true }); }\n"
"          catch (e2) { this[key] = 1; }\n"
"          oa.call(this, t, function () { r.seen = 1; }, true);\n"
"        }\n"
"      } catch (e) {}\n"
"      return oa.apply(this, arguments);\n"
"    };\n"
"    try { obj.__stallAEL = 1; } catch (e) {}\n"
"  };\n"
   /* window, document, and the Element prototype -- the three places a page
      can register from. The prototype is found by asking a real element which
      object owns the method, the same walk js_events.c does, rather than by
      naming a constructor this browser may not expose. */
"  wrapAEL(G);\n"
"  try { if (G.document && G.document !== G) wrapAEL(G.document); } catch (e) {}\n"
"  try {\n"
"    var el = G.document && G.document.createElement ? G.document.createElement('div') : null;\n"
"    var o = el;\n"
"    while (o && !Object.prototype.hasOwnProperty.call(o, 'addEventListener')) o = Object.getPrototypeOf(o);\n"
"    if (o) wrapAEL(o);\n"
"  } catch (e) {}\n"
"  return 1;\n"
"})()\n";

/* The report generator. Runs in the page's own context because that is where
 * the counters are; it reads only `__stall` and produces a string. */
static const char STALL_DUMP[] =
"(function () {\n"
"  var S = globalThis.__stall;\n"
"  if (!S) return '';\n"
"  var L = [], n;\n"
"  var pend = function (m) {\n"
"    var out = [], k;\n"
"    for (k in m) if (m[k] > 0) out.push(k + ' x' + m[k]);\n"
"    return out;\n"
"  };\n"
"  L.push('fetch started=' + S.fetch.started + ' settled=' + S.fetch.settled +\n"
"         ' failed=' + S.fetch.failed +\n"
"         ' outstanding=' + (S.fetch.started - S.fetch.settled));\n"
"  pend(S.fetch.org).forEach(function (s) { L.push('fetch-waiting ' + s); });\n"
"  L.push('xhr started=' + S.xhr.started + ' settled=' + S.xhr.settled +\n"
"         ' outstanding=' + (S.xhr.started - S.xhr.settled));\n"
"  pend(S.xhr.org).forEach(function (s) { L.push('xhr-waiting ' + s); });\n"
"  L.push('streamread issued=' + S.read.issued + ' settled=' + S.read.settled +\n"
"         ' outstanding=' + (S.read.issued - S.read.settled));\n"
"  var nlive = function (m) { var k, c = 0; for (k in m) c++; return c; };\n"
"  L.push('raf queued=' + S.raf.queued + ' ran=' + S.raf.ran +\n"
"         ' cancelled=' + S.raf.cancelled + ' outstanding=' + nlive(S.raf.live));\n"
"  L.push('timeout armed=' + S.to.armed + ' fired=' + S.to.fired +\n"
"         ' cleared=' + S.to.cleared + ' outstanding=' + nlive(S.to.live));\n"
"  L.push('interval armed=' + S.iv.armed + ' fired=' + S.iv.fired +\n"
"         ' cleared=' + S.iv.cleared + ' outstanding=' + nlive(S.iv.live));\n"
"  for (n in S.obs) {\n"
"    var o = S.obs[n];\n"
"    if (!o.made && !o.observed) continue;\n"
"    L.push('observer ' + n + ' constructed=' + o.made + ' observe_calls=' + o.observed +\n"
"           ' callbacks=' + o.fired + ' records=' + o.records +\n"
"           (o.observed && !o.fired ? '   NEVER-FIRED' : ''));\n"
"  }\n"
   /* Two lines, and the split is the finding: what the page waited for and got,
    * and what it waited for and never got. The second list is the one this
    * whole instrument exists to print. */
"  if (S.ce.defined || S.ce.shadow)\n"
"    L.push('customelement defined=' + S.ce.defined + ' connected=' + S.ce.connected +\n"
"           ' disconnected=' + S.ce.disconnected + ' attrchanged=' + S.ce.attrchanged +\n"
"           ' attachShadow=' + S.ce.shadow +\n"
"           (S.ce.defined && !S.ce.connected ? '   NEVER-FIRED(connectedCallback)' : ''));\n"
"  var got = [], never = [];\n"
"  for (n in S.life) {\n"
"    var e = S.life[n];\n"
"    if (!e.lsn) continue;\n"
"    (e.seen ? got : never).push(n + '(' + e.lsn + ')');\n"
"  }\n"
"  if (got.length) L.push('listened-and-fired ' + got.join(' '));\n"
"  if (never.length) {\n"
"    L.push('LISTENED-NEVER-FIRED ' + never.join(' '));\n"
   /* THE LINE THAT STOPS THIS LIST FROM BEING READ AS A BUG LIST.
    *
    * `click` and `keydown` appear in it on every page and mean nothing: an
    * automated load produces no pointer and no keyboard, so those listeners
    * are correctly still waiting. The list is only evidence about the BROWSER
    * for a type that fires without user input -- and the reader has to be told
    * that where the list is printed, not in a document they may not have.
    * A list that mixes "this browser cannot do it" with "nobody clicked" and
    * says so is useful; the same list without this line trains people to
    * ignore it. */
"    L.push('  (note: a load with no user input produces no pointer/keyboard/');\n"
"    L.push('   focus events, so those entries are the RUN, not the browser.');\n"
"    L.push('   Types that a browser fires unprompted -- scroll, resize,');\n"
"    L.push('   pageshow, pagehide, visibilitychange -- are the evidence.)');\n"
"  }\n"
"  try { L.push('readyState ' + globalThis.document.readyState); } catch (x) {}\n"
"  return L.join('\\n');\n"
"})()\n";

/* Non-zero once the prelude has been evaluated in `ctx`. Asked of the page
 * rather than kept in a C static: js_page_close() destroys the context and a
 * static would survive it, so the next page would be reported as armed when
 * its globals are bare. */
static int armed(JSContext *ctx)
{
    JSValue g, v;
    int on;
    if (!ctx) return 0;
    g = JS_GetGlobalObject(ctx);
    v = JS_GetPropertyStr(ctx, g, "__stall");
    on = !JS_IsUndefined(v) && !JS_IsNull(v) && !JS_IsException(v);
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, g);
    return on;
}

int js_stall_arm(JSContext *ctx)
{
    JSValue v;
    int ok;
    if (!ctx) return 0;
    if (armed(ctx)) return 1;
    v = JS_Eval(ctx, STALL_PRELUDE, strlen(STALL_PRELUDE), "<stall>",
                JS_EVAL_TYPE_GLOBAL);
    ok = !JS_IsException(v);
    if (!ok) {
        /* An instrument that fails to install and says nothing is the exact
         * failure this whole file exists to catch, so it is not swallowed. */
        JSValue e = JS_GetException(ctx);
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, v);
    return ok;
}

/* A bounded appender: every writer below goes through it, so the report can be
 * assembled without a single length calculation being repeated (and therefore
 * without one of them being wrong). */
struct app { char *b; int cap, len; };

static void ap_str(struct app *a, const char *s)
{
    if (!s) return;
    while (*s && a->len < a->cap - 1) a->b[a->len++] = *s++;
    a->b[a->len] = '\0';
}

static void ap_int(struct app *a, long long v)
{
    char t[24];
    int i = 0, neg = v < 0;
    unsigned long long u = neg ? (unsigned long long)(-v) : (unsigned long long)v;
    if (!u) t[i++] = '0';
    while (u) { t[i++] = (char)('0' + (u % 10)); u /= 10; }
    if (neg && a->len < a->cap - 1) a->b[a->len++] = '-';
    while (i-- > 0 && a->len < a->cap - 1) a->b[a->len++] = t[i];
    a->b[a->len] = '\0';
}

static void ap_kv(struct app *a, const char *k, long long v)
{
    ap_str(a, k); ap_str(a, "="); ap_int(a, v); ap_str(a, " ");
}

/* Fold identical (func, file, line) frames. A framework parks dozens of async
 * calls on the same await; thirty identical lines is a wall of text, and
 * "x30" beside one line is the finding. */
#define STALL_FRAMES 64

static int frame_same(const JSStallFrame *x, const JSStallFrame *y)
{
    return x->line == y->line && x->pc == y->pc &&
           !strcmp(x->func, y->func) && !strcmp(x->file, y->file);
}

int js_stall_report(JSContext *ctx, char *buf, int cap)
{
    JSStallCensus c;
    JSStallFrame fr[STALL_FRAMES];
    int seen[STALL_FRAMES], nuniq = 0;
    struct app a;
    int i, j;

    if (!buf || cap <= 1) return 0;
    a.b = buf; a.cap = cap; a.len = 0; buf[0] = '\0';
    if (!ctx) return 0;

    JS_StallCensus(JS_GetRuntime(ctx), &c, fr, STALL_FRAMES);

    ap_str(&a, "promise ");
    ap_kv(&a, "total", c.promise_total);
    ap_kv(&a, "pending", c.promise_pending);
    ap_kv(&a, "pending_awaited", c.promise_pending_awaited);
    ap_kv(&a, "fulfilled", c.promise_fulfilled);
    ap_kv(&a, "rejected", c.promise_rejected);
    ap_kv(&a, "rejected_unhandled", c.promise_rejected_unhandled);
    ap_str(&a, "\nasync ");
    ap_kv(&a, "suspended", c.async_suspended);
    ap_str(&a, "\n");

    /* The await sites, folded. */
    for (i = 0; i < c.async_frames; i++) {
        for (j = 0; j < nuniq; j++)
            if (frame_same(&fr[seen[j]], &fr[i])) break;
        if (j < nuniq) continue;
        seen[nuniq++] = i;
    }
    for (j = 0; j < nuniq; j++) {
        int k = seen[j], n = 0;
        for (i = 0; i < c.async_frames; i++) if (frame_same(&fr[i], &fr[k])) n++;
        ap_str(&a, "async-at ");
        ap_str(&a, fr[k].func[0] ? fr[k].func : "<anonymous>");
        ap_str(&a, " ");
        ap_str(&a, fr[k].file[0] ? fr[k].file : "<no-debug-info>");
        ap_str(&a, ":");
        ap_int(&a, fr[k].line);
        ap_str(&a, " pc=");
        ap_int(&a, fr[k].pc);
        if (n > 1) { ap_str(&a, " x"); ap_int(&a, n); }
        ap_str(&a, "\n");
    }
    if (c.async_suspended > c.async_frames) {
        ap_str(&a, "async-at (");
        ap_int(&a, c.async_suspended - c.async_frames);
        ap_str(&a, " more, frame buffer full)\n");
    }

    /* The armed half, if it is there. */
    if (armed(ctx)) {
        JSValue v = JS_Eval(ctx, STALL_DUMP, strlen(STALL_DUMP), "<stall>",
                            JS_EVAL_TYPE_GLOBAL);
        if (!JS_IsException(v)) {
            const char *s = JS_ToCString(ctx, v);
            if (s && s[0]) { ap_str(&a, s); ap_str(&a, "\n"); }
            if (s) JS_FreeCString(ctx, s);
        } else {
            JSValue e = JS_GetException(ctx);
            JS_FreeValue(ctx, e);
            ap_str(&a, "stall-report FAILED (the instrument, not the page)\n");
        }
        JS_FreeValue(ctx, v);
    } else {
        ap_str(&a, "unarmed (native census only; no fetch/observer/timer attribution)\n");
    }
    return a.len;
}

int js_stall_blocked(JSContext *ctx)
{
    JSStallCensus c;
    char buf[4096];
    int n, i;

    if (!ctx) return 0;
    JS_StallCensus(JS_GetRuntime(ctx), &c, 0, 0);
    if (c.promise_pending_awaited > 0 || c.async_suspended > 0) return 1;

    /* The armed counters can show a block the census cannot: an outstanding
     * rAF holds no promise at all, and an observer that never fired holds
     * nothing whatsoever -- it is a callback the browser owes the page. So the
     * report is scanned for a non-zero `outstanding=` and for the two verdict
     * words. buf is NUL-terminated, so strncmp cannot run off the end. */
    n = js_stall_report(ctx, buf, (int)sizeof buf);
    for (i = 0; i < n; i++) {
        if (!strncmp(buf + i, "NEVER-FIRED", 11)) return 1;
        if (!strncmp(buf + i, "outstanding=", 12)) {
            const char *d = buf + i + 12;
            if (*d == '-') continue;               /* a wrapped counter, not a wait */
            while (*d >= '0' && *d <= '9') { if (*d != '0') return 1; d++; }
        }
    }
    return 0;
}
