/* Selector queries and element collections: querySelectorAll,
 * getElementsByTagName / ClassName / Name, matches, closest.
 *
 * WHY THIS IS ITS OWN FILE, AND WHY IT IS WRITTEN TO BE DELETED
 * These belong to the DOM bindings (js_dom.c), which another line owns and
 * which is being extended in parallel. They are here because the probe put
 * them at the top of the list and because the acceptance case depends on them:
 * with performance and the lifecycle filled in, bing's remaining two dead
 * scripts both die on `TypeError: not a function`, and channel 1 names the
 * functions -- `document.querySelectorAll` and `document.getElementsByTagName`,
 * two references each. They are the last measured blockers on that page.
 *
 * So everything here installs ONLY IF ABSENT, and the whole file is one
 * function with no state. The moment js_dom.c grows a real, C-side,
 * live-collection implementation, this stops installing anything and can be
 * removed in one deletion with nothing else to unpick.
 *
 * WHAT IT HONESTLY IS
 * A tree walk plus a small selector matcher, in JavaScript, over the bindings
 * js_dom.c already publishes (children, tagName, id, className, getAttribute,
 * parentNode). Two deviations from the spec, both deliberate and both visible:
 *
 *   - The collections are STATIC. getElementsByTagName returns a live
 *     HTMLCollection in a browser; this returns a snapshot. Live collections
 *     need a mutation hook in the DOM itself, which is precisely the part that
 *     is not ours. Code that appends and re-reads .length sees the old count.
 *   - The selector grammar is the structural subset: type, *, #id, .class,
 *     [attr], [attr=v] and its ^= $= *= ~= |= forms, :not(), and the four
 *     combinators. Pseudo-CLASSES that depend on state or layout (:hover,
 *     :nth-child, :checked) match nothing rather than guessing, and a selector
 *     the parser does not understand throws SyntaxError rather than silently
 *     returning [] -- an empty result is indistinguishable from "no matches",
 *     and that is how a selector bug hides for a month.
 */
#include "quickjs.h"
#include "js_platform.h"
#include <string.h>

int printf(const char *, ...);

static const char *SELECT_PRELUDE =
"(function () {\n"
"'use strict';\n"
"var G = globalThis;\n"
"var doc = G.document;\n"
"if (!doc || typeof doc.createElement !== 'function') return;\n"
/* The element prototype is not reachable as a global: js_dom.c registers the
 * class but publishes no `Element` constructor. One throwaway element is the
 * only way to get at the prototype every element shares. */
"var probe = doc.createElement('div');\n"
"var EP = Object.getPrototypeOf(probe);\n"
"if (!EP) return;\n"

/* ---- the selector parser ----
 * Produces a list of alternatives (the comma list); each alternative is a list
 * of {compound, combinator} steps in DOCUMENT order, matched right to left. */
"function parseCompound(s, i) {\n"
"  var c = { tag: null, id: null, cls: [], attr: [], not: [], never: false };\n"
"  var start = i, n = s.length;\n"
"  while (i < n) {\n"
"    var ch = s[i];\n"
"    if (ch === '*') { i++; continue; }\n"
"    if (ch === '#') {\n"
"      var j = i + 1; while (j < n && /[-\\w\\u00a0-\\uffff]/.test(s[j])) j++;\n"
"      c.id = s.slice(i + 1, j); i = j; continue;\n"
"    }\n"
"    if (ch === '.') {\n"
"      var k = i + 1; while (k < n && /[-\\w\\u00a0-\\uffff]/.test(s[k])) k++;\n"
"      c.cls.push(s.slice(i + 1, k)); i = k; continue;\n"
"    }\n"
"    if (ch === '[') {\n"
"      var e = s.indexOf(']', i);\n"
"      if (e < 0) throw new SyntaxError(\"unterminated attribute selector in '\" + s + \"'\");\n"
"      var body = s.slice(i + 1, e);\n"
"      var m = /^\\s*([-\\w:]+)\\s*(?:([~^$*|]?=)\\s*(.*?)\\s*)?$/.exec(body);\n"
"      if (!m) throw new SyntaxError(\"bad attribute selector '\" + body + \"'\");\n"
"      var val = m[3];\n"
"      if (val && (val[0] === '\"' || val[0] === \"'\")) val = val.slice(1, -1);\n"
"      c.attr.push({ name: m[1], op: m[2] || null, val: val });\n"
"      i = e + 1; continue;\n"
"    }\n"
"    if (ch === ':') {\n"
"      var isNot = s.slice(i).toLowerCase().indexOf(':not(') === 0;\n"
"      if (isNot) {\n"
"        var depth = 1, p = i + 5;\n"
"        while (p < n && depth > 0) { if (s[p] === '(') depth++; else if (s[p] === ')') depth--; p++; }\n"
"        if (depth) throw new SyntaxError(\"unterminated :not() in '\" + s + \"'\");\n"
"        var innerSrc = s.slice(i + 5, p - 1);\n"
"        var inner = parseCompound(innerSrc, 0);\n"
"        c.not.push(inner.c);\n"
"        i = p; continue;\n"
"      }\n"
         /* A pseudo-class we cannot evaluate must match NOTHING, not
            EVERYTHING: `a:hover` selecting every link would be worse than
            selecting none, because the page would then style or click them. */
