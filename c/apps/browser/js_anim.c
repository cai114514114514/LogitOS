/* js_anim.c -- Element.prototype.animate, and only as much of Web Animations
 * as 2,202 subtests actually ask for.
 *
 * THE MEASUREMENT THAT SIZED THIS, because "implement Web Animations" is a
 * month and this is not that.
 *
 * WPT's interpolation-testcommon.js drives 337 files in css/ and produces
 * 47,140 of the 106,130 subtest failures there. Its Web Animations method
 * begins every subtest with
 *
 *     assert_true(interpolationMethod.isSupported(), ...)
 *
 * and for that method isSupported() is, in full, `'animate' in Element.prototype`.
 * 10,714 subtests fail on that one line. They are not all reachable: take each
 * failing `Web Animations: ...` subtest name, substitute `CSS Animations`, and
 * ask whether that twin passes. 8,513 twins ALSO fail -- those need
 * CSS.supports and belong to the LibCSS line. 2,202 twins PASS, and those are
 * gated on nothing but the property existing.
 *
 * WHAT THE HARNESS ACTUALLY TOUCHES, which is the whole specification of this
 * file:
 *
 *     var animation = target.animate(keyframes, {fill, duration, easing});
 *     animation.pause();
 *     animation.currentTime = 50 * 1000;
 *     ... later ...
 *     getComputedStyle(target).getPropertyValue(prop)
 *
 * It never reads the animation back. The object is a HANDLE; the observable is
 * the target's computed style. So the work is not the API surface, it is
 * making a computed read reflect the interpolated value at a given time --
 * which is what css_interp.c already computes.
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
 * Together: the overlay changes an answer only where it has a real
 * interpolation of a property the engine really reports. Everywhere else the
 * browser behaves exactly as it did before this file existed, which is also
 * what makes the A/B measurement in the report meaningful.
 *
 * DELIBERATELY NOT HERE, and named rather than omitted: no timeline that
 * advances on its own (nothing drives currentTime but the setter), no
 * animation events (animationstart/finish/cancel), no composite modes
 * (add/accumulate -- the `Compositing Web Animations` subtests are a separate
 * 2,384 and they also need CSS.supports), no @keyframes/CSS-animation
 * integration, no ScrollTimeline, no commitStyles, no pseudo-element targets.
 */
#include "css_interp.h"

#include <string.h>
#include <stdio.h>

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
/* ---- property-name spellings ------------------------------------------
 * The harness hands animate() CAMEL-CASE names (it converts them itself,
 * with `offset` -> `cssOffset` and `float` -> `cssFloat`) and then reads the
 * result back with the DASHED name. One canonical form, and both spellings
 * fold into it, or the value is computed for a key nothing ever looks up. */
"function dash(p){\n"
"  p = String(p);\n"
"  if (p.slice(0,2) === '--') return p;\n"
"  if (p === 'cssFloat') return 'float';\n"
"  if (p === 'cssOffset') return 'offset';\n"
"  return p.replace(/[A-Z]/g, function(c){ return '-' + c.toLowerCase(); });\n"
"}\n"
"\n"
/* ---- easing ------------------------------------------------------------
 * THE PART THE HARNESS DEPENDS ON MOST, and it is easy to miss why. The
 * timeline is never advanced: duration is 100s, currentTime is set to 50s, so
 * the input progress is ALWAYS exactly 0.5. Every distinct `at` the suite
 * tests -- -0.3, 0, 0.3, 0.5, 0.6, 1, 1.5 -- is produced entirely by the
 * easing function, which interpolation-testcommon.js builds as
 *
 *     y == 0   -> 'steps(1, end)'
 *     y == 1   -> 'steps(1, start)'
 *     y == 0.5 -> 'linear'
 *     else     -> 'cubic-bezier(0, b, 1, b)' with b = (8y - 1) / 6
 *
 * So a cubic-bezier solver that is merely close, or one that clamps its
 * output to [0,1], collapses the whole suite onto two or three values and
 * every `at` outside [0,1] silently becomes an endpoint. The control points
 * here are deliberately outside the unit square and the OUTPUT must be too. */
