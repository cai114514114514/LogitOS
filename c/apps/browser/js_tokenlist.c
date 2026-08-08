/* DOMTokenList -- element.classList, to the letter of the DOM specification.
 *
 * WHY THIS EXISTS WHEN js_dom.c ALREADY HAS ONE
 * js_dom.c has a classList and it is a reasonable-looking one: add/remove/
 * toggle/replace/contains/item/length/value, an exotic hook for cl[0], and
 * Array.prototype's iterator borrowed for for-of. It scores 765/1420 on
 * dom/nodes/Element-classlist.html, and the 655 it misses are not exotic
 * corners -- they are the four rules the interface is actually made of:
 *
 *   1. THE ERRORS ARE THE INTERFACE. 405 of the 655 are one assertion:
 *      assert_throws_dom. An empty token is a SyntaxError; a token holding any
 *      ASCII whitespace is an InvalidCharacterError; and both are thrown
 *      BEFORE anything is written, so a failed call leaves no MutationRecord.
 *      js_dom.c's version throws nothing at all -- classList.add('') silently
 *      appends nothing and classList.add('a b') silently makes ONE token
 *      called "a b" that no selector will ever match again. That is the shape
 *      of bug this whole file is about: not a missing method, a wrong answer.
 *
 *   2. THE TOKEN SET IS AN ORDERED SET, SO IT DEDUPLICATES. class="aa aa" has
 *      length 1 and item(1) is null. js_dom.c reports 2 and "aa".
 *
 *   3. ASCII WHITESPACE IS FIVE CHARACTERS, NOT ONE. js_dom.c's word_has()
 *      splits on U+0020 only, so on class="\ra\na\ta\f" -- which the HTML
 *      parser produces from perfectly ordinary indented markup --
 *      contains('a') is false and remove('a') removes nothing.
 *
 *   4. EVERY SUCCESSFUL MUTATION RE-SERIALISES. add() of a token already
 *      present still rewrites "a  b" as "a b"; that is not cosmetic, it is
 *      what makes the attribute and the token set the same object. js_dom.c
 *      skips the write when nothing was added, so the two drift.
 *
 * ITS OWN FILE, AND ITS OWN LAYER. js_dom.c belongs to another line and is
 * being extended in parallel, so this does not touch it: it redefines the
 * `classList` accessor on Element.prototype and reaches the element only
 * through getAttribute/setAttribute -- the same two calls a page makes. That
 * has a real consequence worth stating: invalidation, the id/class indexes and
 * MutationObserver records all come from setAttribute, so they cannot disagree
 * with classList by construction. There is no second writer.
 *
 * WHY A PROXY. A DOMTokenList is LIVE: `var l = e.classList; e.className = 'x';
 * l[0]` must read 'x'. Materialised index properties cannot do that, which is
 * why js_dom.c needed a C exotic hook. In JS the equivalent is a Proxy, and the
 * fallback when a runtime has none is a snapshot -- correct in everything
 * except liveness, which is the right way round to degrade.
 */
#include "quickjs.h"
#include "js_tokenlist.h"
#include <string.h>

int printf(const char *, ...);

static const char *TOKENLIST_PRELUDE =
"(function () {\n"
"'use strict';\n"
"var G = globalThis;\n"
"if (G.__logit_tokenlist) return;\n"
"var doc = G.document;\n"
"if (!doc || typeof doc.createElement !== 'function') return;\n"
"var probe = doc.createElement('div');\n"
"var EP = probe && Object.getPrototypeOf(probe);\n"
"if (!EP) return;\n"

/* The DOM's ASCII whitespace: TAB LF FF CR SPACE. Not \s -- that also matches
 * U+00A0 and the Unicode spaces, none of which separate class tokens. */
"var WS = /[\\t\\n\\f\\r ]/;\n"
"var WSPLIT = /[\\t\\n\\f\\r ]+/;\n"

/* A real DOMException, because assert_throws_dom checks `.name` and the legacy
 * `.code`, and because a page's `catch (e) { if (e.name === 'SyntaxError') }`
 * is the idiom this interface exists to feed. js_platform.c publishes the
 * constructor; it installs after this file but long before any of these
 * functions is CALLED, so the lookup is deliberately late. */
"function domThrow(name, msg) {\n"
"  var e = null, DE = G.DOMException;\n"
"  if (typeof DE === 'function') { try { e = new DE(msg, name); } catch (q) { e = null; } }\n"
"  if (!e) { e = new Error(msg); e.name = name; }\n"
"  throw e;\n"
"}\n"
"function checkToken(t) {\n"
"  if (t === '') domThrow('SyntaxError', 'The token provided must not be empty.');\n"
"  if (WS.test(t)) domThrow('InvalidCharacterError',\n"
"    \"The token provided ('\" + t + \"') contains HTML space characters, which are not valid in tokens.\");\n"
"}\n"

