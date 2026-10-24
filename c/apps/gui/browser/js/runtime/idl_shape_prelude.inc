/* The shape pass's prelude -- idl_shape.c's header says what and why; this is
 * how. It is ONE function expression, called once per realm with the global
 * and mkIface (a native illegal-constructor factory). Original contract: it
 * returns nothing. Correction (2026-09-30): it returns a private record object
 * records for the following native walk; no author-visible key holds them.
 *
 * Written as C string lines so its comments stay out of the JS: the engine
 * never sees them, and with 'use strip' it keeps no source text either. The
 * JS uses single quotes only and no backslashes, so no line here needs C
 * escaping beyond the quotes around it. */
"(function (G, mkIface, eventTypes) {\n"
"'use strip';\n"
/* 'use strip' drops the source text QuickJS would otherwise keep for every
 * function below, so Function.prototype.toString on a moved getter answers
 * with the engine's [native code] form rather than handing a page this file.
 * It also drops line numbers from their stacks, which is the same trade. */
"var O = Object, dP = O.defineProperty, gOPD = O.getOwnPropertyDescriptor,\n"
"    gPO = O.getPrototypeOf, sPO = O.setPrototypeOf, ownKeys = Reflect.ownKeys,\n"
"    rApply = Reflect.apply, TE = TypeError, TAG = Symbol.toStringTag, WM = WeakMap,\n"
"    isProto = O.prototype.isPrototypeOf, hasOwnP = O.prototype.hasOwnProperty,\n"
"    freeze = O.freeze;\n"
/* Every intrinsic above is captured NOW, before any page script runs: a page
 * that later replaces Function.prototype.apply or Object.defineProperty must
 * not be able to reach inside a platform getter through them. */
"function has(o, k) { return rApply(hasOwnP, o, [k]); }\n"
"function isObj(o) { return o !== null && (typeof o === 'object' || typeof o === 'function'); }\n"
/* Turning data into a getter hides its object from a quiet descriptor walk.
 * Preserve the actual value and its original key, not a later getter result:
 * invoking a getter can run code, and marking the prelude's bytecode changes
 * only future closures, not these already-created namespace operations.
 * A one-property record also keeps a function-valued attribute's real name
 * available to the walk. These records live only until the C caller frees
 * the returned object after the single native pass.
 *
 * The first handoff used a packed array: reflection listed 12 properties,
 * but JS_GetOwnPropertyQuiet cannot read packed elements, so the real gate
 * remained 48/2. Use ordinary own slots on null-prototype objects; otherwise
 * an apparently populated metadata list silently seeds no roots at all. */
"var hidden = O.create(null), hiddenCount = 0;\n"
"function keepData(k, v) {\n"
"  if (!isObj(v)) return;\n"
"  var record = O.create(null); record[k] = v;\n"
"  hidden[hiddenCount++] = record;\n"
"}\n"
"function illegal() { throw new TE('Illegal invocation'); }\n"
"function arity(f, len) { try { dP(f, 'length', { value: len, configurable: true }); } catch (e) {} return f; }\n"
/* Method-definition syntax, never `function`: a WebIDL operation has no
 * .prototype and is not a constructor, and a computed key names it exactly
 * ('get userAgent', 'pushState') without a defineProperty on 'name'. */
"function method(name, len, body) {\n"
"  var o = { [name](...a) { return body(this, a); } };\n"
"  return arity(o[name], len);\n"
"}\n"
"function getter(name, body) { return gOPD({ get [name]() { return body(this); } }, name).get; }\n"
"function setter(name, body) { return gOPD({ set [name](v) { body(this, v); } }, name).set; }\n"
"function defM(P, name, f) { dP(P, name, { value: f, writable: true, enumerable: true, configurable: true }); }\n"
"function defA(P, name, g, s) { dP(P, name, { get: g, set: s, enumerable: true, configurable: true }); }\n"
"function drop(o, k) { try { delete o[k]; } catch (e) {} }\n"
"\n"
/* ==== singletons: one object per realm, so the brand is identity ============
 * `navigator`, `history`, ... are the ONLY objects in their realm with their
 * interface's prototype in the chain, so "is this receiver a Navigator" is
 * "is it THE navigator". A Proxy over it is not it -- which is also what a
 * browser answers. Each table row says what the member is: a method with its
 * WebIDL length, a readonly attribute (data or accessor today, a getter
 * returning the SAME value after -- [SameObject] by construction), or a
 * read-write attribute with the setter's WebIDL conversion. A name NOT in a
 * table is left where it is, and tests/unit/idl_shape_test.c's "owns
 * nothing" checks are what make that loud. */