"function bez(x1,y1,x2,y2){\n"
"  function cx(t,a){ return ((1-3*a[1]+3*a[0])*t + (3*a[1]-6*a[0]))*t*t + 3*a[0]*t; }\n"
"  function C(t,p1,p2){ return (((1-3*p2+3*p1)*t + (3*p2-6*p1))*t + 3*p1)*t; }\n"
"  function dC(t,p1,p2){ return 3*(1-3*p2+3*p1)*t*t + 2*(3*p2-6*p1)*t + 3*p1; }\n"
"  return function(x){\n"
"    if (x1 === y1 && x2 === y2) return x;\n"
/*     Outside [0,1] the INPUT extends linearly along the tangent at the
 *     nearest endpoint. Note this is about x, not y: the corpus always feeds
 *     x = 0.5, so this branch is not what carries it -- see the note on the
 *     OUTPUT range below, which is the one that does. */
"    if (x <= 0 || x >= 1) {\n"
"      var s0 = (x1 > 0) ? y1 / x1 : ((y2 > 0 || x2 > 0) ? y2 / x2 : 0);\n"
"      var s1 = (x2 < 1) ? (y2 - 1) / (x2 - 1) : ((y1 !== 1 || x1 !== 1) ? (y1 - 1) / (x1 - 1) : 0);\n"
"      return (x <= 0) ? s0 * x : 1 + s1 * (x - 1);\n"
"    }\n"
"    var t = x, i, d, xe;\n"
"    for (i = 0; i < 12; i++) {\n"
"      xe = C(t,x1,x2) - x;\n"
"      if (xe < 1e-10 && xe > -1e-10) return C(t,y1,y2);\n"
"      d = dC(t,x1,x2);\n"
"      if (d < 1e-10 && d > -1e-10) break;\n"
"      t = t - xe / d;\n"
"    }\n"
"    var lo = 0, hi = 1; t = x;\n"
"    for (i = 0; i < 60; i++) {\n"
"      xe = C(t,x1,x2);\n"
"      if (xe > x) hi = t; else lo = t;\n"
"      t = (lo + hi) / 2;\n"
"    }\n"
"    return C(t,y1,y2);\n"
"  };\n"
"}\n"
"function steps(n, pos){\n"
"  return function(x){\n"
"    var jumpStart = (pos === 'start' || pos === 'jump-start' || pos === 'jump-both');\n"
"    var jumpNone = (pos === 'jump-none');\n"
"    var d = jumpNone ? (n - 1) : n;\n"
"    if (d <= 0) d = 1;\n"
"    var step = jumpStart ? Math.ceil(x * n) : Math.floor(x * n);\n"
"    if (pos === 'jump-both') step = Math.floor(x * n) + 1;\n"
"    return step / d;\n"
"  };\n"
"}\n"
"function easingOf(s){\n"
"  s = String(s == null ? 'linear' : s).trim();\n"
"  if (s === 'linear' || s === '') return function(x){ return x; };\n"
"  if (s === 'ease') return bez(0.25,0.1,0.25,1);\n"
"  if (s === 'ease-in') return bez(0.42,0,1,1);\n"
"  if (s === 'ease-out') return bez(0,0,0.58,1);\n"
"  if (s === 'ease-in-out') return bez(0.42,0,0.58,1);\n"
"  if (s === 'step-start') return steps(1,'start');\n"
"  if (s === 'step-end') return steps(1,'end');\n"
"  var m = /^cubic-bezier\\(([^)]*)\\)$/.exec(s);\n"
"  if (m) {\n"
"    var a = m[1].split(',').map(function(v){ return parseFloat(v); });\n"
"    if (a.length === 4 && a.every(function(v){ return v === v; })) return bez(a[0],a[1],a[2],a[3]);\n"
"  }\n"
"  m = /^steps\\(([^)]*)\\)$/.exec(s);\n"
"  if (m) {\n"
"    var b = m[1].split(',');\n"
"    var n = parseInt(b[0], 10);\n"
"    if (n === n && n > 0) return steps(n, b.length > 1 ? b[1].trim() : 'end');\n"
"  }\n"
"  return function(x){ return x; };\n"
"}\n"
"\n"
/* ---- keyframes ---------------------------------------------------------
 * Both spellings the API accepts: the array form the harness uses, and the
 * object form ({opacity: [0, 1]}) a real page uses. Offsets that are null are
 * spread evenly, which is the spec's rule and also what makes a two-element
 * array mean "from" and "to". */
