/* js_anim.c -- Element.prototype.animate, and now a real KeyframeEffect /
 * AnimationEffect / DocumentTimeline underneath it.
 *
 * THE MEASUREMENT THAT SIZED THE ORIGINAL VERSION OF THIS FILE, because
 * "implement Web Animations" is a month and that first cut was not that.
 *
 * WPT's interpolation-testcommon.js drives 337 files in css/ and produces
 * 47,140 of the 106,130 subtest failures there. Its Web Animations method
 * begins every subtest with
 *
 *     assert_true(interpolationMethod.isSupported(), ...)
 *
 * and for that method isSupported() is, in full, `'animate' in Element.prototype`.
 * 10,714 subtests failed on that one line. They were not all reachable: take
 * each failing `Web Animations: ...` subtest name, substitute `CSS
 * Animations`, and ask whether that twin passes. 8,513 twins ALSO failed --
 * those need CSS.supports and belong to the LibCSS line. 2,202 twins PASSED,
 * and those are what the first version of this file was sized against; that
 * pool has since collapsed to zero (see js_anim.c's own header before this
 * rewrite, and the M-item that closed it).
 *
 * THIS REWRITE is a second, separate measurement: 599 subtests across
 * KeyframeEffect/constructor.html, setKeyframes.html,
 * processing-a-keyframes-argument-001.html, AnimationEffect's
 * getTiming/updateTiming/getComputedTiming, DocumentTimeline, and the
 * ComputedKeyframe shape Animatable/animate.html checks -- none of them
 * reachable from a plain-object `this.effect` with three methods, which is
 * what `animate()` built before. `KeyframeEffect` and `AnimationEffect` are
 * now real constructors with real prototypes; `document.timeline` is a real
 * `DocumentTimeline`; `Animation` takes `(effect, timeline)` per spec instead
 * of `(target, keyframes, options)`.
 *
 * WHAT THE HARNESS ACTUALLY TOUCHES, downstream of the constructor work,
 * which is still the whole specification of the VALUE side of this file:
 *
 *     var animation = target.animate(keyframes, {fill, duration, easing});
 *     animation.pause();
 *     animation.currentTime = 50 * 1000;
 *     ... later ...
 *     getComputedStyle(target).getPropertyValue(prop)
 *
 * interpolation-testcommon.js never reads the animation back. The object is
 * a HANDLE; the observable is the target's computed style. So that half of
 * the work is not the API surface, it is making a computed read reflect the
 * interpolated value at a given time -- which is what css_interp.c already
 * computes, unchanged by this rewrite.
 *
 * WHY A JS PRELUDE OVER ONE NATIVE, and not a C implementation. The house
 * style, for the reason js_events.c states: nothing here is hot (constructing
 * an animation is not a page's cost), and the files that own the two things
 * this must compose with -- js_dom.c's computed CSSStyleDeclaration and
 * css_engine.c's cascade -- belong to other lines. A JS layer over a native
 * primitive composes with them without editing either.
 *
 * TWO RULES KEEP THIS FROM BEING A REGRESSION, and they are the reason it can
 * only add passes. A great many interpolation subtests currently pass
 * VACUOUSLY: `getComputedStyle(el).getPropertyValue('margin')` is "" here
 * because `margin` is a shorthand the computed-style table does not model, the
 * expected element answers "" too, and assert_equals("", "") passes. An
 * overlay that answered "15px" for the target and left the expected element at
 * "" would turn those passes into failures. So:
 *
 *   RULE 1  Never invent a value for a property the engine does not already
 *           report. If the underlying computed read is "", the overlay is
 *           silent. It cannot break a vacuous pass.
 *   RULE 2  If the interpolation DECLINES -- css_interp.c returns -1 for a
 *           shape it cannot bridge, `initial`/`inherit`/`unset` keyframes
 *           among them -- the overlay is silent. Today's answer stands.
 *
 * A THIRD RULE arrives with this rewrite, for the keyframe-PROCESSING half
 * rather than the value half:
 *
 *   RULE 3  Never invent CSS value validity. There is no general
 *           "is this a legal <length>" oracle in this tree (that is
 *           LibCSS's job). An invalid property value in a keyframe is
 *           therefore NOT dropped the way a conformant UA drops it -- it is
 *           carried through like any other string. Structural validity
 *           (offset range/order, easing grammar, composite enum) IS
 *           enforced, because all three are closed, fully-specified
 *           grammars this file can parse honestly without guessing at a
 *           property's syntax.
 *
 * Together: the overlay changes an answer only where it has a real
 * interpolation of a property the engine really reports, and the constructor
 * throws only where the spec's own closed grammars say it must. Everywhere
 * else the browser behaves exactly as it did before, which is also what makes
 * the A/B measurement in the report meaningful.
 *
 * COMPOSITE OPERATIONS (`add`, `accumulate`, and `iterationComposite`) are
 * unchanged in mechanism from the version before this rewrite -- only their
 * home moved, from the plain `this.effect` object to a real
 * `KeyframeEffect.prototype.composite` / `.iterationComposite` accessor
 * pair, each validated against its OWN vocabulary (a per-keyframe composite
 * accepts 'auto'; the effect-level one does not; iterationComposite accepts
 * neither 'auto' nor 'add'). What the measurement found on the way in the
 * first time is still worth more than the feature: 2,122 `Compositing ...`
 * subtests fail in css/; 1,928 of them never reach a value, failing on
 * `assert_true(CSS.supports(property, value))` three lines earlier over
 * values LibCSS still rejects. The 194 that do reach a value were each wrong
 * by EXACTLY the underlying value -- and were only reachable at all after the
 * keyframe resolution below stopped returning the same string for both
 * endpoints. See __resolveValues.
 *
 * STILL DELIBERATELY NOT HERE, and named rather than omitted: no timeline
 * that drives an Animation's OWN currentTime (document.timeline.currentTime
 * is now a real moving clock -- see the DocumentTimeline section below --
 * but nothing ticks a playing Animation from it; currentTime is exactly what
 * play()/pause()/the setter last left it at), no animation events
 * (animationstart/finish/cancel), no ScrollTimeline, no commitStyles, no
 * pseudo-element TARGETING of the getComputedStyle overlay (KeyframeEffect's
 * `pseudoElement` property is real and validated, but `patched()` below
 * still answers only for the no-pseudo call, exactly as before), and no CSS
 * value validity checking (RULE 3 above).
 *
 * [2026-08-30] ONE ITEM LEFT THAT LIST, and the correction is kept beside
 * the old claim: @keyframes/CSS-animation integration NOW EXISTS, as PART 2
 * at the bottom of this file -- a clock that runs CSS `animation` and
 * `transition` off the page's one deadline queue. The `Compositing CSS
 * Animations` half of every composition file (1,061 subtests) is still out
 * of reach: composition still happens only within one WAAPI effect, never
 * between a WAAPI effect and a concurrently-running CSS animation, and the
 * two clocks do not read each other's currentTime.
 */
#include "css_interp.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "quickjs.h"

/* ======================================================================
 * The one native: interpolate two CSS values.
 *
 * `transform` is routed to the transform API rather than the generic one --
 * it needs the element's box for percentages and a choice between the
 * computed function-list and the resolved matrix serialisation, and
 * ci_value_interp declines it on purpose so a caller cannot get the
 * componentwise-always behaviour by accident.
 *
 * The transform branch is INERT TODAY and that is expected: RULE 1 above
 * silences the overlay for any property the computed-style table does not
 * report, and LibCSS does not know `transform`, so the underlying read is "".
 * It lights up the moment the CSS.supports line teaches LibCSS the property,
 * with no change here. That is the payoff for css_interp.c having been built
 * first.
 * ====================================================================== */
static JSValue js_anim_interp(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    if (argc < 4) return JS_NULL;
    const char *prop = JS_ToCString(ctx, argv[0]);
    const char *from = JS_ToCString(ctx, argv[1]);
    const char *to   = JS_ToCString(ctx, argv[2]);
    double p = 0;
    JS_ToFloat64(ctx, &p, argv[3]);

    JSValue out = JS_NULL;
    if (!prop || !from || !to) goto done;

    if (!strcmp(prop, "transform")) {
        struct ci_xform a, b, r;
        if (ci_transform_parse(from, -1, 16, 16, &a) == 0 &&
            ci_transform_parse(to, -1, 16, 16, &b) == 0) {
            ci_transform_interp(&a, &b, p, &r);
            double m[16];
            char buf[1024];
            ci_transform_matrix(&r, 0, 0, m);
            if (ci_matrix_text(m, buf, sizeof buf) > 0)
                out = JS_NewString(ctx, buf);
        }
        goto done;
    }

    {
        char buf[1024];
        int n = ci_value_interp(prop, from, to, p, buf, sizeof buf);
        if (n > 0) out = JS_NewString(ctx, buf);
    }

done:
    if (prop) JS_FreeCString(ctx, prop);
    if (from) JS_FreeCString(ctx, from);
    if (to) JS_FreeCString(ctx, to);
    return out;
}

/* ======================================================================
 * The second native: composite a keyframe value onto the underlying one.
 *
 *     __anim_composite(prop, underlying, value, op)  ->  string | null
 *
 * `op` is the STRING "add" or "accumulate"; anything else, "replace"
 * included, returns null. null also means "these two cannot be combined" --
 * a shape mismatch, a discrete type, or no underlying value at all -- and the
 * caller then uses the keyframe value unchanged, which is what a type with no
 * addition defined does.
 *
 * Returning null for `replace` rather than echoing the value back is
 * deliberate: the caller has to know whether a composition HAPPENED, because
 * a composed endpoint and a replaced one are then interpolated identically
 * and there would be no other way to tell a working `add` from a silently
 * ignored one.
 * ====================================================================== */
static JSValue js_anim_composite(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    if (argc < 4) return JS_NULL;
    const char *prop = JS_ToCString(ctx, argv[0]);
    const char *und  = JS_ToCString(ctx, argv[1]);
    const char *val  = JS_ToCString(ctx, argv[2]);
    const char *ops  = JS_ToCString(ctx, argv[3]);

    JSValue out = JS_NULL;
    if (!prop || !und || !val || !ops) goto done;
    int op = ci_composite_op(ops);
    if (op == CI_COMPOSITE_REPLACE) goto done;

    if (!strcmp(prop, "transform")) {
        struct ci_xform a, b, r;
        char buf[2048];
        if (ci_transform_parse(und, -1, 16, 16, &a) == 0 &&
            ci_transform_parse(val, -1, 16, 16, &b) == 0 &&
            ci_transform_composite(&a, &b, op, &r) >= 0 &&
            ci_transform_text(&r, buf, sizeof buf) > 0)
            out = JS_NewString(ctx, buf);
        goto done;
    }

    {
        char buf[1024];
        int n = ci_value_composite(prop, und, val, op, buf, sizeof buf);
        if (n > 0) out = JS_NewString(ctx, buf);
    }

done:
    if (prop) JS_FreeCString(ctx, prop);
    if (und)  JS_FreeCString(ctx, und);
    if (val)  JS_FreeCString(ctx, val);
    if (ops)  JS_FreeCString(ctx, ops);
    return out;
}

/* ======================================================================
 * The prelude.
 * ====================================================================== */
static const char ANIM_JS[] =
"(function(){\n"
"'use strict';\n"
"var EP = (typeof Element !== 'undefined' && Element.prototype) || null;\n"
"if (!EP) return;\n"
"var gcs = (typeof getComputedStyle === 'function') ? getComputedStyle : null;\n"
"if (!gcs) return;\n"
"var II = __anim_interp;\n"
"var CC = __anim_composite;\n"
"\n"
"/* A real DOMException when one exists, the same fallback idiom js_events.c\n"
" * uses for domErr(): assert_throws_dom checks e.name and e instanceof\n"
" * DOMException, and a plain Error with a matching .name satisfies neither,\n"
" * but is still better than crashing the install. */\n"
"function animDomErr(msg, name) {\n"
"  if (typeof DOMException === 'function') { try { return new DOMException(msg, name); } catch (q) {} }\n"
"  var e = new Error(msg); e.name = name; return e;\n"
"}\n"
"\n"
"/* ---- property-name spellings -------------------------------------------\n"
" * dash() is the CSS-property-to-IDL-attribute algorithm run BACKWARDS: it is\n"
" * what a page hands animate() (camelCase, `offset` -> `cssOffset` because the\n"
" * plain name collides with the keyframe timing member, `float` -> `cssFloat`\n"
" * because `float` is a reserved word). idlName() is the SAME algorithm\n"
" * forwards, and getKeyframes() is specified to report property names through\n"
" * it -- a keyframe built from `marginTop` must read back `marginTop`, not\n"
" * `margin-top`, which is why the internal keyframe records below store the\n"
" * CANONICAL DASHED name (what css_interp.c and the resolver want) and\n"
" * getKeyframes() converts back to IDL form on the way out, once, at the\n"
" * boundary. */\n"
"function dash(p) {\n"
"  p = String(p);\n"
"  if (p.slice(0, 2) === '--') return p;\n"
"  if (p === 'cssFloat') return 'float';\n"
"  if (p === 'cssOffset') return 'offset';\n"
"  return p.replace(/[A-Z]/g, function (c) { return '-' + c.toLowerCase(); });\n"
"}\n"
"function idlName(p) {\n"
"  if (p.slice(0, 2) === '--') return p;\n"
"  if (p === 'float') return 'cssFloat';\n"
"  if (p === 'offset') return 'cssOffset';\n"
"  return p.replace(/-([a-zA-Z])/g, function (_, c) { return c.toUpperCase(); });\n"
"}\n"
"\n"
"/* ---- easing: parse, validate, canonicalize -----------------------------\n"
" * ONE function answers three questions an implementation usually answers\n"
" * three different ways and lets drift: is this string a legal\n"
" * <easing-function>, what is its canonical serialization, and does it equal\n"
" * some other legal spelling of the same function. parseEasing returns the\n"
" * canonical form or null; isValidEasing is parseEasing(...) !== null, so the\n"
" * validity rule and the parse can never disagree with each other. */\n"
"function stripEasingToken(raw) {\n"
"  var s = String(raw);\n"
"  /* CSS comments are whitespace, wherever they fall. */\n"
"  s = s.replace(/\\/\\*[\\s\\S]*?\\*\\//g, '');\n"
"  /* CSS escapes: a backslash followed by 1-6 hex digits (and one optional\n"
"   * trailing whitespace character that is PART OF the escape, not a\n"
"   * separator) denotes a code point; a backslash followed by anything else\n"
"   * denotes that character literally. 'Ease\\2d in-out' is 'ease-in-out'\n"
"   * this way -- it is not a made-up case, WPT's own easing-parsing corpus\n"
"   * depends on it. */\n"
"  s = s.replace(/\\\\([0-9a-fA-F]{1,6})[ \\t\\n\\r\\f]?|\\\\([^\\r\\n\\f0-9a-fA-F])/g,\n"
"    function (m, hex, ch) {\n"
"      if (hex !== undefined) {\n"
"        try { return String.fromCodePoint(parseInt(hex, 16)); } catch (e) { return ''; }\n"
"      }\n"
"      return ch;\n"
"    });\n"
"  return s.trim();\n"
"}\n"
"var EASING_KEYWORDS = {\n"
"  linear: 1, ease: 1, 'ease-in': 1, 'ease-out': 1, 'ease-in-out': 1,\n"
"  'step-start': 1, 'step-end': 1\n"
"};\n"
"function parseEasing(raw) {\n"
"  var s = stripEasingToken(raw);\n"
"  var low = s.toLowerCase();\n"
/* step-start/step-end are ALIASES, not their own serialization -- their
 * canonical form is the steps() call they name (verified against
 * easing-tests.js's own `serialization` field: 'step-start' ->
 * 'steps(1, start)', 'step-end' -> 'steps(1)'). Every other keyword
 * ('ease', 'linear', ...) serializes as itself; only these two expand. */