"function moveSingleton(inst, P, table) {\n"
"  if (!isObj(inst) || !isObj(P) || gPO(inst) !== P) return;\n"
"  var brand = function (t) { if (t !== inst) illegal(); };\n"
"  ownKeys(inst).forEach(function (k) {\n"
"    if (typeof k !== 'string' || !has(table, k)) return;\n"
"    var spec = table[k], d = gOPD(inst, k);\n"
"    if (!d || !d.configurable) return;\n"
"    if (spec.m !== undefined) {\n"
"      var f = d.value;\n"
"      if (typeof f !== 'function') return;\n"
"      defM(P, k, method(k, spec.m, function (t, a) { brand(t); return rApply(f, inst, a); }));\n"
"    } else if (spec.rw) {\n"
"      var cur = has(d, 'value') ? d.value : (d.get ? rApply(d.get, inst, []) : undefined);\n"
"      if (has(d, 'value')) keepData(k, cur);\n"
"      defA(P, k, getter(k, function (t) { brand(t); return cur; }),\n"
"                 setter(k, function (t, v) { brand(t); var s = spec.rw(v); if (s !== undefined) cur = s; }));\n"
"    } else if (d.get) {\n"
"      var g = d.get, s = d.set;\n"
"      defA(P, k, getter(k, function (t) { brand(t); return rApply(g, inst, []); }),\n"
"                 s ? setter(k, function (t, v) { brand(t); rApply(s, inst, [v]); }) : undefined);\n"
"    } else {\n"
"      var v = d.value;\n"
"      keepData(k, v);\n"
"      if (spec.freeze && isObj(v)) { try { freeze(v); } catch (e) {} }\n"
"      defA(P, k, getter(k, function (t) { brand(t); return v; }), undefined);\n"
"    }\n"
"    drop(inst, k);\n"
"  });\n"
"}\n"
"var A = {};\n"
"function M(n) { return { m: n }; }\n"
/* languages is a FrozenArray: one array, frozen. Nothing in the tree writes it
 * after page.c builds it (grep 'languages' c/apps/gui/browser). */
"var NAV = { userAgent: A, appName: A, platform: A, language: A, languages: { freeze: true },\n"
"  onLine: A, cookieEnabled: A, hardwareConcurrency: A, maxTouchPoints: A, scheduling: A,\n"
"  doNotTrack: A, webdriver: A, sendBeacon: M(1), vendor: A, javaEnabled: M(0), product: A,\n"
"  mimeTypes: A, plugins: A, clipboard: A, serviceWorker: A };\n"
"var SCR = { width: A, height: A, colorDepth: A, pixelDepth: A };\n"
/* scrollRestoration is WRITABLE, and routers write it from strict-mode module
 * code: a getter-only accessor would make `history.scrollRestoration =
 * 'manual'` throw. A WebIDL enum setter ignores a value outside the enum. */
"var HIS = { pushState: M(2), replaceState: M(2), back: M(0), forward: M(0), go: M(0),\n"
"  length: A, state: A,\n"
"  scrollRestoration: { rw: function (v) { v = String(v); return (v === 'auto' || v === 'manual') ? v : undefined; } } };\n"
/* timing must come back as the SAME object: platform.c's __platMarkTiming
 * writes its fields in strict mode as the lifecycle advances, and readyState
 * only moves on after those writes succeed. */
