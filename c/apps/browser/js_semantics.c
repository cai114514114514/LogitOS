/* js_semantics.c -- the HTML *element* interfaces: the members that hang off
 * HTMLDialogElement, HTMLTableElement, HTMLSelectElement and the rest, plus the
 * two invoker mechanisms (popover and command/commandfor) that the modern HTML
 * spec attaches to <button> and <input>.
 *
 * WHY THIS FILE EXISTS, measured rather than assumed. `html/semantics` is the
 * largest subset in the vendored WPT corpus and almost none of it had been
 * looked at. Ranking its failures by cause (see tests/semantics.mk for the
 * table and how to reproduce it) put ONE mechanism on top by a factor of four
 * over anything else:
 *
 *     html/semantics/popovers              1 / 2733   subtests
 *     html/semantics/the-button-element    9 /  355   (346 of them command/commandfor)
 *     html/semantics/interactive-elements 28 /  492   (466 of them <dialog>)
 *
 * That is 3,580 subtests, 98% of them failing, and they are not 3,580 bugs.
 * js_dom_iface.inc already builds a real prototype hierarchy -- `HTMLDialogElement`
 * exists, `Object.getPrototypeOf(dialog)` is its prototype -- and js_reflect.c
 * already reflects 373 content attributes onto it. What is missing is the
 * MEMBERS: `HTMLDialogElement.prototype` carried exactly one property (`open`,
 * from reflection) and no `showModal`, `close` or `returnValue`;
 * `HTMLTableElement.prototype` carried the nine legacy presentational
 * attributes and no `rows`. And underneath all of it, `HTMLElement.prototype`
 * had no `click()` AT ALL -- so every WPT file that activates a control
 * synthetically (which is how the corpus tests activation behaviour without a
 * test driver) died on `el.click is not a function` before reaching its
 * assertion.
 *
 * THE SEAM. Nothing here edits js_dom.c. It installs from OUTSIDE, onto the
 * prototypes js_dom_iface.inc publishes as globals -- the same seam js_media.c,
 * js_forms.c and js_select.c use, and for the same reason. It asks for a
 * prototype BY NAME (`HTMLTableElement.prototype`) rather than by walking up
 * from `document.createElement('div')`, which is the trap js_select.c's header
 * documents: since 7fc2bec a div's immediate prototype is HTMLDivElement's, so
 * that walk lands a member on <div> and on nothing else.
 *
 * WHAT IS C AND WHAT IS JS. Almost all of it is JS, for the reason js_select.c
 * gives: the DOM this builds on is already fully exposed to script
 * (getAttribute, dispatchEvent, getRootNode, isConnected), so a native
 * implementation would be the same algorithm with a marshalling layer added.
 * The C in this file is two things only: the install entry point, and two
 * predicate hooks published on globalThis for js_select.c to consult, because
 * `:popover-open` and `:modal` are STATE this file owns and the selector engine
 * has no other way to see it.
 *
 * ------------------------------------------------------------------------
 * THE NEGATIVE CONTROL -- -DSEMANTICS_STATIC_COLLECTIONS
 * ------------------------------------------------------------------------
 * Every collection here (`table.rows`, `tbody.rows`, `tr.cells`,
 * `select.options`, `form.elements`, `table.tBodies`) is a LIVE collection: the
 * spec says it reflects the tree as it is now, not as it was when the property
 * was read. A plausible wrong implementation returns a static snapshot -- and
 * it works perfectly on any page that never mutates, which is most pages and
 * most casual testing. The control build makes exactly that mistake: the first
 * read of a collection is cached and returned forever. `make
 * test-semantics-negctl` requires this file's suite to FAIL in that build. It
 * is deliberately NOT "delete the collections": a control that removes the
 * feature proves only that the test calls it.
 */

#include "quickjs.h"
#include <string.h>

int printf(const char *, ...);

/* --------------------------------------------------------------------------
 * The prelude.
 *
 * One IIFE, evaluated once per page, taking no native arguments -- everything
 * it needs is already on the global object by the time js_page.c calls us
 * (last, after js_forms_install; see the ordering comment there).
 * ------------------------------------------------------------------------ */
static const char *SEMANTICS_PRELUDE =
"(function (STATIC_COLLECTIONS) {\n"
"'use strict';\n"
"var G = globalThis, doc = G.document;\n"
"if (!doc || typeof doc.createElement !== 'function') return;\n"

/* ASCII lowercase. Not toLowerCase(): every keyword comparison in HTML is
 * ASCII case-insensitive, and toLowerCase folds U+0130 and the Turkish dotless
 * i, which would make `popover="I"` a different answer in a Turkish locale. */
"function lc(s) { return String(s).replace(/[A-Z]/g, function (c) {\n"
"  return String.fromCharCode(c.charCodeAt(0) + 32); }); }\n"
"function tagOf(el) { return (el && el.tagName) ? lc(el.tagName) : ''; }\n"

/* A DOMException by name. js_platform.c publishes the constructor; the lookup
 * is late (inside the function) because this prelude may run before it in some
 * link, and a page's `catch (e) { e.name === 'InvalidStateError' }` and
 * assert_throws_dom's `e.code` both need the real thing when it is there. */
"function domErr(name, msg) {\n"
"  var DE = G.DOMException;\n"
"  if (typeof DE === 'function') { try { return new DE(msg || name, name); } catch (q) {} }\n"
"  var e = new Error(msg || name); e.name = name; return e;\n"
"}\n"
"function throwDom(name, msg) { throw domErr(name, msg); }\n"

/* Define an accessor pair on a prototype. Silent when the prototype is absent
 * (a link without js_dom_iface.inc's globals) -- this file must never be the
 * reason a page fails to load. */
"function acc(p, name, get, set) {\n"
"  if (!p) return;\n"
"  try { Object.defineProperty(p, name, { configurable: true, enumerable: true,\n"
"    get: get, set: set }); } catch (e) {}\n"
"}\n"
"function meth(p, name, fn) {\n"
"  if (!p) return;\n"
"  try { Object.defineProperty(p, name, { configurable: true, enumerable: true,\n"
"    writable: true, value: fn }); } catch (e) {}\n"
"}\n"
"function P(name) {\n"
"  var C = G[name];\n"
"  return (typeof C === 'function' && C.prototype) ? C.prototype : null;\n"
"}\n"
"var EP = P('HTMLElement'), ELP = P('Element');\n"
"if (!EP) return;\n"

/* ======================================================================
 * 1. Live collections
 * ======================================================================
 * The spec's HTMLCollection is live. Our js_dom.c publishes an array-like for
 * `children`, so the shape to imitate is that one: indices, `length`, `item`,
 * `namedItem`, and Array iteration. `filterFn` is re-run on every property
 * access, which is what "live" means.
 *
 * THE NEGATIVE CONTROL LIVES HERE. With STATIC_COLLECTIONS set, `snapshot()`
 * is computed once and frozen -- so a page that never mutates sees no
 * difference at all, and every test that inserts a row or an option gets a
 * stale answer that looks entirely plausible. */
/* The identity of a collection is stable: `t.rows === t.rows` is required by
 * the spec and asserted by the corpus, so the proxy is built once per
 * (element, name) and kept. */
"var COLL_KEY = Symbol('logitCollections');\n"
"function cachedCollection(el, name, filterFn) {\n"
"  var rec = el[COLL_KEY];\n"
"  if (!rec) {\n"
"    rec = {};\n"
"    try { Object.defineProperty(el, COLL_KEY, { value: rec, configurable: true }); }\n"
"    catch (e) { return collection(function () { return el; }, filterFn); }\n"
"  }\n"
"  if (!rec[name]) rec[name] = collection(function () { return el; }, filterFn);\n"
"  return rec[name];\n"
"}\n"
"function collection(rootFn, filterFn) {\n"
"  var frozen = null;\n"
"  function snapshot() {\n"
"    if (STATIC_COLLECTIONS && frozen) return frozen;\n"
"    var out = [], root = rootFn();\n"
"    if (root) filterFn(root, out);\n"
"    if (STATIC_COLLECTIONS) frozen = out;\n"
"    return out;\n"
"  }\n"
"  var HC = G.HTMLCollection;\n"
"  var target = (typeof HC === 'function' && HC.prototype)\n"
"    ? Object.create(HC.prototype) : {};\n"
"  return new Proxy(target, {\n"
"    get: function (t, k, r) {\n"
"      if (k === 'length') return snapshot().length;\n"
"      if (k === 'item') return function (i) {\n"
"        i = i >>> 0; var a = snapshot(); return i < a.length ? a[i] : null; };\n"
"      if (k === 'namedItem') return function (n) {\n"
"        var a = snapshot(); n = String(n);\n"
"        for (var i = 0; i < a.length; i++)\n"
"          if (a[i].getAttribute && a[i].getAttribute('id') === n) return a[i];\n"
"        for (var j = 0; j < a.length; j++)\n"
"          if (a[j].getAttribute && a[j].getAttribute('name') === n) return a[j];\n"
"        return null; };\n"
"      if (k === Symbol.iterator) return function () {\n"
"        return snapshot()[Symbol.iterator](); };\n"
"      if (typeof k === 'string' && /^[0-9]+$/.test(k)) {\n"
"        var a = snapshot(), i = +k; return i < a.length ? a[i] : undefined;\n"
"      }\n"
       /* A name that is not an index: HTMLCollection's named getter. Falls
        * back to the prototype so `Array.prototype.slice.call(rows)` and
        * `rows.constructor` still behave. */
"      if (typeof k === 'string') {\n"
"        var b = snapshot();\n"
"        for (var m = 0; m < b.length; m++)\n"
"          if (b[m].getAttribute && (b[m].getAttribute('id') === k ||\n"
"                                    b[m].getAttribute('name') === k)) return b[m];\n"
"      }\n"
"      return Reflect.get(t, k, r);\n"
"    },\n"
"    has: function (t, k) {\n"
"      if (k === 'length' || k === 'item' || k === 'namedItem') return true;\n"
"      if (typeof k === 'string' && /^[0-9]+$/.test(k)) return +k < snapshot().length;\n"
"      return Reflect.has(t, k);\n"
"    },\n"
"    ownKeys: function (t) {\n"
"      var a = snapshot(), keys = [];\n"
"      for (var i = 0; i < a.length; i++) keys.push(String(i));\n"
"      keys.push('length');\n"
"      return keys;\n"
"    },\n"
"    getOwnPropertyDescriptor: function (t, k) {\n"
"      if (k === 'length') return { value: snapshot().length, writable: false,\n"
"        enumerable: false, configurable: true };\n"
"      if (typeof k === 'string' && /^[0-9]+$/.test(k)) {\n"
"        var a = snapshot(), i = +k;\n"
"        if (i < a.length) return { value: a[i], writable: false,\n"
"          enumerable: true, configurable: true };\n"
"        return undefined;\n"
"      }\n"
"      return Reflect.getOwnPropertyDescriptor(t, k);\n"
"    }\n"
"  });\n"
"}\n"

/* Element children of `n` whose tag is in `tags` (a lookup object). */
"function childElems(n, tags, out) {\n"
"  for (var c = n.firstChild; c; c = c.nextSibling) {\n"
"    if (c.nodeType !== 1) continue;\n"
"    if (!tags || tags[lc(c.tagName)]) out.push(c);\n"
"  }\n"
"}\n"
"function descendants(n, want, out) {\n"
"  for (var c = n.firstChild; c; c = c.nextSibling) {\n"
"    if (c.nodeType !== 1) continue;\n"
"    if (want[lc(c.tagName)]) out.push(c);\n"
"    descendants(c, want, out);\n"
"  }\n"
"}\n"