"function normalize(kf){\n"
"  var out = [];\n"
"  if (kf == null) return out;\n"
"  if (Array.isArray(kf)) {\n"
"    for (var i = 0; i < kf.length; i++) {\n"
"      var k = kf[i], e = { offset: null, easing: k.easing, composite: k.composite, props: {} };\n"
"      if (k.offset !== undefined && k.offset !== null) e.offset = Number(k.offset);\n"
"      for (var p in k) {\n"
"        if (p === 'offset' || p === 'easing' || p === 'composite') continue;\n"
"        e.props[dash(p)] = String(k[p]);\n"
"      }\n"
"      out.push(e);\n"
"    }\n"
"  } else {\n"
"    var names = [], n;\n"
"    for (n in kf) if (n !== 'offset' && n !== 'easing' && n !== 'composite') names.push(n);\n"
"    var len = 0;\n"
"    for (var q = 0; q < names.length; q++) {\n"
"      var v = kf[names[q]];\n"
"      if (Array.isArray(v) && v.length > len) len = v.length;\n"
"    }\n"
"    if (!len) len = 1;\n"
"    for (var j = 0; j < len; j++) {\n"
"      var ee = { offset: null, easing: undefined, composite: kf.composite, props: {} };\n"
"      for (var r = 0; r < names.length; r++) {\n"
"        var vv = kf[names[r]];\n"
"        var val = Array.isArray(vv) ? vv[Math.min(j, vv.length - 1)] : vv;\n"
"        if (val !== undefined && val !== null) ee.props[dash(names[r])] = String(val);\n"
"      }\n"
"      out.push(ee);\n"
"    }\n"
"  }\n"
"  if (out.length === 1 && out[0].offset === null) out[0].offset = 1;\n"
"  if (out.length) {\n"
"    if (out[0].offset === null) out[0].offset = 0;\n"
"    if (out[out.length-1].offset === null) out[out.length-1].offset = 1;\n"
"  }\n"
"  var last = 0;\n"
"  for (var s = 0; s < out.length; s++) {\n"
"    if (out[s].offset === null) {\n"
"      var e2 = s; while (e2 < out.length && out[e2].offset === null) e2++;\n"
"      var lo = (s > 0) ? out[s-1].offset : 0;\n"
"      var hi = (e2 < out.length) ? out[e2].offset : 1;\n"
"      for (var u = s; u < e2; u++) out[u].offset = lo + (hi - lo) * (u - s + 1) / (e2 - s + 1);\n"
"      s = e2 - 1;\n"
"    } else last = out[s].offset;\n"
"  }\n"
"  return out;\n"
"}\n"
"\n"
"function timingOf(opt){\n"
"  var t = { duration: 0, delay: 0, endDelay: 0, iterations: 1, iterationStart: 0,\n"
"            direction: 'normal', fill: 'auto', easing: 'linear', id: undefined,\n"
"            composite: 'replace', iterationComposite: 'replace' };\n"
"  if (typeof opt === 'number') { t.duration = opt; return t; }\n"
"  if (!opt || typeof opt !== 'object') return t;\n"
"  if (opt.duration !== undefined && opt.duration !== 'auto') t.duration = Number(opt.duration) || 0;\n"
"  if (opt.delay !== undefined) t.delay = Number(opt.delay) || 0;\n"
"  if (opt.endDelay !== undefined) t.endDelay = Number(opt.endDelay) || 0;\n"
"  if (opt.iterations !== undefined) t.iterations = Number(opt.iterations);\n"
"  if (opt.iterationStart !== undefined) t.iterationStart = Number(opt.iterationStart) || 0;\n"
"  if (opt.direction) t.direction = String(opt.direction);\n"
"  if (opt.fill) t.fill = String(opt.fill);\n"
"  if (opt.easing !== undefined) t.easing = String(opt.easing);\n"
"  if (opt.id !== undefined) t.id = String(opt.id);\n"
"  if (opt.composite) t.composite = String(opt.composite);\n"
"  if (opt.iterationComposite) t.iterationComposite = String(opt.iterationComposite);\n"
"  return t;\n"
"}\n"
"\n"
"var REG = new WeakMap();\n"
"function listFor(el, make){\n"
"  var l = REG.get(el);\n"
"  if (!l && make) { l = []; REG.set(el, l); }\n"
"  return l;\n"
"}\n"
"\n"
"function Animation(target, kf, opt){\n"
"  this.__target = target;\n"
"  this.__kf = normalize(kf);\n"
"  this.__t = timingOf(opt);\n"
"  this.__ease = easingOf(this.__t.easing);\n"
"  this.__hold = 0;\n"
"  this.__state = 'running';\n"
"  this.id = this.__t.id === undefined ? '' : this.__t.id;\n"
"  this.playbackRate = 1;\n"
"  this.startTime = null;\n"
"  this.timeline = null;\n"
"  this.effect = {\n"
"    target: target,\n"
"    getTiming: (function(t){ return function(){ return t; }; })(this.__t),\n"
"    getComputedTiming: (function(a){ return function(){\n"
"      var t = a.__t;\n"
"      return { delay: t.delay, endDelay: t.endDelay, fill: t.fill === 'auto' ? 'none' : t.fill,\n"
"               iterations: t.iterations, iterationStart: t.iterationStart,\n"
"               duration: t.duration, direction: t.direction, easing: t.easing,\n"
"               activeDuration: t.duration * t.iterations,\n"
"               localTime: a.__hold, progress: a.__progress(), currentIteration: 0 };\n"
"    }; })(this),\n"
"    getKeyframes: (function(k){ return function(){ return k.slice(); }; })(this.__kf)\n"
"  };\n"
"}\n"
"\n"
/* The proportion through THIS iteration, eased -- or null when the animation
 * is not in effect (before/after its active interval with no matching fill),
 * which is how the overlay knows to stay silent. */
