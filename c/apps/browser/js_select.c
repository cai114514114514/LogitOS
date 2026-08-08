/* Selector queries and element collections: querySelector/All, matches,
 * closest, getElementsByTagName/NS/ClassName/Name.
 *
 * WHAT CHANGED, AND WHY IT IS NOT "ONLY IF ABSENT" ANY MORE
 * This file used to install every method with `def()` -- first definition
 * wins -- so that js_dom.c could take the job back one method at a time. It
 * had already taken ONE: `document.querySelector`. That C implementation is
 * three lines long and understands exactly three selectors, "#id", ".class"
 * and "tag" (js_dom.c's find_sel/matches). Everything else fell through to a
 * tag-name comparison against the whole selector string and returned null.
 *
 * So `document.querySelector('[data-x]')` was null. `document.querySelector(
 * 'ul > li')` was null. `document.querySelector('div.a')` was null. Not an
 * error -- NULL, which is indistinguishable from "no such element", which is
 * why nobody found it by using the browser. WPT found it in one run:
 * dom/nodes/querySelector-id-nth-child.html, ParentNode-querySelector-case-
 * insensitive.html and every "with querySelector" subtest in the
 * attribute-selectors corpus all fail with "expected Element but got null",
 * while the ELEMENT-side querySelector -- which reached this file's matcher --
 * passed the same selector.
 *
 * Query-side selector matching is this file's job, so it now installs
 * UNCONDITIONALLY over the query methods and `def()` (install-if-absent) is
 * kept only for the things this file is merely filling in. Overriding
 * `document.querySelector` does not disturb js_dom.c: find_sel is still what
 * document.body/head/documentElement resolve through in C, and that path never
 * sees a page's selector.
 *
 * THE GRAMMAR IS NOW SELECTORS LEVEL 4, not the structural subset:
 *   - identifiers with CSS escapes (\26 , \+, \.) -- 55 of the 68 subtests in
 *     ParentNode-querySelector-escapes.html were one missing backslash rule
 *   - attribute selectors with the `i` and `s` case-sensitivity flags, the
 *     HTML case-insensitive attribute list, and namespace prefixes
 *   - :is() :where() :not() :has(), all taking a full selector LIST
 *   - :nth-child(An+B of S) and the whole structural family
 *   - an unknown pseudo-class is a SYNTAX ERROR, not a no-match. That is the
 *     spec, and it is also the honest answer: silently matching nothing is how
 *     a selector bug hides for a month.
 *
 * CASE SENSITIVITY IS THE POINT OF THIS FILE'S HALF OF THE WORK, so it is
 * stated once, here, rather than being spread over the comparisons:
 *
 *   element names   ASCII case-insensitive for an HTML element in an HTML
 *                   document; case-SENSITIVE for a foreign (SVG/MathML) one.
 *                   `querySelector('DIV')` finds a div; `querySelector(
 *                   'clippath')` does NOT find an SVG <clipPath>.
 *   attribute names same rule.
 *   attribute VALUES case-sensitive by default. `i` forces insensitive, `s`
 *                   forces sensitive, and -- the rule that is always missed --
 *                   an HTML element in an HTML document matches a fixed list
 *                   of 44 legacy attributes (align, lang, type, rel...)
 *                   case-INSENSITIVELY with no flag at all.
 *   class and id    case-sensitive in standards mode, case-INSENSITIVE in
 *                   quirks mode. Quirks is read from the document
 *                   (dom_doc_quirks) through a native hook below, not guessed.
 *
 * KNOWN LIMITS, stated rather than hidden. Our DOM stores no namespaced
 * ATTRIBUTES and every element is in one of three namespaces, so `[x|attr]`
 * with a declared prefix cannot match -- which in querySelector is moot,
 * because a prefix that is not `*` or empty has no declaration to resolve
 * against and is therefore a syntax error, and that is what happens. `|attr`
 * and `|type` (the explicitly-no-namespace forms) are treated as the ordinary
 * form. State-dependent pseudo-classes with no state behind them (:hover,
 * :active, :visited) parse and match nothing, deliberately: :visited matching
 * would be a privacy leak and :hover matching every link would be worse than
 * matching none.
 */
#include "quickjs.h"
#include "js_platform.h"
#include "js_tokenlist.h"
#include "js_dom.h"
#include "dom.h"
#include <string.h>

int printf(const char *, ...);

/* Quirks mode, from the document that actually parsed. It is not reachable
 * from JS -- nothing publishes document.compatMode -- and it decides whether
 * `.Foo` matches class="foo", so guessing it is not an option. The prelude
 * publishes compatMode from this, which is the property pages read anyway. */
static JSValue sel_quirks(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t; (void)argc; (void)argv;
    struct node *r = js_dom_root();
    int q = (r && r->doc) ? dom_doc_quirks(r->doc) : 0;
    return JS_NewBool(ctx, q == QM_QUIRKS);
}

static const char *SELECT_PRELUDE =
"(function (nativeQuirks) {\n"
"'use strict';\n"
"var G = globalThis;\n"
"var doc = G.document;\n"
"if (!doc || typeof doc.createElement !== 'function') return;\n"
/* THE ELEMENT PROTOTYPE IS `Element.prototype`, and this file used to take
 * `Object.getPrototypeOf(document.createElement('div'))` instead. When that
 * was written js_dom.c had ONE shared element prototype and the two were the
 * same object; since the interface hierarchy landed (7fc2bec) they are not --
 * a div's immediate prototype is HTMLDivElement.prototype, and every method
 * installed there belongs to <div> alone.
 *
 * That is invisible in ordinary use and invisible in most of the corpus,
 * because a test that wants an element reaches for a div. It is not invisible
 * in `svg.querySelector(...)`, `input.matches(...)` or `td.closest(...)`,
 * which is most of what these methods are for. iface_install publishes
 * `Element` as a global, so ask by name; the walk is the fallback for a build
 * that has not got there. */
"var probe = doc.createElement('div');\n"
"var EP = null;\n"
"if (typeof G.Element === 'function' && G.Element.prototype) EP = G.Element.prototype;\n"
"if (!EP && probe) {\n"
"  for (var pp = Object.getPrototypeOf(probe); pp && pp !== Object.prototype;\n"
"       pp = Object.getPrototypeOf(pp))\n"
"    if (Object.getOwnPropertyDescriptor(pp, 'getAttribute')) { EP = pp; break; }\n"
"  if (!EP) EP = Object.getPrototypeOf(probe);\n"
"}\n"
"if (!EP) return;\n"

"var HTMLNS = 'http://www.w3.org/1999/xhtml';\n"
"var QUIRKS = false;\n"
"try { QUIRKS = !!nativeQuirks(); } catch (e) {}\n"
"if (!('compatMode' in doc)) {\n"
"  try { Object.defineProperty(doc, 'compatMode', { configurable: true,\n"
"    get: function () { return QUIRKS ? 'BackCompat' : 'CSS1Compat'; } }); } catch (e) {}\n"
"}\n"

