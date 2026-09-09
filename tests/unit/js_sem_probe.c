/* js_sem_probe -- eval a .js file and print what the engine says.
 *
 * ONE source file, TWO targets, the tests/unit/js_bench.c precedent: the host
 * build is the fast iteration loop and the guest build (/bin/jssem, linked
 * from the same $(ENGINE_OBJ) object files browser.elf links) is the one that
 * counts. A probe that is a different program on the two sides cannot be
 * compared across them, and the whole point of this harness is that the host
 * QuickJS in this tree is NOT the browser's QuickJS -- different -D flags,
 * different libc, different arena.
 *
 * It installs exactly one global, `print`, and nothing else. In particular it
 * does NOT install queueMicrotask or structuredClone: those come from
 * c/apps/browser/js_platform.c, so a row about them here is a fact about the
 * bare engine and has to be confirmed in the browser before it is a fact about
 * the browser.
 *
 * The BEGIN/END markers exist so the guest run can be sliced out of a serial
 * log that also carries boot chatter.
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
        if (!s)
            return JS_EXCEPTION;
        fputs(s, stdout);
        if (i + 1 < argc)
            fputc(' ', stdout);
        JS_FreeCString(ctx, s);
    }
    fputc('\n', stdout);
    return JS_UNDEFINED;
}

static void drain(JSRuntime *rt)
{
    for (;;) {
        JSContext *c1;
        int r = JS_ExecutePendingJob(rt, &c1);
        if (r == 0)
            break;
        if (r < 0) {
            JSValue e = JS_GetException(c1);
            const char *s = JS_ToCString(c1, e);
            printf("JOB EXCEPTION: %s\n", s ? s : "?");
            JS_FreeCString(c1, s);
            JS_FreeValue(c1, e);
        }
    }
}

static int run_one(const char *path)
{
    FILE *f;
    long n;
    char *b;
    JSRuntime *rt;
    JSContext *ctx;
    JSValue g, v;

    f = fopen(path, "rb");
    if (!f) {
        printf("=== BEGIN %s\nHARNESS: cannot open %s\n=== END %s\n", path, path, path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)n + 1);
    if (!b) { fclose(f); printf("HARNESS: oom\n"); return 1; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); printf("HARNESS: short read\n"); return 1; }
    b[n] = 0;
    fclose(f);

    rt = JS_NewRuntime();
    ctx = JS_NewContext(rt);
    g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "print", JS_NewCFunction(ctx, js_print, "print", 1));
    JS_FreeValue(ctx, g);

    printf("=== BEGIN %s\n", path);
    fflush(stdout);
    v = JS_Eval(ctx, b, (size_t)n, path, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        JSValue e = JS_GetException(ctx);
        const char *s = JS_ToCString(ctx, e);
        printf("EXCEPTION: %s\n", s ? s : "?");
        JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, v);
    drain(rt);
    printf("=== END %s\n", path);
    fflush(stdout);

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    free(b);
    return 0;
}

int main(int argc, char **argv)
{
    int i;
    if (argc < 2) {
        printf("usage: jssem <file.js>...\n");
        return 2;
    }
    for (i = 1; i < argc; i++)
        run_one(argv[i]);
    printf("JSSEM-DONE\n");
    fflush(stdout);
    return 0;
}
