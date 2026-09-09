/* js_sem -- a byte-diff differential of JavaScript SEMANTICS against node.
 *
 * ONE source file, TWO targets, exactly the js_bench.c precedent and for the
 * same reason: the guest differs from the host in the ways that break engines
 * (mini-libc's arena allocator, -msse2, -DLOGIT_OS, -DCONFIG_STACK_CHECK,
 * -DNDEBUG killing 176 asserts), and a probe that is a different program on
 * the two sides cannot be compared across them.
 *
 * THE ORACLE IS node. This file writes NO expectations. Each case is a small
 * program that PRINTS; the same program runs under node and under this engine
 * and the two stdouts are diffed byte for byte -- the c/apps/libc gate shape,
 * whose stated reason applies here word for word: it gives every case a
 * REFERENCE instead of a hand-written expectation that only records what its
 * author already believed.
 *
 * OUTPUT CONTRACT (identical on both sides -- see tests/unit/js_sem_node.js):
 *   "## <case>"        one per file, before it runs
 *   "| <text>"         one per print() call
 *   "! <ErrorName>"    an uncaught exception, by CONSTRUCTOR NAME ONLY
 *   "## end <case>"    after the microtask queue has drained
 *
 * The exception line carries the name and not the message on purpose: two
 * engines word "x is not a function" differently and that is not a finding,
 * whereas SyntaxError-where-node-had-none is the biggest finding available --
 * a parse failure kills a whole bundle. The full message goes to stderr, which
 * is not diffed, so it is still readable while it cannot manufacture a diff.
 *
 * Microtasks: after JS_Eval returns, the pending job queue is drained to
 * completion, which is what a browser's checkpoint does and what node does
 * between the script and process exit. ORDER is the product; the tags a
 * program prints as its callbacks fire ARE the answer, so no clock is
 * involved and nothing can flake.
 *
 *   js_sem <file.js>...
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"

#define SEM_MAX_FILES 64

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *b = malloc((size_t)n + 1);
    if (!b) { fclose(f); return NULL; }
    size_t got = fread(b, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(b); return NULL; }
    b[n] = 0; *len = got;
    return b;
}

static const char *basename_of(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* print(...args) -- args joined by one space, one line, "| " prefix.
 * String() semantics via JS_ToCString, which is what node's String() gives for
 * everything a case is allowed to print. */
static JSValue js_print(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    (void)this_val;
    fputs("| ", stdout);
    for (int i = 0; i < argc; i++) {
        const char *s = JS_ToCString(ctx, argv[i]);
        if (!s) return JS_EXCEPTION;
        if (i) fputc(' ', stdout);
        fputs(s, stdout);
        JS_FreeCString(ctx, s);
    }
    fputc('\n', stdout);
    fflush(stdout);
    return JS_UNDEFINED;
}

/* An uncaught exception prints its CONSTRUCTOR NAME to stdout and everything
 * else to stderr. See the header: the message is engine wording, the name is
 * the finding. */
static void report_exception(JSContext *ctx)
{
    JSValue ex = JS_GetException(ctx);
    const char *name = NULL;
    JSValue nv = JS_UNDEFINED;

    if (JS_IsObject(ex)) {
        JSValue ctor = JS_GetPropertyStr(ctx, ex, "constructor");
        if (JS_IsObject(ctor)) {
            nv = JS_GetPropertyStr(ctx, ctor, "name");
            name = JS_ToCString(ctx, nv);
        }
        JS_FreeValue(ctx, ctor);
    }
    printf("! %s\n", name ? name : "(non-object throw)");
    fflush(stdout);

    const char *msg = JS_ToCString(ctx, ex);
    fprintf(stderr, "  [exception] %s\n", msg ? msg : "?");
    if (msg) JS_FreeCString(ctx, msg);
    if (JS_IsObject(ex)) {
        JSValue st = JS_GetPropertyStr(ctx, ex, "stack");
        if (!JS_IsUndefined(st)) {
            const char *s = JS_ToCString(ctx, st);
            if (s) { fprintf(stderr, "%s", s); JS_FreeCString(ctx, s); }
        }
        JS_FreeValue(ctx, st);
    }
    if (name) JS_FreeCString(ctx, name);
    JS_FreeValue(ctx, nv);
    JS_FreeValue(ctx, ex);
}

/* Drain to completion. A job that throws reports and the drain continues --
 * node prints an unhandled rejection and keeps going too, and stopping here
 * would hide every tag after the first failure. */
static void drain_jobs(JSRuntime *rt)
{
    JSContext *jctx;
    for (;;) {
        int r = JS_ExecutePendingJob(rt, &jctx);
        if (r == 0) break;
        if (r < 0) report_exception(jctx);
    }
}

int main(int argc, char **argv)
{
    const char *files[SEM_MAX_FILES];
    int nfiles = 0;

    for (int i = 1; i < argc; i++)
        if (nfiles < SEM_MAX_FILES) files[nfiles++] = argv[i];
    if (!nfiles) { printf("usage: js_sem <file.js>...\n"); return 2; }

    for (int i = 0; i < nfiles; i++) {
        const char *base = basename_of(files[i]);
        size_t len = 0;
        char *src = slurp(files[i], &len);

        printf("## %s\n", base);
        fflush(stdout);
        if (!src) { printf("! MISSING\n"); fflush(stdout); continue; }

        /* A fresh runtime per case: one case's atoms, shapes or leftover jobs
         * must not be able to move the next case's printed order. */
        JSRuntime *rt = JS_NewRuntime();
        JSContext *ctx = JS_NewContext(rt);

        JSValue g = JS_GetGlobalObject(ctx);
        JS_SetPropertyStr(ctx, g, "print",
                          JS_NewCFunction(ctx, js_print, "print", 1));
        JS_FreeValue(ctx, g);

        JSValue v = JS_Eval(ctx, src, len, base, JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(v)) report_exception(ctx);
        JS_FreeValue(ctx, v);

        drain_jobs(rt);

        printf("## end %s\n", base);
        fflush(stdout);

        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        free(src);
    }

    printf("JSSEM-DONE %d\n", nfiles);
    fflush(stdout);
    return 0;
}