/* An invalid selector is a DOMException named SyntaxError, not a plain
 * SyntaxError: `assert_throws_dom(SYNTAX_ERR, ...)` reads `.code`, and a page's
 * `catch (e) { e.name === 'SyntaxError' }` reads the name. js_platform.c
 * publishes the constructor after this file installs and long before any of
 * these functions is called, so the lookup is deliberately late. */
"function synErr(msg) {\n"
"  var DE = G.DOMException, e = null;\n"
"  if (typeof DE === 'function') { try { e = new DE(msg, 'SyntaxError'); } catch (q) { e = null; } }\n"
"  if (!e) e = new SyntaxError(msg);\n"
"  throw e;\n"
"}\n"

"function lower(s) { return String(s).replace(/[A-Z]/g, function (c) {\n"
"  return String.fromCharCode(c.charCodeAt(0) + 32); }); }\n"
/* ASCII only. String.prototype.toLowerCase folds U+0130 and the Turkish
 * dotless i, and the corpus tests exactly those: [foo='i' i] must NOT match
 * foo=\"\\u0130\". */
"function ciEq(a, b) { return lower(a) === lower(b); }\n"

/* ====================== the selector parser ======================
 * Produces a list of alternatives (the comma list); each alternative is a list
 * of {c: compound, comb: combinator} steps in DOCUMENT order, matched right to
 * left. A compound is a list of simple selectors, all of which must hold. */

"var NAMECH = /[-\\w\\u0080-\\uffff]/;\n"
"var WSCH = /[\\t\\n\\f\\r ]/;\n"
"var HEX = /[0-9a-fA-F]/;\n"

/* A CSS escape: a backslash then either up to six hex digits (with one
 * optional trailing whitespace that is part of the ESCAPE, not a separator) or
 * any single character that is not a newline. This is the whole reason
 * `querySelector('#\\36 8')` and `.a\\.b` used to fail: the old ident scanner
 * had no notion of a backslash and stopped dead at one. */
"function readEscape(s, i) {\n"
"  var n = s.length, j = i + 1;\n"
"  if (j >= n) synErr('trailing backslash in selector');\n"
"  var c = s[j];\n"
"  if (HEX.test(c)) {\n"
"    var hex = '';\n"
"    while (j < n && hex.length < 6 && HEX.test(s[j])) { hex += s[j]; j++; }\n"
"    if (j < n && WSCH.test(s[j])) { if (s[j] === '\\r' && s[j + 1] === '\\n') j++; j++; }\n"
"    var cp = parseInt(hex, 16);\n"
       /* NULL, a lone surrogate and anything past the last plane all become
          U+FFFD -- the CSS tokenizer's rule, and the corpus checks it. */
"    if (!cp || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) cp = 0xfffd;\n"
"    return { ch: String.fromCodePoint ? String.fromCodePoint(cp)\n"
"                 : String.fromCharCode(cp), i: j };\n"
"  }\n"
"  if (c === '\\n' || c === '\\r' || c === '\\f') synErr('newline after backslash');\n"
"  return { ch: c, i: j + 1 };\n"
"}\n"

"function readIdent(s, i) {\n"
"  var n = s.length, out = '', j = i;\n"
   /* An ident may not start with a digit, nor with '-' then a digit; those are
      numbers, and `.1` / `#-1` are invalid selectors rather than odd names. */
"  if (j < n && /[0-9]/.test(s[j])) return null;\n"
"  if (s[j] === '-' && j + 1 < n && /[0-9]/.test(s[j + 1])) return null;\n"
"  while (j < n) {\n"
"    var c = s[j];\n"
"    if (c === '\\\\') { var e = readEscape(s, j); out += e.ch; j = e.i; continue; }\n"
"    if (NAMECH.test(c)) { out += c; j++; continue; }\n"
"    break;\n"
"  }\n"
"  return out === '' ? null : { v: out, i: j };\n"
"}\n"

"function readString(s, i) {\n"
"  var q = s[i], n = s.length, j = i + 1, out = '';\n"
"  while (j < n) {\n"
"    var c = s[j];\n"
"    if (c === q) return { v: out, i: j + 1 };\n"
"    if (c === '\\n' || c === '\\r' || c === '\\f') synErr('newline in string');\n"
"    if (c === '\\\\') {\n"
"      if (s[j + 1] === '\\n') { j += 2; continue; }\n"
"      var e = readEscape(s, j); out += e.ch; j = e.i; continue;\n"
"    }\n"
"    out += c; j++;\n"
"  }\n"
"  synErr('unterminated string in selector');\n"
"}\n"

"function skipWS(s, i) { while (i < s.length && WSCH.test(s[i])) i++; return i; }\n"

/* The 44 attributes whose VALUES an HTML element in an HTML document matches
 * ASCII-case-insensitively with no flag at all. From HTML's "case-sensitivity
 * of selectors"; the list is closed, which is why it is spelled out rather
 * than approximated by a rule. */
"var HTML_CI_ATTR = {};\n"
"('accept accept-charset align alink axis bgcolor charset checked clear codetype color '\n"
" + 'compact declare defer dir direction disabled enctype face frame hreflang http-equiv '\n"
" + 'lang language link media method multiple nohref noresize noshade nowrap readonly rel '\n"
" + 'rev rules scope scrolling selected shape target text type valign valuetype vlink')\n"
"  .split(' ').forEach(function (a) { HTML_CI_ATTR[a] = 1; });\n"

/* Pseudo-classes that exist but describe state this engine does not model.
 * They PARSE (so the selector is valid and does not throw) and match nothing.
 * Separating them from the unknown-name case is the whole point: an unknown
 * name is a syntax error, and conflating the two would make every typo silent. */
"var INERT_PSEUDO = {};\n"
"('hover active visited focus-visible focus-within target-within local-link '\n"
" + 'current past future playing paused seeking buffering stalled muted volume-locked '\n"
" + 'user-invalid user-valid fullscreen modal picture-in-picture popover-open '\n"
" + 'autofill open host host-context defined')\n"
"  .split(' ').forEach(function (p) { INERT_PSEUDO[p] = 1; });\n"

/* Functional pseudo-classes taking a selector LIST. :not/:is/:where/:has are
 * matched here; the rest is a compile error rather than a silent pass. */
"var LIST_PSEUDO = { 'not': 1, 'is': 1, 'where': 1, 'has': 1,\n"
"                    'matches': 1, 'any': 1, '-webkit-any': 1, '-moz-any': 1 };\n"
"var NTH_PSEUDO = { 'nth-child': 1, 'nth-last-child': 1,\n"
"                   'nth-of-type': 1, 'nth-last-of-type': 1 };\n"
/* Plain pseudo-classes we evaluate. */
"var PLAIN_PSEUDO = {};\n"
"('root empty first-child last-child only-child first-of-type last-of-type only-of-type '\n"
" + 'scope link any-link enabled disabled checked required optional read-only read-write '\n"
" + 'indeterminate default placeholder-shown target focus in-range out-of-range valid invalid')\n"
"  .split(' ').forEach(function (p) { PLAIN_PSEUDO[p] = 1; });\n"
/* Pseudo-ELEMENTS. Valid in a selector, but an element is never a pseudo
 * element, so the compound can never match. Both spellings, because the four
 * originals are still legal with one colon. */