"  if (low === 'step-start') return 'steps(1, start)';\n"
"  if (low === 'step-end') return 'steps(1)';\n"
"  if (Object.prototype.hasOwnProperty.call(EASING_KEYWORDS, low)) return low;\n"
"  /* cubic-bezier: only x1/x2 are constrained to [0,1] -- they are the\n"
"   * horizontal axis and the curve must stay a function of it; y1/y2 are\n"
"   * unconstrained on purpose (a bounce or an overshoot needs y outside\n"
"   * [0,1], and interpolation-testcommon.js's own createEasing() constructs\n"
"   * exactly that: cubic-bezier(0, b, 1, b) with b as large as 1.83 to reach\n"
"   * `at = 1.5`). Clamping y here would silently cap every extrapolation test\n"
"   * in the whole css/ corpus at its endpoint. */\n"
"  var m = /^cubic-bezier\\(\\s*([^,]+),\\s*([^,]+),\\s*([^,]+),\\s*([^,]+)\\)$/i.exec(s);\n"
"  if (m) {\n"
"    var x1 = parseFloat(m[1]), y1 = parseFloat(m[2]), x2 = parseFloat(m[3]), y2 = parseFloat(m[4]);\n"
"    if (![x1, y1, x2, y2].every(function (v) { return v === v; })) return null;\n"
"    if (x1 < 0 || x1 > 1 || x2 < 0 || x2 > 1) return null;\n"
"    return 'cubic-bezier(' + x1 + ', ' + y1 + ', ' + x2 + ', ' + y2 + ')';\n"
"  }\n"
"  m = /^steps\\(\\s*(-?\\d+)\\s*(?:,\\s*(jump-start|jump-end|jump-none|jump-both|start|end)\\s*)?\\)$/i.exec(s);\n"
"  if (m) {\n"
"    var n = parseInt(m[1], 10);\n"
"    var pos = m[2] ? m[2].toLowerCase() : null;\n"
"    var effPos = pos || 'end';\n"
"    if (effPos === 'jump-none') { if (!(n >= 2)) return null; }\n"
"    else if (!(n >= 1)) return null;\n"
"    return 'steps(' + n + (pos ? ', ' + pos : '') + ')';\n"
"  }\n"
"  return null;\n"
"}\n"
"function isValidEasing(raw) { return parseEasing(raw) !== null; }\n"
"\n"
"/* Turn an ALREADY-VALIDATED canonical easing string into a pacing function.\n"
" * Never called on an unvalidated string -- every caller below has gone\n"
" * through parseEasing() first, which is what lets this stay a dumb\n"
" * dispatch with a linear fallback instead of a second copy of the grammar. */\n"
"function bez(x1, y1, x2, y2) {\n"
"  function C(t, p1, p2) { return (((1 - 3 * p2 + 3 * p1) * t + (3 * p2 - 6 * p1)) * t + 3 * p1) * t; }\n"
"  function dC(t, p1, p2) { return 3 * (1 - 3 * p2 + 3 * p1) * t * t + 2 * (3 * p2 - 6 * p1) * t + 3 * p1; }\n"
"  return function (x) {\n"
"    if (x1 === y1 && x2 === y2) return x;\n"
"    if (x <= 0 || x >= 1) {\n"
"      var s0 = (x1 > 0) ? y1 / x1 : ((y2 > 0 || x2 > 0) ? y2 / x2 : 0);\n"
"      var s1 = (x2 < 1) ? (y2 - 1) / (x2 - 1) : ((y1 !== 1 || x1 !== 1) ? (y1 - 1) / (x1 - 1) : 0);\n"
"      return (x <= 0) ? s0 * x : 1 + s1 * (x - 1);\n"
"    }\n"
"    var t = x, i, d, xe;\n"
"    for (i = 0; i < 12; i++) {\n"
"      xe = C(t, x1, x2) - x;\n"
"      if (xe < 1e-10 && xe > -1e-10) return C(t, y1, y2);\n"
"      d = dC(t, x1, x2);\n"
"      if (d < 1e-10 && d > -1e-10) break;\n"
"      t = t - xe / d;\n"
"    }\n"
"    var lo = 0, hi = 1; t = x;\n"
"    for (i = 0; i < 60; i++) {\n"
"      xe = C(t, x1, x2);\n"
"      if (xe > x) hi = t; else lo = t;\n"
"      t = (lo + hi) / 2;\n"
"    }\n"
"    return C(t, y1, y2);\n"
"  };\n"
"}\n"
"function stepsFn(n, pos) {\n"
"  return function (x) {\n"
"    var jumpStart = (pos === 'start' || pos === 'jump-start' || pos === 'jump-both');\n"
"    var jumpNone = (pos === 'jump-none');\n"
"    var d = jumpNone ? (n - 1) : n;\n"
"    if (d <= 0) d = 1;\n"
"    var step = jumpStart ? Math.ceil(x * n) : Math.floor(x * n);\n"
"    if (pos === 'jump-both') step = Math.floor(x * n) + 1;\n"
"    return step / d;\n"
"  };\n"
"}\n"
"function easingFn(s) {\n"
"  if (s === 'linear' || s === '') return function (x) { return x; };\n"
"  if (s === 'ease') return bez(0.25, 0.1, 0.25, 1);\n"
"  if (s === 'ease-in') return bez(0.42, 0, 1, 1);\n"
"  if (s === 'ease-out') return bez(0, 0, 0.58, 1);\n"
"  if (s === 'ease-in-out') return bez(0.42, 0, 0.58, 1);\n"
"  if (s === 'step-start') return stepsFn(1, 'start');\n"
"  if (s === 'step-end') return stepsFn(1, 'end');\n"
"  var m = /^cubic-bezier\\(([^)]*)\\)$/.exec(s);\n"
"  if (m) {\n"
"    var a = m[1].split(',').map(function (v) { return parseFloat(v); });\n"
"    return bez(a[0], a[1], a[2], a[3]);\n"
"  }\n"
"  m = /^steps\\(([^)]*)\\)$/.exec(s);\n"
"  if (m) {\n"
"    var b = m[1].split(',');\n"
"    var n = parseInt(b[0], 10);\n"
"    return stepsFn(n, b.length > 1 ? b[1].trim() : 'end');\n"
"  }\n"
"  return function (x) { return x; };\n"
"}\n"
"\n"
"/* ---- composite operations: three vocabularies, not one -----------------\n"
" * A per-KEYFRAME composite accepts 'auto' (meaning \"use the effect's\"); the\n"
" * effect-level `composite` (constructor option and the `.composite`\n"
" * accessor) does not, because 'auto' deferring to itself is not an\n"
" * operation; `iterationComposite` is a different axis entirely (time, not\n"
" * value) and only 'replace'/'accumulate' are defined for it. Collapsing\n"
" * these to one set accepts values the spec refuses and refuses values the\n"
" * spec accepts, in each direction at once. */\n"
"function isValidKeyframeComposite(s) { return s === 'replace' || s === 'add' || s === 'accumulate' || s === 'auto'; }\n"
"function isValidEffectComposite(s) { return s === 'replace' || s === 'add' || s === 'accumulate'; }\n"
"function isValidIterationComposite(s) { return s === 'replace' || s === 'accumulate'; }\n"
"\n"
"/* ---- pseudo-element selectors -------------------------------------------\n"
" * Only the double-colon FORM and only a KNOWN NAME -- 'before', ':abc' and\n"
" * '::abc' all throw SyntaxError, which is the corpus's own way of saying\n"
" * this is not a general selector parser, it is a fixed enumeration. */\n"
"var PSEUDO_NAMES = {\n"
"  before: 1, after: 1, marker: 1, placeholder: 1, 'first-line': 1,\n"
"  'first-letter': 1, selection: 1, backdrop: 1, 'file-selector-button': 1\n"
"};\n"
"function validatePseudo(v) {\n"
"  if (v === null || v === undefined) return null;\n"
"  var s = String(v);\n"
"  var m = /^::([A-Za-z-]+)$/.exec(s);\n"
"  var name = m ? m[1].toLowerCase() : '';\n"
"  if (!m || !Object.prototype.hasOwnProperty.call(PSEUDO_NAMES, name)) {\n"
"    throw animDomErr(\"Failed to set the 'pseudoElement' property: '\" + s +\n"
"      \"' is not a valid pseudo-element selector.\", 'SyntaxError');\n"
"  }\n"
"  return '::' + name;\n"
"}\n"
"\n"
"/* ---- timing: duration/iterations are the trap ---------------------------\n"
" * Both accept +Infinity and reject everything else non-finite-or-negative\n"
" * with the SAME coercion path, which is what makes '-Infinity', 'NaN' and\n"
" * '-1' all throw without three separate checks: Number(x) !== Number(x) is\n"
" * the NaN test, and Number(x) < 0 is false for +Infinity and true for\n"
" * -Infinity, so one guard clears all three plus every ordinary negative\n"
" * number and every non-numeric string ('merrychristmas' -> NaN) at once. */\n"
"function toFiniteNonNegative(v) {\n"
"  var n = Number(v);\n"
"  if (n !== n || n < 0) throw new TypeError('value must be a non-negative number (or +Infinity)');\n"
"  return n;\n"
"}\n"
/* iterationStart is the one non-negative quantity that does NOT accept
 * +Infinity (gBadIterationStartValues lists it as invalid, unlike duration
 * and iterations) -- a distinct check rather than a shared one, because
 * silently reusing toFiniteNonNegative here would accept it. */
"function toStrictNonNegative(v) {\n"
"  var n = Number(v);\n"
"  if (n !== n || n < 0 || n === Infinity) throw new TypeError('value must be a non-negative finite number');\n"
"  return n;\n"
"}\n"
/* delay/endDelay: finite in BOTH directions (negative is fine -- a negative
 * delay is how an animation starts partway through -- but NaN and either
 * infinity are not, per gBadDelayValues). */
"function toFiniteNumber(v) {\n"
"  var n = Number(v);\n"
"  if (n !== n || n === Infinity || n === -Infinity) throw new TypeError('value must be a finite number');\n"
"  return n;\n"
"}\n"
/* duration's own (UnrestrictedDouble or DOMString) union: a STRING is only
 * ever meaningful as the literal 'auto' -- '100' and 'abc' are both
 * REJECTED as strings, not coerced through Number(), which is what
 * Animatable/animate.html's 'invalid duration value: \"100\" using a
 * dictionary object' case depends on (100 alone would be a perfectly good
 * duration; '100' is not, because the DOMString branch of the union does
 * not fall back to numeric parsing). A non-string goes through the shared
 * non-negative-or-Infinity check. */
"function toDurationValue(v) {\n"
"  if (typeof v === 'string') {\n"
"    if (v === 'auto') return 'auto';\n"
"    throw new TypeError(\"invalid duration: '\" + v + \"'\");\n"
"  }\n"
"  return toFiniteNonNegative(v);\n"
"}\n"
"function parseTiming(opt) {\n"
"  /* duration defaults to 'auto', NOT 0 -- KeyframeEffect/constructor.html's\n"
"   * own default-value test checks this literally, and 'auto' is also what\n"
"   * lets getComputedTiming() distinguish \"no duration was ever given\" from\n"
"   * \"duration: 0\" (both currently resolve to 0 internally, since neither\n"
"   * this engine nor the corpus it is measured against has a use for the\n"
"   * distinction beyond that one readback). */\n"
"  var t = {\n"
"    duration: 'auto', delay: 0, endDelay: 0, iterations: 1, iterationStart: 0,\n"
"    direction: 'normal', fill: 'auto', easing: 'linear'\n"
"  };\n"
"  if (opt === undefined || opt === null) return t;\n"
/* A non-object options argument is the (double or KeyframeAnimationOptions)
 * union's OTHER member: WebIDL tries the dictionary conversion first, but a
 * number/string/boolean is never a valid dictionary source, so it falls to
 * ToNumber -- 'abc' becomes NaN and is rejected the same way a literal NaN
 * duration is, which is animate.html's own reasoning for skipping the
 * numeric-looking string cases in that position (parseFloat succeeds, so
 * the browser-observable outcome is ambiguous and the corpus does not test
 * it here). This is NOT the same rule as the duration MEMBER's own
 * DOMString-means-only-'auto' rule above -- two different union types. */
"  if (typeof opt !== 'object') { t.duration = toFiniteNonNegative(opt); return t; }\n"
"  if (opt.duration !== undefined) t.duration = toDurationValue(opt.duration);\n"
"  if (opt.delay !== undefined) t.delay = toFiniteNumber(opt.delay);\n"
"  if (opt.endDelay !== undefined) t.endDelay = toFiniteNumber(opt.endDelay);\n"
"  if (opt.iterations !== undefined) t.iterations = toFiniteNonNegative(opt.iterations);\n"
"  if (opt.iterationStart !== undefined) t.iterationStart = toStrictNonNegative(opt.iterationStart);\n"
"  if (opt.direction !== undefined) t.direction = String(opt.direction);\n"
"  if (opt.fill !== undefined) t.fill = String(opt.fill);\n"
"  if (opt.easing !== undefined) {\n"
"    var es = String(opt.easing);\n"
"    if (!isValidEasing(es)) throw new TypeError(\"invalid easing: '\" + es + \"'\");\n"
"    t.easing = parseEasing(es);\n"
"  }\n"
"  return t;\n"
"}\n"
"\n"
"/* ---- keyframes: the process-a-keyframes-argument algorithm -------------\n"
" *\n"
" * THE ONE THING THAT IS NOT IN THIS FILE, named rather than faked: no value\n"
" * validity checking. There is no general \"is this a legal <length> for\n"
" * `top`\" oracle in this tree (that is LibCSS's job, and calling into it from\n"
" * here for one keyframe value at a time is a different, much larger change).\n"
" * So an invalid property value -- `left: 'invalid'` -- is NOT dropped the\n"
" * way a real UA drops it; it is carried through as a string like any other.\n"
" * Rule 1 applies: better to be silently wrong on the handful of subtests\n"
" * that specifically probe invalid-value handling than to invent a validator\n"
" * that is wrong in a way nobody has measured.\n"
" *\n"
" * Two lists behave differently on purpose, and it is not an oversight if\n"
" * they look inconsistent: OFFSETS are index-bound (offsets[i] only applies\n"
" * when i < offsets.length; anything past the end of a short offsets array is\n"
" * never even looked at, which is why 'not strictly ascending in the UNUSED\n"
" * part of the array' is valid). EASINGS and COMPOSITES are modulo-bound\n"
" * (easings[i % easings.length] applies to every keyframe, which is why a\n"
" * SINGLE easing value with no properties still has to be a legal easing --\n"
" * 'empty property-indexed keyframe with an invalid easing' throws even\n"
" * though there is nothing else in the object) and are validated over their\n"
" * WHOLE list up front, including entries a short keyframe count will never\n"
" * index into ('an invalid easing in the unused part of the array' throws\n"
" * too). Both behaviours are measured against keyframe-tests.js, not\n"
" * invented; they happen to be the actual specification. */\n"
"function normalizeMemberList(raw, dflt) {\n"
"  if (raw === undefined) return dflt.slice();\n"
"  if (Array.isArray(raw)) return raw.length ? raw.slice() : dflt.slice();\n"
"  return [raw];\n"
"}\n"
"function offsetMemberList(raw) {\n"
"  if (raw === undefined) return [];\n"
"  if (Array.isArray(raw)) return raw.slice();\n"
"  return [raw];\n"
"}\n"
"/* Reserved member names, on EITHER form of keyframes input -- plus\n"
" * `computedOffset`, which is not a CSS property and is not a spec-reserved\n"
" * member either, but IS a key getKeyframes() puts on every object it\n"
" * returns. Without excluding it here, `new KeyframeEffect(t,\n"
" * effect.getKeyframes())` -- the roundtrip every gKeyframesTests entry is\n"
" * also tested as -- would read its own output back as a bogus\n"
" * `computed-offset` style property and corrupt the very shape it was\n"
" * supposed to reproduce. A real engine would reach the same place by a\n"
" * different road: `computed-offset` is not a property IT knows either, so\n"
" * a supported-property filter would drop it just the same. */\n"
"var KF_RESERVED = { offset: 1, easing: 1, composite: 1, computedOffset: 1 };\n"
"\n"
/* Only names SHAPED like a CSS-property-to-IDL-attribute result are read at
 * all -- checked before the property is ever ACCESSED (getter and all), not
 * after, because processing-a-keyframes-argument-001.html's whole method is
 * a property with a counting getter that asserts it was never called for a
 * name that should be skipped. Two rules, both general spec facts and
 * neither invented for this corpus: a literal hyphen outside a leading `--`
 * can never be the result of the CSS-property-to-IDL-attribute algorithm
 * (dash() always removes them), so a key like `font-size` is not a
 * property THIS algorithm would ever produce and is not read; and bare
 * `float` is specifically excluded because the IDL name for the `float`
 * property is `cssFloat` -- `float` is a reserved word in older bindings,
 * which is the entire reason that rename exists, so the bare spelling maps
 * to nothing. What this does NOT do, named rather than faked: it does not
 * know which recognized-shaped names are actually ANIMATABLE CSS
 * properties (`direction`, `unicodeBidi`, `willChange` and the rest of
 * processing-a-keyframes-argument-001.html's `gNonAnimatableProps` are
 * syntactically fine IDL names and are still read here) -- that needs a
 * real animatable-property registry this tree does not have, and RULE 1
 * applies: absent rather than an invented, possibly-wrong list. */
