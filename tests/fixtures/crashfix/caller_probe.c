/* caller_probe -- assert the vendored QuickJS's Function.prototype.caller
 * behaviour against CALLER-BASELINE, line by line.
 *
 * WHY A RATCHET AND NOT AN ASSERTION OF THE CORRECT BEHAVIOUR
 * The correct behaviour (a non-strict function's .caller is the function that
 * called it, or null at the top frame) is what every browser ships and what
 * the kimi specimen needs -- and this tree's quickjs.c does not implement it:
 * js_function_proto_caller (third_party/quickjs/quickjs.c:14993) returns
 * JS_UNDEFINED for every non-strict function. The fix belongs to quickjs's
 * owner (the patch is in the crashfix wave report), so a gate asserting the
 * browser behaviour would be permanently red, and a permanently red gate is
 * noise. A gate asserting the CURRENT behaviour, against a file that says so,
 * turns silent into visible: when the patch lands this goes red until the
 * baseline is updated, and if a quickjs upgrade ever changes .caller on its
 * own this goes red in the other direction.
 *
 * Usage: caller_probe <baseline-file>
 * Prints one line per check as "name=result" and exits nonzero on any
 * mismatch with the baseline.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"

static JSValue eval_js(JSContext *ctx, const char *src)
{
    return JS_Eval(ctx, src, strlen(src), "<probe>", JS_EVAL_TYPE_GLOBAL);
}

static char *answer(JSContext *ctx, const char *expr)
{
    JSValue v = eval_js(ctx, expr);
    /* the expression under test throws on some shapes; report that too */
    if (JS_IsException(v)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        char *out = malloc(strlen(m ? m : "?") + 4);
        sprintf(out, "THROW:%s", m ? m : "?");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
        return out;
    }
    const char *m = JS_ToCString(ctx, v);
    char *out = strdup(m ? m : "?");
    if (m) JS_FreeCString(ctx, m);
    JS_FreeValue(ctx, v);
    return out;
}

/* THE PROBE PROGRAM. Three questions, each one the kimi specimen's crash
 * depends on, expressed without any site in them:
 *   1. what does a non-strict function's .caller return when it WAS called
 *      by another function (browsers: the calling function object)
 *   2. what does the second hop return at the top of the chain (browsers:
 *      null -- and the SDK's loop terminates on it)
 *   3. what does a STRICT function's .caller do (browsers and quickjs agree:
 *      TypeError -- the one part that already matches)
 */
static const char *PROBE =
    "var out = {};"
    /* 1: g is called by f; g.caller should be f */
    "function f() { function g() { return typeof g.caller; } return g(); }"
    "out.called_by_fn = f();"
    /* 2: walk to the top; a browser ends with null, never undefined-in-the-
     * middle, and the walk terminates rather than throwing */
    "out.walk = (function () {"
    "  try {"
    "    var hops = 0, fr = arguments.callee.caller;"
    "    while (fr && hops < 32) { fr = fr.caller; hops++; }"
    "    return (fr === null) ? 'null-at-top' :"
    "           (fr === undefined) ? 'undefined-at-top' : 'overran';"
    "  } catch (e) { return 'THROW:' + e.message; }"
    "})();"
    /* 3: strict functions throw on .caller in every engine */
    "out.strict = (function () {"
    "  'use strict';"
    "  try { return typeof arguments.callee.caller; }"
    "  catch (e) { return 'THREW'; }"
    "})();"
    "JSON.stringify(out)";

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: caller_probe <baseline>\n"); return 2; }
    FILE *bf = fopen(argv[1], "r");
    if (!bf) { fprintf(stderr, "cannot open baseline %s\n", argv[1]); return 2; }
    char baseline[4096] = "";
    size_t n = fread(baseline, 1, sizeof baseline - 1, bf);
    baseline[n] = 0;
    fclose(bf);

    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    char *got = answer(ctx, PROBE);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    /* the baseline is one JSON line; compare on the serialized answers */
    const char *want = NULL;
    for (char *ln = strtok(baseline, "\n"); ln; ln = strtok(NULL, "\n"))
        if (ln[0] != '#') { want = ln; break; }

    printf("caller_probe: got    %s\n", got);
    printf("caller_probe: want   %s\n", want ? want : "(no baseline line)");
    if (want && strcmp(got, want) == 0) {
        printf("caller_probe: MATCHES the baseline\n");
        return 0;
    }
    printf("caller_probe: MISMATCH -- behaviour changed; update the baseline "
           "IN THE SAME COMMIT as the change, and say why in the file\n");
    return 1;
}