"var PSEUDO_ELEM = {};\n"
"('before after first-line first-letter selection backdrop placeholder marker '\n"
" + 'file-selector-button grammar-error spelling-error target-text cue part slotted')\n"
"  .split(' ').forEach(function (p) { PSEUDO_ELEM[p] = 1; });\n"

/* An An+B expression: 'odd', 'even', '3', '-n+2', '2n + 1'. Returns {a,b}. */
"function parseAnB(src) {\n"
"  var s = String(src).replace(/[\\t\\n\\f\\r ]+/g, '').toLowerCase();\n"
"  if (s === 'odd') return { a: 2, b: 1 };\n"
"  if (s === 'even') return { a: 2, b: 0 };\n"
"  var m = /^([-+]?[0-9]+)$/.exec(s);\n"
"  if (m) return { a: 0, b: parseInt(m[1], 10) };\n"
"  m = /^([-+]?[0-9]*)n([-+][0-9]+)?$/.exec(s);\n"
"  if (!m) return null;\n"
"  var a = m[1];\n"
"  a = (a === '' || a === '+') ? 1 : (a === '-' ? -1 : parseInt(a, 10));\n"
"  return { a: a, b: m[2] ? parseInt(m[2], 10) : 0 };\n"
"}\n"

/* Balanced-paren span starting at the '('; returns the index just past ')'. */
"function matchParen(s, i) {\n"
"  var d = 0, n = s.length;\n"
"  for (var j = i; j < n; j++) {\n"
"    var c = s[j];\n"
"    if (c === '\\'' || c === '\"') { var r = readString(s, j); j = r.i - 1; continue; }\n"
"    if (c === '\\\\') { var e = readEscape(s, j); j = e.i - 1; continue; }\n"
"    if (c === '(') d++;\n"
"    else if (c === ')') { d--; if (!d) return j + 1; }\n"
"  }\n"
"  synErr('unbalanced ( in selector');\n"
"}\n"

/* A namespace-qualified name: `ns|x`, `*|x`, `|x` or plain `x`.
 * In querySelector there are no @namespace declarations, so any prefix other
 * than `*` or empty has nothing to resolve against and is a syntax error --
 * which is the spec's answer and not a limitation of this DOM. */
"function readQName(s, i, allowStar) {\n"
"  var n = s.length, ns = null, name = null, j = i, r;\n"
"  var first = null;\n"
"  if (s[j] === '*') { first = '*'; j++; }\n"
"  else { r = readIdent(s, j); if (r) { first = r.v; j = r.i; } }\n"
"  if (s[j] === '|' && s[j + 1] !== '=') {\n"
"    ns = first === null ? '' : first;\n"
"    j++;\n"
"    if (s[j] === '*') { if (!allowStar) synErr('* is not a valid attribute name'); name = '*'; j++; }\n"
"    else { r = readIdent(s, j); if (!r) synErr('expected a name after |'); name = r.v; j = r.i; }\n"
"    if (ns !== '' && ns !== '*') synErr(\"undeclared namespace prefix '\" + ns + \"'\");\n"
"  } else {\n"
"    if (first === null) return null;\n"
"    name = first;\n"
"  }\n"
"  if (name === '*' && !allowStar) synErr('* is not a valid attribute name');\n"
"  return { ns: ns, name: name, i: j };\n"
"}\n"

"function parseCompound(s, i, depth) {\n"
"  var simples = [], n = s.length, start = i, r;\n"
"  while (i < n) {\n"
"    var ch = s[i];\n"
"    if (ch === '*' || ch === '|' || NAMECH.test(ch) || ch === '\\\\') {\n"
       /* A type selector is only legal FIRST in a compound. `div p` is two
          compounds; `divp` is one name; `div*` is invalid. */
"      if (simples.length) break;\n"
"      r = readQName(s, i, true);\n"
"      if (!r) break;\n"
"      if (r.name !== '*' || r.ns !== null) simples.push({ t: 'type', ns: r.ns, name: r.name });\n"
"      else simples.push({ t: 'univ' });\n"
"      i = r.i; continue;\n"
"    }\n"
"    if (ch === '#') {\n"
"      r = readIdent(s, i + 1);\n"
"      if (!r) synErr(\"'\" + s + \"' is not a valid selector\");\n"
"      simples.push({ t: 'id', v: r.v }); i = r.i; continue;\n"
"    }\n"
"    if (ch === '.') {\n"
"      r = readIdent(s, i + 1);\n"
"      if (!r) synErr(\"'\" + s + \"' is not a valid selector\");\n"
"      simples.push({ t: 'class', v: r.v }); i = r.i; continue;\n"
"    }\n"
"    if (ch === '[') { i = parseAttr(s, i, simples); continue; }\n"
"    if (ch === ':') { i = parsePseudo(s, i, simples, depth); continue; }\n"
"    break;\n"
"  }\n"
"  if (i === start) synErr(\"'\" + s + \"' is not a valid selector\");\n"
"  return { c: simples, i: i };\n"
"}\n"

"function parseAttr(s, i, out) {\n"
"  var n = s.length, j = skipWS(s, i + 1);\n"
"  var q = readQName(s, j, false);\n"
"  if (!q) synErr('expected an attribute name in [ ]');\n"
"  j = skipWS(s, q.i);\n"
"  if (s[j] === ']') { out.push({ t: 'attr', ns: q.ns, name: q.name, op: null }); return j + 1; }\n"
"  var op = null;\n"
"  if ('~^$*|'.indexOf(s[j]) >= 0 && s[j + 1] === '=') { op = s[j] + '='; j += 2; }\n"
"  else if (s[j] === '=') { op = '='; j++; }\n"
"  else synErr('bad attribute operator in selector');\n"
"  j = skipWS(s, j);\n"
"  var val, r;\n"
"  if (s[j] === '\"' || s[j] === '\\'') { r = readString(s, j); val = r.v; j = r.i; }\n"
"  else { r = readIdent(s, j); if (!r) synErr('expected an attribute value'); val = r.v; j = r.i; }\n"
"  j = skipWS(s, j);\n"
   /* The case-sensitivity flag. `i` and `s` only; anything else is invalid,
      and that is checked rather than ignored. */
"  var flag = null;\n"
"  if (j < n && s[j] !== ']') {\n"
"    var f = readIdent(s, j);\n"
"    if (!f || (f.v !== 'i' && f.v !== 'I' && f.v !== 's' && f.v !== 'S'))\n"
"      synErr('bad attribute selector flag');\n"
"    flag = lower(f.v); j = skipWS(s, f.i);\n"
"  }\n"
"  if (s[j] !== ']') synErr('unterminated attribute selector');\n"
"  out.push({ t: 'attr', ns: q.ns, name: q.name, op: op, val: val, flag: flag });\n"
"  return j + 1;\n"
"}\n"