"var PERF = { now: M(0), timeOrigin: A, mark: M(1), measure: M(1), getEntries: M(0),\n"
"  getEntriesByName: M(1), getEntriesByType: M(1), clearMarks: M(0), clearMeasures: M(0),\n"
"  clearResourceTimings: M(0), setResourceTimingBufferSize: M(1), toJSON: M(0),\n"
"  timing: A, navigation: A };\n"
"var CRY = { getRandomValues: M(1), randomUUID: M(0), subtle: A };\n"
"\n"
/* Screen and Crypto have an instance and no interface object: webapi.c's
 * singleton loop runs before `screen` (webapi.c, after the loop) and `crypto`
 * (platform.c) exist, and skips them. Mint the interface the way that loop
 * would have -- a fresh prototype spliced in under the instance, a native
 * illegal constructor -- but only where the instance exists and the name is
 * free. An interface with no instance would be a new way to claim a feature. */
"function mint(name, inst) {\n"
"  if (!isObj(inst)) return null;\n"
"  var C = G[name];\n"
"  if (C !== undefined) return (typeof C === 'function' && isObj(C.prototype) && gPO(inst) === C.prototype) ? C.prototype : null;\n"
"  if (gPO(inst) !== O.prototype) return null;\n"
"  var P = {};\n"
"  C = mkIface(name, P);\n"
"  sPO(inst, P);\n"
"  dP(G, name, { value: C, writable: true, enumerable: false, configurable: true });\n"
"  return P;\n"
"}\n"
/* The other three come from webapi.c. A build without it has no Navigator, and
 * then nothing moves: members must never land on Object.prototype. */
"function protoOf(name, inst) {\n"
"  var C = G[name];\n"
"  return (typeof C === 'function' && isObj(C.prototype) && isObj(inst) && gPO(inst) === C.prototype) ? C.prototype : null;\n"
"}\n"
"moveSingleton(G.navigator, protoOf('Navigator', G.navigator), NAV);\n"
"moveSingleton(G.history, protoOf('History', G.history), HIS);\n"
"moveSingleton(G.performance, protoOf('Performance', G.performance), PERF);\n"
"moveSingleton(G.screen, mint('Screen', G.screen), SCR);\n"
"moveSingleton(G.crypto, mint('Crypto', G.crypto), CRY);\n"
"\n"
/* ==== document: a SHARED prototype ===========================================
 * Document.prototype is also the prototype of every createHTMLDocument()
 * document (dom_iface.inc maps is_docx to IF_DOCUMENT), and nearly every
 * member being moved is a closure over THIS page's document, or C that ignores
 * `this` and reads the page root. Moved as they are, a scratch document would
 * read the page's cookie jar, write into the page at the current script, and
 * hand out the page's Selection. So every member dispatches on its receiver:
 *   the page document       -> exactly what it did before the move
 *   another Document        -> its own answer where one exists (a per-node
 *                              C slot, a search of ITS tree), else the spec's
 *                              answer for a document with no browsing context
 *   anything else           -> TypeError: Illegal invocation
 * "Another Document" is decided by the NATIVE nodeType getter captured here
 * (9 for the page and for createHTMLDocument documents), or by the DOMParser
 * document prototype -- a DOMParser document reaching an extracted method via
 * .call() is a real idiom (DOMPurify's getElementsByTagName.call). */
"var D = G.document, DC = G.Document, DP = DC && DC.prototype;\n"
"var NP = G.Node && G.Node.prototype, EP = G.Element && G.Element.prototype;\n"
"var NT = NP && gOPD(NP, 'nodeType'), PN = NP && gOPD(NP, 'parentNode'), LN = EP && gOPD(EP, 'localName');\n"
"NT = NT && NT.get; PN = PN && PN.get; LN = LN && LN.get;\n"
"var DPDOC = null;\n"
"try { DPDOC = G.DOMParser && G.DOMParser.__dpDocProto; } catch (e) {}\n"
"function nodeType(o) { try { return NT ? rApply(NT, o, []) : undefined; } catch (e) { return undefined; } }\n"
"function docKind(o) {\n"
"  if (o === D) return 1;\n"
"  if (!isObj(o)) return 0;\n"
"  if (nodeType(o) === 9) return 2;\n"
"  try { if (DPDOC && rApply(isProto, DPDOC, [o])) return 2; } catch (e) {}\n"
"  return 0;\n"
"}\n"
/* [SameObject] per document: one answer per foreign document, kept weakly. */
"function perDoc(make) {\n"
"  var m = new WM();\n"
"  return function (t) {\n"
"    if (!m.has(t)) { var v = null; try { v = make(t); } catch (e) {} m.set(t, v); }\n"
"    return m.get(t);\n"
"  };\n"
"}\n"
/* select.c's getElementsByTagName already answers for any Document. */
"var byTagFn = DP && gOPD(DP, 'getElementsByTagName');\n"
"byTagFn = byTagFn && typeof byTagFn.value === 'function' ? byTagFn.value : null;\n"
"function byTag(t, tag) { try { return byTagFn ? rApply(byTagFn, t, [tag]) : []; } catch (e) { return []; } }\n"
"function elOn(name) {\n"
"  var d = EP && gOPD(EP, name);\n"
"  return d && d.get && d.set ? d : null;\n"
"}\n"
"function ctorOr(name) {\n"
"  var C = G[name];\n"
"  return typeof C === 'function' ? function () { return new C(); } : function () { return null; };\n"
"}\n"
"var noop = function () {};\n"
/* A collection built here answers what the page's own does: select.c gives its
 * snapshots NodeList.prototype / HTMLCollection.prototype (which chain to
 * Array.prototype in this engine -- dom_iface.inc says why). */