"      var q = i + 1;\n"
"      if (s[q] === ':') q++;\n"
"      while (q < n && /[-\\w]/.test(s[q])) q++;\n"
"      if (s[q] === '(') { var d2 = 1; q++; while (q < n && d2 > 0) { if (s[q] === '(') d2++; else if (s[q] === ')') d2--; q++; } }\n"
"      c.never = true;\n"
"      i = q; continue;\n"
"    }\n"
"    if (/[-\\w\\u00a0-\\uffff|]/.test(ch)) {\n"
"      var t = i; while (t < n && /[-\\w\\u00a0-\\uffff|]/.test(s[t])) t++;\n"
"      c.tag = s.slice(i, t).toLowerCase(); i = t; continue;\n"
"    }\n"
"    break;\n"
"  }\n"
"  if (i === start) throw new SyntaxError(\"'\" + s + \"' is not a valid selector\");\n"
"  return { c: c, i: i };\n"
"}\n"

"function parseSelector(sel) {\n"
"  var alts = [], s = String(sel), i = 0, n = s.length;\n"
"  var steps = [], comb = null;\n"
"  while (i < n) {\n"
"    while (i < n && /\\s/.test(s[i])) { if (steps.length && comb === null) comb = ' '; i++; }\n"
"    if (i >= n) break;\n"
"    var ch = s[i];\n"
"    if (ch === ',') {\n"
"      if (!steps.length) throw new SyntaxError(\"'\" + sel + \"' is not a valid selector\");\n"
"      alts.push(steps); steps = []; comb = null; i++; continue;\n"
"    }\n"
"    if (ch === '>' || ch === '+' || ch === '~') { comb = ch; i++; continue; }\n"
"    var r = parseCompound(s, i);\n"
"    steps.push({ c: r.c, comb: steps.length ? (comb || ' ') : null });\n"
"    comb = null;\n"
"    i = r.i;\n"
"  }\n"
"  if (steps.length) alts.push(steps);\n"
"  if (!alts.length) throw new SyntaxError(\"'\" + sel + \"' is not a valid selector\");\n"
"  return alts;\n"
"}\n"

/* ---- matching ---- */
"function tagOf(el) { var t = el && el.tagName; return t ? String(t).toLowerCase() : ''; }\n"
"function classesOf(el) {\n"
"  var c = el && el.className;\n"
"  if (typeof c !== 'string') c = (el && el.getAttribute) ? (el.getAttribute('class') || '') : '';\n"
"  return c ? c.split(/\\s+/) : [];\n"
"}\n"
"function matchCompound(el, c) {\n"
"  if (!el || el.nodeType !== 1) return false;\n"
"  if (c.never) return false;\n"
"  if (c.tag && c.tag !== '*' && tagOf(el) !== c.tag) return false;\n"
"  if (c.id !== null && String(el.id || '') !== c.id) return false;\n"
"  if (c.cls.length) {\n"
"    var have = classesOf(el);\n"
"    for (var i = 0; i < c.cls.length; i++) if (have.indexOf(c.cls[i]) < 0) return false;\n"
"  }\n"
"  for (var j = 0; j < c.attr.length; j++) {\n"
"    var a = c.attr[j];\n"
"    var v = el.getAttribute ? el.getAttribute(a.name) : null;\n"
"    if (v === null || v === undefined) return false;\n"
"    if (!a.op) continue;\n"
"    v = String(v);\n"
"    if (a.op === '=' && v !== a.val) return false;\n"
"    if (a.op === '^=' && v.indexOf(a.val) !== 0) return false;\n"
"    if (a.op === '$=' && (a.val === '' || v.slice(-a.val.length) !== a.val)) return false;\n"
"    if (a.op === '*=' && (a.val === '' || v.indexOf(a.val) < 0)) return false;\n"
"    if (a.op === '~=' && v.split(/\\s+/).indexOf(a.val) < 0) return false;\n"
"    if (a.op === '|=' && v !== a.val && v.indexOf(a.val + '-') !== 0) return false;\n"
"  }\n"
"  for (var k = 0; k < c.not.length; k++) if (matchCompound(el, c.not[k])) return false;\n"
"  return true;\n"
"}\n"
/* Right to left: the rightmost compound is the cheapest filter, and every step
 * left of it only has to look at ancestors or previous siblings. */
