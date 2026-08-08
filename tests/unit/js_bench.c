/* js_bench -- what compiling a REAL page's JavaScript costs.
 *
 * ONE source file, TWO targets, on purpose:
 *   make bench-js      builds it against glibc and runs it on the host
 *   make bench-js-os   builds it against mini-libc into /bin/jsbench, boots
 *                      LogitOS and runs it there
 * The guest is where the answer actually is -- it differs from the host in the
 * two ways that break engines, mini-libc's arena allocator and -msse2 -- and a
 * benchmark that is a different program on the two sides cannot be compared
 * across them. So it is the same program.
 *
 * WHY THIS EXISTS
 * "QuickJS is starting not to hold up" is not a number. This makes it one: for
 * each bundle in tests/fixtures/jsperf, the median compile time over N runs
 * plus the spread. Median and spread, not a single sample: the host runs other
 * agents' QEMU concurrently, and a line today nearly reported a regression that
 * was its own concurrent load.
 *
 * WHAT IT MEASURES
 *   compile   JS_Eval with JS_EVAL_FLAG_COMPILE_ONLY -- tokenize, parse, emit
 *             bytecode, resolve variables and labels, compute stack sizes.
 *             This is the whole cost a page pays before one line of its JS has
 *             run, and on a 1.55 MB bundle it dominates everything else.
 *   live KB   JS_ComputeMemoryUsage with the compiled function still alive,
 *             minus the same number on an empty runtime: what the bundle's
 *             bytecode and atoms cost in the arena. Nothing in this tree had
 *             ever measured a page's live JS heap before.
 *
 * WHAT IT DOES NOT MEASURE
 * Running the code. These bundles touch document/window/Intl and would die on
 * their first line; what they cost to EXECUTE is a benchmark of the Web API
 * surface, not of the engine. Compile is the part that is unavoidable,
 * identical on every load, and entirely the engine's own.
 *
 * A .mjs fixture is compiled with JS_EVAL_TYPE_MODULE against a loader that
 * hands back an EMPTY module for every specifier, so the number is this file's
 * own compile cost and not its dependency tree's. The stub is required, not a
 * convenience: QuickJS resolves -- that is, LOADS -- the whole static import
 * graph inside JS_Eval even with JS_EVAL_FLAG_COMPILE_ONLY (see
 * js_parse_program's unconditional js_resolve_module, and upstream's own
 * comment there, "Could add a flag to avoid resolution if necessary"). Without
 * a loader the real 1.55 MB module fixture does not compile at all, it fails
 * with `could not load module './rolldown-runtime-....js'`.
 *
 *   js_bench [-n ITERS] <file.js|file.mjs>...
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "quickjs.h"

#define BENCH_MAX_FILES 64

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    char *b = malloc((size_t)n + 1);
    if (!b) { fclose(f); return NULL; }
    size_t got = fread(b, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(b); return NULL; }
    b[n] = 0; *len = got;
    return b;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* A .mjs fixture is an ES module; anything else is a classic script. Same
 * decision js_module.c makes from <script type>, kept here as a file extension
 * so the fixture directory is self-describing. */
static int is_module(const char *path)
{
    size_t n = strlen(path);
    return n >= 4 && strcmp(path + n - 4, ".mjs") == 0;
}