"function listOf(arr, name) {\n"
"  var C = G[name];\n"
"  if (typeof C === 'function' && isObj(C.prototype)) { try { sPO(arr, C.prototype); } catch (e) {} }\n"
"  return arr;\n"
"}\n"
/* What a document that is NOT this page's answers, per member. 'same': the
 * member never read the page (a factory, a clone). 'element': the event trio,
 * served by Element.prototype's this-aware copy (events.c's wrapper over the
 * el_* natives), because the page's own copy is bound to the page root. 'on':
 * Element.prototype's per-node C slot. A function: the default itself. */
"var FOREIGN = {\n"
"  domain: 'domain',\n"
"  cookie: { get: function () { return ''; }, set: noop },\n"
"  compatMode: function () { return 'CSS1Compat'; },\n"
"  getElementsByName: function (t, a) {\n"
"    var want = String(a[0]), out = [], root = null;\n"
"    try { root = t.documentElement; } catch (e) {}\n"
"    var walk = function (el) {\n"
"      for (var c = el.firstElementChild; c; c = c.nextElementSibling) {\n"
"        if (c.getAttribute && c.getAttribute('name') === want) out.push(c);\n"
"        walk(c);\n"
"      }\n"
"    };\n"
"    if (root) {\n"
"      if (root.getAttribute && root.getAttribute('name') === want) out.push(root);\n"
"      walk(root);\n"
"    }\n"
"    return listOf(out, 'NodeList');\n"
"  },\n"
"  scripts: function (t) { return byTag(t, 'script'); },\n"
"  images: function (t) { return byTag(t, 'img'); },\n"
"  forms: function (t) { return byTag(t, 'form'); },\n"
"  links: function (t) {\n"
"    var out = [];\n"
"    ['a', 'area'].forEach(function (n) {\n"
"      var l = byTag(t, n);\n"
"      for (var i = 0; i < l.length; i++) if (l[i].getAttribute('href') !== null) out.push(l[i]);\n"
"    });\n"
"    return listOf(out, 'HTMLCollection');\n"
"  },\n"
"  activeElement: function (t) { return t.body || t.documentElement || null; },\n"
"  readyState: function () { return 'complete'; },\n"
"  visibilityState: function () { return 'hidden'; },\n"
"  hidden: function () { return true; },\n"
"  currentScript: function () { return null; },\n"
"  scrollingElement: function (t) { return t.documentElement || null; },\n"
"  createTreeWalker: 'same', importNode: 'same', adoptNode: 'same', createEvent: 'same',\n"
"  createRange: 'same', queryCommandSupported: 'same',\n"
"  write: noop, writeln: noop,\n"
"  addEventListener: 'element', removeEventListener: 'element', dispatchEvent: 'element',\n"
"  styleSheets: perDoc(function () { return []; }),\n"
"  elementFromPoint: function () { return null; },\n"
"  elementsFromPoint: function () { return []; },\n"
"  getSelection: function () { return null; },\n"
"  execCommand: function () { return false; },\n"
"  queryCommandEnabled: function () { return false; },\n"
"  queryCommandIndeterm: function () { return false; },\n"
"  queryCommandState: function () { return false; },\n"
"  queryCommandValue: function () { return ''; },\n"
"  timeline: perDoc(ctorOr('DocumentTimeline')),\n"
"  getAnimations: function () { return []; },\n"
"  fonts: perDoc(ctorOr('FontFaceSet'))\n"
"};\n"
"var DLEN = { getElementsByName: 1, createTreeWalker: 1, importNode: 1, adoptNode: 1, write: 0,\n"
"  writeln: 0, createEvent: 1, addEventListener: 2, removeEventListener: 2, dispatchEvent: 1,\n"
"  elementFromPoint: 2, elementsFromPoint: 2, createRange: 0, getSelection: 0, execCommand: 1,\n"
"  queryCommandEnabled: 1, queryCommandIndeterm: 1, queryCommandState: 1,\n"
"  queryCommandSupported: 1, queryCommandValue: 1, getAnimations: 0 };\n"
/* The original list named 24 GlobalEventHandlers. Adding invalid/formdata to
 * dom.c left them behind as document own properties (three shape assertions
 * failed). Derive from the producer so the next handler cannot drift again.
 * `unload` is
 * the 25th, and it is not a Document member at all -- 'onunload' in document
 * is false in a browser, and nothing here ever dispatches unload -- so it is
 * dropped rather than moved. */