/* ======================================================================
 * 2. Attribute-associated elements (popovertarget, commandfor)
 * ======================================================================
 * `button.popoverTargetElement = el` does NOT write an id: it records the
 * element itself and sets the content attribute to the empty string. The
 * getter then prefers that recorded element -- but only while it is in the
 * same tree, which is what
 *
 *     assert_equals(invoker.popoverTargetElement, null,
 *                   'targetElement should be null before the popover is in the document')
 *
 * is checking, and it is the assertion an id-only implementation gets wrong
 * without noticing. Setting the content attribute by any other route clears
 * the recorded element, so setAttribute/removeAttribute/toggleAttribute are
 * wrapped below for exactly the two attribute names involved. */
/* ==================================================================== state ==
 * PER-ELEMENT STATE IS KEYED ON THE WRAPPER AND HELD *STRONGLY*, AND THAT IS
 * NOT AN OVERSIGHT.
 *
 * js_dom.c caches one wrapper per node in the node's `jsw` slot and says so in
 * its own comment: "The slot takes no reference: the finalizer clears it." So
 * a wrapper that nothing in JS is holding is collected, and the next `wrap()`
 * of the same node builds a NEW object. A WeakMap keyed on the wrapper
 * therefore loses its entry at an arbitrary GC -- which is exactly the bug
 * this file was written with and it is invisible in a single expression:
 *
 *     option.selected = true; select.selectedIndex     // 1, correct
 *     ---- next script turn, a GC in between ----
 *     select.selectedIndex                             // 0, silently wrong
 *
 * Holding the wrapper keeps `jsw` populated, so every later wrap() of that
 * node returns the SAME object and the lookup holds. It is safe to hold:
 * js_dom.c's handle carries a per-node serial and node_of() rejects a stale
 * one, so a wrapper that outlives its node degrades to "no node", never to a
 * dangling pointer.
 *
 * The cost is a bounded leak -- one wrapper per element that ACQUIRES state
 * (a popover that was shown, a dialog that was opened, an option that was
 * selected), not one per element in the document. `collCache` is the one that
 * would not be bounded that way, so it is the exception: it hangs off a
 * SYMBOL-keyed own property of the wrapper instead, which is invisible to
 * getOwnPropertyNames/JSON and dies with the wrapper. A collection is cheap to
 * rebuild, so losing its identity to a GC costs nothing but a fresh object --
 * the same guarantee a page's own `el.foo = {}` expando has here.
 */
"var explicitEl = new Map();\n"
"function setExplicit(el, attr, target) {\n"
"  var rec = explicitEl.get(el);\n"
"  if (!rec) { rec = {}; explicitEl.set(el, rec); }\n"
"  rec[attr] = target;\n"
"}\n"
"function clearExplicit(el, attr) {\n"
"  var rec = explicitEl.get(el);\n"
"  if (rec) delete rec[attr];\n"
"}\n"
"function rootOf(n) {\n"
"  if (n && typeof n.getRootNode === 'function') { try { return n.getRootNode(); } catch (e) {} }\n"
"  return doc;\n"
"}\n"
"function attrElement(el, attr) {\n"
"  var rec = explicitEl.get(el), ex = rec ? rec[attr] : undefined;\n"
"  if (ex !== undefined) {\n"
"    if (!ex || ex.nodeType !== 1) return null;\n"
       /* Same tree, which for a connected invoker means "in the document". */
"    return rootOf(el) === rootOf(ex) ? ex : null;\n"
"  }\n"
"  var id = el.getAttribute(attr);\n"
"  if (id === null || id === '') return null;\n"
"  var root = rootOf(el);\n"
"  if (root && typeof root.getElementById === 'function') return root.getElementById(id) || null;\n"
"  var out = [];\n"
"  (function walk(n) {\n"
"    for (var c = n.firstChild; c; c = c.nextSibling) {\n"
"      if (c.nodeType !== 1) continue;\n"
"      if (!out.length && c.getAttribute('id') === id) out.push(c);\n"
"      if (!out.length) walk(c);\n"
"    }\n"
"  })(root || doc);\n"
"  return out.length ? out[0] : null;\n"
"}\n"
/* ORDER MATTERS AND IT IS NOT OBVIOUS: setAttribute is wrapped below to clear
 * the recorded element (any other route to the content attribute must), so
 * recording first and writing second would erase what we just recorded. */
"function setAttrElement(el, attr, v) {\n"
"  if (v === null || v === undefined) { el.removeAttribute(attr); clearExplicit(el, attr); return; }\n"
"  el.setAttribute(attr, '');\n"
"  setExplicit(el, attr, v);\n"
"}\n"
/* The clearing wrappers. Two attribute names, checked case-insensitively, and
 * everything else goes straight through -- this is on the hot path of every
 * setAttribute a page makes, so it does no work it does not have to. */
"var TRACKED_ATTR = { popovertarget: 1, commandfor: 1, interestfor: 1 };\n"
"function wrapAttrWriter(name) {\n"
"  if (!ELP) return;\n"
"  var orig = ELP[name];\n"
"  if (typeof orig !== 'function' || orig.__logit_sem) return;\n"
/* THIS WRAPPER IS ON THE HOT PATH -- every setAttribute a page makes goes
 * through it -- so it does no work it does not have to. Attribute names are
 * already lowercase in essentially all real code, so the fast path is two
 * hash lookups on the string as given; lc() (a regex replace) runs only when
 * the cheap check misses AND the name actually contains an uppercase letter. */
"  var wrapped = function (a) {\n"
"    var an = arguments.length ? a : '';\n"
"    if (typeof an !== 'string') an = String(an);\n"
"    var hit = TRACKED_ATTR[an] === 1 || an === 'open';\n"
"    if (!hit) {\n"
"      var l = lc(an);\n"
"      if (l !== an) { an = l; hit = TRACKED_ATTR[an] === 1 || an === 'open'; }\n"
"    }\n"
"    if (!hit) return orig.apply(this, arguments);\n"
"    if (TRACKED_ATTR[an] === 1) clearExplicit(this, an);\n"
"    var r = orig.apply(this, arguments);\n"
       /* The exclusive-accordion rule. `<details name=x>` is the one attribute
        * change in HTML that reaches OTHER elements, so it has to be observed
        * where the attribute is written rather than where `open` is read --
        * and `details.open = true` is reflection, which lands right here. */
"    if (an === 'open' && tagOf(this) === 'details') enforceDetailsName(this);\n"
"    return r;\n"
"  };\n"
"  wrapped.__logit_sem = 1;\n"
"  meth(ELP, name, wrapped);\n"
"}\n"
"function detailsName(d) {\n"
"  var n = d.getAttribute ? d.getAttribute('name') : null;\n"
"  return (n === null || n === '') ? null : n;\n"
"}\n"
"function enforceDetailsName(d) {\n"
"  if (!d.hasAttribute('open')) return;\n"
"  var nm = detailsName(d);\n"
"  if (nm === null) return;\n"
"  var root = rootOf(d), all = [];\n"
"  descendants(root && root.nodeType ? root : doc, { details: 1 }, all);\n"
"  for (var i = 0; i < all.length; i++) {\n"
"    var o = all[i];\n"
"    if (o === d || detailsName(o) !== nm || !o.hasAttribute('open')) continue;\n"
"    o.removeAttribute('open');\n"
"    queueToggle(o, 'open', 'closed');\n"
"  }\n"
"}\n"
"wrapAttrWriter('setAttribute'); wrapAttrWriter('removeAttribute');\n"
"wrapAttrWriter('toggleAttribute');\n"

/* ======================================================================
 * 3. Enumerated-attribute reflection this build did not have
 * ======================================================================
 * js_reflect.c generates its table from the corpus's own elements-*.js, and
 * that data does not carry `popover`, `popovertargetaction` or `command` --
 * they are typed `element` / a bespoke enum upstream, which the generator
 * records as RT_SKIP. So they are written by hand, here, next to the behaviour
 * that reads them. */
"function enumAttr(p, prop, attr, keywords, missing, invalid) {\n"
"  acc(p, prop, function () {\n"
"    var v = this.getAttribute(attr);\n"
"    if (v === null) return missing;\n"
"    var k = keywords[lc(v)];\n"
"    return k === undefined ? invalid : k;\n"
"  }, function (v) {\n"
       /* A nullable IDL attribute (`DOMString?`) removes on null AND on
        * undefined -- WebIDL maps both to null for a nullable type. A
        * non-nullable one stringifies, so `x.popoverTargetAction = null`
        * really does write the four characters "null". */
"    if (missing === null && (v === null || v === undefined)) { this.removeAttribute(attr); return; }\n"
"    this.setAttribute(attr, String(v));\n"
"  });\n"
"}\n"

/* --- the popover attribute, on every HTML element ----------------------- */
"var POPOVER_KW = { '': 'auto', 'auto': 'auto', 'manual': 'manual', 'hint': 'hint' };\n"
"enumAttr(EP, 'popover', 'popover', POPOVER_KW, null, 'manual');\n"

/* --- the invoker attributes, on <button> and <input> -------------------- */
"var PTA_KW = { 'toggle': 'toggle', 'show': 'show', 'hide': 'hide' };\n"
"var INVOKERS = [P('HTMLButtonElement'), P('HTMLInputElement')];\n"
"for (var iv = 0; iv < INVOKERS.length; iv++) {\n"
"  var ip = INVOKERS[iv];\n"
"  if (!ip) continue;\n"
"  enumAttr(ip, 'popoverTargetAction', 'popovertargetaction', PTA_KW, 'toggle', 'toggle');\n"
"  acc(ip, 'popoverTargetElement',\n"
"      function () { return attrElement(this, 'popovertarget'); },\n"
"      function (v) { setAttrElement(this, 'popovertarget', v); });\n"
"}\n"

/* --- command / commandfor, on <button> ---------------------------------- */
/* `command` is a DOMString whose GETTER filters: a custom command (leading
 * "--") comes back verbatim, a built-in comes back canonically lowercased, and
 * anything else comes back as the empty string. The setter is a plain
 * reflection -- which is why `button.command = 'nonsense'` leaves
 * `getAttribute('command')` as "nonsense" while `button.command` reads "". */
"var BUILTIN_COMMANDS = {};\n"
"('toggle-popover show-popover hide-popover close request-close show-modal show-picker '\n"
" + 'step-up step-down toggle-openable open-openable close-openable play-pause '\n"
" + 'pause play toggle-muted')\n"
"  .split(' ').forEach(function (c) { BUILTIN_COMMANDS[c] = 1; });\n"
"var BTN = P('HTMLButtonElement');\n"
"if (BTN) {\n"
"  acc(BTN, 'command', function () {\n"
"    var v = this.getAttribute('command');\n"
"    if (v === null) return '';\n"
"    if (v.slice(0, 2) === '--') return v;\n"
"    var l = lc(v);\n"
"    return BUILTIN_COMMANDS[l] ? l : '';\n"
"  }, function (v) { this.setAttribute('command', String(v)); });\n"
"  acc(BTN, 'commandForElement',\n"
"      function () { return attrElement(this, 'commandfor'); },\n"
"      function (v) { setAttrElement(this, 'commandfor', v); });\n"
/* `button.type` is a reflected enumerated attribute and js_reflect.c already
 * has it -- with the OLD invalid/missing value default, "submit". The rule
 * changed when command/commandfor arrived: a button that names a command has
 * no business submitting a form by accident, so a missing or invalid `type` on
 * such a button defaults to "button" instead. That one default decides the
 * whole activation behaviour below, which is why it is here next to it and not
 * in the generated table -- the generator's input (the corpus's
 * elements-*.js) records `type` as a plain enum and cannot express it. */
"  var BTYPE = { 'submit': 'submit', 'reset': 'reset', 'button': 'button' };\n"
"  acc(BTN, 'type', function () {\n"
"    var v = this.getAttribute('type');\n"
"    var k = (v === null) ? undefined : BTYPE[lc(v)];\n"
"    if (k) return k;\n"
"    return (this.hasAttribute('command') || this.hasAttribute('commandfor'))\n"
"           ? 'button' : 'submit';\n"
"  }, function (v) { this.setAttribute('type', String(v)); });\n"
"}\n"