"Animation.prototype.__progress = function(){\n"
"  var t = this.__t, ct = this.__hold;\n"
"  if (ct === null || this.__state === 'idle') return null;\n"
"  var d = t.duration, it = t.iterations;\n"
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
/* THE NEGATIVE CONTROL, and it is not "delete animate()" -- any test catches
 * that. This is the sentence an implementation writes without thinking:
 * progress is a fraction, so clamp it to [0, 1].
 *
 * The eased progress is NOT a fraction. A cubic-bezier whose control points
 * lie outside the unit square returns values outside it, and that is the
 * entire mechanism interpolation-testcommon.js uses to test extrapolation:
 * the input is always exactly 0.5 and the OUTPUT is the `at` being asked for,
 * which is -0.3 and 1.5 in two of every seven subtests across all 337 files.
 * Clamping leaves every ordinary animation correct and every page looking
 * right, and quietly reports the endpoint value for those two.
 *
 * Note it clamps the OUTPUT. Clamping the input, which was the first attempt
 * at this control, changes nothing at all and the suite stayed green -- the
 * input is 0.5 and never leaves the interval. A control that does not fail is
 * not a control, and that near-miss is why this comment is this long. */
"  var __p = this.__ease(f);\n"
"  return __p < 0 ? 0 : (__p > 1 ? 1 : __p);\n"
"};\n"
#else
"  return this.__ease(f);\n"
"};\n"
#endif
"\n"
"Animation.prototype.__valueAt = function(prop){\n"
"  var kf = this.__kf;\n"
"  if (!kf.length) return null;\n"
"  var p = this.__progress();\n"
"  if (p === null) return null;\n"
"  var lo = -1, hi = -1, i;\n"
"  for (i = 0; i < kf.length; i++) if (prop in kf[i].props) { if (lo < 0) lo = i; hi = i; }\n"
"  if (lo < 0) return null;\n"
"  if (lo === hi) return kf[lo].props[prop];\n"
"  var a = lo, b = hi;\n"
"  for (i = lo; i <= hi; i++) {\n"
"    if (!(prop in kf[i].props)) continue;\n"
"    if (kf[i].offset <= p) a = i;\n"
"  }\n"
"  b = a;\n"
"  for (i = a + 1; i <= hi; i++) if (prop in kf[i].props) { b = i; break; }\n"
"  if (b === a) { a = lo; for (i = lo + 1; i <= hi; i++) if (prop in kf[i].props) { b = i; break; } }\n"
"  if (b === a) return kf[a].props[prop];\n"
"  var span = kf[b].offset - kf[a].offset;\n"
/*  A local progress that runs OUTSIDE [0,1] is not an error here: the eased
 *  progress itself can be -0.3 or 1.5, and the value is extrapolated. */