"function isRecognizedPropName(pn) {\n"
"  if (pn.slice(0, 2) === '--') return true;\n"
"  if (pn === 'float') return false;\n"
"  return /^[a-zA-Z][a-zA-Z0-9]*$/.test(pn);\n"
"}\n"
"\n"
"function buildFrames(kfInput) {\n"
"  if (kfInput === null || kfInput === undefined) return [];\n"
"  var isSeq = Array.isArray(kfInput);\n"
"  var n, offsetRaw, easingRaw, compositeRaw, perFrame;\n"
"\n"
"  if (isSeq) {\n"
"    n = kfInput.length;\n"
"    if (n === 0) return [];\n"
"    offsetRaw = []; easingRaw = []; compositeRaw = []; perFrame = [];\n"
"    for (var idx = 0; idx < n; idx++) {\n"
"      var k = kfInput[idx];\n"
"      var off = null, eas = 'linear', comp = 'auto';\n"
"      if (k !== null && k !== undefined) {\n"
"        if (k.offset !== undefined && k.offset !== null) off = k.offset;\n"
"        if (k.easing !== undefined) eas = k.easing;\n"
"        if (k.composite !== undefined) comp = k.composite;\n"
"      }\n"
"      offsetRaw.push(off);\n"
"      easingRaw.push(eas);\n"
"      compositeRaw.push(comp);\n"
"      var props = {};\n"
"      if (k !== null && typeof k === 'object') {\n"
"        for (var pn in k) {\n"
"          if (Object.prototype.hasOwnProperty.call(KF_RESERVED, pn)) continue;\n"
"          if (!isRecognizedPropName(pn)) continue;\n"
"          var pv = k[pn];\n"
"          if (pv === undefined || pv === null) continue;\n"
"          props[dash(pn)] = String(pv);\n"
"        }\n"
"      }\n"
"      perFrame.push(props);\n"
"    }\n"
"  } else {\n"
"    if (typeof kfInput !== 'object') return [];\n"
"    var offsetMember = kfInput.offset;\n"
"    var easingMember = kfInput.easing;\n"
"    var compositeMember = kfInput.composite;\n"
"    var propNames = [];\n"
"    for (var pn2 in kfInput) {\n"
"      if (Object.prototype.hasOwnProperty.call(KF_RESERVED, pn2)) continue;\n"
"      if (!isRecognizedPropName(pn2)) continue;\n"
"      propNames.push(pn2);\n"
"    }\n"
"    var propVals = {};\n"
"    n = 0;\n"
"    for (var q = 0; q < propNames.length; q++) {\n"
"      var raw = kfInput[propNames[q]];\n"
"      var arr = Array.isArray(raw) ? raw : [raw];\n"
"      propVals[propNames[q]] = arr;\n"
"      if (arr.length > n) n = arr.length;\n"
"    }\n"
"    if (n === 0) n = 1;\n"
"    offsetRaw = offsetMemberList(offsetMember);\n"
"    easingRaw = normalizeMemberList(easingMember, ['linear']);\n"
"    compositeRaw = normalizeMemberList(compositeMember, ['auto']);\n"
"    perFrame = [];\n"
"    for (var i2 = 0; i2 < n; i2++) perFrame.push({});\n"
"    /* Each property's own value list is spread across the FULL 0..n-1 grid\n"
"     * independently of every other property -- a value at list-index j of\n"
"     * L goes to keyframe slot round(j * (n-1) / (L-1)), so a two-value list\n"
"     * against a five-keyframe grid lands on slots 0 and 4, never on 1/2/3.\n"
"     * That is why a shorter property is ABSENT from the frames in between,\n"
"     * not repeated into them -- confirmed against keyframe-tests.js's\n"
"     * \"different numbers of values\" case, which is exactly this shape. */\n"
"    for (var q2 = 0; q2 < propNames.length; q2++) {\n"
"      var vals = propVals[propNames[q2]];\n"
"      var L = vals.length;\n"
"      for (var j = 0; j < L; j++) {\n"
"        var v2 = vals[j];\n"
"        if (v2 === undefined || v2 === null) continue;\n"
"        var slot = (L <= 1) ? 0 : Math.round(j * (n - 1) / (L - 1));\n"
"        perFrame[slot][dash(propNames[q2])] = String(v2);\n"
"      }\n"
"    }\n"
"  }\n"
"\n"
"  var easingParsed = [];\n"
"  for (var e = 0; e < easingRaw.length; e++) {\n"
"    var es = String(easingRaw[e]);\n"
"    if (!isValidEasing(es)) throw new TypeError(\"invalid keyframe easing: '\" + es + \"'\");\n"
"    easingParsed.push(parseEasing(es));\n"
"  }\n"
"  if (!easingParsed.length) easingParsed = ['linear'];\n"
"\n"
"  var compParsed = [];\n"
"  for (var c = 0; c < compositeRaw.length; c++) {\n"
"    var cs = compositeRaw[c];\n"
"    if (cs === null || cs === undefined || !isValidKeyframeComposite(String(cs)))\n"
"      throw new TypeError(\"invalid keyframe composite: '\" + cs + \"'\");\n"
"    compParsed.push(String(cs));\n"
"  }\n"
"  if (!compParsed.length) compParsed = ['auto'];\n"
"\n"
"  /* Offsets: range-checked and LOOSELY sorted (non-decreasing; duplicates at\n"
"   * the same offset are fine, a later smaller one is not) -- but only over\n"
"   * the index-bound portion, and only comparing SPECIFIED (non-null)\n"
"   * entries to each other. computedOffset starts equal to offset and is\n"
"   * filled in for the nulls by the spacing pass below. */\n"
"  var out = [];\n"
"  var lastSpecified = null;\n"
"  for (var ii = 0; ii < n; ii++) {\n"
"    var offVal = null;\n"
"    if (ii < offsetRaw.length) {\n"
"      var ov = offsetRaw[ii];\n"
"      if (ov !== null && ov !== undefined) {\n"
"        var num = Number(ov);\n"
"        if (num !== num || num < 0 || num > 1)\n"
"          throw new TypeError('keyframe offset out of range: ' + ov);\n"
"        if (lastSpecified !== null && num < lastSpecified)\n"
"          throw new TypeError('keyframe offsets not loosely sorted by offset');\n"
"        lastSpecified = num;\n"
"        offVal = num;\n"
"      }\n"
"    }\n"
"    out.push({\n"
"      offset: offVal,\n"
"      computedOffset: offVal,\n"
"      easing: easingParsed[ii % easingParsed.length],\n"
"      composite: compParsed[ii % compParsed.length],\n"
/* `props` is the SPECIFIED value the caller wrote -- getKeyframes() reads
 * this and only this, forever. `resolved` starts absent and is filled in
 * by __resolveValues() with a SEPARATE dict; nothing here ever overwrites
 * `props` in place, which is the bug this comment is standing in for: a
 * first cut resolved values into `props` directly, and
 * `Animatable/animate.html`'s own `left: ['10px', '20px']` case caught it
 * immediately -- `left` on a statically positioned div computes to `auto`,
 * so getKeyframes() started reporting `auto` for a keyframe the page wrote
 * as `10px`, which is correct for the INTERPOLATION math and wrong for the
 * one thing getKeyframes() is specified to report: what was written. */
"      props: perFrame[ii] || {},\n"
"      resolved: null\n"
"    });\n"
"  }\n"
"\n"
"  /* A SOLE keyframe with no offset lands at 1, not 0 -- both\n"
"   * '{left:['10px']}' and '{left:'10px'}' (array-of-one vs bare value, which\n"
"   * collapse to the same n=1 case above) resolve this way, and it is the\n"
"   * one case the general \"first null -> 0, last null -> 1\" rule below must\n"
"   * NOT be allowed to touch first, because for a single frame first and\n"
"   * last are the same frame and the general rule would set it to 0. */\n"
"  if (out.length === 1 && out[0].computedOffset === null) out[0].computedOffset = 1;\n"
"  if (out.length) {\n"
"    if (out[0].computedOffset === null) out[0].computedOffset = 0;\n"
"    if (out[out.length - 1].computedOffset === null) out[out.length - 1].computedOffset = 1;\n"
"  }\n"
"  for (var s = 0; s < out.length; s++) {\n"
"    if (out[s].computedOffset === null) {\n"
"      var e2 = s;\n"
"      while (e2 < out.length && out[e2].computedOffset === null) e2++;\n"
"      var lo = (s > 0) ? out[s - 1].computedOffset : 0;\n"
"      var hi = (e2 < out.length) ? out[e2].computedOffset : 1;\n"
"      for (var u = s; u < e2; u++) out[u].computedOffset = lo + (hi - lo) * (u - s + 1) / (e2 - s + 1);\n"
"      s = e2 - 1;\n"
"    }\n"
"  }\n"
"  return out;\n"
"}\n"
"\n"
"/* ======================================================================\n"
" * AnimationEffect / KeyframeEffect / DocumentTimeline\n"
" * ====================================================================== */\n"
"function AnimationEffect() {}\n"
"AnimationEffect.prototype.getTiming = function () {\n"
"  var t = this.__timing;\n"
"  return {\n"
"    delay: t.delay, endDelay: t.endDelay, fill: t.fill, iterations: t.iterations,\n"
"    iterationStart: t.iterationStart, duration: t.duration, direction: t.direction,\n"
"    easing: t.easing\n"
"  };\n"
"};\n"
"AnimationEffect.prototype.updateTiming = function (opt) {\n"
"  if (opt === undefined || opt === null) return;\n"
"  if (typeof opt !== 'object') return;\n"
"  var t = this.__timing;\n"
"  if (opt.duration !== undefined) t.duration = toDurationValue(opt.duration);\n"
"  if (opt.delay !== undefined) t.delay = toFiniteNumber(opt.delay);\n"
"  if (opt.endDelay !== undefined) t.endDelay = toFiniteNumber(opt.endDelay);\n"
"  if (opt.iterations !== undefined) t.iterations = toFiniteNonNegative(opt.iterations);\n"
"  if (opt.iterationStart !== undefined) t.iterationStart = toStrictNonNegative(opt.iterationStart);\n"
"  if (opt.direction !== undefined) t.direction = String(opt.direction);\n"
"  if (opt.fill !== undefined) t.fill = String(opt.fill);\n"
"  if (opt.easing !== undefined) {\n"
"    var es = String(opt.easing);\n"
"    if (!isValidEasing(es)) throw new TypeError(\"invalid easing: '\" + es + \"'\");\n"
"    t.easing = parseEasing(es);\n"
"  }\n"
"};\n"
"AnimationEffect.prototype.getComputedTiming = function () {\n"
"  var t = this.__timing;\n"
"  var owner = this.__owner;\n"
"  var ct = owner ? owner.__hold : null;\n"
"  var d = (t.duration === 'auto') ? 0 : t.duration;\n"
"  var it = t.iterations;\n"
"  var effIt = (it > 0) ? it : ((it === 0) ? 0 : 1);\n"
/* duration here is the RESOLVED number (0 for 'auto'), unlike getTiming()'s
 * duration which stays the literal 'auto' -- Animatable/animate.html checks
 * both readbacks of the same effect and expects them to differ exactly this
 * way. */
"  return {\n"
"    delay: t.delay, endDelay: t.endDelay, fill: t.fill === 'auto' ? 'none' : t.fill,\n"
"    iterations: t.iterations, iterationStart: t.iterationStart,\n"
"    duration: d, direction: t.direction, easing: t.easing,\n"
"    activeDuration: d * effIt,\n"
"    localTime: ct, progress: this.__progress(), currentIteration: 0\n"
"  };\n"
"};\n"
"\n"
"function KeyframeEffect(target, keyframes, options) {\n"
/* The COPY constructor is a distinct overload, not this one with a shape
 * nobody built for it: `new KeyframeEffect(sourceEffect)` -- one argument,
 * and the first one is itself a KeyframeEffect. Distinguishing on argument
 * COUNT (arguments.length, not `keyframes === undefined`) matters because
 * `new KeyframeEffect(existingEffect, null)` -- an existing effect handed
 * in as a TARGET, which is legal (a KeyframeEffect is not an Element, so it
 * is a strange target, but nothing here forbids it) -- must NOT be treated
 * as a copy. copy-constructor.html's own corpus depends on the copy being a
 * true clone: mutating the copy's keyframes or timing must not be visible
 * through the source, which is why every field below is a fresh object,
 * not a shared reference. */
