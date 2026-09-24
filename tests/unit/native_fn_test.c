/* native_fn_test.c -- QuickJS's host-native mark (JS_MarkNativeFunction,
 * JS_MarkNativeCode, JS_GetOwnPropertyQuiet), on a bare engine.
 *
 * WHAT IS BEING CLAIMED. A function the embedder marks is, to the page, a
 * built-in:
 *   - Function.prototype.toString gives "function NAME() { [native code] }",
 *     one line, NAME its own `name` -- and so does every other closure of
 *     the same code, including ones created AFTER the mark;
 *   - an operation has no own `prototype` and `new` throws TypeError, while
 *     its name, length and behaviour are untouched;
 *   - a constructor mark changes the spelling only;
 *   - .caller/.arguments throw, .fileName is undefined, and it is never
 *     handed out as a page function's .caller;
 *   - a function evidently used as a class is not stripped whatever the
 *     embedder asked, because a later `new` would break;
 *   - and an UNMARKED function is exactly what it was: source text,
 *     `prototype`, constructible.
 * JS_MarkNativeCode(ctx, JS_NATIVE_CODE_OPERATIONS) marks what the embedder
 * compiled and NOT what eval / the Function constructor compiled (a host
 * prelude compiling a page's onload="..." attribute is compiling page code),
 * and makes every closure of host code made AFTER it a built-in operation
 * unless a capital name, at the code or at the binding, says constructor.
 * Plus the engine-wide spelling of native code (the C built-ins print the
 * same single line) and the quiet descriptor reader the page pass walks with.
 *
 * TWO CONTROLS, each the most plausible wrong version, each required to fail
 * EXACTLY its own tag and nothing else -- tests/native_shape.mk counts:
 *   -DJS_NATIVE_NEGCTL_KEEP_CONSTRUCT  `prototype` deleted, [[Construct]]
 *       kept. Reads right to every check except the ones that `new` it:
 *       [construct].
 *   -DJS_NATIVE_NEGCTL_MARK_EVAL       JS_MarkNativeCode's first version,
 *       which marked everything the realm had compiled: [eval]. */
#include <stdio.h>
#include <string.h>
#include "quickjs.h"

static JSContext *ctx;
static int checks, fails, construct_fails;

static void ck(const char *tag, const char *what, const char *expr)
{
    checks++;
    JSValue v = JS_Eval(ctx, expr, strlen(expr), "<check>", JS_EVAL_TYPE_GLOBAL);
    int ok = !JS_IsException(v) && JS_ToBool(ctx, v);
    if (JS_IsException(v)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        printf("FAIL [%s] %s\n     threw: %s\n", tag, what, m ? m : "?");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
    } else if (!ok) {
        printf("FAIL [%s] %s\n     expr: %s\n", tag, what, expr);
    } else {
        printf("ok   [%s] %s\n", tag, what);
    }
    if (!ok) { fails++; if (!strcmp(tag, "construct")) construct_fails++; }
    JS_FreeValue(ctx, v);
}

static void run(const char *src, const char *file)
{
    JSValue v = JS_Eval(ctx, src, strlen(src), file, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        printf("FAIL setup %s threw: %s\n", file, m ? m : "?");
        fails++;
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, v);
}

static JSValue global_get(const char *name)
{
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue v = JS_GetPropertyStr(ctx, g, name);
    JS_FreeValue(ctx, g);
    return v;
}

static int mark(const char *name, int flags)
{
    JSValue f = global_get(name);
    int r = JS_MarkNativeFunction(ctx, f, flags);
    JS_FreeValue(ctx, f);
    return r;
}

static int get_calls;
static JSValue counting_getter(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)c; (void)this_val; (void)argc; (void)argv;
    get_calls++;
    return JS_NewInt32(c, 7);
}

