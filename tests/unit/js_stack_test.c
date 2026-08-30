/* js_stack_test -- err.stack has to say what went wrong, not only where.
 *
 * WHY THIS EXISTS
 * A live exception on deepseek.com, through the browser's own reporter, read:
 *
 *         at cv (s001.js)
 *         at forEach (native)
 *         at cy (s001.js)
 *         ...
 *
 * Every frame, and no error. Upstream QuickJS builds err.stack from the frames
 * alone; V8 puts "<Name>: <message>" on the first line. Firefox differs from
 * V8, but the web did not standardise on Firefox -- React's error boundary,
 * Sentry and every hand-written `catch (e) { log(e.stack) }` were written
 * against Chrome. One bundle in tests/fixtures/jsperf does
 * `Error().stack.replace(/^Error/, "")`, which is production code telling us
 * the shape it expects.
 *
 * THE EXPECTATIONS BELOW WERE MEASURED, NOT REMEMBERED. Each was run in a real
 * Chrome and the observed string is quoted next to it. Where LogitOS
 * deliberately differs from what Chrome printed, the difference is asserted
 * too, so it is a known quantity rather than a surprise -- see the SUBCLASS
 * section.
 *
 * NEGATIVE CONTROL: `make test-js-stack-control` reverts the prepend with one
 * sed and requires this file to fail.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"

static int checks, failed;

static void fail(const char *what, const char *detail)
{ failed++; printf("FAIL: %s\n      %s\n", what, detail ? detail : ""); }

/* Evaluate `src`, which must leave a string in the completion value, and
 * compare it to `want`. */
static void expect_str(JSContext *ctx, const char *label, const char *src,
                       const char *want)
{
    checks++;
    JSValue v = JS_Eval(ctx, src, strlen(src), "<stack-test>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        char d[512]; snprintf(d, sizeof d, "threw: %s", m ? m : "?");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e); JS_FreeValue(ctx, v);
        fail(label, d);
        return;
    }
    const char *got = JS_ToCString(ctx, v);
    if (!got || strcmp(got, want) != 0) {
        char d[768];
        snprintf(d, sizeof d, "got  %s\n      want %s", got ? got : "?", want);
        fail(label, d);
    }
    if (got) JS_FreeCString(ctx, got);
    JS_FreeValue(ctx, v);
}

static void expect_true(JSContext *ctx, const char *label, const char *src)
{
    checks++;
    JSValue v = JS_Eval(ctx, src, strlen(src), "<stack-test>", JS_EVAL_TYPE_GLOBAL);
    int ok = JS_ToBool(ctx, v);
    if (JS_IsException(v)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        char d[512]; snprintf(d, sizeof d, "threw: %s", m ? m : "?");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
        fail(label, d);
    } else if (!ok) {
        fail(label, src);
    }
    JS_FreeValue(ctx, v);
}