"eventTypes.forEach(function (ty) { if (ty !== 'unload') FOREIGN['on' + ty] = 'on'; });\n"
"\n"
"function moveDocument() {\n"
  /* The flat build (-DJSDOM_NO_INTERFACE_HIERARCHY, test-dom-iface-negctl):
   * document's prototype is the one class prototype EVERY node shares and
   * Document.prototype is an orphan. Moving page closures onto the first
   * would hand them to every element; onto the second, strip them from
   * document. Either way the pass must not run. */
"  if (!isObj(D) || !isObj(DP) || gPO(D) !== DP) return;\n"
"  var host = '';\n"
"  try { host = String(D.domain); } catch (e) {}\n"
"  ownKeys(D).forEach(function (k) {\n"
"    if (typeof k !== 'string') return;\n"
"    if (k === 'onunload') { drop(D, k); return; }\n"
    /* `location` stays: [LegacyUnforgeable]. So does anything new -- the
     * gate's "document owns exactly [location]" is what reports it. */
"    if (!has(FOREIGN, k)) return;\n"
"    var d = gOPD(D, k), other = FOREIGN[k];\n"
"    if (!d || !d.configurable) return;\n"
"    if (other === 'on') {\n"
"      var eo = elOn(k), g0 = d.get, s0 = d.set;\n"
"      if (!g0 || !s0) return;\n"
"      defA(DP, k,\n"
"        getter(k, function (t) {\n"
"          var w = docKind(t);\n"
"          if (w === 1) return rApply(g0, D, []);\n"
"          if (w === 2) return eo ? rApply(eo.get, t, []) : null;\n"
"          illegal();\n"
"        }),\n"
"        setter(k, function (t, v) {\n"
"          var w = docKind(t);\n"
"          if (w === 1) { rApply(s0, D, [v]); return; }\n"
"          if (w === 2) { if (eo) rApply(eo.set, t, [v]); return; }\n"
"          illegal();\n"
"        }));\n"
"    } else if (other === 'domain') {\n"
      /* A page write is STORED and read back (webapi.c's comment on this
       * property relies on that); no cookie is ever scoped from it. A
       * document with no browsing context refuses the write, as the spec's
       * setter does. */