/* ======================================================================
 * 4. Event subclasses: ToggleEvent, CommandEvent
 * ======================================================================
 * Built over the native Event rather than beside it, so `e instanceof Event`
 * holds, the native dispatcher accepts the object (it looks the event up by
 * class id, not by prototype) and `preventDefault` is the real one. The
 * constructor RETURNS the object, which is what makes `new ToggleEvent(...)`
 * yield it despite the body not being a class. */
"function eventSubclass(name, fields) {\n"
"  var Base = G.Event;\n"
"  if (typeof Base !== 'function' || G[name]) return G[name] || null;\n"
"  var Ctor = function (type, init) {\n"
"    if (arguments.length < 1)\n"
"      throw new TypeError(\"Failed to construct '\" + name + \"': 1 argument required, but only 0 present.\");\n"
"    init = (init === null || init === undefined) ? {} : init;\n"
"    if (typeof init !== 'object')\n"
"      throw new TypeError(\"Failed to construct '\" + name + \"': the provided value is not of type '\" + name + \"Init'.\");\n"
"    var e = new Base(String(type), init);\n"
"    try { Object.setPrototypeOf(e, Ctor.prototype); } catch (q) {}\n"
"    for (var f in fields) {\n"
"      var v = init[f];\n"
"      Object.defineProperty(e, f, { configurable: true, enumerable: true,\n"
"        value: fields[f](v) });\n"
"    }\n"
"    return e;\n"
"  };\n"
"  Ctor.prototype = Object.create(Base.prototype);\n"
"  Object.defineProperty(Ctor.prototype, 'constructor',\n"
"    { configurable: true, writable: true, value: Ctor });\n"
"  Object.defineProperty(Ctor, 'name', { configurable: true, value: name });\n"
"  try { Object.defineProperty(G, name, { configurable: true, writable: true, value: Ctor }); }\n"
"  catch (q) { G[name] = Ctor; }\n"
"  return Ctor;\n"
"}\n"
"function asStr(v) { return v === undefined ? '' : String(v); }\n"
"function asEl(v) { return (v === undefined || v === null) ? null : v; }\n"
"var ToggleEventCtor = eventSubclass('ToggleEvent', { oldState: asStr, newState: asStr });\n"
"var CommandEventCtor = eventSubclass('CommandEvent', { command: asStr, source: asEl });\n"

/* ---- on<type> event-handler properties ---------------------------------
 * js_dom.c installs the on* accessors from a fixed table, and the five event
 * types this file introduces (`command`, `toggle`, `beforetoggle`, `close`,
 * `cancel`) are not in it. Rather than grow another line's table, they are
 * installed here, only where absent -- so the day that table grows, this stops
 * doing anything.
 *
 * The CONTENT attribute is compiled lazily, on read, because there is no other
 * moment to do it: an element with `oncommand="..."` may be created at any
 * time. `primeOn` is the read that makes it happen before the event is fired,
 * and it is called at each of the three dispatch sites -- without it the
 * attribute form works only for a page that happened to read the property
 * first. */
"function onRecord(el, KEY) { return el[KEY]; }\n"
"function setOn(el, KEY, type, fn) {\n"
"  var rec = el[KEY];\n"
"  if (rec) { try { el.removeEventListener(type, rec.wrap); } catch (e) {} }\n"
"  if (typeof fn !== 'function') {\n"
"    try { Object.defineProperty(el, KEY, { value: null, configurable: true, writable: true }); }\n"
"    catch (e) {}\n"
"    return;\n"
"  }\n"
"  var wrap = function (ev) {\n"
"    var r = fn.call(el, ev);\n"
       /* An on-handler returning exactly false cancels the event. */
"    if (r === false && ev && typeof ev.preventDefault === 'function') ev.preventDefault();\n"
"  };\n"
"  try { Object.defineProperty(el, KEY, { value: { fn: fn, wrap: wrap },\n"
"    configurable: true, writable: true }); } catch (e) { return; }\n"
"  try { el.addEventListener(type, wrap); } catch (e) {}\n"
"}\n"
"var ON_KEYS = {};\n"
"function installOnHandler(proto, type) {\n"
"  var name = 'on' + type;\n"
"  if (!proto || (name in proto)) return;\n"
"  var KEY = Symbol(name);\n"
"  ON_KEYS[type] = KEY;\n"
"  acc(proto, name, function () {\n"
"    var rec = onRecord(this, KEY);\n"
"    if (rec !== undefined) return rec ? rec.fn : null;\n"
"    var src = this.getAttribute ? this.getAttribute(name) : null;\n"
"    if (src === null || src === undefined) return null;\n"
"    var fn = null;\n"
"    try { fn = new Function('event', src); } catch (e) { return null; }\n"
"    setOn(this, KEY, type, fn);\n"
"    return fn;\n"
"  }, function (v) { setOn(this, KEY, type, (typeof v === 'function') ? v : null); });\n"
"}\n"
"['command', 'toggle', 'beforetoggle', 'close', 'cancel']\n"
"  .forEach(function (t) { installOnHandler(EP, t); });\n"
"function primeOn(el, type) {\n"
"  var KEY = ON_KEYS[type];\n"
"  if (!KEY || !el) return;\n"
"  try { void el['on' + type]; } catch (e) {}\n"
"}\n"

"function fireEvent(target, Ctor, type, init) {\n"
"  primeOn(target, type);\n"
"  var e;\n"
"  if (Ctor) { e = new Ctor(type, init); }\n"
"  else if (typeof G.Event === 'function') { e = new G.Event(type, init); }\n"
"  else return true;\n"
"  return target.dispatchEvent(e);\n"
"}\n"
/* The spec queues `toggle` as a TASK, not a microtask: a page that shows and
 * hides in the same turn must see one toggle, and the coalescing is what the
 * task boundary buys. setTimeout is that boundary here; queueMicrotask is the
 * fallback for a link without timers. */
"var pendingToggle = new WeakMap();\n"
"function queueToggle(el, oldState, newState) {\n"
"  var rec = pendingToggle.get(el);\n"
"  if (rec) { rec.newState = newState; return; }\n"
"  rec = { oldState: oldState, newState: newState };\n"
"  pendingToggle.set(el, rec);\n"
"  var run = function () {\n"
"    pendingToggle.delete(el);\n"
"    if (rec.oldState === rec.newState) return;\n"
"    fireEvent(el, ToggleEventCtor, 'toggle',\n"
"      { bubbles: false, cancelable: false, oldState: rec.oldState, newState: rec.newState });\n"
"  };\n"
"  if (typeof G.setTimeout === 'function') G.setTimeout(run, 0);\n"
"  else if (typeof G.queueMicrotask === 'function') G.queueMicrotask(run);\n"
"  else run();\n"
"}\n"

/* ======================================================================
 * 5. The popover API
 * ====================================================================== */
"var showingPopovers = new Map();\n"   /* el -> true while in the top layer */
"var topLayer = [];\n"

"function popoverType(el) {\n"
"  var v = el.getAttribute && el.getAttribute('popover');\n"
"  if (v === null || v === undefined) return null;\n"
"  var k = POPOVER_KW[lc(v)];\n"
"  return k === undefined ? 'manual' : k;\n"
"}\n"
"function popoverIsShowing(el) { return !!showingPopovers.get(el); }\n"

/* The spec's "check popover validity". `throwing` false makes it a predicate,
 * which is what the re-check after the (cancelable) beforetoggle needs: a
 * handler may have removed the element from the document, and that is not an
 * error, it is a reason to stop. */
"function checkPopoverValidity(el, expectShowing, throwing) {\n"
"  if (popoverType(el) === null) {\n"
"    if (throwing) throwDom('NotSupportedError', 'Not supported on element that does not have a valid value for the popover attribute.');\n"
"    return false;\n"
"  }\n"
"  if (popoverIsShowing(el) !== expectShowing) {\n"
"    if (throwing) throwDom('InvalidStateError', 'Invalid on popover' + (expectShowing ? ' not' : '') + ' being shown.');\n"
"    return false;\n"
"  }\n"
"  if (!expectShowing && !el.isConnected) {\n"
"    if (throwing) throwDom('InvalidStateError', 'Invalid on disconnected popover elements.');\n"
"    return false;\n"
"  }\n"
"  if (tagOf(el) === 'dialog' && el.hasAttribute('open')) {\n"
"    if (throwing) throwDom('InvalidStateError', 'Invalid on open dialog elements.');\n"
"    return false;\n"
"  }\n"
"  return true;\n"
"}\n"

/* Showing an `auto` popover closes every other auto popover that is not one of
 * its ancestors. `hint` closes other hints. `manual` closes nothing -- which is
 * the whole difference between the two states the corpus parameterises over. */
"function ancestorPopovers(el) {\n"
"  var set = [];\n"
"  for (var n = el; n; n = n.parentNode) {\n"
"    if (n.nodeType === 1 && popoverType(n)) set.push(n);\n"
"  }\n"
"  return set;\n"
"}\n"
"function hideAllPopoversUntil(target, kinds) {\n"
"  var keep = target ? ancestorPopovers(target) : [];\n"
"  for (var i = topLayer.length - 1; i >= 0; i--) {\n"
"    var p = topLayer[i];\n"
"    if (!p) continue;\n"
"    if (kinds.indexOf(popoverType(p)) < 0) continue;\n"
"    if (p === target || keep.indexOf(p) >= 0) continue;\n"
"    hidePopoverInternal(p, true);\n"
"  }\n"
"}\n"
"function hidePopoverInternal(el, fireEvents) {\n"
"  if (!popoverIsShowing(el)) return;\n"
"  if (fireEvents)\n"
"    fireEvent(el, ToggleEventCtor, 'beforetoggle',\n"
"      { bubbles: false, cancelable: false, oldState: 'open', newState: 'closed' });\n"
"  if (!popoverIsShowing(el)) return;\n"
"  showingPopovers.delete(el);\n"
"  var ix = topLayer.indexOf(el);\n"
"  if (ix >= 0) topLayer.splice(ix, 1);\n"
"  if (fireEvents) queueToggle(el, 'open', 'closed');\n"
"}\n"

"meth(EP, 'showPopover', function (options) {\n"
"  var el = this;\n"
"  checkPopoverValidity(el, false, true);\n"
"  var kind = popoverType(el);\n"
"  if (!fireEvent(el, ToggleEventCtor, 'beforetoggle',\n"
"        { bubbles: false, cancelable: true, oldState: 'closed', newState: 'open' })) return;\n"
"  if (!checkPopoverValidity(el, false, false)) return;\n"
"  if (kind === 'auto') hideAllPopoversUntil(el, ['auto', 'hint']);\n"
"  else if (kind === 'hint') hideAllPopoversUntil(el, ['hint']);\n"
"  showingPopovers.set(el, true);\n"
"  topLayer.push(el);\n"
"  queueToggle(el, 'closed', 'open');\n"
"});\n"
"meth(EP, 'hidePopover', function () {\n"
"  checkPopoverValidity(this, true, true);\n"
"  hidePopoverInternal(this, true);\n"
"});\n"
"meth(EP, 'togglePopover', function (force) {\n"
"  var want;\n"
"  if (force === undefined || force === null) want = !popoverIsShowing(this);\n"
"  else if (typeof force === 'object' && force !== null && 'force' in force) want = !!force.force;\n"
"  else want = !!force;\n"
"  if (want) { if (!popoverIsShowing(this)) this.showPopover(); }\n"
"  else { if (popoverIsShowing(this)) this.hidePopover();\n"
"         else checkPopoverValidity(this, false, true); }\n"
"  return popoverIsShowing(this);\n"
"});\n"

/* ======================================================================
 * 6. <dialog>
 * ====================================================================== */