"function parsePseudo(s, i, out, depth) {\n"
"  var n = s.length, dbl = s[i + 1] === ':';\n"
"  var j = i + (dbl ? 2 : 1);\n"
"  var r = readIdent(s, j);\n"
"  if (!r) synErr('expected a pseudo-class name');\n"
"  var name = lower(r.v);\n"
"  j = r.i;\n"
"  var arg = null;\n"
"  if (s[j] === '(') { var e = matchParen(s, j); arg = s.slice(j + 1, e - 1); j = e; }\n"

"  if (dbl || (arg === null && PSEUDO_ELEM[name])) {\n"
"    if (!PSEUDO_ELEM[name] && !INERT_PSEUDO[name]) synErr(\"unknown pseudo-element '\" + name + \"'\");\n"
"    out.push({ t: 'never' }); return j;\n"
"  }\n"
"  if (LIST_PSEUDO[name]) {\n"
"    if (arg === null) synErr(':' + name + ' requires an argument');\n"
"    if (depth > 8) synErr('selector nested too deep');\n"
"    var kind = (name === 'not') ? 'not' : (name === 'has' ? 'has' : 'is');\n"
"    out.push({ t: kind, sels: parseSelector(arg, depth + 1, kind === 'has') });\n"
"    return j;\n"
"  }\n"
"  if (NTH_PSEUDO[name]) {\n"
"    if (arg === null) synErr(':' + name + ' requires an argument');\n"
"    var of = null, expr = arg;\n"
"    var k = arg.search(/\\bof\\b/i);\n"
"    if (k >= 0 && (name === 'nth-child' || name === 'nth-last-child')) {\n"
"      expr = arg.slice(0, k); of = parseSelector(arg.slice(k + 2), depth + 1, false);\n"
"    }\n"
"    var ab = parseAnB(expr);\n"
"    if (!ab) synErr(\"'\" + arg + \"' is not a valid An+B expression\");\n"
"    out.push({ t: 'nth', name: name, a: ab.a, b: ab.b, of: of });\n"
"    return j;\n"
"  }\n"
"  if (name === 'lang' || name === 'dir') {\n"
"    if (arg === null) synErr(':' + name + ' requires an argument');\n"
"    out.push({ t: name, v: lower(arg.replace(/[\\t\\n\\f\\r ]/g, '').replace(/^[\"']|[\"']$/g, '')) });\n"
"    return j;\n"
"  }\n"
"  if (PLAIN_PSEUDO[name]) {\n"
"    if (arg !== null) synErr(':' + name + ' takes no argument');\n"
"    out.push({ t: 'pc', v: name }); return j;\n"
"  }\n"
"  if (INERT_PSEUDO[name]) { out.push({ t: 'never' }); return j; }\n"
"  synErr(\"unknown pseudo-class ':\" + name + \"'\");\n"
"}\n"

/* A selector list. `relative` allows a leading combinator, which only :has()
 * does (`:has(> p)`). */
"function parseSelector(sel, depth, relative) {\n"
"  depth = depth || 0;\n"
"  var alts = [], s = String(sel), i = 0, n = s.length;\n"
"  var steps = [], comb = null, sawWS = false;\n"
"  while (i < n) {\n"
"    var w = skipWS(s, i);\n"
"    if (w !== i) { sawWS = true; i = w; }\n"
"    if (i >= n) break;\n"
"    var ch = s[i];\n"
"    if (ch === ',') {\n"
"      if (!steps.length) synErr(\"'\" + sel + \"' is not a valid selector\");\n"
"      alts.push(steps); steps = []; comb = null; sawWS = false; i++; continue;\n"
"    }\n"
"    if (ch === '>' || ch === '+' || ch === '~') {\n"
"      if (comb !== null) synErr('two combinators in a row');\n"
"      if (!steps.length && !relative) synErr(\"'\" + sel + \"' is not a valid selector\");\n"
"      comb = ch; sawWS = false; i++; continue;\n"
"    }\n"
"    var r = parseCompound(s, i, depth);\n"
"    var c = comb;\n"
"    if (c === null) c = steps.length ? (sawWS ? ' ' : null) : null;\n"
"    if (steps.length && c === null) synErr('two compounds with no combinator');\n"
"    if (!steps.length && relative && comb === null && !alts.length) c = null;\n"
"    steps.push({ c: r.c, comb: steps.length ? c : (relative ? (comb || ' ') : null) });\n"
"    comb = null; sawWS = false;\n"
"    i = r.i;\n"
"  }\n"
"  if (comb !== null) synErr('trailing combinator');\n"
"  if (steps.length) alts.push(steps);\n"
"  if (!alts.length) synErr(\"'\" + sel + \"' is not a valid selector\");\n"
"  return alts;\n"
"}\n"

/* Compiled selectors are cached: a page that calls querySelectorAll in a loop
 * re-parses the same string every time otherwise, and the parser is the
 * expensive half now that it is a real one. */
"var pcache = Object.create(null), pcount = 0;\n"
"function compile(sel) {\n"
"  if (sel === undefined) synErr('querySelector requires a selector');\n"
"  var k = '#' + String(sel);\n"
"  var hit = pcache[k];\n"
"  if (hit) { if (hit.err) throw hit.err; return hit.v; }\n"
"  var v;\n"
"  try { v = parseSelector(String(sel), 0, false); }\n"
"  catch (e) { if (pcount < 512) { pcache[k] = { err: e }; pcount++; } throw e; }\n"
"  if (pcount < 512) { pcache[k] = { v: v }; pcount++; }\n"
"  return v;\n"
"}\n"

/* ====================== matching ====================== */

"function isHTML(el) { return el.namespaceURI === HTMLNS || el.namespaceURI === undefined; }\n"
/* localName, derived: js_dom.c uppercases tagName for an HTML element and
 * leaves a foreign one verbatim, so this inverts exactly that. */
"function localOf(el) {\n"
"  var t = el.tagName;\n"
"  if (t === undefined || t === null) return '';\n"
"  t = String(t);\n"
"  return isHTML(el) ? lower(t) : t;\n"
"}\n"
"function classesOf(el) {\n"
"  var c = el.getAttribute ? el.getAttribute('class') : null;\n"
"  if (c === null || c === undefined) return [];\n"
"  c = String(c);\n"
"  return c ? c.split(/[\\t\\n\\f\\r ]+/).filter(function (x) { return x; }) : [];\n"
"}\n"

/* An attribute's value, honouring the fact that attribute NAMES are ASCII
 * case-insensitive only for HTML elements. dom.c's lookup is case-insensitive
 * unconditionally, so a foreign element needs the verbatim name list -- and
 * only pays for it when the loose lookup already said yes, which is the only
 * case where the answer can differ. */
