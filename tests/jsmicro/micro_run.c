/* jssem_run.c -- ONE source file, TWO targets, on purpose.
 *
 * The differential runner for tests/jssem: evaluate a .js file, print whatever
 * the program prints, drain the microtask queue exactly the way the browser
 * does, and exit.  stdout is then diffed BYTE FOR BYTE against node running the
 * same file.  That is the c/apps/libc gate shape -- "each test source compiles
 * twice ... and the two stdouts are diffed byte for byte" -- and its reason
 * carries over word for word: it gives every case a REFERENCE instead of a
 * hand-written expectation that only records what its author already believed.
 *
 * Built twice from these bytes:
 *
 *   A. host  -- $(QJS_SRC) for darwin/arm64, but with the GUEST's defines
 *               (-DLOGIT_OS -DCONFIG_STACK_CHECK -DNDEBUG).  Fast iteration.
 *               No finding is reported from A alone.
 *   B. guest -- /bin/jssem.aex, linked from $(ENGINE_OBJ), which is the literal
 *               object set build/browser.elf links.  Same objects, same JS_CF,
 *               same mini-libc arena, same -msse2.  This is the one that counts.
 *
 * TWO THINGS THE HOST MUST SUPPLY OR IT MEASURES ITSELF
 * ----------------------------------------------------
 * 1. QuickJS never runs a queued job on its own.  js_dom.c:2736 drains after
 *    every script evaluation; so does this.  Without it `.then` never fires.
 * 2. `queueMicrotask` is NOT in the engine.  It is a JS prelude in
 *    js_platform.c:542.  A harness that does not install it reports the browser
 *    as lacking a function the browser has.  The prelude below is the same
 *    text, character for character, so any ordering difference it causes is the
 *    browser's real ordering difference and not the harness's.
 *
 * print() is the only host function.  Everything a case prints goes through it,
 * so a case is a pure function from the engine to a string.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"

static JSValue js_print(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    int i;
    (void)t;
    for (i = 0; i < argc; i++) {
        const char *s = JS_ToCString(ctx, argv[i]);
        if (!s) return JS_EXCEPTION;
        fputs(s, stdout);
        if (i + 1 < argc) fputc(' ', stdout);
        JS_FreeCString(ctx, s);
    }
    fputc('\n', stdout);
    fflush(stdout);
    return JS_UNDEFINED;
}

/* Verbatim from c/apps/browser/js_platform.c:542.  Do not "improve" it: the
 * point is that the guest's queueMicrotask is a Promise reaction, and if that
 * costs a turn a real queueMicrotask does not, this harness must show it. */
static const char *PRELUDE =
    "globalThis.queueMicrotask = function (fn) {\n"
    "  if (typeof fn !== 'function') throw new TypeError('queueMicrotask requires a function');\n"
    "  Promise.resolve().then(function () { fn(); });\n"
    "};\n";

static void report(JSContext *ctx, const char *where)
{
    JSValue e = JS_GetException(ctx);
    const char *s = JS_ToCString(ctx, e);
    printf("EXCEPTION[%s]: %s\n", where, s ? s : "?");
    if (s) JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, e);
    fflush(stdout);
}

/* The browser's pump, same cap, same swallow-and-continue on a throwing job. */
static void drain(JSRuntime *rt, JSContext *ctx)
{
    int n;
    for (n = 0; n < 100000; n++) {
        JSContext *jc = 0;
        int r = JS_ExecutePendingJob(rt, &jc);
        if (r == 0) break;
        if (r < 0) report(jc ? jc : ctx, "job");
    }
}

int main(int argc, char **argv)
{
    FILE *f;
    long n;
    char *b;
    JSRuntime *rt;
    JSContext *ctx;
    JSValue g, v;

    if (argc < 2) { fprintf(stderr, "usage: jssem <file.js>\n"); return 2; }
    f = fopen(argv[1], "rb");
    if (!f) { printf("CANNOT OPEN %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    b = (char *)malloc((size_t)n + 1);
    if (!b) { printf("OOM\n"); return 2; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { printf("SHORT READ\n"); return 2; }
    b[n] = 0;
    fclose(f);

    rt = JS_NewRuntime();
    /* QuickJS's default guard is 256 KiB; the browser sets 2 MiB
     * (js_page.c:667).  Neither number can be used here, and the reason is an
     * apparatus trap that cost a crash before it was understood:
     *
     *   - 256 KiB is not the browser's runtime.  A runner on the default
     *     measures a recursion ceiling (~246 frames) no page ever runs at.
     *   - 2 MiB IS the browser's, but it is only safe because browser.aex asks
     *     for 2048 stack pages = 8 MiB (Makefile:1034) and the GUI launch path
     *     honours the request (wm.c:1752).  THIS program is a CLI .aex, and
     *     c/kernel/exec/exec.c:30 hardcodes CLI_STACK_PAGES 256 = 1 MiB and
     *     never calls aex_stack_pages() at all.  Setting a 2 MiB guard over a
     *     1 MiB stack means the guard can never fire: the first run of
     *     m53_async_recursion in the guest did not throw, it took a page fault
     *     (`sig 11 -> /core.1 ... cr2=0x53efff54`) and the process died.
     *
     * So the guard is set to half this program's REAL stack.  The consequence
     * is stated rather than hidden: the recursion DEPTH measured here is this
     * runner's, a quarter of the browser's, and only the ratio-free half of
     * that result -- which exception constructor and message the engine throws
     * -- carries over to the browser. */
    JS_SetMaxStackSize(rt, 512 * 1024);
    ctx = JS_NewContext(rt);
    g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "print", JS_NewCFunction(ctx, js_print, "print", 1));
    JS_FreeValue(ctx, g);

    v = JS_Eval(ctx, PRELUDE, strlen(PRELUDE), "<prelude>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) report(ctx, "prelude");
    JS_FreeValue(ctx, v);
    drain(rt, ctx);

    v = JS_Eval(ctx, b, (size_t)n, argv[1], JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) report(ctx, "eval");
    JS_FreeValue(ctx, v);

    drain(rt, ctx);

    JS_FreeValue(ctx, JS_UNDEFINED);
    fflush(stdout);
    /* Deliberately NOT freeing the runtime: JS_FreeRuntime asserts on live
     * objects in a -DNDEBUG-less build and the case is already over.  The
     * process exit is the free. */
    return 0;
}
