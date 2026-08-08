/* anim_test.c -- js_anim.c's timing math, against WPT's own createEasing.
 *
 * WHY THIS TEST AND NOT A WPT RUN. The corpus proves the feature works; it
 * takes half an hour and needs 239 MB of vendored data. This is the same
 * question in two seconds, and it asks it more sharply -- because the ORACLE
 * here is the exact function the corpus uses to generate its own expectations.
 *
 * interpolation-testcommon.js never advances a timeline. Duration is 100s,
 * currentTime is set to 50s, so the input progress is ALWAYS exactly 0.5, and
 * every distinct `at` the whole 337-file corpus tests is produced by
 *
 *     createEasing(y):  y == 0   -> 'steps(1, end)'
 *                       y == 1   -> 'steps(1, start)'
 *                       y == 0.5 -> 'linear'
 *                       else     -> cubic-bezier(0, b, 1, b), b = (8y - 1) / 6
 *
 * transcribed verbatim below. So the property under test is an INVERSE one:
 * feed our easing the 0.5 that the corpus feeds it, and it must give back the
 * `at` the corpus asked for. Interpolating 0px -> 100px then reads that
 * progress out as a number of pixels, which turns "the bezier solver is
 * approximately right" into a string comparison.
 *
 * The two `at` values outside the unit interval, -0.3 and 1.5, are the ones
 * that matter. A cubic-bezier solver that clamps its output to [0, 1] -- the
 * obvious implementation, and what a naive "progress is a fraction" reading
 * produces -- returns 0px and 100px for those, passes every other row here,
 * and silently collapses two of every seven subtests in the corpus onto an
 * endpoint.
 *
 * The last two cases are the two RULES that make the overlay incapable of
 * regressing a passing subtest, and they are tested because they are load
 * bearing in the opposite direction from everything else: they are the reason
 * this feature can only add.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "quickjs.h"

void js_anim_install(JSContext *ctx);

static int g_fail;

/* The environment js_anim.c expects to find: an Element with a prototype, and
 * a getComputedStyle whose answers come from a table the test controls. Small
 * on purpose -- the point is that js_anim.c composes with whatever
 * getComputedStyle is installed rather than reaching past it. */
static const char STUB_JS[] =
"globalThis.Element = function Element(){};\n"
/* The cascade, in miniature. Two behaviours matter and both are real ones the
 * engine has:
 *   - a property the computed-style table does not model answers '' (every
 *     shorthand, and everything LibCSS does not know), which is what RULE 1
 *     is about;
 *   - a property whose computed value does NOT follow its specified value.
 *     `border-top-width` collapses to 0px because border-style is none, and
 *     `top` computes to `auto` on a statically positioned box. Those two
 *     broke 275 previously-passing subtests when the first cut of this
 *     binding reported an interpolated SPECIFIED value, so they are modelled
 *     here rather than only in the corpus. */
"globalThis.__store = { 'margin-left': '0px', 'opacity': '0',\n"
"                       'border-top-width': '0px', 'top': 'auto' };\n"
"globalThis.__collapse = { 'border-top-width': '0px', 'top': 'auto' };\n"
"globalThis.getComputedStyle = function(el, pseudo){\n"
"  return {\n"
"    getPropertyValue: function(p){\n"
"      if (Object.prototype.hasOwnProperty.call(globalThis.__collapse, p))\n"
"        return globalThis.__collapse[p];\n"
"      if (el && el.__inline && Object.prototype.hasOwnProperty.call(el.__inline, p))\n"
"        return el.__inline[p];\n"
"      return Object.prototype.hasOwnProperty.call(globalThis.__store, p)\n"
"           ? globalThis.__store[p] : '';\n"
"    },\n"
"    item: function(){ return ''; },\n"
"    length: 0\n"
"  };\n"
"};\n"
"globalThis.mkel = function(){\n"
"  var e = Object.create(Element.prototype);\n"
"  e.__inline = {};\n"
"  e.style = {\n"
"    setProperty: function(p, v){ e.__inline[p] = String(v); },\n"
"    removeProperty: function(p){ delete e.__inline[p]; },\n"
"    getPropertyValue: function(p){\n"
"      return Object.prototype.hasOwnProperty.call(e.__inline, p) ? e.__inline[p] : '';\n"
"    },\n"
"    getPropertyPriority: function(){ return ''; }\n"
"  };\n"
"  return e;\n"
"};\n";