"  var lp = span > 0 ? (p - kf[a].offset) / span : (p < kf[a].offset ? 0 : 1);\n"
"  var v = II(prop, kf[a].props[prop], kf[b].props[prop], lp);\n"
"  if (v === null || v === undefined) return v;\n"
"  return this.__iterAccum(prop, v, kf[hi].props[prop]);\n"
"};\n"
"\n"
/* iterationComposite: 'accumulate'.
 *
 * The one composite knob that is about TIME rather than about the underlying
 * value: with it, iteration n starts from where iteration n-1 finished instead
 * of from the first keyframe again, so a 10px slide repeated three times ends
 * at 30px rather than sliding back and doing 10px again.
 *
 * INERT IN THIS CORPUS, and said out loud rather than left implied:
 * interpolation-testcommon.js runs one iteration of a 100s effect and never
 * advances the timeline, so this branch is never taken by a WPT subtest here.
 * It is implemented because leaving `iterationComposite` accepted-and-ignored
 * is worse than not accepting it -- a page that sets it would animate wrongly
 * with no way to tell.
 *
 * The accumulation base is the LAST keyframe's value, accumulated once per
 * completed iteration, which is the spec's rule for the ordinary case of a
 * keyframe list whose final offset is 1. */
"Animation.prototype.__iterAccum = function(prop, v, lastv){\n"
"  var t = this.__t;\n"
"  if (t.iterationComposite !== 'accumulate') return v;\n"
"  if (lastv === undefined || lastv === null) return v;\n"
"  var d = t.duration;\n"
"  if (!(d > 0)) return v;\n"
"  var local = this.__hold - t.delay;\n"
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
"Animation.prototype.__props = function(){\n"
"  var s = {}, i, p;\n"
"  for (i = 0; i < this.__kf.length; i++) for (p in this.__kf[i].props) s[p] = 1;\n"
"  return s;\n"
"};\n"
"\n"
"Object.defineProperty(Animation.prototype, 'currentTime', {\n"
"  get: function(){ return this.__hold; },\n"
"  set: function(v){ this.__hold = (v === null) ? null : Number(v); },\n"
"  configurable: true, enumerable: true\n"
"});\n"
"Object.defineProperty(Animation.prototype, 'playState', {\n"
"  get: function(){ return this.__state; }, configurable: true, enumerable: true\n"
"});\n"
"Animation.prototype.pause = function(){ this.__state = 'paused'; };\n"
"Animation.prototype.play = function(){ this.__state = 'running'; if (this.__hold === null) this.__hold = 0; };\n"
"Animation.prototype.finish = function(){\n"
"  var t = this.__t;\n"
"  this.__hold = t.delay + t.duration * (t.iterations > 0 ? t.iterations : 1);\n"
"  this.__state = 'finished';\n"
"};\n"
"Animation.prototype.cancel = function(){\n"
"  this.__state = 'idle'; this.__hold = null;\n"
"  var l = listFor(this.__target, false);\n"
"  if (l) { var i = l.indexOf(this); if (i >= 0) l.splice(i, 1); }\n"
"};\n"
"Animation.prototype.reverse = function(){ this.playbackRate = -this.playbackRate; };\n"
"Animation.prototype.updatePlaybackRate = function(r){ this.playbackRate = Number(r); };\n"
"Animation.prototype.commitStyles = function(){};\n"
"Animation.prototype.persist = function(){};\n"
"Animation.prototype.addEventListener = function(){};\n"
"Animation.prototype.removeEventListener = function(){};\n"
"Object.defineProperty(Animation.prototype, 'finished', {\n"
"  get: function(){ return Promise.resolve(this); }, configurable: true\n"
"});\n"
"Object.defineProperty(Animation.prototype, 'ready', {\n"
"  get: function(){ return Promise.resolve(this); }, configurable: true\n"
"});\n"
"if (typeof globalThis !== 'undefined' && !globalThis.Animation) globalThis.Animation = Animation;\n"
"\n"
/* ---- keyframe values are COMPUTED values, not the strings handed in -------
 *
 * THE BUG THIS EXISTS TO NOT HAVE, found by measuring rather than by reading
 * the spec, and it is the one that turns a gain into a regression.
 *
 * A first cut interpolated the strings from the keyframes and reported the
 * result. That is right for margin-left and wrong wherever the engine's
 * computed value does not follow the specified one:
 *
 *   border-bottom-width: 100px  computes to 0px when border-style is none
 *   top: 100px                  computes to auto on a statically positioned box
 *
 * WPT compares the target against an element with the EXPECTED value set on
 * it, and that element's computed value goes through the same rule -- so both
 * sides read 0px, or both read auto, and the subtest passes. Reporting an
 * interpolated 150px against an expected 0px broke 275 subtests across 17
 * files that had been passing, which is exactly the shape of regression the
 * two silence rules were meant to prevent and did not: the base read is
 * "0px", not "", so RULE 1 let it through.
 *
 * So each endpoint is resolved to a computed value first, by the engine, on
 * the TARGET ELEMENT ITSELF -- same cascade, same inheritance, same
 * border-style and position that made the value collapse. Not a scratch
 * element in some other part of the tree, which would get a different answer
 * for precisely the properties this is about.
 *
 * Doing it here, once per animate(), rather than per computed read: the
 * keyframe values are resolved when the animation is created, which is also
 * what the spec says happens, and it bounds the cost at two extra cascades
 * per animation instead of one per property read.
 *
 * It pays for itself twice over: em, colour keywords and `initial` endpoints
 * all arrive already resolved, so pairs whose SPECIFIED forms have different
 * token shapes ("initial" against "20px") now have matching computed ones and
 * interpolate instead of declining.
 *
 * gcs, the ORIGINAL captured at install, never the patched one -- resolving through the wrapper would ask
 * the animation being constructed what it thinks the value is. */