"  if (arguments.length === 1 && target instanceof KeyframeEffect) {\n"
"    var src = target;\n"
"    this.__target = src.__target;\n"
"    this.__pseudo = src.__pseudo;\n"
"    this.__owner = null;\n"
"    this.__composite = src.__composite;\n"
"    this.__iterationComposite = src.__iterationComposite;\n"
"    var st = src.__timing;\n"
"    this.__timing = {\n"
"      duration: st.duration, delay: st.delay, endDelay: st.endDelay,\n"
"      iterations: st.iterations, iterationStart: st.iterationStart,\n"
"      direction: st.direction, fill: st.fill, easing: st.easing\n"
"    };\n"
"    this.__kf = src.__kf.map(function (k) {\n"
"      var props = {};\n"
"      for (var p in k.props) props[p] = k.props[p];\n"
"      return { offset: k.offset, computedOffset: k.computedOffset, easing: k.easing,\n"
"               composite: k.composite, props: props };\n"
"    });\n"
"    return;\n"
"  }\n"
"  this.__target = (target === undefined || target === null) ? null : target;\n"
"  this.__pseudo = null;\n"
"  this.__owner = null;\n"
"  this.__composite = 'replace';\n"
"  this.__iterationComposite = 'replace';\n"
"  this.__timing = parseTiming(options);\n"
"  if (options !== undefined && options !== null && typeof options === 'object') {\n"
"    if (options.composite !== undefined) {\n"
"      var oc = String(options.composite);\n"
"      if (!isValidEffectComposite(oc)) throw new TypeError(\"invalid composite: '\" + oc + \"'\");\n"
"      this.__composite = oc;\n"
"    }\n"
"    if (options.iterationComposite !== undefined) {\n"
"      var oic = String(options.iterationComposite);\n"
"      if (!isValidIterationComposite(oic)) throw new TypeError(\"invalid iterationComposite: '\" + oic + \"'\");\n"
"      this.__iterationComposite = oic;\n"
"    }\n"
"    if (options.pseudoElement !== undefined) this.__pseudo = validatePseudo(options.pseudoElement);\n"
"  }\n"
"  /* Keyframes are processed AFTER options, so a throwing keyframes getter\n"
"   * (constructor.html's `{ get left(){ throw test_error } }` case) still\n"
"   * propagates the exact object the page threw -- nothing here catches it. */\n"
"  this.__kf = buildFrames(keyframes);\n"
"  try { this.__resolveValues(); } catch (e) {}\n"
"}\n"
"KeyframeEffect.prototype = Object.create(AnimationEffect.prototype);\n"
"KeyframeEffect.prototype.constructor = KeyframeEffect;\n"
/* Symbol.toStringTag on all four prototypes -- assert_class_string() checks
 * {}.toString.call(x) === '[object ClassName]', which is spec-derived from
 * every WebIDL interface getting one automatically; a plain constructor
 * function does not, and without this every one of those checks reports
 * '[object Object]' regardless of how correct the object underneath is. */
"AnimationEffect.prototype[Symbol.toStringTag] = 'AnimationEffect';\n"
"KeyframeEffect.prototype[Symbol.toStringTag] = 'KeyframeEffect';\n"
"\n"
"/* target is re-assignable, and re-assigning it has to (a) move this\n"
" * effect's owning Animation from the old target's registration to the\n"
" * new one's, so getComputedStyle keeps answering for the right element,\n"
" * and (b) re-resolve keyframe values against the new target, because\n"
" * `border-style: none` collapsing `border-top-width` to 0px is a property\n"
" * of the ELEMENT, and the old target's collapse says nothing about the\n"
" * new one's. */\n"
"Object.defineProperty(KeyframeEffect.prototype, 'target', {\n"
"  get: function () { return this.__target; },\n"
"  set: function (v) {\n"
"    var nv = (v === undefined || v === null) ? null : v;\n"
"    var old = this.__target;\n"
"    if (old === nv) return;\n"
"    var owner = this.__owner;\n"
"    this.__target = nv;\n"
"    if (owner) {\n"
"      if (old) {\n"
"        var l = listFor(old, false);\n"
"        if (l) { var i = l.indexOf(owner); if (i >= 0) l.splice(i, 1); }\n"
"      }\n"
"      if (nv) listFor(nv, true).push(owner);\n"
"    }\n"
"    try { this.__resolveValues(); } catch (e) {}\n"
"  },\n"
"  configurable: true, enumerable: true\n"
"});\n"
"Object.defineProperty(KeyframeEffect.prototype, 'pseudoElement', {\n"
"  get: function () { return this.__pseudo; },\n"
"  set: function (v) { this.__pseudo = validatePseudo(v); },\n"
"  configurable: true, enumerable: true\n"
"});\n"
"Object.defineProperty(KeyframeEffect.prototype, 'composite', {\n"
"  get: function () { return this.__composite; },\n"
"  set: function (v) {\n"
"    var s = String(v);\n"
"    if (!isValidEffectComposite(s)) throw new TypeError(\"invalid composite: '\" + s + \"'\");\n"
"    this.__composite = s;\n"
"  },\n"
"  configurable: true, enumerable: true\n"
"});\n"
"Object.defineProperty(KeyframeEffect.prototype, 'iterationComposite', {\n"
"  get: function () { return this.__iterationComposite; },\n"
"  set: function (v) {\n"
"    var s = String(v);\n"
"    if (!isValidIterationComposite(s)) throw new TypeError(\"invalid iterationComposite: '\" + s + \"'\");\n"
"    this.__iterationComposite = s;\n"
"  },\n"
"  configurable: true, enumerable: true\n"
"});\n"
"\n"
"/* getKeyframes() reports COMPUTED keyframes: offset (the specified value,\n"
" * possibly null), computedOffset (always a number), easing, composite, and\n"
" * one entry per animated property under its IDL (camelCase) name -- in that\n"
" * key set exactly, because keyframe-utils.js's assert_frames_equal compares\n"
" * Object.keys(...).sort(), so an extra or missing key fails the comparison\n"
" * even when every value that IS present is correct. */\n"
"KeyframeEffect.prototype.getKeyframes = function () {\n"
"  var out = [];\n"
"  for (var i = 0; i < this.__kf.length; i++) {\n"
"    var k = this.__kf[i];\n"
"    var o = { offset: k.offset, computedOffset: k.computedOffset, easing: k.easing, composite: k.composite };\n"
"    for (var p in k.props) o[idlName(p)] = k.props[p];\n"
"    out.push(o);\n"
"  }\n"
"  return out;\n"
"};\n"
"KeyframeEffect.prototype.setKeyframes = function (keyframes) {\n"
"  this.__kf = buildFrames(keyframes);\n"
"  try { this.__resolveValues(); } catch (e) {}\n"
"};\n"
"\n"
"/* The proportion through THIS iteration, eased -- or null when the effect\n"
" * is not in effect (before/after its active interval with no matching\n"
" * fill, or there is no owning Animation, or that Animation has no\n"
" * currentTime), which is how the overlay knows to stay silent. */\n"
"KeyframeEffect.prototype.__progress = function () {\n"
"  var t = this.__timing;\n"
"  var owner = this.__owner;\n"
"  var ct = owner ? owner.__hold : null;\n"
"  if (ct === null || (owner && owner.__state === 'idle')) return null;\n"
"  var d = (t.duration === 'auto') ? 0 : t.duration;\n"
"  var it = t.iterations;\n"
"  if (!(it > 0)) it = (it === 0) ? 0 : 1;\n"
"  var active = d * it;\n"
"  var local = ct - t.delay;\n"
"  var fill = t.fill === 'auto' ? 'none' : t.fill;\n"
"  if (local < 0) {\n"
"    if (fill !== 'backwards' && fill !== 'both') return null;\n"
"    local = 0;\n"
"  } else if (local >= active) {\n"
"    if (fill !== 'forwards' && fill !== 'both') return null;\n"
"    local = active;\n"
"  }\n"
"  var f;\n"
"  if (!(d > 0)) f = (local >= active) ? 1 : 0;\n"
"  else {\n"
"    f = local / d + t.iterationStart;\n"
"    var whole = Math.floor(f);\n"
"    f = f - whole;\n"
"    if (f === 0 && local >= active && active > 0) f = 1;\n"
"  }\n"
"  var dir = t.direction;\n"
"  if (dir === 'reverse') f = 1 - f;\n"
"  else if (dir === 'alternate' || dir === 'alternate-reverse') {\n"
"    var iter = (d > 0) ? Math.floor(local / d) : 0;\n"
"    if (dir === 'alternate-reverse') iter += 1;\n"
"    if (iter % 2) f = 1 - f;\n"
"  }\n"
#ifdef JS_ANIM_NEGCTL_CLAMP
"/* THE NEGATIVE CONTROL, and it is not \"delete animate()\" -- any test catches\n"
" * that. This is the sentence an implementation writes without thinking:\n"
" * progress is a fraction, so clamp it to [0, 1]. See the long-form version\n"
" * of this comment in git history / the file this replaced; the mechanism\n"
" * (createEasing() feeding cubic-bezier control points outside the unit\n"
" * square so a 0.5 input produces the OUTPUT `at` being tested) is\n"
" * unchanged by this rewrite. */\n"
"  var __p = easingFn(t.easing)(f);\n"
"  return __p < 0 ? 0 : (__p > 1 ? 1 : __p);\n"
"};\n"
#else
"  return easingFn(t.easing)(f);\n"
"};\n"
#endif
"\n"
/* Reads the RESOLVED value when __resolveValues() has computed one, else
 * falls back to the specified value untouched -- membership (`prop in
 * kf[i].props`) is still decided by the specified set, because a property
 * that failed to resolve (target is null, or the read came back "") stays
 * a real keyframe property with its original string, not a missing one. */
"function kfval(k, prop) {\n"
"  return (k.resolved && (prop in k.resolved)) ? k.resolved[prop] : k.props[prop];\n"
"}\n"
"KeyframeEffect.prototype.__valueAt = function (prop) {\n"
"  var kf = this.__kf;\n"
"  if (!kf.length) return null;\n"
"  var p = this.__progress();\n"
"  if (p === null) return null;\n"
"  var lo = -1, hi = -1, i;\n"
"  for (i = 0; i < kf.length; i++) if (prop in kf[i].props) { if (lo < 0) lo = i; hi = i; }\n"
"  if (lo < 0) return null;\n"
"  if (lo === hi) return kfval(kf[lo], prop);\n"
"  var a = lo, b = hi;\n"
"  for (i = lo; i <= hi; i++) {\n"
"    if (!(prop in kf[i].props)) continue;\n"
"    if (kf[i].computedOffset <= p) a = i;\n"
"  }\n"
"  b = a;\n"
"  for (i = a + 1; i <= hi; i++) if (prop in kf[i].props) { b = i; break; }\n"
"  if (b === a) { a = lo; for (i = lo + 1; i <= hi; i++) if (prop in kf[i].props) { b = i; break; } }\n"
"  if (b === a) return kfval(kf[a], prop);\n"
"  var span = kf[b].computedOffset - kf[a].computedOffset;\n"
"  var lp = span > 0 ? (p - kf[a].computedOffset) / span : (p < kf[a].computedOffset ? 0 : 1);\n"
"  var v = II(prop, kfval(kf[a], prop), kfval(kf[b], prop), lp);\n"
"  if (v === null || v === undefined) return v;\n"
"  return this.__iterAccum(prop, v, kfval(kf[hi], prop));\n"
"};\n"
"\n"
"/* iterationComposite: 'accumulate' -- a property of the EFFECT now, not of\n"
" * the timing dict, matching where the constructor and the accessor both put\n"
" * it. Still inert against this corpus for the reason the original comment\n"
" * gave: interpolation-testcommon.js runs one 100s iteration and never\n"
" * advances past it, so this branch has no case here that exercises it --\n"
" * implemented anyway, because an accepted-and-ignored iterationComposite is\n"
" * worse than a rejected one. */\n"
"KeyframeEffect.prototype.__iterAccum = function (prop, v, lastv) {\n"
"  if (this.__iterationComposite !== 'accumulate') return v;\n"
"  if (lastv === undefined || lastv === null) return v;\n"
"  var t = this.__timing;\n"
"  var d = (t.duration === 'auto') ? 0 : t.duration;\n"
"  if (!(d > 0)) return v;\n"
"  var owner = this.__owner;\n"
"  var ct = owner ? owner.__hold : null;\n"
"  if (ct === null) return v;\n"
"  var local = ct - t.delay;\n"
"  var it = Math.floor(local / d + t.iterationStart);\n"
"  if (!(it > 0)) return v;\n"
"  if (it > 1000) it = 1000;\n"
"  for (var i = 0; i < it; i++) {\n"
"    var a = null;\n"
"    try { a = CC(prop, lastv, v, 'accumulate'); } catch (e) { a = null; }\n"
"    if (a === null || a === undefined) break;\n"
"    v = a;\n"
"  }\n"
"  return v;\n"
"};\n"
"\n"
"/* The composite operation in force for keyframe `i`: the keyframe's own if\n"
" * it declares one other than 'auto', otherwise the effect's `.composite`,\n"
" * otherwise 'replace'. */\n"
"KeyframeEffect.prototype.__opOf = function (i) {\n"
"  var k = this.__kf[i];\n"
"  var c = k ? k.composite : 'auto';\n"
"  if (c === 'replace' || c === 'add' || c === 'accumulate') return c;\n"
"  var e = this.__composite;\n"
"  return (e === 'add' || e === 'accumulate') ? e : 'replace';\n"
"};\n"
"\n"
"/* ---- keyframe values are COMPUTED values, not the strings handed in ----\n"
" * Unchanged in substance from the version this replaces (see git history\n"
" * for the full derivation, including the 275-subtest regression this\n"
" * exists to prevent and the 205-subtest regression restricting it to\n"
" * kf.length >= 2 exists to prevent) -- only the home changed, from\n"
" * Animation.prototype to KeyframeEffect.prototype, because the values\n"
" * being resolved belong to the effect and must survive a target\n"
" * re-assignment that leaves the owning Animation untouched. */\n"
"function resolveOn(el, prop, value) {\n"
"  var st = el.style;\n"
"  if (!st || typeof st.setProperty !== 'function') return value;\n"
"  var had, pri = '';\n"
"  try {\n"
"    had = st.getPropertyValue(prop);\n"
"    if (typeof st.getPropertyPriority === 'function') pri = st.getPropertyPriority(prop);\n"
"  } catch (e) { return value; }\n"
"  var out = value;\n"
"  try {\n"
"    st.setProperty(prop, value);\n"
"    var c = gcs.call(globalThis, el).getPropertyValue(prop);\n"
"    if (c !== '' && c !== null && c !== undefined) out = c;\n"
"  } catch (e2) {}\n"
"  try {\n"
"    if (had === '' || had === null || had === undefined) st.removeProperty(prop);\n"
"    else st.setProperty(prop, had, pri);\n"
"  } catch (e3) {}\n"
"  return out;\n"
"}\n"
"\n"
"function ensureResolved(k) { if (!k.resolved) k.resolved = {}; return k.resolved; }\n"
"KeyframeEffect.prototype.__resolveValues = function () {\n"
"  var el = this.__target, i, p;\n"
/* Every call starts from a clean slate: target re-assignment and
 * setKeyframes() both re-run this, and a `resolved` entry left over from
 * the PREVIOUS target/keyframes would silently outlive the state it was
 * computed from. */