"function matchSteps(el, steps, si) {\n"
"  if (si < 0) return true;\n"
"  var step = steps[si];\n"
"  if (!matchCompound(el, step.c)) return false;\n"
"  if (si === 0) return true;\n"
"  var comb = step.comb;\n"
"  if (comb === '>') return matchSteps(el.parentNode, steps, si - 1);\n"
"  if (comb === '+') return matchSteps(el.previousElementSibling, steps, si - 1);\n"
"  if (comb === '~') {\n"
"    for (var p = el.previousElementSibling; p; p = p.previousElementSibling)\n"
"      if (matchSteps(p, steps, si - 1)) return true;\n"
"    return false;\n"
"  }\n"
"  for (var a = el.parentNode; a && a.nodeType === 1; a = a.parentNode)\n"
"    if (matchSteps(a, steps, si - 1)) return true;\n"
"  return false;\n"
"}\n"
"function matchesSel(el, alts) {\n"
"  for (var i = 0; i < alts.length; i++) {\n"
"    var st = alts[i];\n"
"    if (matchSteps(el, st, st.length - 1)) return true;\n"
"  }\n"
"  return false;\n"
"}\n"

/* ---- the walk ----
 * Iterative, with an explicit stack: a recursive walk over a real page's DOM
 * (wikipedia's is 5604 elements deep in places) is a stack-overflow RangeError
 * waiting for the wrong page, and this runtime's stack guard is 2 MiB. */
"function descendants(rootEl, fn) {\n"
"  var stack = [], i;\n"
"  var kids = rootEl.children;\n"
"  if (!kids) return;\n"
"  for (i = kids.length - 1; i >= 0; i--) stack.push(kids[i]);\n"
"  while (stack.length) {\n"
"    var el = stack.pop();\n"
"    fn(el);\n"
"    var ch = el.children;\n"
"    if (ch) for (i = ch.length - 1; i >= 0; i--) stack.push(ch[i]);\n"
"  }\n"
"}\n"
/* Array-like enough for everything real code does with a NodeList: index,
 * length, item(), forEach, for-of, and Array.from / spread. It IS an Array,
 * with the two collection methods added, which is the honest way to say "this
 * is a snapshot". */
"function list(arr) {\n"
"  arr.item = function (i) { return this[i] === undefined ? null : this[i]; };\n"
"  arr.namedItem = function (n) {\n"
"    for (var i = 0; i < this.length; i++)\n"
"      if (this[i].id === n || (this[i].getAttribute && this[i].getAttribute('name') === n)) return this[i];\n"
"    return null;\n"
"  };\n"
"  return arr;\n"
"}\n"

"function rootOf(scope) {\n"
"  if (scope === doc) return doc.documentElement || doc.body;\n"
"  return scope;\n"
"}\n"
"function qsa(scope, sel) {\n"
"  var alts = parseSelector(sel), out = [], r = rootOf(scope);\n"
"  if (!r) return list(out);\n"
   /* The root itself is a candidate for document.querySelectorAll('html') but
      never for element.querySelectorAll -- a selector matches descendants of
      the scope, not the scope. */
"  if (scope === doc && matchesSel(r, alts)) out.push(r);\n"
"  descendants(r, function (el) { if (matchesSel(el, alts)) out.push(el); });\n"
"  return list(out);\n"
"}\n"
"function qs1(scope, sel) {\n"
"  var alts = parseSelector(sel), r = rootOf(scope), found = null;\n"
"  if (!r) return null;\n"
"  if (scope === doc && matchesSel(r, alts)) return r;\n"
"  descendants(r, function (el) { if (!found && matchesSel(el, alts)) found = el; });\n"
"  return found;\n"
"}\n"
"function byTag(scope, name) {\n"
"  var want = String(name).toLowerCase(), all = want === '*', out = [], r = rootOf(scope);\n"
"  if (!r) return list(out);\n"
"  if (scope === doc && (all || tagOf(r) === want)) out.push(r);\n"
"  descendants(r, function (el) { if (all || tagOf(el) === want) out.push(el); });\n"
"  return list(out);\n"
"}\n"
"function byClass(scope, names) {\n"
"  var want = String(names).split(/\\s+/).filter(function (s) { return s; });\n"
"  var out = [], r = rootOf(scope);\n"
"  if (!r || !want.length) return list(out);\n"
"  var ok = function (el) {\n"
"    var have = classesOf(el);\n"
"    for (var i = 0; i < want.length; i++) if (have.indexOf(want[i]) < 0) return false;\n"
"    return true;\n"
"  };\n"
"  if (scope === doc && ok(r)) out.push(r);\n"
"  descendants(r, function (el) { if (ok(el)) out.push(el); });\n"
"  return list(out);\n"
"}\n"

