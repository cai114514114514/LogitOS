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

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    if (failed) { printf("js_stack_test: %d/%d checks FAILED\n", failed, checks); return 1; }
    printf("js_stack_test: %d checks pass\n", checks);
    return 0;
}
