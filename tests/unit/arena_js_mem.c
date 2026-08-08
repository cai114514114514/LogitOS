/* arena_js_mem: can this heap hold a WHOLE web application at once?
 *
 * js_bench (the QuickJS line's) compiles one bundle at a time and frees the
 * runtime between fixtures, which is the right shape for "what does this file
 * cost to compile". It cannot answer the question that decides whether
 * kimi.com can run, because that question is CUMULATIVE: a code-split app
 * loads 79 chunks into ONE runtime and holds them all.
 *
 * So this builds one JSRuntime, compiles every file given into it, and never
 * frees anything -- and reports, after each file, both:
 *   live KB   JS_ComputeMemoryUsage: what the engine believes it is holding.
 *   resid KB  malloc_hwm from c/apps/libc/src/malloc.c, which is linked in
 *             under its real names -- the arena the process actually made
 *             resident, including the allocator's own headers and whatever
 *             fragmentation the engine's allocation pattern produces. That is
 *             the number the machine pays, and it is not derivable from the
 *             engine's own accounting.
 *
 * Modules matter here. Kimi ships ES modules, and QuickJS resolves the whole
 * static import graph inside JS_Eval even with COMPILE_ONLY, so a stub loader
 * is installed for the same reason js_bench installs one: an unresolved import
 * would abort the compile, and fetching a dependency is the HTTP layer's cost.
 * Unlike js_bench, the stub is shared across the whole run, so a chunk imported
 * by several others is counted once -- which is also what the browser does.
 *
 * Usage:  arena_js_mem <file.js>...
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"

extern size_t malloc_hwm;
size_t malloc_arena_size(void);
size_t malloc_arena_limit(void);

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc(n + 1);
    if (!b) { fclose(f); return NULL; }
    size_t got = fread(b, 1, n, f);
    b[got] = 0; fclose(f);
    *len = got;
    return b;
}

static JSModuleDef *stub_module_loader(JSContext *ctx, const char *name, void *opaque)
{
    (void)opaque;
    JSValue v = JS_Eval(ctx, "", 0, name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(v)) return NULL;
    JSModuleDef *m = (JSModuleDef *)JS_VALUE_GET_PTR(v);
    JS_FreeValue(ctx, v);
    return m;
}

static const char *base(const char *p)
{ const char *s = strrchr(p, '/'); return s ? s + 1 : p; }

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: arena_js_mem <file.js>...\n"); return 2; }

    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    JS_SetModuleLoaderFunc(rt, NULL, stub_module_loader, NULL);
    /* No memory limit: the point is to find out what it needs, not to stop it. */
    JS_SetMemoryLimit(rt, (size_t)-1);
    JS_SetMaxStackSize(rt, 8u << 20);

    JSMemoryUsage mu0; JS_ComputeMemoryUsage(rt, &mu0);
    size_t hwm0 = malloc_hwm;

    printf("arena_js_mem: one runtime, every chunk held live\n");
    printf("arena reservation %zu MiB, commit bound %zu MiB\n\n",
           malloc_arena_size() >> 20, malloc_arena_limit() >> 20);
    printf("  %-16s %9s %9s %10s %10s\n", "chunk", "bytes", "cum KB", "live KB", "resid KB");
    printf("  %-16s %9s %9s %10s %10s\n", "----------------", "---------", "---------",
           "----------", "----------");

    size_t total_src = 0;
    int failed = 0, okn = 0;

    for (int i = 1; i < argc; i++) {
        size_t len = 0;
        char *src = slurp(argv[i], &len);
        if (!src) { printf("  %-16s (unreadable)\n", base(argv[i])); continue; }
        total_src += len;

        /* Compile as a module first: everything Kimi ships is one. A classic
         * script compiled as a module still parses (modules are a superset for
         * these bundles); if it does not, fall back rather than lose the file
         * from the total, and say so. */
        int flags = JS_EVAL_FLAG_COMPILE_ONLY | JS_EVAL_TYPE_MODULE;
        JSValue v = JS_Eval(ctx, src, len, base(argv[i]), flags);
        if (JS_IsException(v)) {
            JS_FreeValue(ctx, v);
            JS_GetException(ctx);
            v = JS_Eval(ctx, src, len, base(argv[i]),
                        JS_EVAL_FLAG_COMPILE_ONLY | JS_EVAL_TYPE_GLOBAL);
        }
        free(src);                              /* the SOURCE TEXT is not kept */

        if (JS_IsException(v)) {
            JSValue e = JS_GetException(ctx);
            const char *m = JS_ToCString(ctx, e);
            printf("  %-16s %9zu   FAILED: %s\n", base(argv[i]), len, m ? m : "?");
            if (m) JS_FreeCString(ctx, m);
            JS_FreeValue(ctx, e);
            JS_FreeValue(ctx, v);
            failed++;
            continue;
        }
        /* v is DELIBERATELY LEAKED: holding it is what makes this cumulative. */
        okn++;

        JSMemoryUsage mu; JS_ComputeMemoryUsage(rt, &mu);
        /* Only print every chunk for small runs; otherwise the tail is what
         * matters and the intermediate rows are noise. */
        if (argc <= 12 || i == argc - 1 || (i % 10) == 0)
            printf("  %-16s %9zu %9zu %10lld %10zu\n",
                   base(argv[i]), len, total_src / 1024,
                   (long long)((mu.memory_used_size - mu0.memory_used_size) / 1024),
                   (malloc_hwm - hwm0) / 1024);
    }

    JSMemoryUsage mu; JS_ComputeMemoryUsage(rt, &mu);
    long long live = (long long)((mu.memory_used_size - mu0.memory_used_size) / 1024);
    size_t resid = (malloc_hwm - hwm0) / 1024;

    printf("\n%d chunks compiled, %d failed\n", okn, failed);
    printf("source text        %8zu KB (%.2f MB)\n", total_src / 1024, total_src / 1048576.0);
    printf("engine live heap   %8lld KB (%.2f MB)   = %.2fx source\n",
           live, live / 1024.0, (double)live * 1024.0 / (double)total_src);
    printf("arena made resident%8zu KB (%.2f MB)   = %.2fx source\n",
           resid, resid / 1024.0, (double)resid * 1024.0 / (double)total_src);
    printf("\nresident is the number the machine pays; live is what the engine thinks\n");
    printf("it holds. The gap is allocator headers plus fragmentation.\n");
    return failed ? 1 : 0;
}