"function def(o, k, v) { if (o && !(k in o)) { try { o[k] = v; } catch (e) {} } }\n"

"def(doc, 'querySelectorAll', function (s) { return qsa(doc, s); });\n"
"def(doc, 'querySelector', function (s) { return qs1(doc, s); });\n"
"def(doc, 'getElementsByTagName', function (n) { return byTag(doc, n); });\n"
"def(doc, 'getElementsByClassName', function (n) { return byClass(doc, n); });\n"
"def(doc, 'getElementsByName', function (n) {\n"
"  var out = [], r = rootOf(doc);\n"
"  if (r) descendants(r, function (el) {\n"
"    if (el.getAttribute && el.getAttribute('name') === String(n)) out.push(el);\n"
"  });\n"
"  return list(out);\n"
"});\n"
/* Convenience collections pages read directly. Snapshots, like the rest. */
"if (!('scripts' in doc)) Object.defineProperty(doc, 'scripts', { configurable: true,\n"
"  get: function () { return byTag(doc, 'script'); } });\n"
"if (!('images' in doc)) Object.defineProperty(doc, 'images', { configurable: true,\n"
"  get: function () { return byTag(doc, 'img'); } });\n"
"if (!('forms' in doc)) Object.defineProperty(doc, 'forms', { configurable: true,\n"
"  get: function () { return byTag(doc, 'form'); } });\n"
"if (!('links' in doc)) Object.defineProperty(doc, 'links', { configurable: true,\n"
"  get: function () {\n"
"    var out = [];\n"
"    byTag(doc, 'a').forEach(function (a) { if (a.getAttribute('href') !== null) out.push(a); });\n"
"    byTag(doc, 'area').forEach(function (a) { if (a.getAttribute('href') !== null) out.push(a); });\n"
"    return list(out);\n"
"  } });\n"

"def(EP, 'querySelectorAll', function (s) { return qsa(this, s); });\n"
"def(EP, 'querySelector', function (s) { return qs1(this, s); });\n"
"def(EP, 'getElementsByTagName', function (n) { return byTag(this, n); });\n"
"def(EP, 'getElementsByClassName', function (n) { return byClass(this, n); });\n"
"def(EP, 'matches', function (s) { return matchesSel(this, parseSelector(s)); });\n"
"def(EP, 'webkitMatchesSelector', EP.matches);\n"
"def(EP, 'msMatchesSelector', EP.matches);\n"
"def(EP, 'closest', function (s) {\n"
"  var alts = parseSelector(s);\n"
"  for (var el = this; el && el.nodeType === 1; el = el.parentNode)\n"
"    if (matchesSel(el, alts)) return el;\n"
"  return null;\n"
"});\n"

/* `x instanceof HTMLElement` is how a lot of code checks an argument. The
 * constructors are façades over the prototype js_dom.c actually uses, so the
 * instanceof answer is the true one even though `new Element()` is not a thing
 * a page may do (and throws if it tries, as in a browser). */
"(function () {\n"
"  var mkClass = function (name, proto) {\n"
"    var C = function () { throw new TypeError('Illegal constructor'); };\n"
"    C.prototype = proto;\n"
"    try { Object.defineProperty(C, 'name', { value: name }); } catch (e) {}\n"
"    return C;\n"
"  };\n"
"  ['Node', 'Element', 'HTMLElement', 'HTMLDivElement', 'CharacterData', 'Text'].forEach(function (n) {\n"
"    if (!(n in G)) { try { G[n] = mkClass(n, EP); } catch (e) {} }\n"
"  });\n"
"  var nl = mkClass('NodeList', Array.prototype);\n"
"  if (!('NodeList' in G)) G.NodeList = nl;\n"
"  if (!('HTMLCollection' in G)) G.HTMLCollection = mkClass('HTMLCollection', Array.prototype);\n"
"})();\n"
"})\n";

void js_select_install(JSContext *ctx)
{
    if (!ctx) return;
    JSValue fn = JS_Eval(ctx, SELECT_PRELUDE, strlen(SELECT_PRELUDE), "<select>",
                         JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(fn)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        printf("[select] prelude failed: %s\n", m ? m : "?");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
        JS_FreeValue(ctx, fn);
        return;
    }
    JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 0, 0);
    if (JS_IsException(r)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        printf("[select] install failed: %s\n", m ? m : "?");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, fn);
}