"      var cur = has(d, 'value') ? d.value : host;\n"
"      defA(DP, k,\n"
"        getter(k, function (t) { var w = docKind(t); if (w === 1) return cur; if (w === 2) return host; illegal(); }),\n"
"        setter(k, function (t, v) {\n"
"          var w = docKind(t);\n"
"          if (w === 1) { cur = String(v); return; }\n"
"          if (w === 2) {\n"
"            if (typeof G.DOMException === 'function')\n"
"              throw new G.DOMException('Assignment is forbidden for a document without a browsing context.', 'SecurityError');\n"
"            return;\n"
"          }\n"
"          illegal();\n"
"        }));\n"
"    } else if (typeof d.value === 'function' && !d.get) {\n"
"      var f = d.value, fe = null;\n"
"      if (other === 'element') {\n"
"        var ed = EP && gOPD(EP, k);\n"
"        fe = ed && typeof ed.value === 'function' ? ed.value : null;\n"
"      }\n"
"      defM(DP, k, method(k, has(DLEN, k) ? DLEN[k] : f.length, function (t, a) {\n"
"        var w = docKind(t);\n"
"        if (w === 1) return rApply(f, D, a);\n"
"        if (w === 2) {\n"
"          if (other === 'same') return rApply(f, t, a);\n"
"          if (other === 'element') { if (fe) return rApply(fe, t, a); illegal(); }\n"
"          return other(t, a);\n"
"        }\n"
"        illegal();\n"
"      }));\n"
"    } else if (typeof other === 'object') {\n"
"      var gc = d.get, sc = d.set;\n"
"      if (!gc) return;\n"
"      defA(DP, k,\n"
"        getter(k, function (t) { var w = docKind(t); if (w === 1) return rApply(gc, D, []); if (w === 2) return other.get(t); illegal(); }),\n"
"        setter(k, function (t, v) {\n"
"          var w = docKind(t);\n"
"          if (w === 1) { if (sc) rApply(sc, D, [v]); return; }\n"
"          if (w === 2) { other.set(t, v); return; }\n"
"          illegal();\n"
"        }));\n"
"    } else {\n"
      /* A readonly attribute, accessor or data today (fonts and timeline are
       * data): after, a getter that hands back the SAME object for the page. */
"      var ga = d.get, va = d.value;\n"
"      if (!ga) keepData(k, va);\n"
"      defA(DP, k, getter(k, function (t) {\n"
"        var w = docKind(t);\n"
"        if (w === 1) return ga ? rApply(ga, D, []) : va;\n"
"        if (w === 2) return other(t, []);\n"
"        illegal();\n"
"      }), undefined);\n"
"    }\n"
"    drop(D, k);\n"
"  });\n"
"}\n"
"moveDocument();\n"
"\n"
/* ==== <body>: the Window-reflecting event handlers ============================
 * cssom.c defines the 22 as one descriptor on window AND on the body wrapper,
 * both over one held slot. On HTMLBodyElement.prototype (and
 * HTMLFrameSetElement.prototype, which HTML gives the same set) they alias the
 * Window's handler for a body whose node document is this page's, and read
 * null / ignore a write for a body inside another document, which has no
 * browsing context. A detached body counts as the page's: createHTMLDocument's
 * nodes live in the page's arena and report the page as ownerDocument, so a
 * detached one cannot be told apart, and the page's own createElement('body')
 * is the common case. */
"var BTYPES = ['load', 'unload', 'beforeunload', 'resize', 'scroll', 'error', 'blur', 'focus',\n"
"  'hashchange', 'languagechange', 'message', 'messageerror', 'offline', 'online', 'pagehide',\n"
"  'pageshow', 'popstate', 'rejectionhandled', 'storage', 'unhandledrejection', 'afterprint',\n"
"  'beforeprint'];\n"
"function bodyKind(o, want) {\n"
"  var ln;\n"
"  try { ln = LN ? rApply(LN, o, []) : undefined; } catch (e) { return 0; }\n"
"  if (ln !== want) return 0;\n"
"  var n = o, p;\n"
"  for (var i = 0; i < 100000; i++) {\n"
"    try { p = rApply(PN, n, []); } catch (e) { p = null; }\n"
"    if (!p) break;\n"
"    n = p;\n"
"  }\n"
"  if (n === D) return 1;\n"
"  return nodeType(n) === 9 ? 2 : 1;\n"
"}\n"
"function moveBody() {\n"
"  var BP = G.HTMLBodyElement && G.HTMLBodyElement.prototype;\n"
"  var FP = G.HTMLFrameSetElement && G.HTMLFrameSetElement.prototype;\n"
"  if (!isObj(D) || !isObj(BP) || !LN || !PN) return;\n"
"  var el = null;\n"
"  try { el = D.body || D.documentElement; } catch (e) {}\n"
"  if (!isObj(el)) return;\n"
"  BTYPES.forEach(function (ty) {\n"
"    var k = 'on' + ty, wd = gOPD(G, k), bd = gOPD(el, k);\n"
    /* Only cssom.c's shared slot: the same getter on window and on the body. */