"function attrOf(el, name, html) {\n"
"  if (!el.getAttribute) return null;\n"
"  var v = el.getAttribute(name);\n"
"  if (v === null || v === undefined) return null;\n"
"  if (html) return v;\n"
"  if (typeof el.getAttributeNames === 'function') {\n"
"    var ns;\n"
"    try { ns = el.getAttributeNames(); } catch (e) { return v; }\n"
"    for (var i = 0; i < ns.length; i++) if (ns[i] === name) return v;\n"
"    return null;\n"
"  }\n"
"  return v;\n"
"}\n"

"function attrMatch(el, a) {\n"
"  var html = isHTML(el);\n"
"  var name = html ? lower(a.name) : a.name;\n"
"  var v = a.name === '*' ? null : attrOf(el, name, html);\n"
"  if (a.name === '*') {\n"
     /* [*|attr] with no name is not a thing; [*|foo] carries name='foo' and
        ns='*', which for a DOM with no namespaced attributes is the plain
        lookup. This branch only fires for the degenerate [*|*]. */
"    return false;\n"
"  }\n"
"  if (v === null) return false;\n"
"  if (!a.op) return true;\n"
"  v = String(v);\n"
"  var want = a.val;\n"
   /* The three-way rule this whole file exists for. */
"  var ci = a.flag === 'i' ? true\n"
"         : a.flag === 's' ? false\n"
"         : (html && HTML_CI_ATTR[name] === 1);\n"
"  if (ci) { v = lower(v); want = lower(want); }\n"
"  switch (a.op) {\n"
"    case '=':  return v === want;\n"
"    case '^=': return want !== '' && v.slice(0, want.length) === want;\n"
"    case '$=': return want !== '' && want.length <= v.length &&\n"
"                      v.slice(v.length - want.length) === want;\n"
"    case '*=': return want !== '' && v.indexOf(want) >= 0;\n"
"    case '~=': return want !== '' && !WSCH.test(want) &&\n"
"                      v.split(/[\\t\\n\\f\\r ]+/).indexOf(want) >= 0;\n"
"    case '|=': return v === want || v.slice(0, want.length + 1) === want + '-';\n"
"  }\n"
"  return false;\n"
"}\n"

"function elemChildren(p) {\n"
"  var out = [], k = p && p.children;\n"
"  if (!k) return out;\n"
"  for (var i = 0; i < k.length; i++) out.push(k[i]);\n"
"  return out;\n"
"}\n"
"function sameType(a, b) {\n"
"  return localOf(a) === localOf(b) && a.namespaceURI === b.namespaceURI;\n"
"}\n"
"function nthMatch(el, s, ctx) {\n"
"  var p = el.parentNode;\n"
"  if (!p || p.nodeType !== 1) {\n"
     /* An element with no element parent is still the first (and only) of its
        kind for :first-child purposes only when it has a parent at all; a
        detached root matches nothing structural, which is what browsers do. */
"    if (!p) return false;\n"
"  }\n"
"  var sibs = elemChildren(p);\n"
"  var pool = sibs;\n"
"  if (s.name === 'nth-of-type' || s.name === 'nth-last-of-type')\n"
"    pool = sibs.filter(function (x) { return sameType(x, el); });\n"
"  else if (s.of)\n"
"    pool = sibs.filter(function (x) { return matchAny(x, s.of, ctx); });\n"
"  var idx = pool.indexOf(el);\n"
"  if (idx < 0) return false;\n"
"  var pos = (s.name === 'nth-last-child' || s.name === 'nth-last-of-type')\n"
"          ? pool.length - idx : idx + 1;\n"
"  if (s.a === 0) return pos === s.b;\n"
"  var q = (pos - s.b) / s.a;\n"
"  return q >= 0 && q === Math.floor(q);\n"
"}\n"

"function isEmptyEl(el) {\n"
"  for (var c = el.firstChild; c; c = c.nextSibling) {\n"
"    if (c.nodeType === 1) return false;\n"
"    if (c.nodeType === 3 && String(c.nodeValue || '').length) return false;\n"
"  }\n"
"  return true;\n"
"}\n"
"function typeSibs(el, dir) {\n"
"  var p = el.parentNode;\n"
"  if (!p) return null;\n"
"  return elemChildren(p).filter(function (x) { return sameType(x, el); });\n"
"}\n"

"function plainPseudo(el, name, ctx) {\n"
"  var p, sibs;\n"
"  switch (name) {\n"
"    case 'root': return el === (doc.documentElement || null);\n"
"    case 'empty': return isEmptyEl(el);\n"
"    case 'scope': return ctx.scope ? el === ctx.scope : el === doc.documentElement;\n"
"    case 'first-child':\n"
"      p = el.parentNode; return !!p && elemChildren(p)[0] === el;\n"
"    case 'last-child':\n"
"      p = el.parentNode; if (!p) return false;\n"
"      sibs = elemChildren(p); return sibs[sibs.length - 1] === el;\n"
"    case 'only-child':\n"
"      p = el.parentNode; return !!p && elemChildren(p).length === 1;\n"
"    case 'first-of-type': sibs = typeSibs(el); return !!sibs && sibs[0] === el;\n"
"    case 'last-of-type':\n"
"      sibs = typeSibs(el); return !!sibs && sibs[sibs.length - 1] === el;\n"
"    case 'only-of-type': sibs = typeSibs(el); return !!sibs && sibs.length === 1;\n"
"    case 'link': case 'any-link': {\n"
"      var t = localOf(el);\n"
"      return (t === 'a' || t === 'area' || t === 'link') && attrOf(el, 'href', isHTML(el)) !== null;\n"
"    }\n"
"    case 'target': {\n"
"      var h = String(G.location && G.location.hash || '');\n"
"      if (h.length < 2) return false;\n"
"      return String(el.id || '') === h.slice(1);\n"
"    }\n"
"    case 'focus': return doc.activeElement === el;\n"
"    case 'enabled': return CAN_DISABLE[localOf(el)] === 1 && !isDisabled(el);\n"
"    case 'disabled': return CAN_DISABLE[localOf(el)] === 1 && isDisabled(el);\n"
"    case 'checked': {\n"
"      var t2 = localOf(el);\n"
"      if (t2 === 'option') return !!el.selected || attrOf(el, 'selected', 1) !== null;\n"
"      if (t2 !== 'input') return false;\n"
"      var ty = lower(attrOf(el, 'type', 1) || '');\n"
"      if (ty !== 'checkbox' && ty !== 'radio') return false;\n"
"      return el.checked !== undefined ? !!el.checked : attrOf(el, 'checked', 1) !== null;\n"
"    }\n"
"    case 'indeterminate': return !!el.indeterminate;\n"
"    case 'required': return CAN_REQUIRE[localOf(el)] === 1 && attrOf(el, 'required', 1) !== null;\n"
"    case 'optional': return CAN_REQUIRE[localOf(el)] === 1 && attrOf(el, 'required', 1) === null;\n"
"    case 'read-only': {\n"
"      var t3 = localOf(el);\n"
"      if (t3 === 'textarea') return attrOf(el, 'readonly', 1) !== null || isDisabled(el);\n"
"      if (t3 === 'input') return attrOf(el, 'readonly', 1) !== null || isDisabled(el);\n"
"      return !isContentEditable(el);\n"
"    }\n"
"    case 'read-write': {\n"
"      var t4 = localOf(el);\n"
"      if (t4 === 'textarea' || t4 === 'input')\n"
"        return attrOf(el, 'readonly', 1) === null && !isDisabled(el);\n"
"      return isContentEditable(el);\n"
"    }\n"
"    case 'default': return attrOf(el, 'checked', 1) !== null || attrOf(el, 'selected', 1) !== null;\n"
"    case 'placeholder-shown': {\n"
"      var t5 = localOf(el);\n"
"      if (t5 !== 'input' && t5 !== 'textarea') return false;\n"
"      return attrOf(el, 'placeholder', 1) !== null && !String(el.value || '').length;\n"
"    }\n"
"    case 'valid': case 'in-range': return false;\n"
"    case 'invalid': case 'out-of-range': return false;\n"
"  }\n"
"  return false;\n"
"}\n"
"var CAN_DISABLE = { button: 1, input: 1, select: 1, textarea: 1, optgroup: 1,\n"
"                    option: 1, fieldset: 1 };\n"
"var CAN_REQUIRE = { input: 1, select: 1, textarea: 1 };\n"
"function isDisabled(el) {\n"
"  if (attrOf(el, 'disabled', 1) !== null) return true;\n"
   /* A control inside a disabled <fieldset> is disabled too -- except in its
      first <legend>. Cheap, and it is what the forms corpus asks. */