"  var kf0 = this.__kf;\n"
"  for (i = 0; i < kf0.length; i++) kf0[i].resolved = null;\n"
"  if (!el || !el.style) return;\n"
"  var par = el.parentNode;\n"
"  var kf = this.__kf, jobs = [];\n"
"  if (kf.length >= 2 && par && typeof el.cloneNode === 'function') {\n"
"    for (i = 0; i < kf.length; i++) {\n"
"      var any = false; for (p in kf[i].props) { any = true; break; }\n"
"      if (!any) continue;\n"
"      var s = null;\n"
"      try { s = el.cloneNode(false); } catch (e) { s = null; }\n"
"      if (!s || !s.style) continue;\n"
"      try { s.removeAttribute('id'); } catch (e1) {}\n"
"      var ok = true;\n"
"      for (p in kf[i].props) { try { s.style.setProperty(p, kf[i].props[p]); } catch (e2) { ok = false; } }\n"
"      if (!ok) continue;\n"
"      try { par.insertBefore(s, el.nextSibling); } catch (e3) { continue; }\n"
"      jobs.push({ i: i, s: s });\n"
"    }\n"
"  }\n"
"  var base = {}, needBase = false;\n"
"  for (i = 0; i < kf.length; i++)\n"
"    for (p in kf[i].props) { base[p] = ''; needBase = true; }\n"
"  if (needBase) {\n"
"    try {\n"
"      var bcs = gcs.call(globalThis, el);\n"
"      for (p in base) { try { base[p] = bcs.getPropertyValue(p); } catch (eb) { base[p] = ''; } }\n"
"    } catch (eb2) {}\n"
"  }\n"
"\n"
"  for (var j = 0; j < jobs.length; j++) {\n"
"    var cs = null;\n"
"    try { cs = gcs.call(globalThis, jobs[j].s); } catch (e4) { cs = null; }\n"
"    if (!cs) continue;\n"
"    var props = kf[jobs[j].i].props;\n"
"    var res = ensureResolved(kf[jobs[j].i]);\n"
"    jobs[j].done = {};\n"
"    for (p in props) {\n"
"      var c = '';\n"
"      try { c = cs.getPropertyValue(p); } catch (e5) { c = ''; }\n"
"      if (c === '' || c === null || c === undefined) continue;\n"
"      res[p] = c;\n"
"      jobs[j].done[p] = 1;\n"
"    }\n"
"  }\n"
"  for (j = 0; j < jobs.length; j++) {\n"
"    try { jobs[j].s.parentNode.removeChild(jobs[j].s); } catch (e6) {}\n"
"  }\n"
"  var inlineWins = {};\n"
"  for (p in base) {\n"
"    var iv = '';\n"
"    try { iv = el.style.getPropertyValue(p); } catch (e7) { iv = ''; }\n"
"    inlineWins[p] = (iv !== '' && iv !== null && iv !== undefined && iv === base[p]);\n"
"  }\n"
"  for (i = 0; i < kf.length; i++) {\n"
"    var got = null;\n"
"    for (j = 0; j < jobs.length; j++) if (jobs[j].i === i) got = jobs[j].done;\n"
"    for (p in kf[i].props) {\n"
"      if (got && got[p]) continue;\n"
"      if (inlineWins[p]) continue;\n"
"      ensureResolved(kf[i])[p] = resolveOn(el, p, kf[i].props[p]);\n"
"    }\n"
"  }\n"
"\n"
"  for (i = 0; i < kf.length; i++) {\n"
"    var op = this.__opOf(i);\n"
"    if (op === 'replace') continue;\n"
"    for (p in kf[i].props) {\n"
"      var u = base[p];\n"
"      if (u === '' || u === null || u === undefined) continue;\n"
"      var cv = null;\n"
"      try { cv = CC(p, u, kfval(kf[i], p), op); } catch (ec) { cv = null; }\n"
"      if (cv !== null && cv !== undefined) ensureResolved(kf[i])[p] = cv;\n"
"    }\n"
"  }\n"
"};\n"
"\n"
"if (typeof globalThis !== 'undefined') {\n"
"  if (!globalThis.AnimationEffect) globalThis.AnimationEffect = AnimationEffect;\n"
"  if (!globalThis.KeyframeEffect) globalThis.KeyframeEffect = KeyframeEffect;\n"
"}\n"
"\n"
"/* ---- DocumentTimeline ----------------------------------------------------\n"
" * A REAL clock (performance.now(), falling back to Date.now()), not a\n"
" * frozen 0 -- constructor.html builds a second DocumentTimeline with a\n"
" * non-zero originTime and checks it lags/leads document.timeline by exactly\n"
" * that amount, which only holds if both read the same moving clock. What is\n"
" * still true, and was true of the file this replaces: nothing here ADVANCES\n"
" * an Animation's own currentTime from this clock -- an Animation's\n"
" * currentTime stays exactly what play()/pause()/the setter leave it at,\n"
" * because nothing in this engine drives a frame loop that would tick it,\n"
" * and inventing that tick is a materially different (and untested) feature\n"
" * from exposing the clock a real timeline reads. */\n"
"function __clockNow() {\n"
"  return (typeof performance !== 'undefined' && typeof performance.now === 'function')\n"
"    ? performance.now() : Date.now();\n"
"}\n"
"function DocumentTimeline(options) {\n"
"  var origin = 0;\n"
"  if (options && options.originTime !== undefined) origin = Number(options.originTime) || 0;\n"
"  this.__origin = origin;\n"
"}\n"
"Object.defineProperty(DocumentTimeline.prototype, 'currentTime', {\n"
"  get: function () { return __clockNow() - this.__origin; },\n"
"  configurable: true, enumerable: true\n"
"});\n"
"DocumentTimeline.prototype[Symbol.toStringTag] = 'DocumentTimeline';\n"
"if (typeof globalThis !== 'undefined' && !globalThis.DocumentTimeline) globalThis.DocumentTimeline = DocumentTimeline;\n"
"var __DEFAULT_TIMELINE = new DocumentTimeline();\n"
"if (typeof document !== 'undefined' && document && !document.timeline) document.timeline = __DEFAULT_TIMELINE;\n"
"\n"
"/* ======================================================================\n"
" * Animation\n"
" * ====================================================================== */\n"
"function Animation(effect, timeline) {\n"
"  this.effect = (effect === undefined) ? null : effect;\n"
"  if (this.effect) this.effect.__owner = this;\n"
"  this.timeline = (timeline === undefined) ? __DEFAULT_TIMELINE : timeline;\n"
"  this.__hold = 0;\n"
"  this.__state = 'running';\n"
"  this.id = '';\n"
"  this.playbackRate = 1;\n"
"  this.startTime = null;\n"
"  if (this.effect && this.effect.__target) listFor(this.effect.__target, true).push(this);\n"
"}\n"
"var REG = new WeakMap();\n"
"function listFor(el, make) {\n"
"  var l = REG.get(el);\n"
"  if (!l && make) { l = []; REG.set(el, l); }\n"
"  return l;\n"
"}\n"
"Object.defineProperty(Animation.prototype, 'currentTime', {\n"
"  get: function () { return this.__hold; },\n"
"  set: function (v) { this.__hold = (v === null || v === undefined) ? null : Number(v); },\n"
"  configurable: true, enumerable: true\n"
"});\n"
"Object.defineProperty(Animation.prototype, 'playState', {\n"
"  get: function () { return this.__state; }, configurable: true, enumerable: true\n"
"});\n"
"Animation.prototype.pause = function () { this.__state = 'paused'; };\n"
"Animation.prototype.play = function () { this.__state = 'running'; if (this.__hold === null) this.__hold = 0; };\n"
"Animation.prototype.finish = function () {\n"
"  var t = this.effect ? this.effect.__timing : null;\n"
"  if (!t) { this.__state = 'finished'; return; }\n"
"  var d = (t.duration === 'auto') ? 0 : t.duration;\n"
"  this.__hold = t.delay + d * (t.iterations > 0 ? t.iterations : 1);\n"
"  this.__state = 'finished';\n"
"};\n"
"Animation.prototype.cancel = function () {\n"
"  this.__state = 'idle'; this.__hold = null;\n"
"  var el = this.effect ? this.effect.__target : null;\n"
"  var l = el ? listFor(el, false) : null;\n"
"  if (l) { var i = l.indexOf(this); if (i >= 0) l.splice(i, 1); }\n"
"};\n"
"Animation.prototype.reverse = function () { this.playbackRate = -this.playbackRate; };\n"
"Animation.prototype.updatePlaybackRate = function (r) { this.playbackRate = Number(r); };\n"
"Animation.prototype.commitStyles = function () {};\n"
"Animation.prototype.persist = function () {};\n"
"Animation.prototype.addEventListener = function () {};\n"
"Animation.prototype.removeEventListener = function () {};\n"
"Object.defineProperty(Animation.prototype, 'finished', {\n"
"  get: function () { return Promise.resolve(this); }, configurable: true\n"
"});\n"
"Object.defineProperty(Animation.prototype, 'ready', {\n"
"  get: function () { return Promise.resolve(this); }, configurable: true\n"
"});\n"
"Animation.prototype[Symbol.toStringTag] = 'Animation';\n"
"if (typeof globalThis !== 'undefined' && !globalThis.Animation) globalThis.Animation = Animation;\n"
"\n"
/* KeyframeAnimationOptions extends KeyframeEffectOptions with exactly two
 * fields THIS method reads and the KeyframeEffect constructor above never
 * sees: `id` and `timeline`. Both are read off the SAME options object
 * passed to `new KeyframeEffect(...)` -- harmless, because the effect
 * constructor only ever looks at the members it knows about and ignores
 * the rest. `timeline` is checked with `in`, not `!== undefined`, so that
 * an explicit `{ timeline: null }` (animate.html's own case) is
 * distinguished from an absent one: the former means "no timeline", the
 * latter means "the default document timeline". */
"EP.animate = function (keyframes, options) {\n"
"  var effect = new KeyframeEffect(this, keyframes, options);\n"
"  var timeline = __DEFAULT_TIMELINE;\n"
"  var id = '';\n"
"  if (options !== undefined && options !== null && typeof options === 'object') {\n"
"    if ('timeline' in options) timeline = options.timeline;\n"
"    if (options.id !== undefined) id = String(options.id);\n"
"  }\n"
"  var a = new Animation(effect, timeline);\n"
"  a.id = id;\n"
"  return a;\n"
"};\n"
"EP.getAnimations = function () { var l = listFor(this, false); return l ? l.slice() : []; };\n"
"\n"
"/* ---- the computed-style overlay ----------------------------------------\n"
" * Unchanged from the version this replaces except that a list entry is now\n"
" * an Animation whose VALUE comes from `.effect`, not from the animation\n"
" * object itself -- see the long-form comment this section carried before\n"
" * (Proxy over copying the ~63-accessor CSSStyleDeclaration; methods bound to\n"
" * the target, not the proxy, because they are natives that read an opaque\n"
" * pointer off `this`). */\n"
"function wrap(base, el) {\n"
"  return new Proxy(base, {\n"
"    get: function (t, k, r) {\n"
"      if (k === 'getPropertyValue') return function (p) {\n"
"        var d = dash(p);\n"
"        var v = animVal(el, d, t);\n"
"        return (v !== null) ? v : t.getPropertyValue(p);\n"
"      };\n"
"      if (typeof k === 'string' && k !== 'cssText' && k !== 'length' && k !== 'item') {\n"
"        var v2 = animVal(el, dash(k), t);\n"
"        if (v2 !== null) return v2;\n"
"      }\n"
"      var val = Reflect.get(t, k);\n"
"      return (typeof val === 'function') ? val.bind(t) : val;\n"
"    }\n"
"  });\n"
"}\n"
"function animVal(el, prop, base) {\n"
"  var l = listFor(el, false);\n"
"  if (!l || !l.length) return null;\n"
"  for (var i = l.length - 1; i >= 0; i--) {\n"
"    var anim = l[i];\n"
"    if (!anim.effect) continue;\n"
"    var v = anim.effect.__valueAt(prop);\n"
"    if (v === null || v === undefined) continue;\n"
"    if (base.getPropertyValue(prop) === '') return null;\n"
"    return v;\n"
"  }\n"
"  return null;\n"
"}\n"
"var origGCS = gcs;\n"
"function patched(el, pseudo) {\n"
"  var base = origGCS.call(this === undefined ? globalThis : this, el, pseudo);\n"
"  if (pseudo) return base;\n"
"  var l = (el && typeof el === 'object') ? listFor(el, false) : null;\n"
"  if (!l || !l.length) return base;\n"
"  try { return wrap(base, el); } catch (e) { return base; }\n"
"}\n"
"if (typeof globalThis !== 'undefined') globalThis.getComputedStyle = patched;\n"
"if (typeof window !== 'undefined') window.getComputedStyle = patched;\n"
"\n"
"if (typeof document !== 'undefined' && document && !document.getAnimations) {\n"
"  document.getAnimations = function () { return []; };\n"
"}\n"
"\n"
"})();\n";

void js_anim_install(JSContext *ctx)
{
    if (!ctx) return;
    JSValue g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "__anim_composite",
                      JS_NewCFunction(ctx, js_anim_composite, "__anim_composite", 4));
    JS_SetPropertyStr(ctx, g, "__anim_interp",
                      JS_NewCFunction(ctx, js_anim_interp, "__anim_interp", 4));
    JS_FreeValue(ctx, g);

    JSValue r = JS_Eval(ctx, ANIM_JS, sizeof ANIM_JS - 1, "<js_anim>",
                        JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        /* Loud, for the reason js_events.c gives: a prelude that fails to
         * install leaves the page with no animate() and nothing says so, which
         * is indistinguishable from this file not existing. */
        JSValue e = JS_GetException(ctx);
        const char *s = JS_ToCString(ctx, e);
        fprintf(stderr, "js_anim: install failed: %s\n", s ? s : "(unprintable)");
        if (s) JS_FreeCString(ctx, s);
        JSValue st = JS_GetPropertyStr(ctx, e, "stack");
        if (!JS_IsUndefined(st)) {
            const char *ss = JS_ToCString(ctx, st);
            if (ss) { fprintf(stderr, "%s\n", ss); JS_FreeCString(ctx, ss); }
        }
        JS_FreeValue(ctx, st);
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, r);
}

/* ======================================================================
 * PART 2 -- the CSS animation/transition clock
 *
 * Everything above this line is the WAAPI surface (Element.animate, a JS
 * prelude over two natives). Everything below is the CSS half: `animation:`
 * and `transition:` driven by a real clock instead of approximated to their
 * static END-STATE, which is what this tree did before 2026-08-30 because
 * nothing drove frames (see the corrected comment at css_extra.c's
 * walk_anim).
 *
 * THE ARCHITECTURE IN ONE PARAGRAPH, so the seams below read as design and
 * not omission:
 *
 *   - ONE clock. There is no second timer anywhere in this half. The tick
 *     (css_anim_tick) is a consumer on the page's EXISTING deadline queue
 *     in js_page.c -- the same queue setTimeout/rAF live in, the same
 *     injected monotonic clock, the same computed sleep in browser.c's
 *     main loop. js_page_pending() asks css_anim_active(), js_page_next_due()
 *     merges css_anim_next_due(), js_page_run_due() calls the tick. An
 *     idle page with no animation and no timer costs exactly what it did
 *     before this file grew a second half.
 *
 *   - The engine owns no rendering. It overlays the CURRENT animated value
 *     onto the same cstyle fields the painter already reads --
 *     st->opacity and st->xraw[XR_TRANSFORM] -- which is the C-side twin
 *     of the WAAPI overlay at the top of this file (that one patches the
 *     computed-style ANSWER a page reads; this one patches the style the
 *     painter sees). browser_paint.c needed no change, by construction.
 *
 *   - Capture is css_extra.c's: @keyframes into the css_kf table, the
 *     `animation`/`transition` SHORTHAND spans into cstyle.anim_raw/
 *     trans_raw. This file resolves spans into entries and COPIES the two
 *     endpoint values it interpolates between, because the spans point
 *     into css_extra's private sheet copy, which a viewport change frees
 *     and recompiles mid-flight; css_extra_sheet_gen() is how an entry
 *     notices and rebuilds.
 *
 * WHAT ANIMATES: opacity and transform, and nothing else. Both are
 * paint-time values; transform is read LIVE by the painter (no relayout
 * per frame), opacity is snapshotted into the display list at layout (so
 * an opacity tick costs one layout -- css_anim_needs_layout() tells
 * browser.c which kind of frame it owes). Every other property a
 * @keyframes rule may name -- width, margin, color, border-radius, ... --
 * keeps the element's CASCADE BASE value, which is the same end-state-
 * shaped answer the pre-clock tree gave. The refusal is deliberate and
 * gated (tests/fixtures/anim/static.html animates margin-left and must
 * not move): each layout-affecting property added here buys a per-frame
 * relayout, and the cap below exists precisely because relayout is what
 * an animation costs.
 *
 * THE CAP, and the degradation past it, stated rather than implied: at
 * most CSS_ANIM_CAP (32) concurrent entries exist. Elements past the cap
 * -- first come in the note walk's document order, so the degradation is
 * deterministic -- are never adopted: they keep their cascade base value,
 * which is exactly what CSS says a finished fill:none animation leaves
 * behind, so over-cap degrades to "animation already over" rather than to
 * jank, and the [css-anim] cap line on serial says so. 32 was set from
 * the guest measurement in tests/fixtures/anim/cap.html (delivered rAF
 * frames per 1.5 s at 0/8/32/96/192 simultaneous animations); the gate's
 * report carries the numbers.
 *
 * LIFETIME, both halves enforced here: entries hold node pointers and
 * navigation frees the DOM arena out from under them, so css_anim_reset()
 * drops everything WITHOUT touching the nodes (browser.c calls it before
 * the teardown dom_free). A node destroyed between navigations is caught
 * by the DOM's own generation stamp -- node->serial, the same mechanism
 * JS wrappers use; a recycled slot's serial differs, and a freed-but-
 * unrecycled node is merely detached, so overlaying it paints nothing.
 *
 * NOT DONE, named: per-keyframe `animation-timing-function` (the
 * animation-level easing is applied to the whole iteration, which is
 * exact for the two-stop rules real pages ship), animation events
 * (animationstart/end/iteration stay absent in BOTH halves of this file),
 * compositing a CSS animation against a WAAPI effect (the two clocks do
 * not read each other), and longhand-only animation/transition
 * declarations (the capture takes the shorthand; a longhand-only sheet
 * keeps the end-state behaviour -- css_extra.c states why).
 * ====================================================================== */