"function resolveOn(el, prop, value){\n"
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
/* WHY THE ENDPOINTS ARE RESOLVED ON SCRATCH TWINS AND ALL AT ONCE, and this
 * is not an optimisation -- it is the difference between two endpoints and
 * one.
 *
 * The obvious shape is a loop: write endpoint 1 onto the target, read it back,
 * restore; write endpoint 2, read it back, restore. That is what this was, and
 * it silently returns THE SAME STRING for both, because a computed read does
 * not see the write above it.
 *
 * css_engine.c's css_ensure_styled() -- "update style before a computed read"
 * -- is guarded by inval_fingerprint(), which mixes the DOM arena's high-water
 * mark and, for each MARKED SCOPE ROOT, that root's own node/serial/attributes.
 * A scope root covers a subtree and js_dom.c's mark() coalesces into the
 * nearest one already held, so a style write on a DESCENDANT of a marked root
 * changes no term of the fingerprint, the flush is skipped, and the read
 * answers out of the previous cascade. Measured on a div inside a div inside
 * <body>, with css_style_flushes() alongside:
 *
 *     style.margin-left = 100px -> getComputedStyle = 100px   (flushes: 1)
 *     style.margin-left = 200px -> getComputedStyle = 100px   (flushes: 1)
 *     style.margin-left = 300px -> getComputedStyle = 100px   (flushes: 1)
 *
 * One cascade per document, ever. So `from` resolved correctly, `to` came back
 * as `from`, and every interpolation was a constant -- invisible, because the
 * constant is a real and plausible computed value.
 *
 * Forcing extra flushes is NOT the answer, and that was measured too rather
 * than assumed: an attribute write on documentElement between the write and
 * the read does re-arm the fingerprint and does make every read fresh, and it
 * costs 3,533 subtests in css/, because 2,867 of them are `CSS Transitions`
 * subtests that pass today only because BOTH the target and the expected
 * element read the same stale value. Un-sticking the cascade for everybody
 * exposes a feature this file does not implement. That belongs to whoever owns
 * css_engine.c, with the transitions work alongside it; it is not a change an
 * animation overlay gets to make on its way past.
 *
 * What IS available is the ordering. The single flush happens at the first
 * computed read after a mutation, and it styles EVERY element in the document
 * as it stands at that moment. So: create one scratch twin per keyframe,
 * carrying that keyframe's values, insert them all, and only then start
 * reading. Every endpoint is resolved by the one cascade instead of the first
 * one taking it and the rest reading its answer.
 *
 * A twin and not the target: cloneNode(false) keeps the tag, the classes and
 * the inline style, so `border-style: none` and `position: static` -- the two
 * cases this resolution exists for -- collapse the value exactly as they do on
 * the target, and the target itself is never mutated. The `id` goes, so a
 * duplicate never wins a getElementById.
 *
 * When the read still comes back "" (the flush already happened earlier in
 * this document, so the twins were never styled) the fallback is the target's
 * own computed value for that property -- which is byte-for-byte what the
 * write-and-read-back loop returned in that case, so the stale path behaves
 * exactly as it did before this change and only the fresh path improves. */