"  for (var p = el.parentNode; p && p.nodeType === 1; p = p.parentNode)\n"
"    if (localOf(p) === 'fieldset' && attrOf(p, 'disabled', 1) !== null) return true;\n"
"  return false;\n"
"}\n"
"function isContentEditable(el) {\n"
"  for (var p = el; p && p.nodeType === 1; p = p.parentNode) {\n"
"    var v = attrOf(p, 'contenteditable', 1);\n"
"    if (v === null) continue;\n"
"    v = lower(v);\n"
"    if (v === 'false') return false;\n"
"    return true;\n"
"  }\n"
"  return false;\n"
"}\n"

"function matchSimple(el, s, ctx) {\n"
"  switch (s.t) {\n"
"    case 'univ': return true;\n"
"    case 'never': return false;\n"
"    case 'type': {\n"
       /* `|div` (explicit no-namespace) has nothing to match here: every
          element this DOM builds is in one of three real namespaces. */
"      if (s.ns === '') return false;\n"
"      return isHTML(el) ? localOf(el) === lower(s.name) : localOf(el) === s.name;\n"
"    }\n"
"    case 'id': {\n"
"      var id = el.getAttribute ? el.getAttribute('id') : null;\n"
"      if (id === null || id === undefined) return false;\n"
"      return QUIRKS ? ciEq(String(id), s.v) : String(id) === s.v;\n"
"    }\n"
"    case 'class': {\n"
"      var cs = classesOf(el);\n"
"      if (!QUIRKS) return cs.indexOf(s.v) >= 0;\n"
"      for (var i = 0; i < cs.length; i++) if (ciEq(cs[i], s.v)) return true;\n"
"      return false;\n"
"    }\n"
"    case 'attr': return attrMatch(el, s);\n"
"    case 'not': return !matchAny(el, s.sels, ctx);\n"
"    case 'is': return matchAny(el, s.sels, ctx);\n"
"    case 'has': return hasMatch(el, s.sels, ctx);\n"
"    case 'nth': return nthMatch(el, s, ctx);\n"
"    case 'pc': return plainPseudo(el, s.v, ctx);\n"
"    case 'lang': {\n"
"      for (var p = el; p && p.nodeType === 1; p = p.parentNode) {\n"
"        var v = attrOf(p, 'lang', isHTML(p));\n"
"        if (v === null) continue;\n"
"        v = lower(v);\n"
"        return v === s.v || v.slice(0, s.v.length + 1) === s.v + '-';\n"
"      }\n"
"      return false;\n"
"    }\n"
"    case 'dir': return false;\n"
"  }\n"
"  return false;\n"
"}\n"

"function matchCompound(el, c, ctx) {\n"
"  if (!el || el.nodeType !== 1) return false;\n"
"  for (var i = 0; i < c.length; i++) if (!matchSimple(el, c[i], ctx)) return false;\n"
"  return true;\n"
"}\n"
/* Right to left: the rightmost compound is the cheapest filter, and every step
 * left of it only has to look at ancestors or previous siblings. */
"function matchSteps(el, steps, si, ctx) {\n"
"  if (si < 0) return true;\n"
"  var step = steps[si];\n"
"  if (!matchCompound(el, step.c, ctx)) return false;\n"
"  if (si === 0) return true;\n"
"  var comb = step.comb;\n"
"  if (comb === '>') return matchSteps(el.parentNode, steps, si - 1, ctx);\n"
"  if (comb === '+') return matchSteps(el.previousElementSibling, steps, si - 1, ctx);\n"
"  if (comb === '~') {\n"
"    for (var p = el.previousElementSibling; p; p = p.previousElementSibling)\n"
"      if (matchSteps(p, steps, si - 1, ctx)) return true;\n"
"    return false;\n"
"  }\n"
"  for (var a = el.parentNode; a && a.nodeType === 1; a = a.parentNode)\n"
"    if (matchSteps(a, steps, si - 1, ctx)) return true;\n"
"  return false;\n"
"}\n"
"function matchAny(el, alts, ctx) {\n"
"  for (var i = 0; i < alts.length; i++) {\n"
"    var st = alts[i];\n"
"    if (matchSteps(el, st, st.length - 1, ctx)) return true;\n"
"  }\n"
"  return false;\n"
"}\n"

/* :has() is the one that runs FORWARD: every other combinator is answered by
 * walking up or back from the candidate, and this one has to look at what the
 * candidate contains. A relative selector's leftmost step carries the
 * combinator that joins it to the anchor. */
"function hasMatch(anchor, alts, ctx) {\n"
"  for (var i = 0; i < alts.length; i++) {\n"
"    var st = alts[i];\n"
"    var lead = st[0].comb || ' ';\n"
"    var pool = [];\n"
"    if (lead === '>') pool = elemChildren(anchor);\n"
"    else if (lead === '+') { var nx = anchor.nextElementSibling; if (nx) pool = [nx]; }\n"
"    else if (lead === '~') {\n"
"      for (var q = anchor.nextElementSibling; q; q = q.nextElementSibling) pool.push(q);\n"
"    } else descendants(anchor, function (e) { pool.push(e); });\n"
"    var sub = { scope: anchor };\n"
"    for (var k = 0; k < pool.length; k++) {\n"
"      if (st.length === 1) { if (matchCompound(pool[k], st[0].c, sub)) return true; continue; }\n"
"      if (matchStepsBounded(pool[k], st, st.length - 1, sub, anchor)) return true;\n"
"    }\n"
"  }\n"
"  return false;\n"
"}\n"
/* Like matchSteps, but the leftmost step's combinator joins to `anchor`
 * itself, so the walk stops there instead of running off up the document. */
