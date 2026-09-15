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
"globalThis.__store = { 'margin-left': '0px', 'opacity': '0', 'transform':'none',\n"
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

/* Correction 2026-09-09, kept beside the historical claim above: the
 * margin-left proxy assertions proved interpolation math, not presentation.
 * That property never reached a display-list producer. Use transform's real
 * supported subset to observe the SAME seven easing values; native paint has
 * its separate test-waapi-paint gate. Unsupported properties must now refuse. */
static const char TEST_JS[] =
"var out=[];\n"
"function say(name,got,want){out.push((String(got)===String(want)?'ok   ':'FAIL ')+name+(String(got)===String(want)?'':'  got '+got+' want '+want));}\n"
"function createEasing(y){if(y==0)return 'steps(1, end)';if(y==1)return 'steps(1, start)';if(y==.5)return 'linear';var b=(8*y-1)/6;return 'cubic-bezier(0, '+b+', 1, '+b+')';}\n"
"function translation(el){var v=getComputedStyle(el).getPropertyValue('transform');return Math.round(Number(v.slice(7,-1).split(',')[4])*1000)/1000;}\n"
"say('animate exists on Element.prototype','animate' in Element.prototype,true);\n"
"var ats=[-.3,0,.3,.5,.6,1,1.5];\n"
"for(var i=0;i<ats.length;i++){\n"
" var at=ats[i],el=mkel(),a=el.animate([{transform:'translateX(0px)'},{transform:'translateX(100px)'}],{fill:'both',duration:100000,easing:createEasing(at)});\n"
" a.pause();a.currentTime=50000;say('at '+at,translation(el),at*100);\n"
"}\n"
"(function(){\n"
" var el=mkel(),a=el.animate([{opacity:0},{opacity:1}],{duration:100,fill:'both'});\n"
" a.pause();a.currentTime=25;say('named opacity accessor',getComputedStyle(el).opacity,'0.25');\n"
" a.cancel();say('cancel restores base',getComputedStyle(el).getPropertyValue('opacity'),'0');\n"
"})();\n"
"(function(){\n"
" var el=mkel();el.style.setProperty('opacity','.7');\n"
" var a=el.animate([{opacity:0},{opacity:1}],{duration:100,delay:1000});a.pause();a.currentTime=0;\n"
" say('before delay restores base',getComputedStyle(el).getPropertyValue('opacity'),'.7');\n"
" a.currentTime=1050;say('inside interval',getComputedStyle(el).getPropertyValue('opacity'),'0.5');\n"
" say('inline value preserved',el.style.getPropertyValue('opacity'),'.7');\n"
"})();\n"
"function refuses(name,kf,opt){try{mkel().animate(kf,opt||100);say(name,'accepted','NotSupportedError');}catch(e){say(name,e.name,'NotSupportedError');}}\n"
"refuses('width cannot report unpainted success',[{width:'0px'},{width:'100px'}]);\n"
"refuses('unknown property refusal',[{frobnicate:'a'},{frobnicate:'b'}]);\n"
"refuses('relative transform refused',[{transform:'translateX(0%)'},{transform:'translateX(100%)'}]);\n"
"refuses('3D transform refused',[{transform:'rotateX(0deg)'},{transform:'rotateX(90deg)'}]);\n"
"refuses('unresolved keyword refused',[{opacity:'initial'},{opacity:1}]);\n"
"refuses('pseudo targeting refused',[{opacity:0},{opacity:1}],{duration:100,pseudoElement:'::before'});\n"
"refuses('composite refused',[{opacity:0},{opacity:1}],{duration:100,composite:'add'});\n"
"globalThis.__result=out.join('\\n');\n"
;

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