static const char TEST_JS[] =
"var out = [];\n"
"function say(name, got, want){\n"
"  out.push((String(got) === String(want) ? 'ok   ' : 'FAIL ') + name +\n"
"           (String(got) === String(want) ? '' : '  got ' + got + ' want ' + want));\n"
"}\n"
"\n"
/* Verbatim from third_party/wpt/css/support/interpolation-testcommon.js. */
"function createEasing(y) {\n"
"  if (y == 0) return 'steps(1, end)';\n"
"  if (y == 1) return 'steps(1, start)';\n"
"  if (y == 0.5) return 'linear';\n"
"  var b = (8 * y - 1) / 6;\n"
"  return 'cubic-bezier(0, ' + b + ', 1, ' + b + ')';\n"
"}\n"
"\n"
"say('animate exists on Element.prototype', ('animate' in Element.prototype), true);\n"
"\n"
/* The corpus's own seven progress values. */
"var ats = [-0.3, 0, 0.3, 0.5, 0.6, 1, 1.5];\n"
"for (var i = 0; i < ats.length; i++) {\n"
"  var at = ats[i];\n"
"  var el = mkel();\n"
"  var a = el.animate([{ offset: 0, marginLeft: '0px' },\n"
"                      { offset: 1, marginLeft: '100px' }],\n"
"                     { fill: 'forwards', duration: 100 * 1000, easing: createEasing(at) });\n"
"  a.pause();\n"
"  a.currentTime = 50 * 1000;\n"
"  say('at ' + at, getComputedStyle(el).getPropertyValue('margin-left'),\n"
"      (at * 100) + 'px');\n"
"}\n"
"\n"
/* The keyframes are camelCase and the read is dashed -- the harness does
 * exactly this, and a binding that does not fold the two computes the right
 * value under a key nothing ever looks up. */
"(function(){\n"
"  var el = mkel();\n"
"  var a = el.animate([{ offset: 0, marginLeft: '0px' }, { offset: 1, marginLeft: '40px' }],\n"
"                     { duration: 100, easing: 'linear' });\n"
"  a.pause(); a.currentTime = 25;\n"
"  say('camelCase keyframe read back dashed',\n"
"      getComputedStyle(el).getPropertyValue('margin-left'), '10px');\n"
"  say('and through the named accessor', getComputedStyle(el).marginLeft, '10px');\n"
"})();\n"
"\n"
/* An un-animated element must come back completely untouched -- same object
 * shape, same answers, no proxy. */
"(function(){\n"
"  var el = mkel();\n"
"  say('no animation, no overlay',\n"
"      getComputedStyle(el).getPropertyValue('margin-left'), '0px');\n"
"})();\n"
"\n"
/* RULE 1. `margin-top` is not in the stub's table, so the engine reports ''\n"
 * for it -- as it really does for every shorthand and every property LibCSS\n"
 * does not know. The overlay must stay silent: a great many corpus subtests\n"
 * pass today only because BOTH the target and the expected element read '',\n"
 * and answering here would turn those passes into failures. */
"(function(){\n"
"  var el = mkel();\n"
"  var a = el.animate([{ offset: 0, marginTop: '0px' }, { offset: 1, marginTop: '100px' }],\n"
"                     { duration: 100, easing: 'linear' });\n"
"  a.pause(); a.currentTime = 50;\n"
"  say('RULE 1: silent for a property the engine does not report',\n"
"      getComputedStyle(el).getPropertyValue('margin-top'), '');\n"
"})();\n"
"\n"
/* RULE 2. css_interp declines `initial` against a length -- resolving a\n"
 * keyword needs the cascade, which this layer does not have. The overlay must\n"
 * leave today's answer standing rather than invent one. */
"(function(){\n"
"  var el = mkel();\n"
"  var a = el.animate([{ offset: 0, marginLeft: 'initial' }, { offset: 1, marginLeft: '100px' }],\n"
"                     { duration: 100, easing: 'linear' });\n"
"  a.pause(); a.currentTime = 50;\n"
"  say('RULE 2: silent when the interpolation declines',\n"
"      getComputedStyle(el).getPropertyValue('margin-left'), '0px');\n"
"})();\n"
"\n"
/* An animation that is not in effect contributes nothing: before its delay\n"
 * with no backwards fill, and after cancel(). */
"(function(){\n"
"  var el = mkel();\n"
"  var a = el.animate([{ offset: 0, marginLeft: '0px' }, { offset: 1, marginLeft: '100px' }],\n"
"                     { duration: 100, delay: 1000, easing: 'linear' });\n"
"  a.pause(); a.currentTime = 0;\n"
"  say('before the delay with fill:none contributes nothing',\n"
"      getComputedStyle(el).getPropertyValue('margin-left'), '0px');\n"
"  a.currentTime = 1050;\n"
"  say('inside the active interval it does',\n"
"      getComputedStyle(el).getPropertyValue('margin-left'), '50px');\n"
"  a.cancel();\n"
"  say('cancel() removes it',\n"
"      getComputedStyle(el).getPropertyValue('margin-left'), '0px');\n"
"})();\n"
"\n"
"\n"
/* THE 275-SUBTEST REGRESSION, kept as a check because measuring found it and
 * reading the spec did not. A keyframe value is a COMPUTED value: the engine
 * collapses border-top-width to 0px when border-style is none, WPT's expected
 * element goes through the same rule and also reads 0px, and a binding that
 * interpolates the specified 100px..200px reports 150px against an expected
 * 0px. Both endpoints must be resolved through the target first. */