"function matchStepsBounded(el, steps, si, ctx, anchor) {\n"
"  if (si === 0) {\n"
"    if (!matchCompound(el, steps[0].c, ctx)) return false;\n"
"    return true;\n"
"  }\n"
"  var step = steps[si];\n"
"  if (!matchCompound(el, step.c, ctx)) return false;\n"
"  var comb = step.comb;\n"
"  if (comb === '>') return matchStepsBounded(el.parentNode, steps, si - 1, ctx, anchor);\n"
"  if (comb === '+') return matchStepsBounded(el.previousElementSibling, steps, si - 1, ctx, anchor);\n"
"  if (comb === '~') {\n"
"    for (var p = el.previousElementSibling; p; p = p.previousElementSibling)\n"
"      if (matchStepsBounded(p, steps, si - 1, ctx, anchor)) return true;\n"
"    return false;\n"
"  }\n"
"  for (var a = el.parentNode; a && a.nodeType === 1; a = a.parentNode) {\n"
"    if (matchStepsBounded(a, steps, si - 1, ctx, anchor)) return true;\n"
"    if (a === anchor) break;\n"
"  }\n"
"  return false;\n"
"}\n"

/* ---- the walk ----
 * Iterative, with an explicit stack: a recursive walk over a real page's DOM
 * (wikipedia's is 5604 elements deep in places) is a stack-overflow RangeError
 * waiting for the wrong page, and this runtime's stack guard is 2 MiB. */
"function descendants(rootEl, fn) {\n"
"  var stack = [], i;\n"
"  var kids = rootEl && rootEl.children;\n"
"  if (!kids) return;\n"
"  for (i = kids.length - 1; i >= 0; i--) stack.push(kids[i]);\n"
"  while (stack.length) {\n"
"    var el = stack.pop();\n"
"    if (fn(el) === false) return;\n"
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
"  var alts = compile(sel), out = [], r = rootOf(scope);\n"
"  if (!r) return list(out);\n"
"  var ctx = { scope: scope === doc ? null : scope };\n"
   /* The root itself is a candidate for document.querySelectorAll('html') but
      never for element.querySelectorAll -- a selector matches descendants of
      the scope, not the scope. */
"  if (scope === doc && matchAny(r, alts, ctx)) out.push(r);\n"
"  descendants(r, function (el) { if (matchAny(el, alts, ctx)) out.push(el); });\n"
"  return list(out);\n"
"}\n"
"function qs1(scope, sel) {\n"
"  var alts = compile(sel), r = rootOf(scope), found = null;\n"
"  if (!r) return null;\n"
"  var ctx = { scope: scope === doc ? null : scope };\n"
"  if (scope === doc && matchAny(r, alts, ctx)) return r;\n"
"  descendants(r, function (el) {\n"
"    if (matchAny(el, alts, ctx)) { found = el; return false; }\n"
"  });\n"
"  return found;\n"
"}\n"

/* getElementsByTagName, to the DOM's rule rather than to lowercase-everything.
 * In an HTML document an HTML element matches the qualified name ASCII-
 * LOWERCASED and a foreign element matches it VERBATIM -- so
 * getElementsByTagName('clipPath') finds the SVG one and
 * getElementsByTagName('clippath') does not, while getElementsByTagName('DIV')
 * finds every div. Getting this wrong is invisible on an all-lowercase page,
 * which is why it survived. */
"function byTag(scope, name) {\n"
"  var want = String(name), all = want === '*', lo = lower(want);\n"
"  var out = [], r = rootOf(scope);\n"
"  if (!r) return list(out);\n"
"  var ok = function (el) {\n"
"    if (all) return true;\n"
"    return isHTML(el) ? localOf(el) === lo : localOf(el) === want;\n"
"  };\n"
"  if (scope === doc && ok(r)) out.push(r);\n"
"  descendants(r, function (el) { if (ok(el)) out.push(el); });\n"
"  return list(out);\n"
"}\n"
/* getElementsByTagNameNS did not exist at all. It is case-SENSITIVE on both
 * arguments in every document, HTML or not -- there is no lowercasing step. */
"function byTagNS(scope, ns, local) {\n"
"  var wantNS = (ns === null || ns === undefined || ns === '') ? null : String(ns);\n"
"  var wantLN = String(local);\n"
"  var anyNS = wantNS === '*', anyLN = wantLN === '*';\n"
"  var out = [], r = rootOf(scope);\n"
"  if (!r) return list(out);\n"
"  var ok = function (el) {\n"
"    if (!anyNS) {\n"
"      var ens = el.namespaceURI;\n"
"      if (ens === undefined) ens = HTMLNS;\n"
"      if ((wantNS === null ? null : wantNS) !== ens) return false;\n"
"    }\n"
"    return anyLN || localOf(el) === wantLN;\n"
"  };\n"
"  if (scope === doc && ok(r)) out.push(r);\n"
"  descendants(r, function (el) { if (ok(el)) out.push(el); });\n"
"  return list(out);\n"
"}\n"
/* getElementsByClassName is ordinary-set membership, and it is the OTHER place
 * quirks mode changes an answer. */
"function byClass(scope, names) {\n"
"  var want = String(names).split(/[\\t\\n\\f\\r ]+/).filter(function (s) { return s; });\n"
"  var out = [], r = rootOf(scope);\n"
"  if (!r || !want.length) return list(out);\n"
"  var ok = function (el) {\n"
"    var have = classesOf(el);\n"
"    for (var i = 0; i < want.length; i++) {\n"
"      if (!QUIRKS) { if (have.indexOf(want[i]) < 0) return false; continue; }\n"
"      var hit = false;\n"
"      for (var j = 0; j < have.length; j++) if (ciEq(have[j], want[i])) { hit = true; break; }\n"
"      if (!hit) return false;\n"
"    }\n"
"    return true;\n"
"  };\n"
"  if (scope === doc && ok(r)) out.push(r);\n"
"  descendants(r, function (el) { if (ok(el)) out.push(el); });\n"
"  return list(out);\n"
"}\n"

"function def(o, k, v) { if (o && !(k in o)) { try { o[k] = v; } catch (e) {} } }\n"
/* Install-over, for the methods this file OWNS. See the header: `def` left the
 * three-selector C stub in place on document.querySelector, and that stub
 * answered null for every selector with a combinator, an attribute or a
 * compound in it. */
"function own(o, k, v) { try { o[k] = v; } catch (e) {\n"
"  try { Object.defineProperty(o, k, { value: v, writable: true, configurable: true }); }\n"
"  catch (q) {}\n"
"} }\n"