"var modalDialogs = new Map();\n"
"var dialogReturn = new Map();\n"
"var DLG = P('HTMLDialogElement');\n"
"if (DLG) {\n"
"  acc(DLG, 'returnValue', function () {\n"
"    var v = dialogReturn.get(this); return v === undefined ? '' : v;\n"
"  }, function (v) { dialogReturn.set(this, String(v)); });\n"
"  var CLOSEDBY_KW = { 'any': 'any', 'closerequest': 'closerequest', 'none': 'none' };\n"
"  acc(DLG, 'closedBy', function () {\n"
"    var v = this.getAttribute('closedby');\n"
"    if (v === null) return modalDialogs.get(this) ? 'closerequest' : 'none';\n"
"    var k = CLOSEDBY_KW[lc(v)];\n"
"    return k === undefined ? (modalDialogs.get(this) ? 'closerequest' : 'none') : k;\n"
"  }, function (v) { this.setAttribute('closedby', String(v)); });\n"
"  meth(DLG, 'show', function () {\n"
"    if (this.hasAttribute('open')) {\n"
"      if (modalDialogs.get(this)) throwDom('InvalidStateError', 'The dialog is already open as a modal dialog.');\n"
"      return;\n"
"    }\n"
"    this.setAttribute('open', '');\n"
"    modalDialogs.delete(this);\n"
"    dialogFocus(this);\n"
"  });\n"
"  meth(DLG, 'showModal', function () {\n"
"    if (this.hasAttribute('open')) {\n"
"      if (!modalDialogs.get(this)) throwDom('InvalidStateError', 'The dialog is already open as a non-modal dialog.');\n"
"      return;\n"
"    }\n"
"    if (!this.isConnected) throwDom('InvalidStateError', 'The element is not in a Document.');\n"
"    if (popoverIsShowing(this)) throwDom('InvalidStateError', 'The dialog is already open as a popover.');\n"
"    this.setAttribute('open', '');\n"
"    modalDialogs.set(this, true);\n"
"    dialogFocus(this);\n"
"  });\n"
"  meth(DLG, 'close', function (rv) {\n"
"    dialogCloseInternal(this, arguments.length ? String(rv) : undefined);\n"
"  });\n"
"  meth(DLG, 'requestClose', function (rv) {\n"
"    if (!this.hasAttribute('open')) return;\n"
"    if (!fireEvent(this, null, 'cancel', { bubbles: false, cancelable: true })) return;\n"
"    dialogCloseInternal(this, arguments.length ? String(rv) : undefined);\n"
"  });\n"
"}\n"
"function dialogCloseInternal(el, rv) {\n"
"  if (!el.hasAttribute('open')) return;\n"
"  el.removeAttribute('open');\n"
"  modalDialogs.delete(el);\n"
"  if (rv !== undefined) dialogReturn.set(el, rv);\n"
"  fireEvent(el, null, 'close', { bubbles: false, cancelable: false });\n"
"}\n"
/* The dialog focusing steps, reduced to what is observable here: the first
 * focusable descendant with `autofocus`, else the first focusable descendant,
 * else the dialog itself. */
"function dialogFocus(el) {\n"
"  var cand = null;\n"
"  try { cand = el.querySelector('[autofocus]'); } catch (e) {}\n"
"  if (!cand) { try { cand = el.querySelector('input,select,textarea,button,a[href],[tabindex]'); } catch (e) {} }\n"
"  var t = cand || el;\n"
"  if (typeof t.focus === 'function') { try { t.focus(); } catch (e) {} }\n"
"}\n"

/* ======================================================================
 * 7. <details> / <summary>
 * ====================================================================== */
"var DET = P('HTMLDetailsElement');\n"
/* `details.open = true` has to go through here, not through js_reflect.c's
 * generated boolean pair. Not because that pair is wrong -- it sets and clears
 * the attribute correctly -- but because it does it in C, below the
 * Element.prototype.setAttribute this file wraps, so the accordion rule would
 * see the content-attribute path and miss the IDL one. Two entry points to the
 * same state need the hook on both or the feature works only when written one
 * particular way, which is worse than not working. */
"if (DET) {\n"
"  acc(DET, 'open', function () { return this.hasAttribute('open'); },\n"
"    function (v) {\n"
"      var was = this.hasAttribute('open');\n"
"      if (v) { this.setAttribute('open', ''); enforceDetailsName(this); }\n"
"      else this.removeAttribute('open');\n"
"      if (!!v !== was) queueToggle(this, was ? 'open' : 'closed', v ? 'open' : 'closed');\n"
"    });\n"
"}\n"
"function detailsSummary(d) {\n"
"  for (var c = d.firstChild; c; c = c.nextSibling)\n"
"    if (c.nodeType === 1 && tagOf(c) === 'summary') return c;\n"
"  return null;\n"
"}\n"
"function detailsToggle(d) {\n"
"  var open = d.hasAttribute('open');\n"
"  if (!fireEvent(d, ToggleEventCtor, 'beforetoggle',\n"
"        { bubbles: false, cancelable: true,\n"
"          oldState: open ? 'open' : 'closed', newState: open ? 'closed' : 'open' })) return;\n"
"  if (open) d.removeAttribute('open'); else d.setAttribute('open', '');\n"
"  queueToggle(d, open ? 'open' : 'closed', open ? 'closed' : 'open');\n"
"}\n"

/* ======================================================================
 * 8. click() and activation behaviour
 * ======================================================================
 * HTMLElement.prototype.click did not exist in this build at all, and the
 * corpus reaches for it constantly -- it is how a test activates a control
 * when there is no test driver. Its absence is why whole files in
 * html/semantics died before their first assertion rather than failing one.
 *
 * The order is the DOM's: dispatch the event, and run the activation behaviour
 * afterwards UNLESS the event was canceled. Only the activation behaviours this
 * file owns are implemented -- the popover invoker, the command invoker and
 * <summary> -- because a half-right form submission here would be a regression
 * in a suite that already passes rather than a gain in one that does not. */
"var INPUT_BUTTONISH = { button: 1, reset: 1, submit: 1, image: 1 };\n"
"function isDisabledCtl(el) {\n"
"  var t = tagOf(el);\n"
"  if (t !== 'button' && t !== 'input' && t !== 'select' && t !== 'textarea' &&\n"
"      t !== 'fieldset' && t !== 'optgroup' && t !== 'option') return false;\n"
"  if (el.hasAttribute('disabled')) return true;\n"
"  for (var p = el.parentNode; p && p.nodeType === 1; p = p.parentNode)\n"
"    if (tagOf(p) === 'fieldset' && p.hasAttribute('disabled')) return true;\n"
"  return false;\n"
"}\n"
"function canInvoke(el) {\n"
"  var t = tagOf(el);\n"
"  if (t === 'button') return !isDisabledCtl(el);\n"
"  if (t === 'input') return !!INPUT_BUTTONISH[lc(el.getAttribute('type') || '')] && !isDisabledCtl(el);\n"
"  return false;\n"
"}\n"
"function runPopoverAction(target, action) {\n"
"  var showing = popoverIsShowing(target);\n"
"  if (action === 'show') { if (!showing) { try { target.showPopover(); } catch (e) {} } }\n"
"  else if (action === 'hide') { if (showing) { try { target.hidePopover(); } catch (e) {} } }\n"
"  else { try { target.togglePopover(); } catch (e) {} }\n"
"}\n"
"function runCommand(invoker, target, command) {\n"
"  primeOn(target, 'command');\n"
"  var ev = CommandEventCtor\n"
       /* composed: true. The event crosses shadow boundaries by definition --
        * an invoker in a shadow tree commands a light-tree element -- and 24
        * subtests in the-button-element assert exactly that flag. */
"    ? new CommandEventCtor('command', { bubbles: false, cancelable: true,\n"
"                                        composed: true,\n"
"                                        command: command, source: invoker })\n"
"    : null;\n"
"  if (ev && !target.dispatchEvent(ev)) return;\n"
"  var t = tagOf(target), c = lc(command);\n"
"  if (c.slice(0, 2) === '--') return;\n"       /* custom: the event IS the API */
"  if (t === 'dialog') {\n"
"    if (c === 'show-modal') { if (!target.hasAttribute('open')) target.showModal(); }\n"
"    else if (c === 'close') { dialogCloseInternal(target, undefined); }\n"
"    else if (c === 'request-close') { if (typeof target.requestClose === 'function') target.requestClose(); }\n"
"    return;\n"
"  }\n"
"  if (popoverType(target) !== null) {\n"
"    if (c === 'show-popover') runPopoverAction(target, 'show');\n"
"    else if (c === 'hide-popover') runPopoverAction(target, 'hide');\n"
"    else if (c === 'toggle-popover') runPopoverAction(target, 'toggle');\n"
"  }\n"
"}\n"
/* A <form>'s reset: every control back to its default. js_forms.c owns control
 * values, so this only puts the CONTENT attributes' defaults back where it can
 * and leaves the rest to it. */
"function resetForm(frm) {\n"
"  var all = []; descendants(frm, LISTED, all);\n"
"  for (var i = 0; i < all.length; i++) {\n"
"    var el = all[i], t = tagOf(el);\n"
"    if (t === 'input') {\n"
"      var ty = lc(el.getAttribute('type') || '');\n"
"      if (ty === 'checkbox' || ty === 'radio') {\n"
"        try { el.checked = el.hasAttribute('checked'); } catch (e) {}\n"
"      } else {\n"
"        try { el.value = el.getAttribute('value') || ''; } catch (e) {}\n"
"      }\n"
"    } else if (t === 'select') {\n"
"      var o = []; selectOptions(el, o);\n"
"      for (var j = 0; j < o.length; j++) optSelected.set(o[j], o[j].hasAttribute('selected'));\n"
"    } else if (t === 'textarea') {\n"
"      try { el.value = el.textContent || ''; } catch (e) {}\n"
"    }\n"
"  }\n"
"}\n"
/* THE ORDER HERE IS THE SPEC'S AND IT IS NOT THE OBVIOUS ONE. A button's TYPE
 * decides everything: type=submit submits its form and does NOT run the
 * command, even when `commandfor` and `command` are both present and valid.
 * That reads as a contradiction until you also have the `type` default above --
 * a button that names a command and gives no valid type IS type=button, so the
 * two rules together say "naming a command opts you out of submitting", which
 * is what the corpus asserts from both directions. */
"function activationBehaviour(el) {\n"
"  var t = tagOf(el);\n"
"  if (t === 'button' && !isDisabledCtl(el)) {\n"
"    var bt = el.type, frm = formOwner(el);\n"
"    if (frm && bt === 'submit') {\n"
"      fireEvent(frm, null, 'submit', { bubbles: true, cancelable: true });\n"
"      return;\n"
"    }\n"
"    if (frm && bt === 'reset') {\n"
"      if (fireEvent(frm, null, 'reset', { bubbles: true, cancelable: true })) resetForm(frm);\n"
"      return;\n"
"    }\n"
"  }\n"
"  if (t === 'input' && !isDisabledCtl(el)) {\n"
"    var it = lc(el.getAttribute('type') || ''), ifrm = formOwner(el);\n"
"    if (ifrm && (it === 'submit' || it === 'image')) {\n"
"      fireEvent(ifrm, null, 'submit', { bubbles: true, cancelable: true });\n"
"      return;\n"
"    }\n"
"    if (ifrm && it === 'reset') {\n"
"      if (fireEvent(ifrm, null, 'reset', { bubbles: true, cancelable: true })) resetForm(ifrm);\n"
"      return;\n"
"    }\n"
"  }\n"
"  if (canInvoke(el)) {\n"
       /* commandfor wins over popovertarget when both are present. */