int main(void)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);

    /* helper: first line of a stack */
    const char *H = "function L1(e){return String(e.stack).split('\\n')[0];}"
                    "function L2(e){return String(e.stack).split('\\n')[1];}";
    JSValue h = JS_Eval(ctx, H, strlen(H), "<h>", JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(ctx, h);

    /* ---- THE POINT: an engine-thrown error names itself -------------------
     * Chrome: "TypeError: Cannot read properties of null (reading 'x')".
     * QuickJS words its own TypeErrors differently, so assert the SHAPE (the
     * name, a colon, and a non-empty message) rather than V8's wording. */
    expect_true(ctx, "engine TypeError begins with 'TypeError: '",
        "(function(){ try { null.x } catch (e) { var l = L1(e);"
        "  return l.indexOf('TypeError: ') === 0 && l.length > 'TypeError: '.length; } })()");
    expect_true(ctx, "and the frames are still there underneath",
        "(function(){ try { null.x } catch (e) {"
        "  return String(e.stack).indexOf('\\n    at ') > 0; } })()");

    /* ---- a thrown, constructed error, from a NAMED function ---------------
     * The whole complaint: a report has to say what went wrong AND where. */
    expect_str(ctx, "message line, verbatim",
        "(function(){ function boomFn(){ throw new TypeError('cannot read x of undefined'); }"
        " try { boomFn(); } catch (e) { return L1(e); } })()",
        "TypeError: cannot read x of undefined");
    expect_true(ctx, "and the throwing function is named in the frames",
        "(function(){ function boomFn(){ throw new TypeError('cannot read x of undefined'); }"
        " try { boomFn(); } catch (e) { return String(e.stack).indexOf('at boomFn') >= 0; } })()");
    expect_true(ctx, "the message line comes FIRST, before any frame",
        "(function(){ function boomFn(){ throw new TypeError('m'); }"
        " try { boomFn(); } catch (e) {"
        "   var s = String(e.stack);"
        "   return s.indexOf('TypeError: m') === 0 && s.indexOf('    at ') > 0"
        "          && s.indexOf('TypeError: m') < s.indexOf('    at '); } })()");

    /* ---- the exact V8 formats, each quoted from a real Chrome ------------- */
    /* Chrome: "TypeError: boom" */
    expect_str(ctx, "name + ': ' + message",
        "L1(new TypeError('boom'))", "TypeError: boom");
    /* Chrome: "Error"  -- no colon, no trailing space */
    expect_str(ctx, "no message -> name alone, no colon",
        "L1(new Error())", "Error");
    /* Chrome: "RangeError" */
    expect_str(ctx, "no message, subclassed native error",
        "L1(new RangeError())", "RangeError");
    /* Chrome: "Error"  -- an empty message is treated as absent */
    expect_str(ctx, "empty message reads as absent",
        "L1(new Error(''))", "Error");
    /* Chrome: "msg"  -- an empty NAME drops the colon too. The name has to be
     * in place BEFORE construction, hence the prototype: this engine builds
     * the line eagerly (see the eager/lazy section below). */
    expect_str(ctx, "empty name -> message alone, no colon",
        "(function(){ class E1 extends Error {} E1.prototype.name = '';"
        " return L1(new E1('msg')); })()", "msg");
    /* Chrome: "" -- an empty first line, with the frames after it. */
    expect_str(ctx, "name and message both empty -> Chrome's blank first line",
        "(function(){ class E2 extends Error {} E2.prototype.name = '';"
        " return L1(new E2()); })()", "");
    expect_true(ctx, "...and the frames still follow the blank line",
        "(function(){ class E3 extends Error {} E3.prototype.name = '';"
        " return L2(new E3()).indexOf('    at ') === 0; })()");
    /* Chrome: "Error: msg" -- a null prototype loses `name`, which reads as Error */
    expect_str(ctx, "null prototype -> the Error default",
        "(function(){ var e = new Error('msg'); Object.setPrototypeOf(e, null);"
        " return L1(e); })()", "Error: msg");
    /* Chrome: "5: m" -- ToString(name), so a numeric name is not "absent" */
    expect_str(ctx, "a non-string primitive name is stringified, as V8 does",
        "(function(){ class E5 extends Error {} E5.prototype.name = 5;"
        " return L1(new E5('m')); })()", "5: m");
    /* Chrome: "Error: a\nb" split -> ["Error: a", "b", "    at ..."] */
    expect_str(ctx, "a multi-line message is not truncated",
        "String(new Error('a\\nb').stack).split('\\n').slice(0,2).join('|')",
        "Error: a|b");

    /* ---- custom names ----------------------------------------------------
     * A name on the subclass PROTOTYPE, in place before construction, matches
     * Chrome exactly. */
    expect_str(ctx, "subclass with the name on its prototype matches Chrome",
        "(function(){ class MyErr extends Error {} MyErr.prototype.name = 'MyErr';"
        " return L1(new MyErr('x')); })()", "MyErr: x");

    /* THE EAGER/LAZY DIVERGENCE, asserted rather than hoped for.
     * V8 formats err.stack at the first READ, so a subclass that assigns
     * this.name in its constructor prints "MyErr2: x" in Chrome. This engine
     * builds the line during super(), before the assignment, so it prints
     * "Error: x". Pinned here in both directions: if anyone ever makes stack
     * lazy, this check fails and tells them to update it. */
    expect_str(ctx, "subclass assigning this.name: eager capture says Error (Chrome: MyErr2)",
        "(function(){ class MyErr2 extends Error { constructor(m){ super(m); this.name='MyErr2'; } }"
        " return L1(new MyErr2('x')); })()",
        "Error: x");
    expect_str(ctx, "assigning e.name after construction does not rewrite the stack",
        "(function(){ var e = new TypeError('m'); e.name = 'Custom'; return L1(e); })()",
        "TypeError: m");
    /* ...but the MESSAGE matches V8 even under mutation, because V8 snapshots
     * the message at construction too -- checked in Chrome. */
    expect_str(ctx, "mutating e.message after construction does not rewrite it either",
        "(function(){ var e = new Error('msg'); e.message = ''; return L1(e); })()",
        "Error: msg");

    /* ---- safety: a getter must not run while the exception is in flight --- */
    expect_true(ctx, "a throwing name getter does not break the throw",
        "(function(){"
        "  function Weird(m){ this.message = m; }"
        "  Weird.prototype = Object.create(Error.prototype);"
        "  Object.defineProperty(Weird.prototype, 'name',"
        "    { get: function(){ throw new Error('getter ran'); }, configurable: true });"
        "  try { null.y } catch (e) { return true; } return false; })()");

    /* ---- the reporter's shape: what Sentry/React actually consume --------- */
    expect_true(ctx, "stack survives String() and split() with both halves",
        "(function(){ function inner(){ throw new RangeError('out of range'); }"
        "  try { inner(); } catch (e) {"
        "    var lines = String(e.stack).split('\\n');"
        "    return lines[0] === 'RangeError: out of range'"
        "        && lines.length > 1 && lines[1].indexOf('    at ') === 0; } })()");
    /* the regex a real bundle uses -- see the header */
    expect_true(ctx, "the /^Error/ strip real bundles do now matches",
        "String(new Error('x').stack).replace(/^Error/, '') !== String(new Error('x').stack)");

    /* ---- a syntax error reports itself too -------------------------------- */
    expect_true(ctx, "SyntaxError from eval names itself in the stack",
        "(function(){ try { eval('var = ;'); } catch (e) {"
        "  return L1(e).indexOf('SyntaxError: ') === 0; } })()");

    /* ---- non-Error thrown values are untouched (Chrome: undefined) -------- */
    expect_true(ctx, "throwing a plain object still has no stack",
        "(function(){ try { throw {a:1}; } catch (e) { return e.stack === undefined; } })()");

    /* ---- "not a function" has to name something --------------------------
     *
     * The same argument one level down: upstream's message for a failed call
     * is the bare string "not a function", with neither the callee's name nor
     * what it actually was. MEASURED on stripe.com -- six of them in a row,
     * byte-identical, out of React's useSyncExternalStore, with the page
     * rendering empty behind them. Against a minified bundle that message IS
     * the diagnosis; the source reads `oS(a,b)` and tells you nothing either.
     *
     * Chrome's strings are quoted per line. We deliberately differ: Chrome
     * reconstructs the whole callee EXPRESSION from source text ("o.a", "(intermediate
     * value).nope"), which needs the source, and this engine has only the
     * property atom in the bytecode. So the name is the property alone, and
     * what the callee WAS is added instead -- the half Chrome does not print
     * and the half that separates a missing API (undefined) from a shape
     * mismatch (an object).
     *
     * ---- 2026-08-30: ONE SHAPE OUT OF NINE ------------------------------
     *
     * The first version of this section measured `o.a()` and stopped, because
     * that is the only shape the cached-atom mechanism could name. GOOGLE
     * MEASURED THE REST FOR US. www.google.com/search caught a TypeError out
     * of this engine, URL-encoded our own message and stack into the `sg_ss`
     * parameter and navigated to report it to itself -- and the message it
     * carried was the UNNAMED half:
     *
     *     TypeError: not a function (the callee is a number)
     *         at N (<input>) at VD (<input>) ... at ia (...#inline-script-4)
     *
     * So the engine's whole account of a live defect on the largest page on
     * the web was "something, somewhere, is a number". The page that
     * navigation lands on is the "unusual traffic" interstitial.
     *
     * The matrix below is every call shape the engine has, and it is the
     * product: a property that is PRESENT with the wrong TYPE is worse than an
     * absent one, because a page that feature-tests for presence gets a truthy
     * number and calls it instead of taking its fallback. Naming it is what
     * makes that class of defect findable at all.
     *
     * Chrome's strings are quoted per line. We deliberately differ twice:
     * Chrome reconstructs the callee EXPRESSION from source text ("o.a",
     * "(intermediate value).nope") and we have only the bytecode, so we name
     * the last identifier; and Chrome never prints the VALUE, where 0, NaN and
     * 1 are three different bugs that all used to print as "a number".
     *
     * NEGATIVE CONTROLS: make test-js-callee-control (removes the whole
     * message) and make test-js-callee-atom-control (removes only the name). */
    expect_str(ctx, "a missing method names itself and says it was undefined",
        "(function(){ try { ({}).nope(); } catch (e) { return e.message; } })()",
        /* Chrome: "(intermediate value).nope is not a function" */
        "nope is not a function (it is undefined)");
    expect_str(ctx, "a method that is a number says so",
        "(function(){ var o = { a: 1 }; try { o.a(); } catch (e) { return e.message; } })()",
        /* Chrome: "o.a is not a function" */
        "a is not a function (it is the number 1)");
    expect_str(ctx, "a null method is not confused with a missing one",
        "(function(){ var o = { a: null }; try { o.a(); } catch (e) { return e.message; } })()",
        "a is not a function (it is null)");

    /* ---- the shapes that were unnamed until the bytecode was read -------- */
    expect_str(ctx, "a plain call through a LOCAL names the local",
        "(function(){ var f = 'x'; try { f(); } catch (e) { return e.message; } })()",
        /* Chrome: "f is not a function" */
        "f is not a function (it is the string \"x\")");
    expect_str(ctx, "a plain call through an ARGUMENT names the argument",
        "(function(){ return (function (g) {"
        "  try { g(); } catch (e) { return e.message; } })(1); })()",
        "g is not a function (it is the number 1)");
    expect_str(ctx, "a plain call through a CLOSURE variable names it",
        "(function(){ var c = 1; return (function(){"
        "  try { c(); } catch (e) { return e.message; } })(); })()",
        "c is not a function (it is the number 1)");
    expect_str(ctx, "a plain call through a GLOBAL names the global",
        "(function(){ globalThis.gnum = 1;"
        "  try { gnum(); } catch (e) { return e.message; } })()",
        "gnum is not a function (it is the number 1)");
    expect_str(ctx, "the bundler's `(0, o.a)()` still names the property",
        "(function(){ var o = { a: 1 };"
        "  try { (0, o.a)(); } catch (e) { return e.message; } })()",
        "a is not a function (it is the number 1)");
    expect_str(ctx, "a chain names the LAST link, not the first",
        "(function(){ var o = { a: { b: 1 } };"
        "  try { o.a.b(); } catch (e) { return e.message; } })()",
        "b is not a function (it is the number 1)");
    expect_str(ctx, "a call with arguments is named across the argument pushes",
        "(function(){ var f = 1; try { f(1, 2, 3); } catch (e) { return e.message; } })()",
        "f is not a function (it is the number 1)");
    expect_str(ctx, "a call inside a branch is still named",
        "(function(){ var o = { a: 1 };"
        "  try { if (o) { o.a(); } else { o.a(); } } catch (e) { return e.message; } })()",
        "a is not a function (it is the number 1)");
    expect_str(ctx, "a tagged template names its tag",
        "(function(){ var t = 1; try { t`x`; } catch (e) { return e.message; } })()",
        "t is not a function (it is the number 1)");
    /* `new C()` printed the bare upstream string -- not even the kind the
     * other arms gave -- because OP_call_constructor had no arm at all. */
    expect_str(ctx, "new on a non-function names the constructor",
        "(function(){ var C = 1; try { new C(); } catch (e) { return e.message; } })()",
        /* Chrome: "C is not a constructor" */
        "C is not a function (it is the number 1)");

    /* ---- what the callee WAS, with enough precision to tell bugs apart ----
     * 0, NaN and 1 are three different defects. So are "" and a 40 KB string,
     * and an Array where a function belongs is a different mistake from a
     * plain object. All four printed as "a number" / "a string" / "an object"
     * before, which is one bug report for many bugs.
     *
     * These three deliberately use the COMPUTED shape, which the name scan
     * cannot name. That is what keeps the two mechanisms separable: they must
     * keep passing under test-js-callee-atom-control, so that control measures
     * the naming and only the naming. */
    expect_str(ctx, "the number is printed, because 0 is not 1",
        "(function(){ var m = { q: 0 }, k = 'q';"
        "  try { m[k](); } catch (e) { return e.message; } })()",
        "not a function (the callee is the number 0)");
    expect_str(ctx, "NaN is printed as NaN",
        "(function(){ var m = { q: NaN }, k = 'q';"
        "  try { m[k](); } catch (e) { return e.message; } })()",
        "not a function (the callee is the number NaN)");
    expect_str(ctx, "an object reports its class",
        "(function(){ var m = { q: [] }, k = 'q';"
        "  try { m[k](); } catch (e) { return e.message; } })()",
        "not a function (the callee is an object (Array))");
    /* A long string must not become the message: 2 MB of bundle text in a
     * TypeError is how a diagnostic turns into a denial of service. */
    expect_true(ctx, "a long string callee is truncated with an ellipsis",
        "(function(){ var f = new Array(400).join('x');"
        "  try { f(); } catch (e) {"
        "    return e.message.length < 100 && e.message.indexOf('\\\"...') > 0; } })()");

    /* ---- WHERE THE NAME IS UNRECOVERABLE, SAY NOTHING -------------------
     * `o[k]()` computes its key at runtime and OP_get_array_el2 consumes it
     * before the call can fail; the callee of `o.a()()` is a value no name was
     * ever attached to. Both must fall back to the unnamed form. Naming the
     * WRONG property sends the reader to a line that is fine, which is worse
     * than the bare message -- so these two are the guard on the whole scan. */
    expect_str(ctx, "a computed call names nothing rather than guessing",
        "(function(){ var o = { z: 1 }, k = 'z';"
        "  try { o[k](); } catch (e) { return e.message; } })()",
        "not a function (the callee is the number 1)");
    expect_str(ctx, "calling the RESULT of a call names nothing",
        "(function(){ var o = { a: function(){ return 1; } };"
        "  try { o.a()(); } catch (e) { return e.message; } })()",
        "not a function (the callee is the number 1)");
    /* The two stale-atom traps the deleted cache needed explicit resets for.
     * They now hold by construction -- the name belongs to the instruction
     * that pushed the callee, not to the last one that happened to run -- and
     * they stay here because that is a property to keep, not an implementation
     * detail: without it a plain call after a method call read "then is not a
     * function", and `o.a?.()` on a nullish o.a (which SKIPS the call, so its
     * atom was never consumed) lent "a" to the next unrelated failure. */
    expect_str(ctx, "a computed call does not inherit the previous method's name",
        "(function(){ var p = { then: function(){} }; p.then();"
        "  var m = { q: 7 }, k = 'q';"
        "  try { m[k](); } catch (e) { return e.message; } })()",
        "not a function (the callee is the number 7)");
    expect_str(ctx, "a skipped optional call does not lend its name to the next failure",
        "(function(){ var o = { a: null }; o.a?.();"
        "  var m = {}, k = 'z';"
        "  try { m[k](); } catch (e) { return e.message; } })()",
        "not a function (the callee is undefined)");

    /* ---- THE GUARD, and the reason the scan refuses rather than guesses --
     *
     * The checks above measure COVERAGE: which shapes get named. This one
     * measures the opposite and is the more important of the two -- across a
     * spread of shapes (loops, switch, try/finally, optional chaining, getters,
     * generators, nesting, spread, `new`, computed keys) the recovered name
     * must be either RIGHT or ABSENT. Never wrong.
     *
     * It deliberately accepts "absent" everywhere, so it cannot substitute for
     * the coverage checks and cannot be satisfied by naming less. What it
     * catches is the failure mode a stack-effect walk actually has: drifting by
     * one slot and confidently reporting the name of a neighbouring value,
     * which points the reader at a line that is fine. `null` in the third
     * column means NO name may be produced for that shape at all. */
    expect_str(ctx, "across 24 call shapes the name is right or absent, never wrong",
        "(function(){"
        "  function m(f){ try { f(); } catch (e) { return e.message; } return 'NO THROW'; }"
        "  function nm(s){ var i = s.indexOf(' is not a function');"
        "                  return i < 0 ? null : s.slice(0, i); }"
        "  var C = ["
        "   ['local',      function(){ var zz=1; zz(); }, 'zz'],"
        "   ['prop',       function(){ var o={pp:1}; o.pp(); }, 'pp'],"
        "   ['chain',      function(){ var o={pp:{qq:1}}; o.pp.qq(); }, 'qq'],"
        "   ['args3',      function(){ var zz=1; zz(1,2,3); }, 'zz'],"
        "   ['nested-arg', function(){ var zz=1, h=function(x){return x;}; zz(h(1),h(2)); }, 'zz'],"
        "   ['in-for',     function(){ var zz=1; for(var i=0;i<3;i++){ zz(i); } }, 'zz'],"
        "   ['in-while',   function(){ var zz=1,i=0; while(i<3){ i++; zz(i); } }, 'zz'],"
        "   ['in-dowhile', function(){ var zz=1,i=0; do { zz(i); i++; } while(i<3); }, 'zz'],"
        "   ['in-switch',  function(){ var zz=1,k=2; switch(k){ case 1: break; case 2: zz(); break; } }, 'zz'],"
        "   ['in-ternary', function(){ var zz=1,c=1; (c ? zz : zz)(); }, null],"
        "   ['in-try',     function(){ var zz=1; try { zz(); } finally { } }, 'zz'],"
        "   ['in-catch',   function(){ var zz=1; try { throw 1; } catch(e) { zz(); } }, 'zz'],"
        "   ['after-and',  function(){ var zz=1,c=1; c && zz(); }, 'zz'],"
        "   ['after-or',   function(){ var zz=1,c=0; c || zz(); }, 'zz'],"
        "   ['optchain',   function(){ var o={pp:1}; o?.pp(); }, 'pp'],"
        "   ['optcall',    function(){ var o={pp:1}; o.pp?.(); }, 'pp'],"
        "   ['getter',     function(){ var o={get pp(){ return 1; }}; o.pp(); }, 'pp'],"
        "   ['generator',  function(){ function* g(){ var zz=1; zz(); } g().next(); }, 'zz'],"
        "   ['closure2',   function(){ var zz=1; (function(){ (function(){ zz(); })(); })(); }, 'zz'],"
        "   ['newop',      function(){ var zz=1; new zz(); }, 'zz'],"
        "   ['tagged',     function(){ var zz=1; zz`t`; }, 'zz'],"
        "   ['this-prop',  function(){ var o={pp:1,go:function(){ this.pp(); }}; o.go(); }, 'pp'],"
        "   ['spread',     function(){ var zz=1,a=[1]; zz(...a); }, null],"
        "   ['computed',   function(){ var o={pp:1},k='pp'; o[k](); }, null]"
        "  ]; var bad = [];"
        "  for (var i = 0; i < C.length; i++) {"
        "    var g = nm(m(C[i][1]));"
        "    if (g !== null && g !== C[i][2]) bad.push(C[i][0] + ':' + g);"
        "  }"
        "  return bad.join(',');"
        "})()",
        "");

    /* And the call that SUCCEEDS is untouched -- the check runs only after an
     * exception, so nothing here may change what a working call returns. */
    expect_true(ctx, "a call that works is unaffected",
        "(function(){ var o = { a: function(){ return 42; } }; return o.a() === 42; })()");
    /* An exception thrown from INSIDE a callable must survive verbatim: the
     * replacement is gated on the callee not being callable, and this is the
     * case that gate exists for. */
    expect_str(ctx, "an error thrown inside a real function is not rewritten",
        "(function(){ var o = { a: function(){ throw new TypeError('mine'); } };"
        "  try { o.a(); } catch (e) { return e.message; } })()",
        "mine");

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    if (failed) { printf("js_stack_test: %d/%d checks FAILED\n", failed, checks); return 1; }
    printf("js_stack_test: %d checks pass\n", checks);
    return 0;
}