"own(doc, 'querySelectorAll', function (s) { return qsa(doc, s); });\n"
"own(doc, 'querySelector', function (s) { return qs1(doc, s); });\n"
"own(doc, 'getElementsByTagName', function (n) { return byTag(doc, n); });\n"
"own(doc, 'getElementsByTagNameNS', function (ns, n) { return byTagNS(doc, ns, n); });\n"
"own(doc, 'getElementsByClassName', function (n) { return byClass(doc, n); });\n"
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

/* Document-object gaps the probe found only once the event loop ran and
 * deepseek's React actually started rendering: it walks up from a node to find
 * the root, and `document` is where that walk ends.
 *   ownerDocument   null for the document itself -- React uses `node.
 *                   ownerDocument || node` to normalise, and undefined there
 *                   makes it treat the document as an element.
 *   getRootNode()   returns the document, which is what ends the walk.
 *   firstChild      the document element, since our tree's document node has
 *                   exactly one element child.
 *   activeElement   nothing has focus and there is no focus model, so it is
 *                   body -- the spec's own fallback, not a stub. */
"if (!('ownerDocument' in doc)) Object.defineProperty(doc, 'ownerDocument',\n"
"  { configurable: true, get: function () { return null; } });\n"
"def(doc, 'getRootNode', function () { return doc; });\n"
"if (!('firstChild' in doc)) Object.defineProperty(doc, 'firstChild',\n"
"  { configurable: true, get: function () { return doc.documentElement || null; } });\n"
"if (!('lastChild' in doc)) Object.defineProperty(doc, 'lastChild',\n"
"  { configurable: true, get: function () { return doc.documentElement || null; } });\n"
"if (!('activeElement' in doc)) Object.defineProperty(doc, 'activeElement',\n"
"  { configurable: true, get: function () { return doc.body || null; } });\n"
"def(doc, 'contains', function (n) {\n"
"  for (var p = n; p; p = p.parentNode) if (p === doc.documentElement) return true;\n"
"  return false;\n"
"});\n"
"def(EP, 'getRootNode', function () { return doc; });\n"

"own(EP, 'querySelectorAll', function (s) { return qsa(this, s); });\n"
"own(EP, 'querySelector', function (s) { return qs1(this, s); });\n"
"own(EP, 'getElementsByTagName', function (n) { return byTag(this, n); });\n"
"own(EP, 'getElementsByTagNameNS', function (ns, n) { return byTagNS(this, ns, n); });\n"
"own(EP, 'getElementsByClassName', function (n) { return byClass(this, n); });\n"
"own(EP, 'matches', function (s) {\n"
"  return matchAny(this, compile(s), { scope: this });\n"
"});\n"
"own(EP, 'webkitMatchesSelector', EP.matches);\n"
"own(EP, 'msMatchesSelector', EP.matches);\n"
"own(EP, 'closest', function (s) {\n"
"  var alts = compile(s), ctx = { scope: this };\n"
"  for (var el = this; el && el.nodeType === 1; el = el.parentNode)\n"
"    if (matchAny(el, alts, ctx)) return el;\n"
"  return null;\n"
"});\n"

/* NodeList and HTMLCollection only. THE ELEMENT INTERFACES MOVED to
 * js_platform.c's installInterfaces, and the move fixed two things that this
 * block got wrong -- both of which it got wrong invisibly, which is why they
 * survived:
 *
 *   - every per-tag name was a façade over the ONE shared element prototype,
 *     so `document.createElement('div') instanceof HTMLInputElement` was true.
 *     A page that branches on that takes a path it must not take, and nothing
 *     reports it. The replacement gives each per-tag interface its own
 *     prototype and a Symbol.hasInstance that tests tagName.
 *   - `HTMLElement` here threw unconditionally, which is right for `new
 *     HTMLElement()` and fatal for `class X extends HTMLElement`: a custom
 *     element's super() call has to hand back the element being upgraded.
 *     MEASURED -- with customElements implemented and this block still in
 *     place, MDN produced 163 `TypeError: Illegal constructor`, every one of
 *     them from THIS constructor, because js_select.c installs before
 *     js_platform.c and first definition wins.
 *
 * The collections stay because they are this file's own: the arrays returned
 * by querySelectorAll and getElementsByTagName are built here. */
"(function () {\n"
"  var mkClass = function (name, proto) {\n"
"    var C = function () { throw new TypeError('Illegal constructor'); };\n"
"    C.prototype = proto;\n"
"    try { Object.defineProperty(C, 'name', { value: name }); } catch (e) {}\n"
"    return C;\n"
"  };\n"
"  if (!('NodeList' in G)) G.NodeList = mkClass('NodeList', Array.prototype);\n"
"  if (!('HTMLCollection' in G)) G.HTMLCollection = mkClass('HTMLCollection', Array.prototype);\n"
"})();\n"

/* `new Image()`. MEASURED, AND ONLY ON THE MACHINE: it does not appear in the
 * host probe's table because bing reaches it from a setTimeout callback, and
 * the probe found it only once tests/qmp/qmp_bing.py ran the page with an
 * event loop turning (`uncaught in timer: ReferenceError: 'Image' is not
 * defined`). It is the oldest idiom on the web -- `new Image().src = url` is
 * how a page sends a beacon or preloads a sprite -- and it is not a class of
 * its own: the spec says it constructs exactly the element createElement('img')
 * makes, which is why this is three lines and not an image implementation.
 * Audio and Option are the same shape and the same one-liner. */
"(function () {\n"
"  if (!('Image' in G)) {\n"
"    G.Image = function Image(w, h) {\n"
"      var el = doc.createElement('img');\n"
"      if (w !== undefined) el.setAttribute('width', String(w));\n"
"      if (h !== undefined) el.setAttribute('height', String(h));\n"
"      return el;\n"
"    };\n"
"    G.Image.prototype = EP;\n"
"  }\n"
"  if (!('Audio' in G)) {\n"
"    G.Audio = function Audio(src) {\n"
"      var el = doc.createElement('audio');\n"
"      if (src !== undefined) el.setAttribute('src', String(src));\n"
"      return el;\n"
"    };\n"
"    G.Audio.prototype = EP;\n"
"  }\n"
"  if (!('Option' in G)) {\n"
"    G.Option = function Option(text, value) {\n"
"      var el = doc.createElement('option');\n"
"      if (text !== undefined) el.textContent = String(text);\n"
"      if (value !== undefined) el.setAttribute('value', String(value));\n"
"      return el;\n"
"    };\n"
"    G.Option.prototype = EP;\n"
"  }\n"
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
    JSValue arg = JS_NewCFunction(ctx, sel_quirks, "quirks", 0);
    JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 1, (JSValueConst *)&arg);
    if (JS_IsException(r)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        printf("[select] install failed: %s\n", m ? m : "?");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, arg);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, fn);

    /* Chained from here rather than from js_page.c: the token list needs
     * exactly the same precondition this file does (js_dom_init has run, so
     * Element.prototype exists) and nothing more, and js_page.c is edited by
     * several lines at once. */
    js_tokenlist_install(ctx);
}