/* The ordered set parser. Deduplicating here is not an optimisation: length,
 * item() and the index properties are all defined over the SET, so a document
 * with class="aa aa" has one token. */
"function parseSet(v) {\n"
"  var out = [], seen = Object.create(null);\n"
"  if (v === null || v === undefined) return out;\n"
"  var parts = String(v).split(WSPLIT);\n"
"  for (var i = 0; i < parts.length; i++) {\n"
"    var p = parts[i];\n"
       /* '#' prefix so a class literally named __proto__ or hasOwnProperty
          cannot collide with the bookkeeping. */
"    if (!p || seen['#' + p]) continue;\n"
"    seen['#' + p] = 1;\n"
"    out.push(p);\n"
"  }\n"
"  return out;\n"
"}\n"

"function TL(el) { this._e = el; }\n"
/* Reuse the published DOMTokenList.prototype when there is one, so
 * `e.classList instanceof DOMTokenList` and the interface tests keep their
 * answer; the methods on it are replaced, the identity is not. */
"var TLProto = null;\n"
"if (typeof G.DOMTokenList === 'function' && G.DOMTokenList.prototype\n"
"    && G.DOMTokenList.prototype !== Object.prototype) TLProto = G.DOMTokenList.prototype;\n"
"if (!TLProto) {\n"
"  TLProto = {};\n"
"  var Ctor = function DOMTokenList() { throw new TypeError('Illegal constructor'); };\n"
"  Ctor.prototype = TLProto;\n"
"  try { Object.defineProperty(TLProto, 'constructor',\n"
"        { value: Ctor, writable: true, configurable: true }); } catch (e) {}\n"
"  try { G.DOMTokenList = Ctor; } catch (e) {}\n"
"}\n"
"TL.prototype = TLProto;\n"

"function def(o, k, v) {\n"
"  try { Object.defineProperty(o, k,\n"
"    { value: v, writable: true, configurable: true, enumerable: false }); } catch (e) {}\n"
"}\n"
"function defget(o, k, g, s) {\n"
"  try { Object.defineProperty(o, k,\n"
"    { get: g, set: s, configurable: true, enumerable: false }); } catch (e) {}\n"
"}\n"

/* The receiver check. Reached through the prototype rather than an instance
 * (`DOMTokenList.prototype.length`) this is a TypeError, not a DOM error --
 * it is a brand check, not a document problem. */
"function elemOf(t) {\n"
"  var e = t && t._e;\n"
"  if (!e) throw new TypeError('not a DOMTokenList');\n"
"  return e;\n"
"}\n"
"function attrOf(el) {\n"
"  var v = el.getAttribute('class');\n"
"  return (v === undefined || v === null) ? null : v;\n"
"}\n"
"function tokensOf(el) { return parseSet(attrOf(el)); }\n"

/* The DOM's "update steps", verbatim, and the first line is the one that is
 * easy to miss: remove() on an element that has no class attribute must NOT
 * create an empty one. Everything else re-serialises, which is what normalises
 * "a  b" to "a b" on any successful mutation. */
"function update(el, set) {\n"
"  if (attrOf(el) === null && set.length === 0) return;\n"
"  el.setAttribute('class', set.join(' '));\n"
"}\n"

"defget(TLProto, 'length', function () { return tokensOf(elemOf(this)).length; });\n"
/* value is the ATTRIBUTE, verbatim -- not the serialised set. Reading it must
 * not normalise; only a mutation does. */
"defget(TLProto, 'value',\n"
"  function () { var v = attrOf(elemOf(this)); return v === null ? '' : v; },\n"
"  function (v) { elemOf(this).setAttribute('class', String(v)); });\n"

"def(TLProto, 'item', function (i) {\n"
"  var el = elemOf(this);\n"
"  var n = Number(i);\n"
"  n = (n !== n || n === Infinity || n === -Infinity) ? 0 : Math.trunc(n);\n"
"  n = n >>> 0;\n"                     /* item() takes an unsigned long */
"  var t = tokensOf(el);\n"
"  return n < t.length ? t[n] : null;\n"
"});\n"
/* contains() does NOT validate: it is a question, and asking whether an empty
 * string is in the list has an answer (no). Only mutations throw. */
"def(TLProto, 'contains', function (t) {\n"
"  return tokensOf(elemOf(this)).indexOf(String(t)) >= 0;\n"
"});\n"