#include "css.h"
#include "dom.h"
#include "../../../include/weaksym.h"

/* The page's monotonic clock, css_extra's tables and the root font size:
 * all live in files a host link of this TU does not carry (js_page.c,
 * css_extra.c, css_engine.c), so every call is guarded with LOGIT_HAVE --
 * calling a stub is a SIGTRAP on the Mach-O host, not a quiet 0 (read
 * include/weaksym.h). The host checker drives the tick with an explicit
 * `now`, so a frozen 0 clock costs it nothing. */
unsigned long long js_page_now_ms(void) LOGIT_WEAK;
LOGIT_WEAK_STUB(js_page_now_ms);
int css_keyframes_find(const char *name, int len, const struct css_kf **out) LOGIT_WEAK;
LOGIT_WEAK_STUB(css_keyframes_find);
int css_extra_sheet_gen(void) LOGIT_WEAK;
LOGIT_WEAK_STUB(css_extra_sheet_gen);
int css_root_px(void) LOGIT_WEAK;
LOGIT_WEAK_STUB(css_root_px);

static unsigned long long clk_now(void)
{
    return LOGIT_HAVE(js_page_now_ms) ? js_page_now_ms() : 0;
}

static int root_px_now(void)
{
    return LOGIT_HAVE(css_root_px) ? css_root_px() : 16;
}

/* One tick per 20 ms. The clock advances in 10 ms steps and rAF's own
 * target (FRAME_MS 16 in js_page.c) rounds up to the same 20 ms in
 * practice, so this rides the page's existing animation rhythm instead of
 * inventing a faster one the compositor cannot keep. */
#define CANIM_FRAME_MS 20

/* One endpoint value, copied out of the sheet at entry creation. 128
 * bytes holds a transform list of about eight functions; the longest in
 * tests/fixtures/cssweb is four. A longer one declines that entry's
 * transform (opacity is unaffected) rather than truncating it -- a cut
 * transform list is a DIFFERENT animation, silently. */
#define CANIM_VAL 128

/* animation-direction: enum CAD_* and struct anim_spec/trans_spec live in
 * css.h beside their prototypes -- the host checker drives the parsers. */

/* A property-specific keyframe list: the subset of a rule's stops that
 * declare the property, with implicit 0%/100% ends filled from the
 * element's cascade base (CSS Animations 5.4: missing from/to are the
 * underlying value). The +2 in the arrays covers those two ends. */
struct pkf { int off; char val[CANIM_VAL]; int len; };

struct canim {
    struct node *node;
    uint32_t     serial;                 /* liveness: dom.c's generation stamp */
    int          gen;                    /* css_extra_sheet_gen at adoption */
    const char  *anim_raw_at;            /* the span adopted from; a different
                                          * span means a different rule -> restart */
    int          anim_rawlen_at;

    /* The animation half. */
    int          is_anim;
    struct anim_spec as;
    int          n_op, n_xf;             /* 0 = the property is not animated */
    struct pkf   op[CSS_KF_MAXSTOP + 2];
    struct pkf   xf[CSS_KF_MAXSTOP + 2];
    unsigned long long t0;
    unsigned long long acc;              /* active ms; paused spans excluded */
    unsigned long long last_now;
    int          anim_done;              /* finished; overlay held iff fill_fwd */

    /* The transition half. is_trans entries WATCH the element even when
     * idle -- the trigger is a future cascade the clock cannot see coming,
     * and css_anim_snapshot() keeps the "from" the change will need. */
    int          is_trans;
    struct trans_spec t_op_spec, t_xf_spec;
    char         w_op[16]; int w_op_n;   /* the pre-cascade snapshot */
    char         w_xf[CANIM_VAL]; int w_xf_n;
    int          trans_op, trans_xf;     /* transitions in flight */
    unsigned long long tt0;
    char         tf_op[16], tt_op[16];
    char         tf_xf[CANIM_VAL], tt_xf[CANIM_VAL];
    int          tf_xf_n, tt_xf_n;

    /* The overlay actually applied, and the base to restore to. */
    int          ov_op, ov_xf;
    int          last_op;                /* last written st->opacity */
    char         ov_xfbuf[CANIM_VAL]; int ov_xflen;
    int          base_op;
    char         base_xf[CANIM_VAL]; int base_xf_len; int base_had_xf;
};

static struct canim g_ca[CSS_ANIM_CAP];
static int          g_ncan;
static int          g_frozen;            /* elements past the cap this sheet */
static int          g_frozen_said;       /* the loud line, once per sheet */
static unsigned long long g_next_frame;  /* next tick boundary, monotonic ms */
static int          g_dirty;             /* a tick changed a pixel value */
static int          g_need_layout;       /* ...and it was an OPACITY change */

/* ---- small scanners (css_extra's equivalents are static; duplicating
 * eight lines is cheaper than widening a link boundary for a scanner that
 * must agree with the CSS grammar anyway) --------------------------------- */

static int ca_ws(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static int ca_id(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                 (c >= '0' && c <= '9') || c == '-' || c == '_'; }

static int ca_num(const char *s, int len, double *out)
{
    int i = 0, seen = 0, neg = 0;
    double v = 0;
    while (i < len && ca_ws(s[i])) i++;
    if (i < len && (s[i] == '+' || s[i] == '-')) { neg = s[i] == '-'; i++; }
    while (i < len && s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); i++; seen = 1; }
    if (i < len && s[i] == '.') {
        i++;
        double sc = 0.1;
        while (i < len && s[i] >= '0' && s[i] <= '9') {
            if (i < 60) v += (s[i] - '0') * sc;   /* precision floor, not a cap */
            sc *= 0.1; i++; seen = 1;
        }
    }
    if (!seen) return 0;
    if (i < len && (s[i] == 'e' || s[i] == 'E')) {
        int j = i + 1, en = 0, eneg = 0;
        if (j < len && (s[j] == '+' || s[j] == '-')) { eneg = s[j] == '-'; j++; }
        int k = j;
        while (k < len && s[k] >= '0' && s[k] <= '9') { en = en * 10 + (s[k] - '0'); k++; }
        if (k > j) {
            double m = 1.0;
            for (int q = 0; q < en && q < 300; q++) m *= 10.0;
            v = eneg ? v / m : v * m;
            i = k;
        }
    }
    *out = neg ? -v : v;
    return 1;
}

/* Top-level whitespace/comma tokens (outside parens, so
 * `cubic-bezier(.4,0,.6,1)` is one token). */
static int ca_tokens(const char *v, int len, int *ts, int *te, int max)
{
    int n = 0, i = 0, depth = 0;
    while (i < len && n < max) {
        while (i < len && (ca_ws(v[i]) || v[i] == ',')) i++;
        if (i >= len) break;
        int s = i;
        for (; i < len; i++) {
            char c = v[i];
            if (depth == 0 && (ca_ws(c) || c == ',')) break;
            if (c == '(') depth++;
            else if (c == ')') depth--;
        }
        int e = i;
        while (e > s && ca_ws(v[e-1])) e--;
        if (e > s) { ts[n] = s; te[n] = e; n++; }
    }
    return n;
}

/* `2s` / `350ms` -> ms. 1 on success. */
static int ca_time(const char *v, int len, double *ms)
{
    double d;
    if (!ca_num(v, len, &d)) return 0;
    int i = 0;
    while (i < len && (ca_ws(v[i]) || v[i] == '+' || v[i] == '-' || v[i] == '.' ||
                       v[i] == 'e' || v[i] == 'E' || (v[i] >= '0' && v[i] <= '9'))) {
        if ((v[i] == 'e' || v[i] == 'E') && i + 1 < len &&
            (v[i+1] == '+' || v[i+1] == '-' || (v[i+1] >= '0' && v[i+1] <= '9'))) { i += 2; continue; }
        i++;
    }
    if (len - i == 1 && (v[i] == 's' || v[i] == 'S')) { *ms = d * 1000.0; return 1; }
    if (len - i == 2 && (v[i] == 'm' || v[i] == 'M') && (v[i+1] == 's' || v[i+1] == 'S')) {
        *ms = d;
        return 1;
    }
    return 0;
}

/* ---- the `animation` shorthand -------------------------------------------
 *
 * Position-independent grammar (CSS Animations 1 4.8): the first <time> is
 * the duration, a second is the delay; everything else is recognised by
 * its own keyword; the first bare <custom-ident> is the animation-name. A
 * SECOND bare ident is ambiguous and refused (-1), which degrades to the
 * end-state fallback rather than guessing which ident was meant.
 *
 * Exported (css.h) so the host checker can drive it without a DOM. */
int css_anim_parse_animation(const char *v, int len, struct anim_spec *out)
{
    if (!v || len <= 0 || !out) return -1;
    memset(out, 0, sizeof *out);
    out->ease.kind = CI_EASE_LINEAR;          /* CSS initial */
    int ts[16], te[16];
    int n = ca_tokens(v, len, ts, te, 16);
    int ndur = 0, nname = 0, niter = 0;
    for (int i = 0; i < n; i++) {
        const char *t = v + ts[i];
        int tl = te[i] - ts[i];
        double ms;
        if (ca_time(t, tl, &ms)) {
            if (ndur == 0) { out->dur_ms = ms; ndur = 1; }
            else if (ndur == 1) { out->delay_ms = ms; ndur = 2; }
            else return -1;                   /* three times: not our grammar */
            continue;
        }
        if (ci_ease_parse(t, tl, &out->ease) == 0) continue;
        if (tl == 6 && !memcmp(t, "normal", 6)) { out->dir = CAD_NORMAL; continue; }
        if (tl == 7 && !memcmp(t, "reverse", 7)) { out->dir = CAD_REVERSE; continue; }
        if (tl == 9 && !memcmp(t, "alternate", 9)) { out->dir = CAD_ALTERNATE; continue; }
        if (tl == 17 && !memcmp(t, "alternate-reverse", 17)) { out->dir = CAD_ALTERNATE_REV; continue; }
        if (tl == 4 && !memcmp(t, "none", 4)) continue;
        if (tl == 8 && !memcmp(t, "forwards", 8)) { out->fill_fwd = 1; continue; }
        if (tl == 9 && !memcmp(t, "backwards", 9)) { out->fill_bwd = 1; continue; }
        if (tl == 4 && !memcmp(t, "both", 4)) { out->fill_fwd = out->fill_bwd = 1; continue; }
        if (tl == 7 && !memcmp(t, "running", 7)) continue;
        if (tl == 6 && !memcmp(t, "paused", 6)) { out->paused = 1; continue; }
        if (tl == 8 && !memcmp(t, "infinite", 8)) { out->infinite = 1; continue; }
        /* A bare number is ALWAYS the iteration count, never the name:
         * <custom-ident> cannot start with a digit, so there is nothing to
         * disambiguate. [The recovered half-edit gated this on
         * `!out->has_name`, which refused `slide 350ms ease-in-out .5s 3
         * alternate` -- the count arriving after the name is the spelling
         * real sheets use; caught by check_anim_clock.c's keyword-soup
         * row, red on its first run. A SECOND number is invalid grammar.]
         * A negative count is not <number> per the animations spec and is
         * refused rather than absolute-valued. */
        double cnt;
        if (ca_num(t, tl, &cnt)) {
            if (niter) return -1;
            if (cnt < 0) return -1;
            out->iters = cnt;
            niter = 1;
            continue;
        }
        if (nname) return -1;                 /* two idents: ambiguous, refuse */
        for (int k = 0; k < tl; k++) if (!ca_id(t[k])) return -1;
        if (tl >= CSS_KF_NAME) return -1;     /* longer than the table stores */
        memcpy(out->name, t, (size_t)tl);
        out->name[tl] = 0;
        out->has_name = 1;
        nname = 1;
    }
    /* iteration-count defaults to 1 only when UNSPECIFIED: a spelled
     * `animation: x 2s 0` is legal CSS and runs zero iterations (the old
     * `iters <= 0 -> 1` clamp silently animated what the author turned
     * off); anim_progress already treats iter >= iters as finished. */
    if (!niter && !out->infinite) out->iters = 1;
    return 0;
}

/* ---- the `transition` shorthand, one property's item ----------------------
 *
 * A comma list; a property is covered by the item that NAMES it, or by an
 * `all` item. The first matching item wins, which is source order and not
 * a choice. Returns 0 with *out filled when covered, -1 when the property
 * is not transitioned at all. `none` covers nothing. */
int css_anim_parse_transition(const char *v, int len, const char *prop,
                              struct trans_spec *out)
{
    if (!v || len <= 0 || !prop || !out) return -1;
    memset(out, 0, sizeof *out);
    out->ease.kind = CI_EASE_LINEAR;
    int i = 0;
    while (i < len) {
        while (i < len && (ca_ws(v[i]) || v[i] == ',')) i++;
        if (i >= len) break;
        int s = i, depth = 0;
        while (i < len && !(depth == 0 && v[i] == ',')) {
            if (v[i] == '(') depth++;
            else if (v[i] == ')') depth--;
            i++;
        }
        int e = i;
        while (e > s && ca_ws(v[e-1])) e--;
        int ts[8], te[8];
        int n = ca_tokens(v + s, e - s, ts, te, 8);
        if (n <= 0) continue;
        const char *p0 = v + s + ts[0];
        int p0l = te[0] - ts[0];
        int pl = (int)strlen(prop);
        int match = (p0l == pl && !memcmp(p0, prop, (size_t)pl)) ||
                    (p0l == 3 && !memcmp(p0, "all", 3));
        if (p0l == 4 && !memcmp(p0, "none", 4)) match = 0;
        if (!match) continue;
        struct trans_spec t;
        memset(&t, 0, sizeof t);
        t.ease.kind = CI_EASE_LINEAR;
        int ndur = 0;
        for (int k = 1; k < n; k++) {
            const char *tk = v + s + ts[k];
            int tkl = te[k] - ts[k];
            double ms;
            if (ca_time(tk, tkl, &ms)) {
                if (ndur == 0) { t.dur_ms = ms; ndur = 1; }
                else if (ndur == 1) { t.delay_ms = ms; ndur = 2; }
            } else if (ci_ease_parse(tk, tkl, &t.ease) == 0) {
                /* an easing */
            } else return -1;                 /* not a transition item we know */
        }
        *out = t;
        return 0;
    }
    return -1;
}

/* ---- @keyframes resolution ---------------------------------------------- */

/* The LAST `key: value` in a stop's declarations, trimmed. Last-wins is
 * source order for one block, matching find_decl's semantics upstream. */