"(function(){\n"
"  var el = mkel();\n"
"  var a = el.animate([{ offset: 0, borderTopWidth: '100px' },\n"
"                      { offset: 1, borderTopWidth: '200px' }],\n"
"                     { duration: 100, easing: 'linear' });\n"
"  a.pause(); a.currentTime = 50;\n"
"  say('endpoints resolve to computed values (collapsing width)',\n"
"      getComputedStyle(el).getPropertyValue('border-top-width'), '0px');\n"
"})();\n"
"(function(){\n"
"  var el = mkel();\n"
"  var a = el.animate([{ offset: 0, top: '100px' }, { offset: 1, top: '200px' }],\n"
"                     { duration: 100, easing: 'linear' });\n"
"  a.pause(); a.currentTime = 50;\n"
"  say('endpoints resolve to computed values (auto)',\n"
"      getComputedStyle(el).getPropertyValue('top'), 'auto');\n"
"})();\n"
"\n"
/* Resolving writes to the target's inline style and must put it back exactly.
 * The composition tests set an underlying value there before calling
 * animate(), and losing it would corrupt the very thing being animated. */
"(function(){\n"
"  var el = mkel();\n"
"  el.style.setProperty('margin-left', '50px');\n"
"  var a = el.animate([{ offset: 0, marginLeft: '0px' }, { offset: 1, marginLeft: '100px' }],\n"
"                     { duration: 100, easing: 'linear' });\n"
"  say('the underlying inline value survives endpoint resolution',\n"
"      el.style.getPropertyValue('margin-left'), '50px');\n"
"  a.pause(); a.currentTime = 50;\n"
"  say('and the animation still runs over it',\n"
"      getComputedStyle(el).getPropertyValue('margin-left'), '50px');\n"
"})();\n"
"\n"
"globalThis.__result = out.join('\\n');\n";

static int run_js(JSContext *ctx, const char *src, size_t n, const char *name)
{
    JSValue r = JS_Eval(ctx, src, n, name, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        JSValue e = JS_GetException(ctx);
        const char *s = JS_ToCString(ctx, e);
        printf("  FAIL %s threw: %s\n", name, s ? s : "(unprintable)");
        if (s) JS_FreeCString(ctx, s);
        JSValue st = JS_GetPropertyStr(ctx, e, "stack");
        if (!JS_IsUndefined(st)) {
            const char *ss = JS_ToCString(ctx, st);
            if (ss) { printf("%s\n", ss); JS_FreeCString(ctx, ss); }
        }
        JS_FreeValue(ctx, st);
        JS_FreeValue(ctx, e);
        JS_FreeValue(ctx, r);
        g_fail++;
        return 0;
    }
    JS_FreeValue(ctx, r);
    return 1;
}

int main(void)
{
    printf("js_anim: Element.prototype.animate timing, against WPT's createEasing\n");
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    if (!rt || !ctx) { printf("FAIL: no QuickJS runtime\n"); return 1; }

    if (!run_js(ctx, STUB_JS, sizeof STUB_JS - 1, "<stub>")) return 1;

    js_anim_install(ctx);

    if (!run_js(ctx, TEST_JS, sizeof TEST_JS - 1, "<anim_test>")) return 1;

    JSValue g = JS_GetGlobalObject(ctx);
    JSValue res = JS_GetPropertyStr(ctx, g, "__result");
    const char *s = JS_ToCString(ctx, res);
    int checks = 0;
    if (s) {
        const char *p = s;
        while (*p) {
            const char *nl = strchr(p, '\n');
            int len = nl ? (int)(nl - p) : (int)strlen(p);
            printf("  %.*s\n", len, p);
            if (len >= 4 && !strncmp(p, "FAIL", 4)) g_fail++;
            checks++;
            if (!nl) break;
            p = nl + 1;
        }
        JS_FreeCString(ctx, s);
    } else {
        printf("  FAIL the test script produced no result\n");
        g_fail++;
    }
    JS_FreeValue(ctx, res);
    JS_FreeValue(ctx, g);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    printf("js_anim: %d checks, %d failed\n", checks, g_fail);
    if (g_fail) { printf("FAIL\n"); return 1; }
    printf("ok\n");
    return 0;
}