"    if (!wd || !bd || !wd.get || !wd.set || bd.get !== wd.get) return;\n"
"    var wg = wd.get, ws = wd.set;\n"
"    [[BP, 'body'], [FP, 'frameset']].forEach(function (pair) {\n"
"      if (!isObj(pair[0])) return;\n"
"      var tag = pair[1];\n"
"      defA(pair[0], k,\n"
"        getter(k, function (t) { var w = bodyKind(t, tag); if (w === 1) return rApply(wg, G, []); if (w === 2) return null; illegal(); }),\n"
"        setter(k, function (t, v) { var w = bodyKind(t, tag); if (w === 1) { rApply(ws, G, [v]); return; } if (w === 2) return; illegal(); }));\n"
"    });\n"
"    drop(el, k);\n"
"  });\n"
"}\n"
"moveBody();\n"
"\n"
/* ==== chains: EventTarget where a browser has it ==============================
 * Does EventTarget.prototype's trio work on an object that is not a node? It
 * does when js_events.c's store is installed. A realm without that file (a
 * network frame's child context) has only the native node-bound trio, and
 * inheriting THAT would hand performance an addEventListener that registers
 * nothing -- so Performance and Screen, which own no event methods of their
 * own, chain only when the store answers a real round trip. */
"var ETC = G.EventTarget, ETP = typeof ETC === 'function' ? ETC.prototype : null;\n"
"function storeWorks() {\n"
"  if (!isObj(ETP) || typeof G.Event !== 'function') return false;\n"
"  try {\n"
"    var o = O.create(ETP), hit = 0, ty = 'idl-shape-probe';\n"
"    rApply(ETP.addEventListener, o, [ty, function () { hit++; }]);\n"
"    rApply(ETP.dispatchEvent, o, [new G.Event(ty)]);\n"
"    return hit === 1;\n"
"  } catch (e) { return false; }\n"
"}\n"
"var STORE = storeWorks();\n"
/* The rest own their event methods on their own prototype, so what they
 * inherit is shadowed and every instance keeps answering exactly as before.
 * 'own' demands the WHOLE trio: XMLHttpRequest, MessagePort and
 * BroadcastChannel own add/removeEventListener but no dispatchEvent, and
 * chaining them would make dispatchEvent resolve to EventTarget's store --
 * present, and never reaching their listeners. They stay unchained until they
 * have a dispatchEvent of their own. */
"function chain(name, need) {\n"
"  var C = G[name], P = typeof C === 'function' ? C.prototype : null;\n"
"  if (!isObj(P) || !isObj(ETP) || P === ETP || gPO(P) !== O.prototype) return;\n"
"  if (need === 'store' && !STORE) return;\n"
"  if (need === 'own' && !['addEventListener', 'removeEventListener', 'dispatchEvent'].every(function (m) {\n"
"        var d = gOPD(P, m); return d && typeof d.value === 'function'; })) return;\n"
"  sPO(P, ETP);\n"
"  try { sPO(C, ETC); } catch (e) {}\n"
"}\n"
"chain('Performance', 'store');\n"
"chain('Screen', 'store');\n"
"['AbortSignal', 'Worker', 'EventSource', 'FontFaceSet', 'XMLHttpRequest', 'MessagePort',\n"
" 'BroadcastChannel'].forEach(function (n) { chain(n, 'own'); });\n"
"\n"
/* window: Window.prototype > the named properties object > EventTarget. The
 * named properties object is WebIDL's, for a [Global] interface, and its class
 * string is the interface name plus 'Properties'. window owns its event trio
 * (dom.c binds it to the document root), so nothing it answers today is
 * shadowed by what it now inherits. platform.c's Window stub carried a
 * Symbol.hasInstance testing identity with window; with the real chain that
 * is what ordinary instanceof answers, so the override goes. */