"Animation.prototype.__resolve = function(){\n"
"  var el = this.__target, i, p;\n"
"  if (!el || !el.style) return;\n"
"  var par = el.parentNode;\n"
"  var kf = this.__kf, jobs = [];\n"
/*   ONLY WHEN THERE IS MORE THAN ONE ENDPOINT TO RESOLVE, and this bound was
 *   measured, not assumed. The twins exist to get two endpoints out of one
 *   cascade; with a single keyframe there is only one endpoint, the ordinary
 *   write-and-read-back already resolves it, and a twin changes the answer for
 *   a reason that has nothing to do with resolution -- it is a different
 *   element, and the document is left slightly stirred by the insert and the
 *   remove. Unrestricted, that stirring cost 205 subtests in css/ (most of
 *   them `from neutral` cases and `CSS Transitions` bystanders) against 85
 *   gained. Restricted to the case it is for, it only pays. */
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
/*   THE UNDERLYING VALUES, read here and not later, and the ordering is
 *   load-bearing in both directions. It has to be AFTER the twins are in the
 *   document (this is the read that triggers the document's one style flush,
 *   and the flush is what styles them) and BEFORE the fallback path below
 *   writes anything onto the target, or the "underlying" value would be an
 *   endpoint of the very animation being composed onto it. */
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
/*   The read pass, strictly after every insertion above. */
"  for (var j = 0; j < jobs.length; j++) {\n"
"    var cs = null;\n"
"    try { cs = gcs.call(globalThis, jobs[j].s); } catch (e4) { cs = null; }\n"
"    if (!cs) continue;\n"
"    var props = kf[jobs[j].i].props;\n"
"    jobs[j].done = {};\n"
/*     PER PROPERTY, not per keyframe. A twin that was inserted but never
 *     styled -- the document's one flush had already happened -- answers ""
 *     for every property, and marking the whole keyframe done on the strength
 *     of having obtained a declaration object left those properties holding
 *     their SPECIFIED strings with no resolution at all. That is the 275-
 *     subtest regression the resolution exists to prevent, reintroduced
 *     through the back door: `border-top-width: 100px` stayed "100px" instead
 *     of collapsing to the "0px" a border-style-less box computes. */
"    for (p in props) {\n"
"      var c = '';\n"
"      try { c = cs.getPropertyValue(p); } catch (e5) { c = ''; }\n"
"      if (c === '' || c === null || c === undefined) continue;\n"
"      props[p] = c;\n"
"      jobs[j].done[p] = 1;\n"
"    }\n"
"  }\n"
"  for (j = 0; j < jobs.length; j++) {\n"
"    try { jobs[j].s.parentNode.removeChild(jobs[j].s); } catch (e6) {}\n"
"  }\n"
/*   THE FALLBACK, for anything the twins could not answer -- and it is two
 *   different answers, because "the read is stale" and "the value collapses on
 *   this element" look identical from here and must not be treated alike.
 *
 *   The write-and-read-back loop returns the target's LAST-CASCADED value for
 *   the property, whatever was written. That is exactly right when the cascade
 *   really does transform the property on this element -- `border-top-width`
 *   on a box with no border-style computes to 0px whatever number you give it,
 *   `top` on a statically positioned box computes to auto -- and exactly wrong
 *   when it does not, because then it throws the endpoint away and reports the
 *   underlying value in its place, which is how `from` and `to` became the
 *   same string in the first place.
 *
 *   The two are told apart by asking the ELEMENT rather than the value: if the
 *   target's own inline declaration for this property survives into its
 *   computed value unchanged, the cascade is not transforming it here and the
 *   keyframe's specified value can stand. If the inline value and the computed
 *   value disagree -- or there is no inline value to compare, which is the
 *   interpolation harness's shape -- the collapse is real (or unknown, and
 *   unknown must be conservative) and the last-cascaded value is the answer.
 *
 *   Measured, per direction: preferring the specified value everywhere gains
 *   89 composition subtests and loses 32 (the border-width and top families,
 *   which need the collapse); preferring the cascaded value everywhere loses
 *   nothing and gains only 18. This test keeps both. */
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
"      kf[i].props[p] = resolveOn(el, p, kf[i].props[p]);\n"
"    }\n"
"  }\n"
"\n"
/*   COMPOSITION, last: it consumes computed values on both sides, so it can
 *   only run once every endpoint has been through the resolution above. A
 *   keyframe whose composite operation is `replace` is untouched, and one the
 *   native declines (a shape it cannot combine, a discrete type, or no
 *   underlying value at all) keeps its resolved value -- which is what a type
 *   with no addition defined is specified to do with `add`. */