/* add/remove take any number of tokens, and EVERY token is validated before
 * ANY is applied. That ordering is the observable part: add('a', '') must
 * leave the attribute untouched, so a MutationObserver sees nothing. */
"def(TLProto, 'add', function () {\n"
"  var el = elemOf(this), a = [], i;\n"
"  for (i = 0; i < arguments.length; i++) a.push(String(arguments[i]));\n"
"  for (i = 0; i < a.length; i++) checkToken(a[i]);\n"
"  var set = tokensOf(el);\n"
"  for (i = 0; i < a.length; i++) if (set.indexOf(a[i]) < 0) set.push(a[i]);\n"
"  update(el, set);\n"
"});\n"
"def(TLProto, 'remove', function () {\n"
"  var el = elemOf(this), a = [], i, k;\n"
"  for (i = 0; i < arguments.length; i++) a.push(String(arguments[i]));\n"
"  for (i = 0; i < a.length; i++) checkToken(a[i]);\n"
"  var set = tokensOf(el);\n"
"  for (i = 0; i < a.length; i++)\n"
"    while ((k = set.indexOf(a[i])) >= 0) set.splice(k, 1);\n"
"  update(el, set);\n"
"});\n"

/* toggle(token, force). The case that is always got wrong: force=true on a
 * token ALREADY present returns true and writes NOTHING -- there is no update
 * step on that branch, so the attribute keeps whatever spacing it had. */
"def(TLProto, 'toggle', function (token, force) {\n"
"  var el = elemOf(this);\n"
"  token = String(token);\n"
"  checkToken(token);\n"
"  var set = tokensOf(el), i = set.indexOf(token);\n"
"  if (i >= 0) {\n"
"    if (force === undefined || !force) { set.splice(i, 1); update(el, set); return false; }\n"
"    return true;\n"
"  }\n"
"  if (force !== undefined && !force) return false;\n"
"  set.push(token);\n"
"  update(el, set);\n"
"  return true;\n"
"});\n"

/* replace(token, newToken) -> did anything change?
 *
 * Both arguments are validated even when the replace will not happen, and the
 * substitution is Infra's "replace within an ordered set", which is NOT
 * remove-then-add: the FIRST instance of EITHER token becomes newToken and
 * every other instance of either is dropped. So replacing 'b' with 'a' in
 * "a b" gives "a", not "a a" and not "b a". Returning false must leave the
 * attribute completely alone -- the WPT file checks that with a
 * MutationObserver and counts the records. */
"def(TLProto, 'replace', function (token, newToken) {\n"
"  var el = elemOf(this);\n"
"  token = String(token); newToken = String(newToken);\n"
"  checkToken(token); checkToken(newToken);\n"
"  var set = tokensOf(el);\n"
"  if (set.indexOf(token) < 0) return false;\n"
"  var out = [], placed = false;\n"
"  for (var i = 0; i < set.length; i++) {\n"
"    var t = set[i];\n"
"    if (t === token || t === newToken) {\n"
"      if (!placed) { out.push(newToken); placed = true; }\n"
"      continue;\n"
"    }\n"
"    out.push(t);\n"
"  }\n"
"  update(el, out);\n"
"  return true;\n"
"});\n"

/* supports() answers from the attribute's supported-tokens list, and `class`
 * has none -- so the correct answer is a TypeError, not false. */
"def(TLProto, 'supports', function () {\n"
"  throw new TypeError('DOMTokenList has no supported tokens.');\n"
"});\n"
"def(TLProto, 'toString', function () {\n"
"  var v = attrOf(elemOf(this)); return v === null ? '' : v;\n"
"});\n"

/* Iteration borrows Array.prototype's generic array-index algorithms, which is
 * what the spec defines these in terms of anyway. */
"def(TLProto, 'forEach', Array.prototype.forEach);\n"
"def(TLProto, 'values', Array.prototype.values);\n"
"def(TLProto, 'keys', Array.prototype.keys);\n"
"def(TLProto, 'entries', Array.prototype.entries);\n"
"if (G.Symbol && G.Symbol.iterator) def(TLProto, G.Symbol.iterator, Array.prototype.values);\n"

/* Array indices only: '01' and '1.0' are ordinary property names, and 2^32-1
 * is not an index. */
"function isIndex(k) {\n"
"  if (typeof k !== 'string') return -1;\n"
"  if (!/^(0|[1-9][0-9]*)$/.test(k)) return -1;\n"
"  var n = +k;\n"
"  return n <= 4294967294 ? n : -1;\n"
"}\n"