"    if (t === 'button') {\n"
"      var cf = attrElement(el, 'commandfor');\n"
"      var cmd = el.command;\n"
"      if (cf && cmd) { runCommand(el, cf, cmd); return; }\n"
"      if (cf || el.hasAttribute('command')) return;\n"
"    }\n"
"    var pt = attrElement(el, 'popovertarget');\n"
"    if (pt && popoverType(pt) !== null) { runPopoverAction(pt, el.popoverTargetAction); return; }\n"
"  }\n"
"  if (t === 'summary') {\n"
"    var d = el.parentNode;\n"
"    if (d && d.nodeType === 1 && tagOf(d) === 'details' && detailsSummary(d) === el) detailsToggle(d);\n"
"    return;\n"
"  }\n"
"}\n"
/* THE PRE-CLICK ACTIVATION STEPS, which are the half of click() that is easy
 * to forget because they happen BEFORE the event.
 *
 * A checkbox is checked by the time its own click handler runs -- that is what
 * `if (this.checked)` inside an onclick reads, and it is why the state is set
 * first and RESTORED if the event turns out to be canceled, rather than set
 * afterwards. The corpus catches the difference directly:
 * html/semantics/selectors/pseudo-classes/checked.html asserts that `:checked`
 * matches a checkbox that was clicked, and a click() that only dispatches an
 * event leaves it unmatched.
 *
 * A radio additionally clears the rest of its group, and neither ever
 * UNchecks: clicking a checked radio is a no-op on its state. */
"function preClickActivation(el) {\n"
"  if (tagOf(el) !== 'input') return null;\n"
"  var ty = lc(el.getAttribute('type') || '');\n"
"  if (ty === 'checkbox') {\n"
"    var was = !!el.checked, wasInd = !!el.indeterminate;\n"
"    try { el.indeterminate = false; } catch (e) {}\n"
"    try { el.checked = !was; } catch (e) {}\n"
"    return { kind: 'checkbox', el: el, checked: was, indeterminate: wasInd,\n"
"             moved: true };\n"
"  }\n"
"  if (ty === 'radio') {\n"
"    var prev = radioGroup(el), before = [], moved = !el.checked;\n"
"    for (var i = 0; i < prev.length; i++) before.push(!!prev[i].checked);\n"
"    for (var j = 0; j < prev.length; j++) {\n"
"      try { prev[j].checked = (prev[j] === el); } catch (e) {}\n"
"    }\n"
"    return { kind: 'radio', group: prev, before: before, moved: moved };\n"
"  }\n"
"  return null;\n"
"}\n"
/* The legacy-canceled-activation behaviour: a canceled click puts the state
 * back. It has to, because the state was set BEFORE the event -- that is the
 * whole point of doing it first -- so without this a preventDefault()ed click
 * on a checkbox leaves it toggled, which is the visible bug every page that
 * validates a checkbox in its own handler would hit. */
"function undoPreClick(st) {\n"
"  if (!st) return;\n"
"  if (st.kind === 'checkbox') {\n"
"    try { st.el.checked = st.checked; } catch (e) {}\n"
"    try { st.el.indeterminate = st.indeterminate; } catch (e) {}\n"
"    return;\n"
"  }\n"
"  if (st.kind === 'radio')\n"
"    for (var i = 0; i < st.group.length; i++) {\n"
"      try { st.group[i].checked = st.before[i]; } catch (e) {}\n"
"    }\n"
"}\n"
"function radioGroup(el) {\n"
"  var nm = el.getAttribute('name');\n"
"  if (!nm) return [el];\n"
"  var scope = formOwner(el) || rootOf(el), all = [], out = [];\n"
"  descendants(scope && scope.nodeType ? scope : doc, { input: 1 }, all);\n"
"  for (var i = 0; i < all.length; i++)\n"
"    if (lc(all[i].getAttribute('type') || '') === 'radio' &&\n"
"        all[i].getAttribute('name') === nm &&\n"
"        (formOwner(all[i]) === formOwner(el))) out.push(all[i]);\n"
"  return out.length ? out : [el];\n"
"}\n"
"var clicking = new WeakMap();\n"
"meth(EP, 'click', function () {\n"
"  var el = this;\n"
"  if (isDisabledCtl(el)) return;\n"
"  if (clicking.get(el)) return;\n"
"  clicking.set(el, true);\n"
"  var pre = null;\n"
"  try { pre = preClickActivation(el); } catch (e) {}\n"
"  try {\n"
"    var C = G.PointerEvent || G.MouseEvent || G.Event;\n"
"    var e;\n"
"    try { e = new C('click', { bubbles: true, cancelable: true, composed: true,\n"
"                               detail: 1, view: G }); }\n"
"    catch (q) { e = new G.Event('click', { bubbles: true, cancelable: true, composed: true }); }\n"
"    var notCanceled = el.dispatchEvent(e);\n"
"    if (notCanceled) {\n"
"      if (pre && pre.moved) {\n"
         /* input then change, in that order, and only for a state that really
          * moved -- clicking an already-checked radio changes nothing and must
          * fire nothing. */
"        fireEvent(el, null, 'input', { bubbles: true, cancelable: false });\n"
"        fireEvent(el, null, 'change', { bubbles: true, cancelable: false });\n"
"      }\n"
"      activationBehaviour(el);\n"
"    } else undoPreClick(pre);\n"
"  } finally { clicking.delete(el); }\n"
"});\n"

/* ======================================================================
 * 9. focus()/blur() reach every element, not only <input>
 * ======================================================================
 * js_forms.c installs focus/blur with
 * `Object.getPrototypeOf(document.createElement('input'))`, which was the ONE
 * shared element prototype when it was written and is HTMLInputElement's since
 * 7fc2bec. So a <button>, <select> or <dialog> has had no focus() at all --
 * and `assert_equals(document.activeElement, invoker)` is in the middle of
 * every invoker test in the corpus.
 *
 * The fix is to REUSE js_forms.c's own function rather than to write a second
 * one: its closure already owns the element-key stamping and the native call.
 * Copying the descriptor up to HTMLElement.prototype makes it general with no
 * duplicated logic, and the `in` guard means that the day js_forms.c installs
 * there itself, this does nothing. */
"var IEP = P('HTMLInputElement');\n"
"var nativeFocus = null, nativeBlur = null;\n"
"if (IEP) {\n"
"  var df = Object.getOwnPropertyDescriptor(IEP, 'focus');\n"
"  var db = Object.getOwnPropertyDescriptor(IEP, 'blur');\n"
"  if (df && typeof df.value === 'function') nativeFocus = df.value;\n"
"  if (db && typeof db.value === 'function') nativeBlur = db.value;\n"
"}\n"

/* AND THEN THE EVENTS HAVE TO ARRIVE, WHICH IS A SECOND PROBLEM ENTIRELY.
 *
 * focus.c does not dispatch anything by itself: it calls through
 * `fc_dispatch`, which is a NULL function pointer until somebody calls
 * `fc_set_dispatch`, and the only caller in the tree is browser.c -- the app
 * shell, which no host test links and which tests/wpt.mk excludes on purpose.
 * So in every host link, `el.focus()` reached the native side, moved focus.c's
 * internal pointer, and fired NOTHING; js_forms.c's `document.activeElement`
 * is tracked from those events, so it stayed on <body> forever. That is 2,100
 * subtests in html/semantics/popovers alone, all failing on
 * `assert_equals(document.activeElement, invoker)`, and not one of them is a
 * popover bug.
 *
 * So focus() finishes the job in JS when the native path did not: it checks
 * whether focus actually landed, and if it did not, fires blur/focusout on the
 * old element and focus/focusin on the new one -- the same four events, in the
 * same order, that focus.c's focus_set() fires. js_forms.c's capture listeners
 * on the document then record activeElement, so the whole model becomes
 * consistent again from one place.
 *
 * WHY THIS IS NOT A WORKAROUND THAT DIVERGES IN THE BROWSER: the fallback is
 * gated on `document.activeElement !== this` AFTER the native call. In the
 * real browser fc_set_dispatch IS wired, the native path moves activeElement,
 * and every line below is skipped. It only runs where the alternative is
 * nothing happening at all. */
"function isFocusable(el) {\n"
"  if (!el || el.nodeType !== 1 || !el.isConnected) return false;\n"
"  if (isDisabledCtl(el)) return false;\n"
"  if (el.hasAttribute('hidden')) return false;\n"
"  if (el.hasAttribute('tabindex')) return true;\n"
"  var t = tagOf(el);\n"
"  if (t === 'input') return lc(el.getAttribute('type') || '') !== 'hidden';\n"
"  if (t === 'select' || t === 'textarea' || t === 'button' ||\n"
"      t === 'iframe' || t === 'summary') return true;\n"
"  if (t === 'a' || t === 'area') return el.hasAttribute('href');\n"
"  if (t === 'audio' || t === 'video') return el.hasAttribute('controls');\n"
"  if (t === 'dialog') return modalDialogs.get(el) === true;\n"
"  var ce = el.getAttribute('contenteditable');\n"
"  if (ce !== null && (ce === '' || lc(ce) === 'true')) return true;\n"
"  return false;\n"
"}\n"
/* A FocusEvent if the constructor is there, an Event otherwise. `relatedTarget`
 * is what a page reads to find where focus came from, so it is set when it can
 * be. */
"function fireFocusEvent(target, type, bubbles, related) {\n"
"  var C = G.FocusEvent || G.UIEvent || G.Event;\n"
"  var e;\n"
"  try { e = new C(type, { bubbles: bubbles, cancelable: false, composed: true,\n"
"                          relatedTarget: related || null, view: G }); }\n"
"  catch (q) { try { e = new G.Event(type, { bubbles: bubbles, cancelable: false }); }\n"
"              catch (q2) { return; } }\n"
"  try { target.dispatchEvent(e); } catch (q3) {}\n"
"}\n"
"function focusFallback(el) {\n"
"  var old = doc.activeElement;\n"
"  if (old === el) return;\n"
"  if (old && old !== doc.body && old !== doc.documentElement) {\n"
"    fireFocusEvent(old, 'blur', false, el);\n"
"    fireFocusEvent(old, 'focusout', true, el);\n"
"  }\n"
"  fireFocusEvent(el, 'focus', false, old || null);\n"
"  fireFocusEvent(el, 'focusin', true, old || null);\n"
"}\n"
"meth(EP, 'focus', function () {\n"
"  if (nativeFocus) { try { nativeFocus.call(this); } catch (e) {} }\n"
"  if (doc.activeElement === this) return;\n"
"  if (!isFocusable(this)) return;\n"
"  focusFallback(this);\n"
"});\n"
"meth(EP, 'blur', function () {\n"
"  if (nativeBlur) { try { nativeBlur.call(this); } catch (e) {} }\n"
"  if (doc.activeElement !== this) return;\n"
"  fireFocusEvent(this, 'blur', false, null);\n"
"  fireFocusEvent(this, 'focusout', true, null);\n"
"});\n"
/* HTMLInputElement.prototype keeps js_forms.c's own pair as an OWN property,
 * which would shadow the one above for the single most-tested element in the
 * corpus. Deleting the shadow makes <input> inherit the same implementation as
 * everything else; js_forms.c's function is still what runs first, because
 * `nativeFocus` above is that function. */
"if (IEP && IEP !== EP) {\n"
"  ['focus', 'blur'].forEach(function (m) {\n"
"    if (Object.getOwnPropertyDescriptor(IEP, m)) { try { delete IEP[m]; } catch (e) {} }\n"
"  });\n"
"}\n"

/* ======================================================================
 * 10. The selector hooks
 * ======================================================================
 * js_select.c models `:popover-open` and `:modal` as INERT -- they parse and
 * match nothing, which was the honest answer while nothing tracked the state.
 * The state exists now and lives here, so it is published as two predicates
 * the selector engine consults. A build without this file leaves them
 * undefined and js_select.c falls back to exactly its old answer. */
"G.__logit_popover_open = function (el) { return popoverIsShowing(el); };\n"
"G.__logit_modal = function (el) { return !!modalDialogs.get(el); };\n"