static const char *basename_of(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* Every import resolves to the same empty module. See the header: this exists
 * because COMPILE_ONLY still loads the import graph, and fetching a bundle's
 * dependencies is the HTTP layer's cost, not the compiler's. */
static JSModuleDef *stub_module_loader(JSContext *ctx, const char *name, void *opaque)
{
    (void)opaque;
    JSValue v = JS_Eval(ctx, "", 0, name,
                        JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(v)) return NULL;
    JSModuleDef *m = (JSModuleDef *)JS_VALUE_GET_PTR(v);
    JS_FreeValue(ctx, v);
    return m;
}

int main(int argc, char **argv)
{
    int iters = 7;
    const char *files[BENCH_MAX_FILES];
    int nfiles = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) { iters = atoi(argv[++i]); continue; }
        if (nfiles < BENCH_MAX_FILES) files[nfiles++] = argv[i];
    }
    if (iters < 1) iters = 1;
    if (!nfiles) { printf("usage: js_bench [-n ITERS] <file.js>...\n"); return 2; }

    printf("JSBENCH: QuickJS compile, %d fixtures x %d iterations\n", nfiles, iters);
    printf("JSBENCH %-24s %9s %9s %9s %9s %9s %8s\n",
           "fixture", "bytes", "med ms", "min ms", "max ms", "KB/s", "live KB");

    double *samples = malloc(sizeof(double) * (size_t)iters);
    double grand_median = 0;
    size_t grand_bytes = 0;
    int failures = 0;

    for (int i = 0; i < nfiles; i++) {
        size_t len = 0;
        char *src = slurp(files[i], &len);
        if (!src) { printf("JSBENCH %-24s  MISSING\n", basename_of(files[i])); failures++; continue; }

        int flags = JS_EVAL_FLAG_COMPILE_ONLY |
                    (is_module(files[i]) ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL);
        char err[192]; err[0] = 0;
        long long live_kb = -1;

        for (int k = 0; k < iters; k++) {
            /* A fresh runtime per sample. Sharing one would let an earlier
             * fixture's atoms make a later one look faster -- exactly the sort
             * of ordering artefact that makes a benchmark lie. */
            JSRuntime *rt = JS_NewRuntime();
            JSContext *ctx = JS_NewContext(rt);
            JS_SetModuleLoaderFunc(rt, NULL, stub_module_loader, NULL);
            JSMemoryUsage before; JS_ComputeMemoryUsage(rt, &before);

            double t0 = now_ms();
            JSValue fn = JS_Eval(ctx, src, len, files[i], flags);
            samples[k] = now_ms() - t0;

            if (JS_IsException(fn)) {
                if (!err[0]) {
                    JSValue ex = JS_GetException(ctx);
                    const char *m = JS_ToCString(ctx, ex);
                    snprintf(err, sizeof err, "%s", m ? m : "?");
                    if (m) JS_FreeCString(ctx, m);
                    JS_FreeValue(ctx, ex);
                }
            } else if (k == 0) {
                JSMemoryUsage after; JS_ComputeMemoryUsage(rt, &after);
                live_kb = (long long)((after.memory_used_size - before.memory_used_size) / 1024);
            }
            JS_FreeValue(ctx, fn);
            JS_FreeContext(ctx);
            JS_FreeRuntime(rt);
        }

        qsort(samples, (size_t)iters, sizeof samples[0], cmp_double);
        double med = samples[iters / 2], lo = samples[0], hi = samples[iters - 1];

        if (err[0]) {
            printf("JSBENCH %-24s %9lu  COMPILE FAILED: %s\n",
                   basename_of(files[i]), (unsigned long)len, err);
            failures++;
        } else {
            printf("JSBENCH %-24s %9lu %9.2f %9.2f %9.2f %9.0f %8lld\n",
                   basename_of(files[i]), (unsigned long)len, med, lo, hi,
                   len / 1024.0 / (med / 1000.0), live_kb);
            grand_median += med;
            grand_bytes += len;
        }
        free(src);
    }

    if (grand_bytes)
        printf("JSBENCH-TOTAL %lu bytes in %.1f ms (sum of medians) "
               "= %.2f ms per 100 KB\n",
               (unsigned long)grand_bytes, grand_median,
               grand_median / ((double)grand_bytes / 102400.0));
    if (failures)
        printf("JSBENCH-FAIL %d fixture(s) did not compile -- that is a "
               "language-coverage failure, not a slow one\n", failures);
    printf("JSBENCH-DONE %d\n", failures);
    free(samples);
    return failures ? 1 : 0;
}