static int kf_val(const char *d, int dlen, const char *key, char *out, int outmax)
{
    int kl = (int)strlen(key), found = 0, vs = 0, ve = 0;
    for (int i = 0; i + kl < dlen; i++) {
        if (i > 0 && !ca_ws(d[i-1]) && d[i-1] != ';' && d[i-1] != '{') continue;
        if (memcmp(d + i, key, (size_t)kl)) continue;
        int j = i + kl;
        while (j < dlen && ca_ws(d[j])) j++;
        if (j >= dlen || d[j] != ':') continue;
        j++;
        vs = j;
        while (j < dlen && d[j] != ';' && d[j] != '}') j++;
        ve = j;
        found = 1;
        i = j;
    }
    if (!found) return 0;
    while (vs < ve && ca_ws(d[vs])) vs++;
    while (ve > vs && ca_ws(d[ve-1])) ve--;
    if (ve - vs <= 0 || ve - vs >= outmax) return 0;
    memcpy(out, d + vs, (size_t)(ve - vs));
    out[ve - vs] = 0;
    return ve - vs;
}

/* cstyle opacity (0..255) as CSS text. Three decimals is below the 1/255
 * quantum, so the round trip through ci_value_interp cannot drift. */
static void op_text(int op, char *out, int outmax)
{
    if (op < 0) op = 0;
    if (op > 255) op = 255;
    snprintf(out, (size_t)outmax, "%d.%03d", op / 255, (op * 1000 / 255) % 1000);
}

/* Property-specific stop list. `base` is the cascade value text (opacity,
 * or the transform span; NULL/"" means `none`). Returns the stop count, 0
 * when the rule does not animate this property. */
static int pkf_build(const struct css_kf *kf, const char *propname, const char *base,
                     struct pkf *out, int outmax)
{
    int n = 0, any = 0;
    for (int i = 0; i < kf->nstop && n < outmax; i++) {
        char val[CANIM_VAL];
        int vl = kf_val(kf->stop[i].decls, kf->stop[i].dlen, propname, val, sizeof val);
        if (vl <= 0) continue;                 /* this stop does not name it */
        any = 1;
        out[n].off = kf->stop[i].off;
        memcpy(out[n].val, val, (size_t)vl + 1);
        out[n].len = vl;
        n++;
    }
    if (!any) return 0;
    const char *b = (base && base[0]) ? base : "none";
    int bl = (int)strlen(b);
    if (bl >= CANIM_VAL) bl = CANIM_VAL - 1;
    if (out[0].off > 0) {                      /* implicit 0% from the base */
        for (int i = n; i > 0; i--) out[i] = out[i - 1];
        memcpy(out[0].val, b, (size_t)bl);
        out[0].val[bl] = 0; out[0].len = bl; out[0].off = 0;
        n++;
    }
    if (n < outmax && out[n - 1].off < 1000) { /* implicit 100% from the base */
        memcpy(out[n].val, b, (size_t)bl);
        out[n].val[bl] = 0; out[n].len = bl; out[n].off = 1000;
        n++;
    }
    return n;
}

/* The interpolated text at (already eased) progress prog over a stop
 * list. Returns the length written, -1/-2 when this frame IS an endpoint
 * (the caller then writes the endpoint's own text, which keeps endpoint
 * frames byte-identical to a non-animated render: the painter parses the
 * very text the sheet carried), or 0 when the pair cannot be bridged. */
static int pkf_interp(const struct pkf *st, int n, double prog, const char *propname,
                      int fs, char *buf, int bufmax)
{
    double target = prog * 1000.0;
    if (target <= (double)st[0].off) return -1;
    if (target >= (double)st[n - 1].off) return -2;
    int a = 0, b = n - 1;
    for (int i = 0; i < n - 1; i++)
        if (target >= (double)st[i].off && target <= (double)st[i + 1].off) {
            a = i; b = i + 1; break;
        }
    if (st[b].off == st[a].off) return -1;
    double p = (target - (double)st[a].off) / (double)(st[b].off - st[a].off);

    if (!strcmp(propname, "transform")) {
        struct ci_xform xa, xb, xr;
        if (ci_transform_parse(st[a].val, st[a].len, (double)fs, (double)root_px_now(), &xa) != 0 ||
            ci_transform_parse(st[b].val, st[b].len, (double)fs, (double)root_px_now(), &xb) != 0)
            return 0;                          /* a shape we cannot bridge */
        ci_transform_interp(&xa, &xb, p, &xr);
        return ci_transform_text(&xr, buf, bufmax);
    }
    int r = ci_value_interp(propname, st[a].val, st[b].val, p, buf, bufmax);
    if (r >= 0) return r;
    /* Discrete fallback (shape mismatch): flip at .5, the rule
     * css_interp.h hands back to ITS callers for the same reason. */
    int l = p < 0.5 ? st[a].len : st[b].len;
    const char *s = p < 0.5 ? st[a].val : st[b].val;
    if (l >= bufmax) return 0;
    memcpy(buf, s, (size_t)l);
    buf[l] = 0;
    return l;
}

/* ---- overlay ------------------------------------------------------------ */

/* Stop overlaying; put the base back. The base texts live in the entry
 * (copied at adoption) so they outlive the sheet the cascade's own xraw
 * spans came from. */
static void ca_release(struct canim *e)
{
    struct cstyle *st = (struct cstyle *)e->node->style;
    if (!st) { e->ov_op = e->ov_xf = 0; return; }
    if (e->ov_op) st->opacity = e->base_op;
    if (e->ov_xf) {
        if (e->base_had_xf) {
            st->xraw[XR_TRANSFORM] = e->ov_xfbuf;
            memcpy(e->ov_xfbuf, e->base_xf, (size_t)e->base_xf_len + 1);
            st->xrawlen[XR_TRANSFORM] = (unsigned short)e->base_xf_len;
        } else {
            st->xraw[XR_TRANSFORM] = 0;
            st->xrawlen[XR_TRANSFORM] = 0;
        }
    }
    if (e->ov_op || e->ov_xf) g_dirty = 1;
    e->ov_op = e->ov_xf = 0;
}

/* Write one frame. opv/xfv NULL = leave that property alone this frame. */
static int ca_write(struct canim *e, const char *opv, const char *xfv, int xfl)
{
    struct cstyle *st = (struct cstyle *)e->node->style;
    if (!st) return 0;
    int changed = 0;
    if (opv) {
        double d;
        if (ca_num(opv, (int)strlen(opv), &d)) {
            if (d < 0) d = 0;
            if (d > 1) d = 1;
            int px = (int)(d * 255.0 + 0.5);
            if (!e->ov_op || px != e->last_op) {
                /* An opacity:0 + animation element must PAINT while it
                 * animates or the fade-in would be invisible for its whole
                 * duration; walk_anim cleared `hidden`, we own the alpha
                 * and base_op remembers what to restore. */
                st->hidden = 0;
                st->opacity = px;
                e->last_op = px;
                e->ov_op = 1;
                changed = 1;
                g_need_layout = 1;             /* opacity is snapshotted at layout */
            }
        }
    }
    if (xfv && xfl > 0 && xfl < CANIM_VAL) {
        if (!e->ov_xf || xfl != e->ov_xflen || memcmp(e->ov_xfbuf, xfv, (size_t)xfl)) {
            memcpy(e->ov_xfbuf, xfv, (size_t)xfl);
            e->ov_xfbuf[xfl] = 0;
            e->ov_xflen = xfl;
            st->xraw[XR_TRANSFORM] = e->ov_xfbuf;
            st->xrawlen[XR_TRANSFORM] = (unsigned short)xfl;
            e->ov_xf = 1;
            changed = 1;                       /* transform is read at paint: no layout */
        }
    }
    /* A write that changed a value IS a pixel change: g_dirty is what
     * css_anim_tick returns, js_page counts it into run_due's total, and
     * browser.c only calls css_anim_needs_layout() when run_due came back
     * nonzero. [2026-08-30: the recovered body returned `changed` to its
     * CALLER and set nothing here -- ca_release was the only g_dirty
     * writer -- so a ticking animation that wrote fresh values every 20 ms
     * reported "nothing happened", run_due returned 0, and the frame
     * block that owes the relayout/repaint was skipped entirely. On the
     * guest that presented as every animated element frozen at its
     * load-time paint.] */
    if (changed) g_dirty = 1;
    return changed;
}

/* Iteration progress for ACTIVE time at (ms since the delay elapsed).
 * Returns eased progress in [0,1], -1 before the active interval, 2 after
 * the final iteration. */
static double anim_progress(const struct canim *e, double at)
{
    double dur = e->as.dur_ms;
    if (dur <= 0) return 2.0;                  /* finished by definition */
    if (at < 0) return -1.0;
    double iter = at / dur;
    if (!e->as.infinite && iter >= e->as.iters) return 2.0;
    double frac = iter - (double)(long long)iter;
    long long k = (long long)iter;
    double p = frac;
    switch (e->as.dir) {
    case CAD_REVERSE:       p = 1.0 - frac; break;
    case CAD_ALTERNATE:     if (k & 1) p = 1.0 - frac; break;
    case CAD_ALTERNATE_REV: if (!(k & 1)) p = 1.0 - frac; break;
    default: break;
    }
    /* The timing function is applied to the WHOLE iteration here, not per
     * keyframe segment (CSS applies it per segment; for the two-stop
     * fade/slide rules real pages ship the two are identical). Listed in
     * the not-done note above. */
    return ci_ease_apply(&e->as.ease, p);
}

/* One animation frame at eased progress prog (out of [0,1] = outside the
 * active interval: fill decides, and without fill the overlay comes OFF). */
static int anim_frame(struct canim *e, double prog)
{
    char opbuf[CANIM_VAL], xfbuf[CANIM_VAL];
    const char *opv = 0, *xfv = 0;
    int xfl = 0;

    if (prog < 0.0) {
        if (e->as.fill_bwd) prog = 0.0;
        else return (e->ov_op || e->ov_xf) ? (ca_release(e), 1) : 0;
    } else if (prog > 1.0) {
        if (e->as.fill_fwd) prog = 1.0;
        else return (e->ov_op || e->ov_xf) ? (ca_release(e), 1) : 0;
    }

    int fs = 16;
    if (e->node->style) fs = ((struct cstyle *)e->node->style)->font_px;

    if (e->n_op) {
        int r = pkf_interp(e->op, e->n_op, prog, "opacity", fs, opbuf, sizeof opbuf);
        if (r == -1) opv = e->op[0].val;
        else if (r == -2) opv = e->op[e->n_op - 1].val;
        else if (r > 0) opv = opbuf;
    }
    if (e->n_xf) {
        int r = pkf_interp(e->xf, e->n_xf, prog, "transform", fs, xfbuf, sizeof xfbuf);
        if (r == -1) { xfv = e->xf[0].val; xfl = e->xf[0].len; }
        else if (r == -2) { xfv = e->xf[e->n_xf - 1].val; xfl = e->xf[e->n_xf - 1].len; }
        else if (r > 0) { xfv = xfbuf; xfl = r; }
    }
    return ca_write(e, opv, xfv, xfl);
}

/* One transition property's frame. prop 0 = opacity, 1 = transform. */
static int trans_frame(struct canim *e, unsigned long long now, int prop)
{
    struct trans_spec *sp = prop == 0 ? &e->t_op_spec : &e->t_xf_spec;
    double at = (double)(now - e->tt0) - sp->delay_ms;
    char buf[CANIM_VAL];

    if (at < 0) {
        /* In the delay the change has not happened yet: hold the FROM. */
        return prop == 0 ? ca_write(e, e->tf_op, 0, 0)
                         : ca_write(e, 0, e->tf_xf, e->tf_xf_n);
    }
    if (sp->dur_ms <= 0 || at >= sp->dur_ms) {
        /* Done. The TO value IS the new cascade base, so it becomes the
         * entry's BASE before the overlay comes off -- ca_release()
         * restores `base`, and restoring the adoption-time base here would
         * snap the element back to the value the class change was supposed
         * to move it FROM. */
        if (prop == 0) {
            double d;
            if (ca_num(e->tt_op, (int)strlen(e->tt_op), &d)) {
                if (d < 0) d = 0;
                if (d > 1) d = 1;
                e->base_op = (int)(d * 255.0 + 0.5);
            }
            e->trans_op = 0;
        } else {
            memcpy(e->base_xf, e->tt_xf, (size_t)e->tt_xf_n + 1);
            e->base_xf_len = e->tt_xf_n;
            e->base_had_xf = 1;
            e->trans_xf = 0;
        }
        if (!e->trans_op && !e->trans_xf) { ca_release(e); return 1; }
        return 1;
    }
    double prog = ci_ease_apply(&sp->ease, at / sp->dur_ms);
    if (prop == 0) {
        int r = ci_value_interp("opacity", e->tf_op, e->tt_op, prog, buf, sizeof buf);
        if (r <= 0) {
            const char *s = prog < 0.5 ? e->tf_op : e->tt_op;   /* discrete flip */
            int l = (int)strlen(s);
            if (l < (int)sizeof buf) { memcpy(buf, s, (size_t)l); buf[l] = 0; r = l; }
        }
        return r > 0 ? ca_write(e, buf, 0, 0) : 0;
    }
    struct ci_xform a, b, xr;
    if (ci_transform_parse(e->tf_xf, e->tf_xf_n, 16.0, (double)root_px_now(), &a) == 0 &&
        ci_transform_parse(e->tt_xf, e->tt_xf_n, 16.0, (double)root_px_now(), &b) == 0) {
        ci_transform_interp(&a, &b, prog, &xr);
        int r = ci_transform_text(&xr, buf, sizeof buf);
        if (r > 0) return ca_write(e, 0, buf, r);
    }
    /* Unbridgeable pair: jump to the end, which is the cascade's value. */
    e->trans_xf = 0;
    if (!e->trans_op && !e->trans_xf) { ca_release(e); return 1; }
    return 1;
}

/* ---- entry lifecycle ----------------------------------------------------- */

static struct canim *ca_find(struct node *n)
{
    for (int i = 0; i < g_ncan; i++)
        if (g_ca[i].node == n) return &g_ca[i];
    return 0;
}

static void ca_retire(struct canim *e)
{
    ca_release(e);
    int i = (int)(e - g_ca);
    if (i < 0 || i >= g_ncan) return;
    g_ca[i] = g_ca[g_ncan - 1];                /* swap-remove; the tail slot */
    g_ncan--;                                  /* is dead space until reused */
}

/* Get or create the entry for n, rebuilding it if the sheet generation or
 * the anim span moved. *fresh says the entry was (re)created this call and
 * the caller must (re)start the animation's clock. Returns 0 when the
 * entry table is full (the caller counts the element as frozen and leaves
 * it at its cascade base). */