"  for (i = 0; i < kf.length; i++) {\n"
"    var op = this.__opOf(i);\n"
"    if (op === 'replace') continue;\n"
"    for (p in kf[i].props) {\n"
"      var u = base[p];\n"
"      if (u === '' || u === null || u === undefined) continue;\n"
"      var cv = null;\n"
"      try { cv = CC(p, u, kf[i].props[p], op); } catch (ec) { cv = null; }\n"
"      if (cv !== null && cv !== undefined) kf[i].props[p] = cv;\n"
"    }\n"
"  }\n"
"};\n"
"\n"
/* The composite operation in force for keyframe `i`: the keyframe's own if it
 * declares one, otherwise the effect-level default from the options bag,
 * otherwise `replace`. That is the spec's order and also the order the corpus
 * exercises -- interpolation-testcommon.js puts `composite` on each keyframe
 * and never on the options. */
"Animation.prototype.__opOf = function(i){\n"
"  var k = this.__kf[i];\n"
"  var c = (k && k.composite) ? String(k.composite) : '';\n"
"  if (c === 'add' || c === 'accumulate' || c === 'replace') return c;\n"
"  var e = this.__t.composite;\n"
"  if (e === 'add' || e === 'accumulate') return e;\n"
"  return 'replace';\n"
"};\n"
"\n"
"EP.animate = function(keyframes, options){\n"
"  var a = new Animation(this, keyframes, options);\n"
"  try { a.__resolve(); } catch (e) {}\n"
"  listFor(this, true).push(a);\n"
"  return a;\n"
"};\n"
"EP.getAnimations = function(){ var l = listFor(this, false); return l ? l.slice() : []; };\n"
"\n"
/* ---- the computed-style overlay ----------------------------------------
 * A Proxy rather than a copied object: a computed CSSStyleDeclaration carries
 * length, item(), indexed access and ~63 named accessors, and rebuilding that
 * to add two overrides would be a second, diverging implementation of
 * js_dom.c's object. The trap answers for an animated property and delegates
 * everything else, so the only observable difference is the value of a
 * property that is actually being animated.
 *
 * Methods are bound to the TARGET, not the proxy: they are js_dom.c natives
 * that read an opaque pointer off `this`, and a proxy is not that object. */
"function wrap(base, el){\n"
"  return new Proxy(base, {\n"
"    get: function(t, k, r){\n"
"      if (k === 'getPropertyValue') return function(p){\n"
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
"\n"
"function animVal(el, prop, base){\n"
"  var l = listFor(el, false);\n"
"  if (!l || !l.length) return null;\n"
"  for (var i = l.length - 1; i >= 0; i--) {\n"
"    var v = l[i].__valueAt(prop);\n"
/*   RULE 2: the interpolation declined -- an `initial` keyframe, a keyword
 *   against a length, a shape css_interp cannot bridge. Say nothing. */
"    if (v === null || v === undefined) continue;\n"
/*   RULE 1: the engine does not report this property at all (a shorthand, or
 *   a property LibCSS does not know). Answering here would break the many
 *   subtests that pass today because BOTH sides read "". */
"    if (base.getPropertyValue(prop) === '') return null;\n"
"    return v;\n"
"  }\n"
"  return null;\n"
"}\n"
"\n"
"var origGCS = gcs;\n"
"function patched(el, pseudo){\n"
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
"  document.getAnimations = function(){ return []; };\n"
"}\n"
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