/* The parser can hand us a document that already breaks the accordion rule --
 * three `<details name=x open>` in the source -- and the spec says only the
 * FIRST stays open. That is a one-off sweep at install rather than part of
 * enforceDetailsName, because the rule for a live mutation ("the one being
 * opened wins") is the opposite of the rule at parse time ("the first wins"). */
"(function () {\n"
"  var all = [], seen = {};\n"
"  descendants(doc, { details: 1 }, all);\n"
"  for (var i = 0; i < all.length; i++) {\n"
"    var d = all[i], nm = detailsName(d);\n"
"    if (nm === null || !d.hasAttribute('open')) continue;\n"
"    if (seen['n' + nm]) d.removeAttribute('open');\n"
"    else seen['n' + nm] = 1;\n"
"  }\n"
"})();\n"

/* ======================================================================
 * 11. Table interfaces
 * ====================================================================== */
"var SECTION_TAGS = { thead: 1, tbody: 1, tfoot: 1 };\n"
"var TBL = P('HTMLTableElement');\n"
"function tableSections(t, out) { childElems(t, SECTION_TAGS, out); }\n"
/* `table.rows` is the <tr> children of <thead>, then the table's own <tr>
 * children and those of every <tbody>, then <tfoot> -- in that order, which is
 * NOT tree order and is the reason this cannot be a querySelectorAll. */
"function tableRows(t, out) {\n"
"  var i, s, secs = [];\n"
"  tableSections(t, secs);\n"
"  for (i = 0; i < secs.length; i++) if (tagOf(secs[i]) === 'thead') childElems(secs[i], { tr: 1 }, out);\n"
"  for (var c = t.firstChild; c; c = c.nextSibling) {\n"
"    if (c.nodeType !== 1) continue;\n"
"    var tg = lc(c.tagName);\n"
"    if (tg === 'tr') out.push(c);\n"
"    else if (tg === 'tbody') childElems(c, { tr: 1 }, out);\n"
"  }\n"
"  for (i = 0; i < secs.length; i++) if (tagOf(secs[i]) === 'tfoot') childElems(secs[i], { tr: 1 }, out);\n"
"}\n"
"if (TBL) {\n"
"  acc(TBL, 'rows', function () { return cachedCollection(this, 'rows', tableRows); });\n"
"  acc(TBL, 'tBodies', function () { return cachedCollection(this, 'tBodies',\n"
"    function (n, out) { childElems(n, { tbody: 1 }, out); }); });\n"
"  acc(TBL, 'caption', function () {\n"
"    for (var c = this.firstChild; c; c = c.nextSibling)\n"
"      if (c.nodeType === 1 && tagOf(c) === 'caption') return c;\n"
"    return null;\n"
"  }, function (v) {\n"
"    var old = this.caption;\n"
"    if (v === null || v === undefined) { if (old) old.parentNode.removeChild(old); return; }\n"
"    if (tagOf(v) !== 'caption') throwDom('HierarchyRequestError', 'caption must be a <caption>');\n"
"    if (old) old.parentNode.removeChild(old);\n"
"    this.insertBefore(v, this.firstChild);\n"
"  });\n"
"  meth(TBL, 'createCaption', function () {\n"
"    var c = this.caption;\n"
"    if (c) return c;\n"
"    c = doc.createElement('caption');\n"
"    this.insertBefore(c, this.firstChild);\n"
"    return c;\n"
"  });\n"
"  meth(TBL, 'deleteCaption', function () {\n"
"    var c = this.caption; if (c) c.parentNode.removeChild(c);\n"
"  });\n"
"  ['tHead', 'tFoot'].forEach(function (prop) {\n"
"    var want = lc(prop);\n"
"    acc(TBL, prop, function () {\n"
"      for (var c = this.firstChild; c; c = c.nextSibling)\n"
"        if (c.nodeType === 1 && tagOf(c) === want) return c;\n"
"      return null;\n"
"    }, function (v) {\n"
"      var old = this[prop];\n"
"      if (v === null || v === undefined) { if (old) old.parentNode.removeChild(old); return; }\n"
"      if (tagOf(v) !== want) throwDom('HierarchyRequestError', prop + ' must be a <' + want + '>');\n"
"      if (old) old.parentNode.removeChild(old);\n"
         /* <thead> goes before the first tbody/tr; <tfoot> goes at the end. */
"      if (want === 'thead') {\n"
"        var ref = null;\n"
"        for (var c2 = this.firstChild; c2; c2 = c2.nextSibling) {\n"
"          if (c2.nodeType !== 1) continue;\n"
"          var t2 = lc(c2.tagName);\n"
"          if (t2 !== 'caption' && t2 !== 'colgroup') { ref = c2; break; }\n"
"        }\n"
"        this.insertBefore(v, ref);\n"
"      } else this.appendChild(v);\n"
"    });\n"
"  });\n"
"  meth(TBL, 'createTHead', function () {\n"
"    if (this.tHead) return this.tHead;\n"
"    var h = doc.createElement('thead'); this.tHead = h; return h;\n"
"  });\n"
"  meth(TBL, 'deleteTHead', function () { var h = this.tHead; if (h) h.parentNode.removeChild(h); });\n"
"  meth(TBL, 'createTFoot', function () {\n"
"    if (this.tFoot) return this.tFoot;\n"
"    var f = doc.createElement('tfoot'); this.tFoot = f; return f;\n"
"  });\n"
"  meth(TBL, 'deleteTFoot', function () { var f = this.tFoot; if (f) f.parentNode.removeChild(f); });\n"
"  meth(TBL, 'createTBody', function () {\n"
"    var b = doc.createElement('tbody');\n"
       /* After the LAST tbody, not at the end -- a <tfoot> must stay last. */
"    var last = null;\n"
"    for (var c = this.firstChild; c; c = c.nextSibling)\n"
"      if (c.nodeType === 1 && tagOf(c) === 'tbody') last = c;\n"
"    this.insertBefore(b, last ? last.nextSibling : null);\n"
"    return b;\n"
"  });\n"
"  meth(TBL, 'insertRow', function (index) {\n"
"    index = (index === undefined) ? -1 : (index | 0);\n"
"    var rows = []; tableRows(this, rows);\n"
"    if (index < -1 || index > rows.length)\n"
"      throwDom('IndexSizeError', 'The index is out of range.');\n"
"    var tr = doc.createElement('tr');\n"
"    if (!rows.length) {\n"
"      var body = null;\n"
"      for (var c = this.firstChild; c; c = c.nextSibling)\n"
"        if (c.nodeType === 1 && tagOf(c) === 'tbody') body = c;\n"
"      if (!body) { body = doc.createElement('tbody'); this.appendChild(body); }\n"
"      body.appendChild(tr);\n"
"    } else if (index === -1 || index === rows.length) {\n"
"      var lastRow = rows[rows.length - 1];\n"
"      lastRow.parentNode.appendChild(tr);\n"
"    } else {\n"
"      var ref = rows[index];\n"
"      ref.parentNode.insertBefore(tr, ref);\n"
"    }\n"
"    return tr;\n"
"  });\n"
"  meth(TBL, 'deleteRow', function (index) {\n"
"    index = index | 0;\n"
"    var rows = []; tableRows(this, rows);\n"
"    if (index === -1) { if (rows.length) rows[rows.length - 1].parentNode.removeChild(rows[rows.length - 1]); return; }\n"
"    if (index < 0 || index >= rows.length)\n"
"      throwDom('IndexSizeError', 'The index is out of range.');\n"
"    rows[index].parentNode.removeChild(rows[index]);\n"
"  });\n"
"}\n"

"var TSEC = P('HTMLTableSectionElement');\n"
"if (TSEC) {\n"
"  acc(TSEC, 'rows', function () { return cachedCollection(this, 'rows',\n"
"    function (n, out) { childElems(n, { tr: 1 }, out); }); });\n"
"  meth(TSEC, 'insertRow', function (index) {\n"
"    index = (index === undefined) ? -1 : (index | 0);\n"
"    var rows = []; childElems(this, { tr: 1 }, rows);\n"
"    if (index < -1 || index > rows.length)\n"
"      throwDom('IndexSizeError', 'The index is out of range.');\n"
"    var tr = doc.createElement('tr');\n"
"    if (index === -1 || index === rows.length) this.appendChild(tr);\n"
"    else this.insertBefore(tr, rows[index]);\n"
"    return tr;\n"
"  });\n"
"  meth(TSEC, 'deleteRow', function (index) {\n"
"    index = index | 0;\n"
"    var rows = []; childElems(this, { tr: 1 }, rows);\n"
"    if (index === -1) { if (rows.length) this.removeChild(rows[rows.length - 1]); return; }\n"
"    if (index < 0 || index >= rows.length)\n"
"      throwDom('IndexSizeError', 'The index is out of range.');\n"
"    this.removeChild(rows[index]);\n"
"  });\n"
"}\n"

"var TROW = P('HTMLTableRowElement');\n"
"if (TROW) {\n"
"  acc(TROW, 'cells', function () { return cachedCollection(this, 'cells',\n"
"    function (n, out) { childElems(n, { td: 1, th: 1 }, out); }); });\n"
"  acc(TROW, 'rowIndex', function () {\n"
"    var t = this.parentNode;\n"
"    while (t && t.nodeType === 1 && tagOf(t) !== 'table') t = t.parentNode;\n"
"    if (!t || t.nodeType !== 1 || tagOf(t) !== 'table') return -1;\n"
"    var rows = []; tableRows(t, rows);\n"
"    return rows.indexOf(this);\n"
"  });\n"
"  acc(TROW, 'sectionRowIndex', function () {\n"
"    var p = this.parentNode;\n"
"    if (!p || p.nodeType !== 1) return -1;\n"
"    var tg = tagOf(p);\n"
"    if (!SECTION_TAGS[tg] && tg !== 'table') return -1;\n"
"    var rows = []; childElems(p, { tr: 1 }, rows);\n"
"    return rows.indexOf(this);\n"
"  });\n"
"  meth(TROW, 'insertCell', function (index) {\n"
"    index = (index === undefined) ? -1 : (index | 0);\n"
"    var cells = []; childElems(this, { td: 1, th: 1 }, cells);\n"
"    if (index < -1 || index > cells.length)\n"
"      throwDom('IndexSizeError', 'The index is out of range.');\n"
"    var td = doc.createElement('td');\n"
"    if (index === -1 || index === cells.length) this.appendChild(td);\n"
"    else this.insertBefore(td, cells[index]);\n"
"    return td;\n"
"  });\n"
"  meth(TROW, 'deleteCell', function (index) {\n"
"    index = index | 0;\n"
"    var cells = []; childElems(this, { td: 1, th: 1 }, cells);\n"
"    if (index === -1) { if (cells.length) this.removeChild(cells[cells.length - 1]); return; }\n"
"    if (index < 0 || index >= cells.length)\n"
"      throwDom('IndexSizeError', 'The index is out of range.');\n"
"    this.removeChild(cells[index]);\n"
"  });\n"
"}\n"

/* ======================================================================
 * 12. <select>, <option>, <form>, <template>
 * ====================================================================== */
"var SEL = P('HTMLSelectElement'), OPT = P('HTMLOptionElement');\n"
"function selectOptions(sel, out) {\n"
"  for (var c = sel.firstChild; c; c = c.nextSibling) {\n"
"    if (c.nodeType !== 1) continue;\n"
"    var t = lc(c.tagName);\n"
"    if (t === 'option') out.push(c);\n"
"    else if (t === 'optgroup') childElems(c, { option: 1 }, out);\n"
"  }\n"
"}\n"
"if (OPT) {\n"
"  acc(OPT, 'selected', function () {\n"
"    var s = optSelected.get(this);\n"
"    return s === undefined ? this.hasAttribute('selected') : s;\n"
/* Selecting an option in a SINGLE select deselects the others -- the spec's
 * "ask for a reset", and the reason `:checked` on a <select> whose markup put
 * `selected` on option1 must stop matching option1 the moment a script selects
 * option2. Without it both match, which is a state no select can be in. */