int main(void)
{
    JSRuntime *rt = JS_NewRuntime();
    ctx = JS_NewContext(rt);

    /* The "host": code compiled from a prelude, as the browser's are. */
    run("var hostLog = [];"
        "globalThis.querySelector = function querySelector(sel) { return 'qs:' + sel; };"
        "globalThis.anon = function (a, b) { return a + b; };"
        "globalThis.acc = { get x() { return 5; }, set x(v) { hostLog.push(v); } };"
        "globalThis.fetchLike = async function fetchLike(u) { return 'got ' + u; };"
        "globalThis.entries = function* entries() { yield 1; yield 2; };"
        "globalThis.Iface = function Iface() { this.made = true; };"
        "globalThis.KlassLike = function KlassLike() { this.k = 1; };"
        "KlassLike.prototype.hello = function () { return 'hi'; };"
        "globalThis.RealClass = class RealClass { m() { return 3; } };"
        "globalThis.mk = function mk() { return function inner() { return 'inner'; }; };"
        "globalThis.early = mk(); globalThis.twin = mk();"
        "globalThis.dispatch = function dispatch(cb) { return cb(); };"
        "globalThis.hostCaller = function hostCaller(cb) { return cb(); };",
        "<host>");
    /* The "page": compiled from a URL, never marked. */
    run("globalThis.pageFn = function pageFn(x) { return x * 2; };"
        "globalThis.PageCtor = function PageCtor() { this.p = 1; };",
        "https://page.example/app.js");

    /* ---- the marks ---- */
    int r_qs = mark("querySelector", 0);
    int r_anon = mark("anon", 0);
    {
        JSValue o = global_get("acc");
        JSAtom ax = JS_NewAtom(ctx, "x");
        JSPropertyDescriptor d;
        int k = JS_GetOwnPropertyQuiet(ctx, &d, o, ax);
        if (k == JS_QUIET_ACCESSOR) {
            JS_MarkNativeFunction(ctx, d.getter, 0);
            JS_MarkNativeFunction(ctx, d.setter, 0);
        } else { printf("FAIL setup: acc.x quiet read gave %d\n", k); fails++; }
        JS_FreeValue(ctx, d.getter); JS_FreeValue(ctx, d.setter); JS_FreeValue(ctx, d.value);
        JS_FreeAtom(ctx, ax);
        JS_FreeValue(ctx, o);
    }
    int r_fetch = mark("fetchLike", 0);
    int r_gen = mark("entries", 0);
    int r_iface = mark("Iface", JS_NATIVE_FN_CONSTRUCTOR);
    int r_klass = mark("KlassLike", 0);          /* asked to strip; must refuse */
    int r_class = mark("RealClass", 0);          /* ditto */
    int r_early = mark("early", 0);
    mark("dispatch", 0);
    int r_math = mark("parseInt", 0);            /* a C built-in */
    {
        JSValue n = JS_NewInt32(ctx, 3);
        int r_num = JS_MarkNativeFunction(ctx, n, 0);
        checks++;
        if (r_num != -1) { fails++; printf("FAIL [shape] a non-function is refused (-1), got %d\n", r_num); }
        else printf("ok   [shape] a non-function is refused (-1)\n");
    }

    printf("== toString ==\n");
    ck("tostring", "a marked method stringifies as native code, one line, own name",
       "Function.prototype.toString.call(querySelector) === 'function querySelector() { [native code] }'");
    ck("tostring", "String(fn) agrees", "String(querySelector) === 'function querySelector() { [native code] }'");
    ck("tostring", "an accessor pair: get x / set x",
       "(function(){ var d = Object.getOwnPropertyDescriptor(acc, 'x');"
       "  return d.get.toString() === 'function get x() { [native code] }' &&"
       "         d.set.toString() === 'function set x() { [native code] }'; })()");
    ck("tostring", "an async host function is spelled 'function', not 'async function'",
       "fetchLike.toString() === 'function fetchLike() { [native code] }'");
    ck("tostring", "a generator host function is spelled 'function', not 'function *'",
       "entries.toString() === 'function entries() { [native code] }'");
    ck("tostring", "an anonymous marked function prints an empty name (the pass names it)",
       "anon.toString() === 'function () { [native code] }'");
    ck("tostring", "a constructor mark: native spelling",
       "Iface.toString() === 'function Iface() { [native code] }'");
    ck("tostring", "the refused-strip class still gets the native spelling",
       "KlassLike.toString() === 'function KlassLike() { [native code] }' &&"
       "RealClass.toString() === 'function RealClass() { [native code] }'");
    ck("tostring", "the mark is on the CODE: a sibling closure made before the mark reads native",
       "twin.toString() === 'function inner() { [native code] }'");
    ck("tostring", "...and so does one made after it",
       "mk().toString() === 'function inner() { [native code] }'");
    ck("tostring", "the name is the OWN name data property, read without running a getter",
       "(function(){ Object.defineProperty(querySelector, 'name', { get: function () { throw new Error('ran'); }, configurable: true });"
       "  var s = querySelector.toString();"
       "  Object.defineProperty(querySelector, 'name', { value: 'querySelector', configurable: true });"
       "  return s === 'function () { [native code] }'; })()");
    ck("tostring", "C built-ins use the same single line",
       "Function.prototype.toString.call(Math.max) === 'function max() { [native code] }' &&"
       "Function.prototype.toString.call(Function.prototype.toString) === 'function toString() { [native code] }'");
    ck("tostring", "the swap-the-name idiom holds between a C built-in and a marked host function",
       "Function.prototype.toString.call(Math.max).replace('max', 'querySelector') === String(querySelector)");
    ck("tostring", "a Proxy of a marked function reads as its target, without a get trap",
       "(function(){ var hit = 0; var p = new Proxy(querySelector, { get: function (t, k) { hit++; return t[k]; } });"
       "  return Function.prototype.toString.call(p) === 'function querySelector() { [native code] }' && hit === 0; })()");

    printf("== operation shape ==\n");
    ck("shape", "a marked method has no own prototype",
       "!Object.prototype.hasOwnProperty.call(querySelector, 'prototype') && !('prototype' in querySelector)");
    ck("shape", "...and owns exactly length and name",
       "Object.getOwnPropertyNames(querySelector).sort().join() === 'length,name'");
    ck("construct", "new on a marked method throws TypeError",
       "(function(){ try { new querySelector('x'); return false; } catch (e) { return e instanceof TypeError; } })()");
    ck("construct", "Reflect.construct refuses it as newTarget too",
       "(function(){ try { Reflect.construct(function(){}, [], querySelector); return false; }"
       "  catch (e) { return e instanceof TypeError; } })()");
    /* Throws in the control build too (no `prototype` object to extend), so
       it is a shape check, not a [construct] one. */
    ck("shape", "class extends a marked method throws",
       "(function(){ try { (class extends querySelector {}); return false; } catch (e) { return e instanceof TypeError; } })()");
    ck("construct", "an anonymous marked function is not constructible either",
       "(function(){ try { new anon(); return false; } catch (e) { return e instanceof TypeError; } })()");
    ck("shape", "accessor functions are not constructible (object-literal accessors never were)",
       "(function(){ var d = Object.getOwnPropertyDescriptor(acc, 'x');"
       "  try { new d.get(); return false; } catch (e) { return e instanceof TypeError; } })()");
    ck("shape", "name and length are preserved, and it still works",
       "querySelector.name === 'querySelector' && querySelector.length === 1 && querySelector('p') === 'qs:p' &&"
       "anon.length === 2 && anon(2, 3) === 5");
    ck("shape", "accessors still work", "(function(){ acc.x = 9; return acc.x === 5 && hostLog[0] === 9; })()");
    ck("shape", "an async host function's [[Prototype]] is Function.prototype",
       "Object.getPrototypeOf(fetchLike) === Function.prototype && fetchLike.constructor === Function");
    ck("shape", "...and it still returns a promise", "fetchLike('u') instanceof Promise");
    ck("shape", "a generator host function: no prototype, Function.prototype parent, still iterates",
       "!('prototype' in entries) && Object.getPrototypeOf(entries) === Function.prototype &&"
       "Array.from(entries()).join() === '1,2' && typeof entries().next === 'function'");
    ck("shape", "a constructor mark keeps constructibility and prototype",
       "new Iface().made === true && typeof Iface.prototype === 'object' && Iface.prototype.constructor === Iface");
    ck("shape", "a function used as a class keeps its shape even when asked to strip",
       "new KlassLike().hello() === 'hi' && new RealClass().m() === 3");
    ck("shape", "per OBJECT: the marked closure lost its prototype, its unmarked twin kept it",
       "!('prototype' in early) && typeof twin.prototype === 'object'");
    ck("shape", "a marked function's .caller and .arguments throw like a built-in's",
       "(function(){ var a = 0; try { dispatch.caller; } catch (e) { a++; } try { dispatch.arguments; } catch (e) { a++; } return a === 2; })()");
    ck("shape", ".fileName and .lineNumber say nothing about host code",
       "querySelector.fileName === undefined && querySelector.lineNumber === undefined &&"
       "pageFn.fileName === 'https://page.example/app.js'");
    ck("shape", "a page callback run by host code sees caller === null",
       "(function(){ var seen = 'unset'; function cb() { seen = cb.caller; } dispatch(cb); return seen === null; })()");

    printf("== unmarked stays itself ==\n");
    ck("unmarked", "a page function still stringifies to its source",
       "pageFn.toString() === 'function pageFn(x) { return x * 2; }'");
    ck("unmarked", "...keeps its prototype and constructs",
       "typeof pageFn.prototype === 'object' && new PageCtor().p === 1");
    ck("unmarked", "an unmarked host function run by host code is still reported as caller (the control for the null above)",
       "(function(){ var seen; function cb() { seen = cb.caller; } hostCaller(cb); return seen === hostCaller; })()");
    ck("unmarked", "an unmarked host function stringifies to its source",
       "hostCaller.toString().indexOf('return cb()') > 0");

    /* ---- JS_MarkNativeCode: everything the embedder compiled so far,
     *      nothing eval compiled, nothing compiled after ---- */
    run("globalThis.laterHost = function laterHost() { return 1; };"
        /* factories whose closures are first made AFTER the mark */
        "globalThis.opFactory = function () { return { item: function (i) { return i; },"
        "                                               Named: function () { this.n = 1; } }; };"
        "globalThis.mkVarCtor = function () { var C = function () { this.c = 1; };"
        "  C.prototype.hi = function () { return 'hi'; }; return C; };"
        "globalThis.mkDeclCtor = function () { function Decl() { this.d = 1; } return Decl; };"
        "globalThis.mkHelper = function () { function helper() { return 2; } return helper; };"
        "globalThis.mkAsync = function () { return async function () { return 3; }; };"
        "globalThis.mkGen = function () { return function* () { yield 4; }; };"
        "globalThis.mkAssigned = function (o) { o.Foo = function () { this.f = 1; }; return o.Foo; };"
        "globalThis.mkComputed = function (k) { return ({ [k]: function () { this.q = 1; } })[k]; };"
        /* the walk's verdict on one closure decides its code's later ones */
        "globalThis.mkLowerIface = function () { return function () { this.x = 1; }; };"
        "globalThis.lowerIface1 = mkLowerIface();"
        "globalThis.mkUpperOp = function () { return function Upper() { return 'u'; }; };"
        "globalThis.upperOp1 = mkUpperOp();"
        /* PAGE content the host compiles at install time -- a <body onload>
           attribute through the Function constructor (js/style/cssom.c) --
           and a direct eval: both the running program's code, never marked */
        "globalThis.attrHandler = new Function('event',"
        "  'globalThis.innerFromAttr = function innerFromAttr() { return 5; }; return 6;');"
        "globalThis.evald = eval('(function evald() { return 7; })');",
        "<host2>");
    int r_low = mark("lowerIface1", JS_NATIVE_FN_CONSTRUCTOR);
    int r_up = mark("upperOp1", 0);
    int n_code = JS_MarkNativeCode(ctx, JS_NATIVE_CODE_OPERATIONS);
    run("globalThis.afterPage = function afterPage() { return 2; };"
        "globalThis.afterPageLower = function () { this.p = 1; };", "https://page.example/late.js");
    printf("== JS_MarkNativeCode ==\n");
    checks++;
    if (n_code <= 0 || r_low != 0 || r_up != 1) {
        fails++;
        printf("FAIL [code] JS_MarkNativeCode marked %d codes (lowerIface1 %d, upperOp1 %d)\n", n_code, r_low, r_up);
    } else printf("ok   [code] JS_MarkNativeCode marked %d codes\n", n_code);
    ck("code", "a function compiled before the call reads native, and an EXISTING closure keeps its shape",
       "laterHost.toString() === 'function laterHost() { [native code] }' && 'prototype' in laterHost");
    ck("code", "(the page functions compiled before it are marked too -- which is why it runs before page code)",
       "pageFn.toString() === 'function pageFn() { [native code] }'");
    ck("code", "a function compiled after it keeps its source",
       "afterPage.toString() === 'function afterPage() { return 2; }'");
    ck("code", "a page function compiled after it keeps its prototype and constructs, whatever its name",
       "new afterPageLower().p === 1 && typeof afterPageLower.prototype === 'object'");
    ck("eval", "code the Function constructor compiled BEFORE the mark is not host code, nor is its nested code",
       "attrHandler.toString().indexOf('innerFromAttr') > 0 && attrHandler() === 6 &&"
       "innerFromAttr.toString() === 'function innerFromAttr() { return 5; }' && typeof innerFromAttr.prototype === 'object'");
    ck("eval", "...nor is code a direct eval compiled",
       "evald.toString() === 'function evald() { return 7; }' && evald() === 7");
    printf("== closures made after the mark ==\n");
    ck("code", "an anonymous host method made after the mark: native, no prototype, still works",
       "(function(){ var f = opFactory().item; return f.toString() === 'function item() { [native code] }' &&"
       "  !('prototype' in f) && f(4) === 4 && Object.getOwnPropertyNames(f).sort().join() === 'length,name'; })()");
    ck("construct", "...and not a constructor",
       "(function(){ var f = opFactory().item; try { new f(1); return false; } catch (e) { return e instanceof TypeError; } })()");
    ck("construct", "a lowercase host function declaration made after the mark is not a constructor",
       "(function(){ var h = mkHelper(); if ('prototype' in h || h() !== 2) return false;"
       "  try { new h(); return false; } catch (e) { return e instanceof TypeError; } })()");
    ck("code", "a closure NAMED with a capital at creation stays a constructor: object-literal key",
       "(function(){ var N = opFactory().Named; return typeof N.prototype === 'object' && new N().n === 1; })()");
    ck("code", "...`var C = function () {}` (SetFunctionName after the closure exists)",
       "(function(){ var C = mkVarCtor(); var c = new C(); return c.c === 1 && c.hi() === 'hi' && C.name === 'C'; })()");
    ck("code", "...and a function declaration with a capital name",
       "(function(){ var D = mkDeclCtor(); return new D().d === 1 && D.prototype.constructor === D; })()");
    ck("code", "...and a COMPUTED key that turns out capitalised (SetFunctionName at run time)",
       "(function(){ var K = mkComputed('Komp'); return K.name === 'Komp' && new K().q === 1; })()");
    ck("construct", "a computed lowercase key names an operation",
       "(function(){ var k = mkComputed('komp'); if (k.name !== 'komp' || 'prototype' in k) return false;"
       "  try { new k(); return false; } catch (e) { return e instanceof TypeError; } })()");
    ck("code", "an async closure made after the mark: Function.prototype parent, still a promise",
       "(function(){ var a = mkAsync(); return Object.getPrototypeOf(a) === Function.prototype && a() instanceof Promise &&"
       "  a.toString() === 'function () { [native code] }'; })()");
    ck("code", "a generator closure made after the mark: no prototype, Function.prototype parent, still iterates",
       "(function(){ var g = mkGen(); return !('prototype' in g) && Object.getPrototypeOf(g) === Function.prototype &&"
       "  Array.from(g()).join() === '4'; })()");
    ck("code", "JS_MarkNativeFunction's CONSTRUCTOR verdict on one closure keeps the code's later closures constructors",
       "(function(){ var L = mkLowerIface(); return typeof L.prototype === 'object' && new L().x === 1; })()");
    ck("code", "...and its operation verdict makes them operations, capital name or not",
       "(function(){ var U = mkUpperOp(); return !('prototype' in U) && U() === 'u' && !('prototype' in upperOp1); })()");
    /* Pinned, because it is the convention's price and should not change
       silently: a constructor made after the mark by plain ASSIGNMENT has no
       name to go on, and is born an operation. Nothing in the tree does this
       after install (see tests/native_shape.mk). */
    ck("code", "(the convention's price: `o.Foo = function () {}` made after the mark is born an operation)",
       "!('prototype' in mkAssigned({}))");

    /* ---- JS_GetOwnPropertyQuiet runs nothing ---- */
    printf("== quiet reads ==\n");
    {
        JSValue o = JS_NewObject(ctx);
        JSValue getter = JS_NewCFunction2(ctx, counting_getter, "get g", 0, JS_CFUNC_getter, 0);
        JSAtom ag = JS_NewAtom(ctx, "g");
        JS_DefineProperty(ctx, o, ag, JS_UNDEFINED, getter, JS_UNDEFINED,
                          JS_PROP_HAS_GET | JS_PROP_CONFIGURABLE);
        JSPropertyDescriptor d;
        int k = JS_GetOwnPropertyQuiet(ctx, &d, o, ag);
        checks++;
        if (k == JS_QUIET_ACCESSOR && get_calls == 0 && JS_IsFunction(ctx, d.getter)) printf("ok   [quiet] an accessor is described, its getter not called\n");
        else { fails++; printf("FAIL [quiet] accessor: kind %d, calls %d\n", k, get_calls); }
        JS_FreeValue(ctx, d.getter); JS_FreeValue(ctx, d.setter); JS_FreeValue(ctx, d.value);
        JS_FreeValue(ctx, getter);
        JS_FreeAtom(ctx, ag);
        JS_FreeValue(ctx, o);

        /* Array.prototype.push is created lazily from QuickJS's function list:
           reading it quietly must say so and leave it lazy. */
        JSValue ap = JS_Eval(ctx, "Array.prototype", 15, "<q>", JS_EVAL_TYPE_GLOBAL);
        JSAtom apush = JS_NewAtom(ctx, "flatMap");
        int k1 = JS_GetOwnPropertyQuiet(ctx, &d, ap, apush);
        int k2 = JS_GetOwnPropertyQuiet(ctx, &d, ap, apush);
        checks++;
        if (k1 == JS_QUIET_LAZY && k2 == JS_QUIET_LAZY) printf("ok   [quiet] a lazy built-in is reported lazy and stays lazy\n");
        else { fails++; printf("FAIL [quiet] lazy: %d then %d\n", k1, k2); }
        JSValue fm = JS_GetProperty(ctx, ap, apush);   /* instantiate it */
        JS_FreeValue(ctx, fm);
        int k3 = JS_GetOwnPropertyQuiet(ctx, &d, ap, apush);
        checks++;
        if (k3 == JS_QUIET_DATA && JS_IsFunction(ctx, d.value)) printf("ok   [quiet] ...and is data once something instantiated it\n");
        else { fails++; printf("FAIL [quiet] after instantiation: %d\n", k3); }
        JS_FreeValue(ctx, d.value);
        JS_FreeAtom(ctx, apush);
        JS_FreeValue(ctx, ap);

        JSValue px = JS_Eval(ctx, "var trapped = 0; new Proxy({a:1}, { getOwnPropertyDescriptor: function () { trapped++; } })",
                             strlen("var trapped = 0; new Proxy({a:1}, { getOwnPropertyDescriptor: function () { trapped++; } })"),
                             "<q>", JS_EVAL_TYPE_GLOBAL);
        JSAtom aa = JS_NewAtom(ctx, "a");
        int k4 = JS_GetOwnPropertyQuiet(ctx, &d, px, aa);
        checks++;
        if (k4 == JS_QUIET_EXOTIC && JS_IsProxy(px)) printf("ok   [quiet] a Proxy is not inspected\n");
        else { fails++; printf("FAIL [quiet] proxy: %d\n", k4); }
        JS_FreeAtom(ctx, aa);
        JS_FreeValue(ctx, px);
        ck("quiet", "...and its trap never ran", "trapped === 0");
    }

    /* return codes */
    checks++;
    if (r_qs == 1 && r_anon == 1 && r_fetch == 1 && r_gen == 1 && r_early == 1 &&
        r_iface == 0 && r_klass == 0 && r_class == 0 && r_math == 0)
        printf("ok   [shape] return codes: stripped 1, constructor/class/C 0\n");
    else {
        fails++;
        printf("FAIL [shape] return codes qs=%d anon=%d fetch=%d gen=%d early=%d iface=%d klass=%d class=%d parseInt=%d\n",
               r_qs, r_anon, r_fetch, r_gen, r_early, r_iface, r_klass, r_class, r_math);
    }

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    printf("native-fn: %d checks, %d failed (%d of them [construct])\n", checks, fails, construct_fails);
    return fails ? 1 : 0;
}