"(function () {\n"
"  var W = G.Window, WP = typeof W === 'function' ? W.prototype : null;\n"
"  if (!isObj(WP) || !isObj(ETP) || gPO(G) !== O.prototype || gPO(WP) !== O.prototype) return;\n"
"  if (!has(G, 'addEventListener') || typeof G.addEventListener !== 'function') return;\n"
"  var props = O.create(ETP);\n"
"  dP(props, TAG, { value: 'WindowProperties', configurable: true });\n"
"  sPO(WP, props);\n"
"  dP(WP, 'constructor', { value: W, writable: true, configurable: true });\n"
"  if (has(W, Symbol.hasInstance)) drop(W, Symbol.hasInstance);\n"
"  try { sPO(W, ETC); } catch (e) {}\n"
"  try { dP(W, 'prototype', { writable: false }); } catch (e) {}\n"
"  sPO(G, WP);\n"
"})();\n"
"\n"
/* ==== @@toStringTag, and interface objects that do not enumerate ============
 * Every platform interface's prototype carries its name as @@toStringTag,
 * {writable:false, enumerable:false, configurable:true}; one already present
 * with the wrong flags (anim.c assigns DocumentTimeline's) is normalised. And
 * interface objects, like every ECMAScript global, are not enumerable on
 * window. ECMAScript's own constructors keep ECMAScript's answers:
 * Uint8Array.prototype has no tag of its own on purpose, and SuppressedError
 * is intl.c's polyfill of an ECMAScript error, so is on the list; anything
 * whose prototype descends from Error.prototype is skipped as well. Console is
 * skipped: console is a namespace, whose class string is 'console', and the
 * Console interface webapi.c publishes has no counterpart in a browser. */
"var ES = ['Object', 'Function', 'Array', 'Number', 'Boolean', 'String', 'Symbol', 'Date',\n"
"  'Promise', 'RegExp', 'Error', 'AggregateError', 'EvalError', 'RangeError', 'ReferenceError',\n"
"  'SyntaxError', 'TypeError', 'URIError', 'InternalError', 'SuppressedError', 'ArrayBuffer',\n"
"  'SharedArrayBuffer', 'DataView', 'Int8Array', 'Uint8Array', 'Uint8ClampedArray', 'Int16Array',\n"
"  'Uint16Array', 'Int32Array', 'Uint32Array', 'Float16Array', 'Float32Array', 'Float64Array',\n"
"  'BigInt64Array', 'BigUint64Array', 'BigInt', 'Map', 'Set', 'WeakMap', 'WeakSet', 'WeakRef',\n"
"  'FinalizationRegistry', 'Proxy', 'Reflect', 'JSON', 'Math', 'Atomics', 'Intl', 'Iterator',\n"
"  'DisposableStack', 'AsyncDisposableStack', 'BigFloat', 'BigDecimal', 'BigFloatEnv', 'Operators'];\n"
"var esSet = O.create(null);\n"
"ES.forEach(function (n) { esSet[n] = true; });\n"
"var ERRP = Error.prototype;\n"
"ownKeys(G).forEach(function (n) {\n"
"  if (typeof n !== 'string' || !/^[A-Z]/.test(n)) return;\n"
"  var d = gOPD(G, n);\n"
"  if (!d || !has(d, 'value') || !isObj(d.value)) return;\n"
"  if (d.enumerable && d.configurable) { try { dP(G, n, { enumerable: false }); } catch (e) {} }\n"
"  if (esSet[n] || n === 'Console') return;\n"
"  var C = d.value, P = typeof C === 'function' ? C.prototype : null;\n"
"  if (!isObj(P)) return;\n"
"  var pc = gOPD(P, 'constructor');\n"
"  if (!pc || pc.value !== C) return;\n"
"  try { if (rApply(isProto, ERRP, [P])) return; } catch (e) {}\n"
"  var td = gOPD(P, TAG);\n"
"  if (!td) dP(P, TAG, { value: n, writable: false, enumerable: false, configurable: true });\n"
"  else if (has(td, 'value') && typeof td.value === 'string' && td.configurable && (td.writable || td.enumerable))\n"
"    dP(P, TAG, { value: td.value, writable: false, enumerable: false, configurable: true });\n"
"});\n"
"return hidden;\n"
"})\n"