"  }, function (v) {\n"
"    optSelected.set(this, !!v);\n"
"    if (!v) return;\n"
"    var sel = ownerSelect(this);\n"
"    if (!sel || sel.hasAttribute('multiple')) return;\n"
"    var o = []; selectOptions(sel, o);\n"
"    for (var i = 0; i < o.length; i++) if (o[i] !== this) optSelected.set(o[i], false);\n"
"  });\n"
"  acc(OPT, 'index', function () {\n"
"    var sel = ownerSelect(this);\n"
"    if (!sel) return 0;\n"
"    var opts = []; selectOptions(sel, opts);\n"
"    var i = opts.indexOf(this);\n"
"    return i < 0 ? 0 : i;\n"
"  });\n"
"  acc(OPT, 'text', function () {\n"
"    return String(this.textContent === undefined ? '' : this.textContent)\n"
"      .replace(/[ \\t\\n\\f\\r]+/g, ' ').replace(/^ | $/g, '');\n"
"  }, function (v) { this.textContent = String(v); });\n"
"  acc(OPT, 'value', function () {\n"
"    var v = this.getAttribute('value');\n"
"    return v === null ? this.text : v;\n"
"  }, function (v) { this.setAttribute('value', String(v)); });\n"
"  acc(OPT, 'label', function () {\n"
"    var v = this.getAttribute('label');\n"
"    return v === null ? this.text : v;\n"
"  }, function (v) { this.setAttribute('label', String(v)); });\n"
"  acc(OPT, 'form', function () { var s = ownerSelect(this); return s ? formOwner(s) : formOwner(this); });\n"
"}\n"
"var optSelected = new Map();\n"
"function ownerSelect(opt) {\n"
"  for (var p = opt.parentNode; p && p.nodeType === 1; p = p.parentNode)\n"
"    if (tagOf(p) === 'select') return p;\n"
"  return null;\n"
"}\n"
"function formOwner(el) {\n"
"  var id = el.getAttribute && el.getAttribute('form');\n"
"  if (id !== null && id !== undefined && id !== '') {\n"
"    var r = rootOf(el);\n"
"    var f = (r && typeof r.getElementById === 'function') ? r.getElementById(id) : null;\n"
"    return (f && tagOf(f) === 'form') ? f : null;\n"
"  }\n"
"  for (var p = el.parentNode; p && p.nodeType === 1; p = p.parentNode)\n"
"    if (tagOf(p) === 'form') return p;\n"
"  return null;\n"
"}\n"
"if (SEL) {\n"
"  acc(SEL, 'options', function () { return cachedCollection(this, 'options', selectOptions); });\n"
"  acc(SEL, 'length', function () { var o = []; selectOptions(this, o); return o.length; },\n"
"    function (v) {\n"
"      var o = []; selectOptions(this, o); var want = v >>> 0;\n"
"      while (o.length > want) { var last = o.pop(); last.parentNode.removeChild(last); }\n"
"      while (o.length < want) { this.appendChild(doc.createElement('option')); o.push(1); }\n"
"    });\n"
"  acc(SEL, 'selectedOptions', function () { return cachedCollection(this, 'selectedOptions',\n"
"    function (n, out) {\n"
"      var o = []; selectOptions(n, o);\n"
"      for (var i = 0; i < o.length; i++) if (o[i].selected) out.push(o[i]);\n"
"    }); });\n"
"  acc(SEL, 'selectedIndex', function () {\n"
"    var o = []; selectOptions(this, o);\n"
"    for (var i = 0; i < o.length; i++) if (o[i].selected) return i;\n"
       /* A single-select with nothing selected still has a selected option:
        * the first non-disabled one. Only a `multiple` or sized select can
        * genuinely have none. */
"    if (!this.hasAttribute('multiple') && !o.length) return -1;\n"
"    if (!this.hasAttribute('multiple')) {\n"
"      for (var j = 0; j < o.length; j++) if (!o[j].hasAttribute('disabled')) return j;\n"
"    }\n"
"    return -1;\n"
"  }, function (v) {\n"
"    var o = []; selectOptions(this, o); var want = v | 0;\n"
"    for (var i = 0; i < o.length; i++) optSelected.set(o[i], i === want);\n"
"  });\n"
"  acc(SEL, 'value', function () {\n"
"    var i = this.selectedIndex; if (i < 0) return '';\n"
"    var o = []; selectOptions(this, o);\n"
"    return i < o.length ? o[i].value : '';\n"
"  }, function (v) {\n"
"    var o = []; selectOptions(this, o); v = String(v);\n"
"    var hit = -1;\n"
"    for (var i = 0; i < o.length; i++) if (o[i].value === v) { hit = i; break; }\n"
"    for (var j = 0; j < o.length; j++) optSelected.set(o[j], j === hit);\n"
"  });\n"
"  acc(SEL, 'type', function () {\n"
"    return this.hasAttribute('multiple') ? 'select-multiple' : 'select-one'; });\n"
"  acc(SEL, 'form', function () { return formOwner(this); });\n"
"  meth(SEL, 'item', function (i) { var o = []; selectOptions(this, o);\n"
"    i = i >>> 0; return i < o.length ? o[i] : null; });\n"
"  meth(SEL, 'namedItem', function (n) { var o = []; selectOptions(this, o); n = String(n);\n"
"    for (var i = 0; i < o.length; i++)\n"
"      if (o[i].getAttribute('id') === n || o[i].getAttribute('name') === n) return o[i];\n"
"    return null; });\n"
"  meth(SEL, 'add', function (el, before) {\n"
"    if (before === undefined || before === null) { this.appendChild(el); return; }\n"
"    if (typeof before === 'number') {\n"
"      var o = []; selectOptions(this, o);\n"
"      var ref = (before >= 0 && before < o.length) ? o[before] : null;\n"
"      this.insertBefore(el, ref); return;\n"
"    }\n"
"    if (before.parentNode !== this) throwDom('NotFoundError', 'The before element is not a child.');\n"
"    this.insertBefore(el, before);\n"
"  });\n"
"  meth(SEL, 'remove', function (i) {\n"
"    if (arguments.length === 0) { if (this.parentNode) this.parentNode.removeChild(this); return; }\n"
"    var o = []; selectOptions(this, o); i = i | 0;\n"
"    if (i >= 0 && i < o.length) o[i].parentNode.removeChild(o[i]);\n"
"  });\n"
"}\n"

"var FRM = P('HTMLFormElement');\n"
"var LISTED = { button: 1, fieldset: 1, input: 1, object: 1, output: 1,\n"
"               select: 1, textarea: 1 };\n"
"if (FRM) {\n"
"  acc(FRM, 'elements', function () { return cachedCollection(this, 'elements',\n"
"    function (n, out) {\n"
"      var all = []; descendants(n, LISTED, all);\n"
"      for (var i = 0; i < all.length; i++) {\n"
"        if (tagOf(all[i]) === 'input' && lc(all[i].getAttribute('type') || '') === 'image') continue;\n"
"        if (formOwner(all[i]) !== n) continue;\n"
"        out.push(all[i]);\n"
"      }\n"
"    }); });\n"
"  acc(FRM, 'length', function () { return this.elements.length; });\n"
"}\n"
/* Every listed control has a `form` -- the nearest ancestor <form>, or the one
 * its `form=` attribute names. `button.form` is read by half the forms corpus
 * and by every framework that walks up from a submit button. */
"['HTMLButtonElement', 'HTMLInputElement', 'HTMLTextAreaElement', 'HTMLFieldSetElement',\n"
" 'HTMLObjectElement', 'HTMLOutputElement', 'HTMLLabelElement']\n"
"  .forEach(function (n) {\n"
"    var p = P(n);\n"
"    if (p && !('form' in p)) acc(p, 'form', function () { return formOwner(this); });\n"
"  });\n"

/* ======================================================================
 * 13. Constraint validation
 * ======================================================================
 * `el.validity`, `checkValidity()`, `reportValidity()`, `willValidate`,
 * `setCustomValidity()`. None of it existed, and "The validity attribute
 * doesn't exist" is 498 subtests in html/semantics/forms on its own -- 831 with
 * the other three names, which makes it the largest single cause left in the
 * subset after the invokers.
 *
 * It is HERE rather than in js_forms.c for the ownership reason this whole file
 * exists for, and it is installed ONLY WHERE ABSENT so that the day that line
 * writes its own, this stops.
 *
 * WHAT IS HONESTLY NOT MODELLED: `badInput`. That is "the user typed something
 * the control could not convert" -- a state that only a real editing UI can
 * produce, and there is no way to reach it from script, so it is always false
 * rather than guessed. `stepMismatch` and the range checks are done for the
 * numeric and date-ish types where the value has a defined numeric form and
 * skipped where it does not, which is what the spec says rather than a
 * shortcut. */
"var customValidity = new Map();\n"
"var VALIDATE_TAGS = { input: 1, select: 1, textarea: 1 };\n"
/* The types barred from constraint validation, and the ones with no value to
 * validate. `hidden` is barred; `reset`/`button` are barred; `image` and
 * `submit` are not submittable *values*. */
"var BARRED_INPUT = { hidden: 1, reset: 1, button: 1 };\n"
"var TEXTY_INPUT = { text: 1, search: 1, url: 1, tel: 1, email: 1, password: 1 };\n"
"var NUMERIC_INPUT = { number: 1, range: 1, date: 1, month: 1, week: 1,\n"
"                      time: 1, 'datetime-local': 1 };\n"
"function ctlValue(el) {\n"
"  var v = el.value;\n"
"  return (v === undefined || v === null) ? '' : String(v);\n"
"}\n"
"function inputType(el) { return lc(el.getAttribute('type') || 'text'); }\n"
"function willValidateEl(el) {\n"
"  var t = tagOf(el);\n"
"  if (!VALIDATE_TAGS[t]) return false;\n"
"  if (isDisabledCtl(el)) return false;\n"
"  if (el.hasAttribute('readonly')) return false;\n"
     /* A control inside a <datalist> is barred, and so is one whose ancestor
      * chain contains one -- the option is a suggestion, not a submission. */