static struct canim *ca_entry(struct node *n, struct cstyle *st, int gen, int *fresh)
{
    struct canim *e = ca_find(n);
    int rebuild = 0;
    if (e) {
        if (e->node->serial != n->serial) rebuild = 1;   /* recycled slot */
        else if (e->gen != gen) rebuild = 1;             /* sheet recompiled */
        else if (st->anim_raw != e->anim_raw_at ||
                 st->anim_rawlen != e->anim_rawlen_at) rebuild = 1;  /* new rule */
        /* ONLY a currently-adopted entry has an overlay that needs
         * releasing before the slot is rebuilt. [2026-08-30: the original
         * shape released on EVERY rebuild, including a slot taken fresh
         * from the tail -- and a fresh slot is one of three corpses: a
         * bss-zero slot (node == NULL: the FIRST adoption of a browsing
         * session died on exactly that, guest-verified -- SIGSEGV reading
         * node->style, cr2=0x50, RIP inside ca_adopt, on the first page
         * whose keyframes animate a supported property), a corpse
         * css_anim_reset() left (its node dangles into the arena reset
         * exists to stop us touching), or dead space a swap-remove left
         * (already released by ca_retire). Releasing any of them is either
         * a fault or a write through a dangling pointer.] */
        if (rebuild) ca_release(e);
    } else {
        if (g_ncan >= CSS_ANIM_CAP) return 0;
        e = &g_ca[g_ncan++];
        rebuild = 1;
    }
    if (rebuild) {
        /* THE BASE SNAPSHOT IS TAKEN HERE AND ONLY HERE. This runs inside
         * css_anim_note's walk, which is BEFORE the overlay re-apply and
         * AFTER the cascade, so st carries BASE values at this moment --
         * snapshotting anywhere else would capture an overlay as the base
         * and the animation would "restore" to its own mid-frame. */
        memset(e, 0, sizeof *e);
        e->node = n;
        e->serial = n->serial;
        e->gen = gen;
        e->base_op = st->opacity;
        e->base_had_xf = st->xraw[XR_TRANSFORM] ? 1 : 0;
        if (e->base_had_xf) {
            int l = st->xrawlen[XR_TRANSFORM];
            if (l >= CANIM_VAL) l = CANIM_VAL - 1;
            memcpy(e->base_xf, st->xraw[XR_TRANSFORM], (size_t)l);
            e->base_xf[l] = 0;
            e->base_xf_len = l;
        }
        e->anim_raw_at = st->anim_raw;
        e->anim_rawlen_at = st->anim_rawlen;
        if (fresh) *fresh = 1;
    } else if (fresh) {
        *fresh = 0;
    }
    return e;
}

/* Adopt one element the note walk found. The entry is created only when
 * something clockable resolved: an animation whose @keyframes exist AND
 * name opacity or transform, or a transition covering either. Everything
 * else -- including an element whose animation stopped resolving, because
 * its @keyframes left the sheet -- is retired to the pre-clock end-state
 * behaviour. */
static void ca_adopt(struct node *n, struct cstyle *st, unsigned long long now)
{
    int gen = LOGIT_HAVE(css_extra_sheet_gen) ? css_extra_sheet_gen() : 0;
    struct canim *e = ca_find(n);

    /* Resolve the animation half against the captured table. */
    struct anim_spec as;
    struct pkf op[CSS_KF_MAXSTOP + 2], xf[CSS_KF_MAXSTOP + 2];
    int n_op = 0, n_xf = 0, have_anim = 0;
    if (st->anim_raw && st->anim_rawlen > 0 &&
        css_anim_parse_animation(st->anim_raw, st->anim_rawlen, &as) == 0 &&
        as.has_name && as.dur_ms > 0 &&
        LOGIT_HAVE(css_keyframes_find)) {
        const struct css_kf *kf = 0;
        if (css_keyframes_find(as.name, (int)strlen(as.name), &kf) && kf) {
            char bop[16], bxf[CANIM_VAL];
            const char *bxf_p = 0;
            op_text(st->opacity, bop, sizeof bop);
            if (st->xraw[XR_TRANSFORM]) {
                int l = st->xrawlen[XR_TRANSFORM];
                if (l >= CANIM_VAL) l = CANIM_VAL - 1;
                memcpy(bxf, st->xraw[XR_TRANSFORM], (size_t)l);
                bxf[l] = 0;
                bxf_p = bxf;
            }
            n_op = pkf_build(kf, "opacity", bop, op, CSS_KF_MAXSTOP + 2);
            n_xf = pkf_build(kf, "transform", bxf_p, xf, CSS_KF_MAXSTOP + 2);
            have_anim = (n_op || n_xf);
        }
    }

    /* Resolve the transition half: does the shorthand cover either
     * property we interpolate? */
    struct trans_spec top, txf;
    int cop = st->trans_raw && st->trans_rawlen > 0 &&
              css_anim_parse_transition(st->trans_raw, st->trans_rawlen,
                                        "opacity", &top) == 0;
    int cxf = st->trans_raw && st->trans_rawlen > 0 &&
              css_anim_parse_transition(st->trans_raw, st->trans_rawlen,
                                        "transform", &txf) == 0;

    if (!have_anim && !cop && !cxf) {
        if (e) ca_retire(e);
        return;
    }
    int fresh = 0;
    e = ca_entry(n, st, gen, &fresh);
    if (!e) { g_frozen++; return; }

    if (have_anim) {
        e->is_anim = 1;
        e->as = as;
        e->n_op = n_op;
        e->n_xf = n_xf;
        memcpy(e->op, op, (size_t)n_op * sizeof op[0]);
        memcpy(e->xf, xf, (size_t)n_xf * sizeof xf[0]);
        if (fresh) {
            /* ONLY a freshly (re)built entry starts its clock here. An
             * unchanged re-adopt -- every full-document css_extra_apply
             * reaches this line -- must NOT restart the animation, or a
             * page that re-styles itself would never get past t=0. */
            e->t0 = now;
            e->acc = 0;
            e->last_now = now;
            e->anim_done = 0;
        }
    } else if (e->is_anim) {
        /* The animation stopped resolving (keyframes left the sheet, or
         * the rule now animates nothing we interpolate): stop the clock,
         * release the overlay, keep any transition watch. */
        if (e->ov_op || e->ov_xf) ca_release(e);
        e->is_anim = 0;
        e->anim_done = 0;
    }
    if (cop || cxf) {
        e->is_trans = 1;
        if (cop) e->t_op_spec = top;
        if (cxf) e->t_xf_spec = txf;
    }
}

/* The note walk: over the scope css_extra was handed (the whole document
 * on a full apply, the invalidated subtree on a scoped restyle). */
static void note_walk(struct node *n, unsigned long long now)
{
    if (n->type == N_ELEM) {
        struct cstyle *st = (struct cstyle *)n->style;
        struct canim *e = ca_find(n);
        if (st && (st->anim_raw || st->trans_raw)) {
            ca_adopt(n, st, now);
            e = ca_find(n);
        } else if (e) {
            /* The style stopped asking for a clock (class removed, rule
             * unmatched). A running animation that is removed stops
             * applying, which for a fill:none animation is the base at
             * every t anyway. */
            ca_retire(e);
            e = 0;
        }
        /* A finished fill:none animation with no overlay left would occupy
         * a cap slot forever; retire it. A fill:fwd one KEEPS its overlay
         * and its slot -- the held frame is the point. */
        if (e && e->is_anim && e->anim_done && !e->is_trans &&
            !e->ov_op && !e->ov_xf) {
            ca_retire(e);
            e = 0;
        }
        /* The transition trigger: the pre-cascade snapshot disagrees with
         * what the cascade just applied. */
        if (e && e->is_trans && st) {
            if (e->w_op_n) {
                char nowb[16];
                op_text(st->opacity, nowb, sizeof nowb);
                if (strcmp(nowb, e->w_op) != 0 && e->t_op_spec.dur_ms > 0 &&
                    !e->is_anim && !e->trans_op) {
                    memcpy(e->tf_op, e->w_op, (size_t)e->w_op_n);
                    e->tf_op[e->w_op_n] = 0;
                    strcpy(e->tt_op, nowb);
                    e->trans_op = 1;
                    e->tt0 = now;
                }
                e->w_op_n = 0;
            }
            if (e->w_xf_n) {
                const char *cur = st->xraw[XR_TRANSFORM];
                int curl = cur ? (int)st->xrawlen[XR_TRANSFORM] : 0;
                int same = curl == e->w_xf_n &&
                           (curl == 0 || memcmp(cur, e->w_xf, (size_t)curl) == 0);
                if (!same && e->t_xf_spec.dur_ms > 0 && !e->is_anim && !e->trans_xf) {
                    memcpy(e->tf_xf, e->w_xf, (size_t)e->w_xf_n);
                    e->tf_xf[e->w_xf_n] = 0;
                    e->tf_xf_n = e->w_xf_n;
                    if (cur) {
                        int l = curl;
                        if (l >= CANIM_VAL) l = CANIM_VAL - 1;
                        memcpy(e->tt_xf, cur, (size_t)l);
                        e->tt_xf[l] = 0;
                        e->tt_xf_n = l;
                    } else {
                        memcpy(e->tt_xf, "none", 5);
                        e->tt_xf_n = 4;
                    }
                    e->trans_xf = 1;
                    e->tt0 = now;
                }
                e->w_xf_n = 0;
            }
        }
    }
    for (struct node *c = n->first_child; c; c = c->next) note_walk(c, now);
}

/* Advance the ANIMATION half of one entry to `now`. Shared by note and
 * tick so a cascade-restyled element and a ticked one take the same path. */
static void anim_advance(struct canim *e, unsigned long long now)
{
    if (e->as.paused) { e->last_now = now; return; }
    e->acc += now - e->last_now;
    e->last_now = now;
    double at = (double)e->acc - e->as.delay_ms;
    double prog = anim_progress(e, at);
    if (prog > 1.5 && !e->as.infinite) {
        e->anim_done = 1;
        if (e->as.fill_fwd) anim_frame(e, 1.0);   /* hold the last frame */
        else if (e->ov_op || e->ov_xf) ca_release(e);
        return;
    }
    anim_frame(e, prog);
}

/* ---- the public clock (css.h) ------------------------------------------- */

#ifndef LOGIT_ANIM_NO_CLOCK

int css_anim_entries(void) { return g_ncan; }
int css_anim_frozen(void)  { return g_frozen; }

void css_anim_reset(void)
{
    /* NOT ca_release() per entry: on navigation the DOM arena is about to
     * be freed, and writing through node pointers here would be the very
     * use-after-free this function exists to prevent. Drop, don't touch. */
    g_ncan = 0;
    g_frozen = 0;
    g_frozen_said = 0;
    g_next_frame = 0;
    g_dirty = g_need_layout = 0;
}

void css_anim_snapshot(struct node *root)
{
    (void)root;                    /* the walk is over ENTRIES, not the tree:
                                    * bounded by the cap, one pass, no alloc */
    for (int i = 0; i < g_ncan; i++) {
        struct canim *e = &g_ca[i];
        if (!e->is_trans || e->trans_op || e->trans_xf) continue;
        if (!e->node || e->node->serial != e->serial) continue;
        struct cstyle *st = (struct cstyle *)e->node->style;
        if (!st) continue;
        op_text(st->opacity, e->w_op, sizeof e->w_op);
        e->w_op_n = (int)strlen(e->w_op);
        const char *x = st->xraw[XR_TRANSFORM];
        int l = x ? (int)st->xrawlen[XR_TRANSFORM] : 0;
        if (l >= CANIM_VAL) l = CANIM_VAL - 1;
        if (x) memcpy(e->w_xf, x, (size_t)l);
        e->w_xf[l] = 0;
        e->w_xf_n = x ? l : 0;
    }
}

void css_anim_note(struct node *root)
{
    if (!root) return;
    unsigned long long now = clk_now();
    g_frozen = 0;                              /* counted per walk; the loud */
    note_walk(root, now);                      /* line prints once per sheet */
    if (g_frozen && !g_frozen_said) {
        printf("[css-anim] cap %d reached: %d element(s) keep their cascade"
               " base value (no animation)\n", CSS_ANIM_CAP, g_frozen);
        g_frozen_said = 1;
    }
    /* Re-apply every running overlay: the cascade that just ran overwrote
     * st->opacity / st->xraw with base values, and one frame at the base
     * would be a visible flash on every scoped restyle of an animated
     * page. Bounded by the cap. */
    for (int i = 0; i < g_ncan; i++) {
        struct canim *e = &g_ca[i];
        if (!e->node || e->node->serial != e->serial) { ca_retire(e); i--; continue; }
        if (!e->node->style) { ca_retire(e); i--; continue; }
        if (e->is_anim && !e->anim_done) anim_advance(e, now);
        if (e->is_anim && e->anim_done && e->as.fill_fwd &&
            (e->ov_op || e->ov_xf)) {
            /* A held fill frame must survive cascades too. */
            struct cstyle *st = (struct cstyle *)e->node->style;
            if (e->ov_op) { st->opacity = e->last_op; st->hidden = 0; }
            if (e->ov_xf) {
                st->xraw[XR_TRANSFORM] = e->ov_xfbuf;
                st->xrawlen[XR_TRANSFORM] = (unsigned short)e->ov_xflen;
            }
        }
        if (e->trans_op) trans_frame(e, now, 0);
        if (e->trans_xf) trans_frame(e, now, 1);
    }
}

int css_anim_active(void)
{
    for (int i = 0; i < g_ncan; i++) {
        struct canim *e = &g_ca[i];
        if (!e->node || e->node->serial != e->serial) continue;
        if (e->is_anim && !e->anim_done) return 1;   /* includes the delay */
        if (e->trans_op || e->trans_xf) return 1;
    }
    return 0;
}

long long css_anim_next_due(void)
{
    if (!css_anim_active()) { g_next_frame = 0; return -1; }
    unsigned long long now = clk_now();
    if (!g_next_frame || g_next_frame <= now) g_next_frame = now + CANIM_FRAME_MS;
    return (long long)g_next_frame;
}

int css_anim_tick(unsigned long long now)
{
    g_dirty = g_need_layout = 0;
    if (g_ncan == 0) return 0;
    g_next_frame = now + CANIM_FRAME_MS;
    for (int i = 0; i < g_ncan; i++) {
        struct canim *e = &g_ca[i];
        if (!e->node || e->node->serial != e->serial) { ca_retire(e); i--; continue; }
        struct cstyle *st = (struct cstyle *)e->node->style;
        if (!st || (!st->anim_raw && !st->trans_raw)) {
            /* The declarations left the style without a note walk seeing
             * it (an inline style= rewrite reaches us this way). Retire. */
            ca_retire(e);
            i--;
            continue;
        }
        if (e->is_anim && !e->anim_done) anim_advance(e, now);
        if (e->trans_op) trans_frame(e, now, 0);
        if (e->trans_xf) trans_frame(e, now, 1);
    }
    return g_dirty;
}

int css_anim_needs_layout(void)
{
    /* 2 = an OPACITY value moved (the display list snapshots opacity at
     * layout: this frame is owed a relayout_page, not just a repaint),
     * 1 = only paint-live values moved (transform: the redraw browser.c
     * already does is enough), 0 = nothing moved. That 2/1/0 contract is
     * browser.c's comment at the call site; [2026-08-30] the recovered
     * body here returned the g_need_layout BOOLEAN -- opacity frames came
     * back as 1 and never bought their relayout, transform-only frames
     * came back as 0 and never bought even the redraw -- and every
     * animated element froze at its load-time paint. Guest-verified
     * before this fix: the @keyframes fade-in box rendered as nothing at
     * all (alpha 0 at first layout, held forever) and a transition's
     * screen never left its pre-change value. */
    int r = g_need_layout ? 2 : (g_dirty ? 1 : 0);
    g_need_layout = 0;
    return r;
}

#else  /* LOGIT_ANIM_NO_CLOCK: the negative control's world. The clock is
        * inert and the browser is behaviourally the pre-clock machine --
        * css_extra's end-state approximation is the whole answer. The
        * PARSERS above stay compiled (the host checker drives them in both
        * worlds); only the clock stops. */

int  css_anim_entries(void) { return 0; }
int  css_anim_frozen(void)  { return 0; }
void css_anim_reset(void) {}
void css_anim_snapshot(struct node *root) { (void)root; }
void css_anim_note(struct node *root) { (void)root; }
int  css_anim_active(void) { return 0; }
long long css_anim_next_due(void) { return -1; }
int  css_anim_tick(unsigned long long now) { (void)now; return 0; }
int  css_anim_needs_layout(void) { return 0; }

#endif /* LOGIT_ANIM_NO_CLOCK */