"var HAVE_PROXY = (typeof G.Proxy === 'function' && typeof G.Reflect === 'object');\n"
"var handler = HAVE_PROXY ? {\n"
"  get: function (t, k, r) {\n"
"    var i = isIndex(k);\n"
"    if (i >= 0) { var s = tokensOf(t._e); return i < s.length ? s[i] : undefined; }\n"
"    return Reflect.get(t, k, r);\n"
"  },\n"
"  has: function (t, k) {\n"
"    var i = isIndex(k);\n"
"    if (i >= 0) return i < tokensOf(t._e).length;\n"
"    return Reflect.has(t, k);\n"
"  },\n"
"  getOwnPropertyDescriptor: function (t, k) {\n"
"    var i = isIndex(k);\n"
"    if (i >= 0) {\n"
"      var s = tokensOf(t._e);\n"
"      if (i >= s.length) return undefined;\n"
"      return { value: s[i], writable: false, enumerable: true, configurable: true };\n"
"    }\n"
"    if (k === '_e') return undefined;\n"
"    return Reflect.getOwnPropertyDescriptor(t, k);\n"
"  },\n"
   /* The backing element is bookkeeping, not a property of the interface, so
      it is reported by neither ownKeys nor gOPD -- Object.keys(list) is the
      indices and nothing else. The invariant that would forbid hiding it does
      not apply: it is configurable and the target is extensible. */
"  ownKeys: function (t) {\n"
"    var s = tokensOf(t._e), out = [];\n"
"    for (var i = 0; i < s.length; i++) out.push(String(i));\n"
"    return out;\n"
"  },\n"
"  set: function (t, k, v, r) {\n"
"    if (isIndex(k) >= 0) return true;\n"
"    return Reflect.set(t, k, v, r);\n"
"  },\n"
"  defineProperty: function (t, k, d) {\n"
"    if (isIndex(k) >= 0) return true;\n"
"    return Reflect.defineProperty(t, k, d);\n"
"  },\n"
"  deleteProperty: function (t, k) {\n"
"    if (isIndex(k) >= 0) return true;\n"
"    return Reflect.deleteProperty(t, k);\n"
"  }\n"
"} : null;\n"

/* The snapshot fallback: everything except liveness. Not cached, because a
 * cached snapshot is worse than a fresh one -- it is stale for the whole life
 * of the element rather than for one statement. */
"function materialise(t) {\n"
"  var s = tokensOf(t._e);\n"
"  for (var i = 0; i < s.length; i++)\n"
"    try { Object.defineProperty(t, String(i),\n"
"      { value: s[i], enumerable: true, configurable: true }); } catch (e) {}\n"
"  return t;\n"
"}\n"

/* One DOMTokenList per element, as the DOM requires: `e.classList ===
 * e.classList`. Keyed off the element WRAPPER, which js_dom.c caches per node,
 * so the identity holds for as long as the node does. */
"var cache = (typeof G.WeakMap === 'function') ? new G.WeakMap() : null;\n"
"function listFor(el) {\n"
"  if (!HAVE_PROXY) return materialise(new TL(el));\n"
"  if (cache) { var c = cache.get(el); if (c) return c; }\n"
"  var p = new G.Proxy(new TL(el), handler);\n"
"  if (cache) { try { cache.set(el, p); } catch (e) {} }\n"
"  return p;\n"
"}\n"

/* The setter is [PutForwards=value]: `e.classList = 'foo'` is `e.classList
 * .value = 'foo'`, not a rebind. Without it the assignment is a silent no-op
 * in sloppy mode and a TypeError in strict mode, and WPT tests both. */
"try {\n"
"  Object.defineProperty(EP, 'classList', {\n"
"    configurable: true,\n"
"    get: function () { return listFor(this); },\n"
"    set: function (v) { this.setAttribute('class', String(v)); }\n"
"  });\n"
"} catch (e) { throw new Error('classList accessor is not replaceable: ' + e); }\n"

"G.__logit_tokenlist = 1;\n"
"try { G.__tlEP = EP;\n"
"  var __c = Object.getOwnPropertyDescriptor(EP, 'classList');\n"
"  G.__tlChk = __c ? (__c.set ? 'get+set' : 'getonly') : 'none';\n"
"} catch (e) { G.__tlChk = 'threw ' + e; }\n"
"})\n";

void js_tokenlist_install(JSContext *ctx)
{
    if (!ctx) return;
    JSValue fn = JS_Eval(ctx, TOKENLIST_PRELUDE, strlen(TOKENLIST_PRELUDE),
                         "<tokenlist>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(fn)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        printf("[tokenlist] prelude failed: %s\n", m ? m : "?");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
        JS_FreeValue(ctx, fn);
        return;
    }
    JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 0, 0);
    if (JS_IsException(r)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        printf("[tokenlist] install failed: %s\n", m ? m : "?");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, fn);
}