"  for (var p = el.parentNode; p && p.nodeType === 1; p = p.parentNode)\n"
"    if (tagOf(p) === 'datalist') return false;\n"
"  if (t === 'input' && BARRED_INPUT[inputType(el)]) return false;\n"
"  return true;\n"
"}\n"
"var EMAIL_RE = /^[a-zA-Z0-9.!#$%&'*+\\/=?^_`{|}~-]+@[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?(?:\\.[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?)*$/;\n"
"function numValue(el, s) {\n"
"  var ty = inputType(el);\n"
"  if (ty === 'number' || ty === 'range') {\n"
"    if (!/^[-+]?(\\d+\\.?\\d*|\\.\\d+)([eE][-+]?\\d+)?$/.test(s)) return null;\n"
"    var n = parseFloat(s);\n"
"    return isNaN(n) ? null : n;\n"
"  }\n"
"  return null;\n"
"}\n"
"function computeValidity(el) {\n"
"  var v = { valueMissing: false, typeMismatch: false, patternMismatch: false,\n"
"            tooLong: false, tooShort: false, rangeUnderflow: false,\n"
"            rangeOverflow: false, stepMismatch: false, badInput: false,\n"
"            customError: false, valid: true };\n"
"  var custom = customValidity.get(el);\n"
"  if (custom) v.customError = true;\n"
"  if (!willValidateEl(el)) {\n"
"    v.valid = !v.customError;\n"
"    return v;\n"
"  }\n"
"  var t = tagOf(el), val = ctlValue(el), ty = (t === 'input') ? inputType(el) : '';\n"
"  var required = el.hasAttribute('required');\n"
"  if (required) {\n"
"    if (t === 'select') {\n"
"      var o = []; selectOptions(el, o);\n"
"      var i = el.selectedIndex;\n"
"      if (i < 0 || (i < o.length && o[i].value === '' && !o[i].hasAttribute('value')\n"
"                    && o[i].text === '')) v.valueMissing = true;\n"
"      else if (i < 0) v.valueMissing = true;\n"
"    } else if (ty === 'checkbox') { if (!el.checked) v.valueMissing = true; }\n"
"    else if (ty === 'radio') {\n"
"      var nm = el.getAttribute('name'), any = false;\n"
"      if (!nm) any = !!el.checked;\n"
"      else {\n"
"        var scope = formOwner(el) || rootOf(el), all = [];\n"
"        descendants(scope && scope.nodeType ? scope : doc, { input: 1 }, all);\n"
"        for (var r = 0; r < all.length; r++)\n"
"          if (lc(all[r].getAttribute('type') || '') === 'radio' &&\n"
"              all[r].getAttribute('name') === nm && all[r].checked) { any = true; break; }\n"
"      }\n"
"      if (!any) v.valueMissing = true;\n"
"    } else if (val === '') v.valueMissing = true;\n"
"  }\n"
"  if (val !== '' && t === 'input') {\n"
"    if (ty === 'email') {\n"
"      if (el.hasAttribute('multiple')) {\n"
"        var parts = val.split(',');\n"
"        for (var p2 = 0; p2 < parts.length; p2++)\n"
"          if (!EMAIL_RE.test(parts[p2].replace(/^[ \\t\\n\\r\\f]+|[ \\t\\n\\r\\f]+$/g, '')))\n"
"            { v.typeMismatch = true; break; }\n"
"      } else if (!EMAIL_RE.test(val)) v.typeMismatch = true;\n"
"    } else if (ty === 'url') {\n"
"      if (typeof G.URL === 'function') { try { new G.URL(val); } catch (e) { v.typeMismatch = true; } }\n"
"      else if (!/^[a-zA-Z][a-zA-Z0-9+.-]*:/.test(val)) v.typeMismatch = true;\n"
"    }\n"
"    var pat = el.getAttribute('pattern');\n"
"    if (pat !== null && (TEXTY_INPUT[ty] || ty === 'text')) {\n"
"      try {\n"
"        var re = new RegExp('^(?:' + pat + ')$', 'v');\n"
"        if (!re.test(val)) v.patternMismatch = true;\n"
"      } catch (e1) {\n"
"        try { if (!(new RegExp('^(?:' + pat + ')$', 'u')).test(val)) v.patternMismatch = true; }\n"
"        catch (e2) {}\n"
"      }\n"
"    }\n"
"    var n = numValue(el, val);\n"
"    if (n !== null) {\n"
"      var mn = el.getAttribute('min'), mx = el.getAttribute('max');\n"
"      if (mn !== null && numValue(el, mn) !== null && n < numValue(el, mn)) v.rangeUnderflow = true;\n"
"      if (mx !== null && numValue(el, mx) !== null && n > numValue(el, mx)) v.rangeOverflow = true;\n"
"      var st = el.getAttribute('step');\n"
"      if (st !== null && lc(st) !== 'any') {\n"
"        var sv = parseFloat(st);\n"
"        if (!isNaN(sv) && sv > 0) {\n"
"          var base = (mn !== null && numValue(el, mn) !== null) ? numValue(el, mn) : 0;\n"
"          var k = (n - base) / sv;\n"
"          if (Math.abs(k - Math.round(k)) > 1e-9) v.stepMismatch = true;\n"
"        }\n"
"      }\n"
"    }\n"
"  }\n"
     /* tooLong/tooShort apply only when the value is DIRTY -- typed or set by
      * script -- which is why a document whose markup contains an over-long
      * `value` attribute is not invalid on load. There is no dirty flag
      * reachable from here, so the maxlength side is left to the control and
      * only the script-set case is caught, via minlength on a non-empty value. */
"  if ((TEXTY_INPUT[ty] || t === 'textarea') && val !== '') {\n"
"    var mnl = parseInt(el.getAttribute('minlength'), 10);\n"
"    if (!isNaN(mnl) && val.length < mnl) v.tooShort = true;\n"
"    var mxl = parseInt(el.getAttribute('maxlength'), 10);\n"
"    if (!isNaN(mxl) && mxl >= 0 && val.length > mxl) v.tooLong = true;\n"
"  }\n"
"  v.valid = !(v.valueMissing || v.typeMismatch || v.patternMismatch || v.tooLong ||\n"
"              v.tooShort || v.rangeUnderflow || v.rangeOverflow || v.stepMismatch ||\n"
"              v.badInput || v.customError);\n"
"  return v;\n"
"}\n"
/* ValidityState is an interface with a name, so pages feature-detect it and
 * `Object.prototype.toString.call(el.validity)` is read. Published if nobody
 * else has. */
"if (typeof G.ValidityState !== 'function') {\n"
"  var VS = function ValidityState() {\n"
"    throw new TypeError('Illegal constructor');\n"
"  };\n"
"  VS.prototype = {};\n"
"  Object.defineProperty(VS.prototype, 'constructor',\n"
"    { configurable: true, writable: true, value: VS });\n"
"  Object.defineProperty(VS.prototype, Symbol.toStringTag,\n"
"    { configurable: true, value: 'ValidityState' });\n"
"  try { Object.defineProperty(G, 'ValidityState',\n"
"    { configurable: true, writable: true, value: VS }); } catch (e) { G.ValidityState = VS; }\n"
"}\n"
"function validityObject(el) {\n"
"  var v = computeValidity(el);\n"
"  var o = Object.create(G.ValidityState.prototype);\n"
"  for (var k in v) Object.defineProperty(o, k, { value: v[k], enumerable: true });\n"
"  return o;\n"
"}\n"
"var VALIDATION_MSG = 'Constraints not satisfied';\n"
"function installValidation(name) {\n"
"  var p = P(name);\n"
"  if (!p) return;\n"
"  if (!('willValidate' in p))\n"
"    acc(p, 'willValidate', function () { return willValidateEl(this); });\n"
"  if (!('validity' in p))\n"
"    acc(p, 'validity', function () { return validityObject(this); });\n"
"  if (!('validationMessage' in p))\n"
"    acc(p, 'validationMessage', function () {\n"
"      if (!willValidateEl(this)) return '';\n"
"      var c = customValidity.get(this);\n"
"      if (c) return c;\n"
"      return computeValidity(this).valid ? '' : VALIDATION_MSG;\n"
"    });\n"
"  if (!('setCustomValidity' in p))\n"
"    meth(p, 'setCustomValidity', function (msg) {\n"
"      var s = String(msg);\n"
"      if (s === '') customValidity.delete(this); else customValidity.set(this, s);\n"
"    });\n"
"  if (!('checkValidity' in p))\n"
"    meth(p, 'checkValidity', function () { return checkValidityOn(this, false); });\n"
"  if (!('reportValidity' in p))\n"
"    meth(p, 'reportValidity', function () { return checkValidityOn(this, true); });\n"
"}\n"
"function checkValidityOn(el, report) {\n"
"  if (tagOf(el) === 'form') {\n"
"    var kids = [], all = [];\n"
"    descendants(el, LISTED, all);\n"
"    var ok = true;\n"
"    for (var i = 0; i < all.length; i++) {\n"
"      if (formOwner(all[i]) !== el) continue;\n"
"      if (!VALIDATE_TAGS[tagOf(all[i])]) continue;\n"
"      if (!checkValidityOn(all[i], report)) ok = false;\n"
"    }\n"
"    return ok;\n"
"  }\n"
"  if (!willValidateEl(el)) return true;\n"
"  if (computeValidity(el).valid) return true;\n"
     /* The `invalid` event is cancelable and fires once per failing control,
      * which is what a page uses to render its own error UI. */
"  fireEvent(el, null, 'invalid', { bubbles: false, cancelable: true });\n"
"  return false;\n"
"}\n"
"['HTMLInputElement', 'HTMLSelectElement', 'HTMLTextAreaElement',\n"
" 'HTMLButtonElement', 'HTMLOutputElement', 'HTMLFieldSetElement']\n"
"  .forEach(installValidation);\n"
"if (FRM) {\n"
"  if (!('checkValidity' in FRM))\n"
"    meth(FRM, 'checkValidity', function () { return checkValidityOn(this, false); });\n"
"  if (!('reportValidity' in FRM))\n"
"    meth(FRM, 'reportValidity', function () { return checkValidityOn(this, true); });\n"
"  if (!('noValidate' in FRM))\n"
"    acc(FRM, 'noValidate', function () { return this.hasAttribute('novalidate'); },\n"
"      function (v) { if (v) this.setAttribute('novalidate', ''); else this.removeAttribute('novalidate'); });\n"
"}\n"

/* ---- textarea and select get js_forms.c's value pair --------------------
 * Same shape as focus() above and the same cause: js_forms.c installs `value`,
 * `defaultValue` and the selection API with
 * `Object.getPrototypeOf(createElement('input'))`, which stopped being the
 * shared element prototype at 7fc2bec. forms.c's fc_value() is keyed on the
 * NODE and already understands <textarea>, so the descriptor is general and
 * only its location was wrong. `select` is excluded on purpose -- its `value`
 * is the selected option's, which this file defines above and fc_value does
 * not model. */
"var TAP = P('HTMLTextAreaElement');\n"
"if (IEP && TAP && IEP !== TAP) {\n"
"  ['value', 'defaultValue', 'selectionStart', 'selectionEnd', 'selectionDirection',\n"
"   'setSelectionRange', 'select', 'setRangeText']\n"
"    .forEach(function (m) {\n"
"      if (m in TAP) return;\n"
"      var d = Object.getOwnPropertyDescriptor(IEP, m);\n"
"      if (!d) return;\n"
"      try { Object.defineProperty(TAP, m, d); } catch (e) {}\n"
"    });\n"
"}\n"

/* <template>.content. The parser puts a template's children in the template
 * element itself here, so `content` is a DocumentFragment holding them --
 * built once and cached on the element, because the spec's content fragment is
 * a stable object identity (`t.content === t.content`). */
"var TPL = P('HTMLTemplateElement');\n"
"var tplContent = new Map();\n"
"if (TPL) {\n"
"  acc(TPL, 'content', function () {\n"
"    var f = tplContent.get(this);\n"
"    if (f) return f;\n"
"    f = doc.createDocumentFragment();\n"
"    var c = this.firstChild;\n"
"    while (c) { var nx = c.nextSibling; f.appendChild(c); c = nx; }\n"
"    tplContent.set(this, f);\n"
"    return f;\n"
"  });\n"
"}\n"
"})";

/* --------------------------------------------------------------------------
 * The install.
 * ------------------------------------------------------------------------ */
#ifdef SEMANTICS_STATIC_COLLECTIONS
static const int SEM_STATIC = 1;   /* the negative control; see the header */
#else
static const int SEM_STATIC = 0;
#endif

void js_semantics_install(JSContext *ctx)
{
    JSValue fn = JS_Eval(ctx, SEMANTICS_PRELUDE, strlen(SEMANTICS_PRELUDE),
                         "<semantics>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(fn)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        printf("js_semantics: prelude failed: %s\n", m ? m : "?");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
        JS_FreeValue(ctx, fn);
        return;
    }
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue arg = JS_NewBool(ctx, SEM_STATIC);
    JSValue r = JS_Call(ctx, fn, g, 1, (JSValueConst *)&arg);
    if (JS_IsException(r)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        printf("js_semantics: install threw: %s\n", m ? m : "?");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, arg);
    JS_FreeValue(ctx, g);
    JS_FreeValue(ctx, fn);
}
