/* SPDX-License-Identifier: MIT */
/* The fixture runs the shipping Streams/Encoding/Fetch modules with the
 * existing stream_net transport apparatus. New Response bodies use real Body
 * readers; this test neither replaces stream constructors nor runs old stream
 * regressions. Job counts detect a stalled promise, not host wall-clock speed. */
#include "stream_net.h"

#define STREAM_JOBS 200000
/* The fragment supplies one authoritative count to the runner and control.
 * Two independently edited copies would agree with stale apparatus as often
 * as with the actual fixture, so compiling outside the gate fails loudly. */
#ifndef HTML_STREAMS_CHECKS
#error "build this runner through tests/html_streams.mk"
#endif

static JSValue fixture_check(JSContext *context, JSValueConst self, int argc,
                             JSValueConst *argv)
{
    (void)self;
    if (argc != 2) return JS_ThrowTypeError(context, "check needs a condition and label");
    const char *label = JS_ToCString(context, argv[1]);
    if (!label) return JS_EXCEPTION;
    ck(JS_ToBool(context, argv[0]), label);
    JS_FreeCString(context, label);
    return JS_UNDEFINED;
}

static void report(JSValueConst error)
{
    const char *message = JS_ToCString(ctx, error);
    fprintf(stderr, "HARNESS: %s\n", message ? message : "unprintable exception");
    if (message) JS_FreeCString(ctx, message);
    JSValue stack = JS_IsObject(error) ? JS_GetPropertyStr(ctx, error, "stack") : JS_UNDEFINED;
    if (JS_IsString(stack)) {
        const char *text = JS_ToCString(ctx, stack);
        if (text) { fprintf(stderr, "%s\n", text); JS_FreeCString(ctx, text); }
    }
    JS_FreeValue(ctx, stack);
}

static char *load(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) { fprintf(stderr, "HARNESS: cannot read %s\n", path); return NULL; }
    if (fseek(file, 0, SEEK_END)) { fclose(file); return NULL; }
    long length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET)) { fclose(file); return NULL; }
    char *source = malloc((size_t)length + 1);
    if (!source || fread(source, 1, (size_t)length, file) != (size_t)length) {
        free(source); fclose(file); return NULL;
    }
    source[length] = 0; fclose(file);
    return source;
}

static int exercise(const char *path)
{
    char *source = load(path);
    if (!source) return 0;
    JSValue value = JS_Eval(ctx, source, strlen(source), path, JS_EVAL_TYPE_GLOBAL);
    free(source);
    if (JS_IsException(value)) {
        JSValue error = JS_GetException(ctx); report(error); JS_FreeValue(ctx, error); return 0;
    }
    JS_FreeValue(ctx, value);
    JSValue global = JS_GetGlobalObject(ctx), test = JS_GetPropertyStr(ctx, global, "testHTMLStreams");
    JS_FreeValue(ctx, global);
    if (!JS_IsFunction(ctx, test)) {
        fprintf(stderr, "HARNESS: testHTMLStreams is absent\n"); JS_FreeValue(ctx, test); return 0;
    }
    JSValue check = JS_NewCFunction(ctx, fixture_check, "check", 2);
    JSValue promise = JS_Call(ctx, test, JS_UNDEFINED, 1, (JSValueConst *)&check);
    JS_FreeValue(ctx, test); JS_FreeValue(ctx, check);
    if (JS_IsException(promise)) {
        JSValue error = JS_GetException(ctx); report(error); JS_FreeValue(ctx, error); return 0;
    }
    int complete = 0;
    for (int jobs = 0; jobs < STREAM_JOBS; jobs++) {
        JSPromiseStateEnum state = JS_PromiseState(ctx, promise);
        if (state == JS_PROMISE_FULFILLED) { complete = 1; break; }
        if (state == JS_PROMISE_REJECTED) {
            JSValue error = JS_PromiseResult(ctx, promise); report(error); JS_FreeValue(ctx, error); break;
        }
        JSContext *job_context = NULL;
        int ran = JS_ExecutePendingJob(rt, &job_context);
        if (ran < 0) {
            JSValue error = JS_GetException(job_context ? job_context : ctx); report(error); JS_FreeValue(ctx, error); break;
        }
        if (!ran) { fprintf(stderr, "HARNESS: Streams fixture has no runnable job\n"); break; }
    }
    JS_FreeValue(ctx, promise);
    if (!complete) return 0;
    if (checks != HTML_STREAMS_CHECKS) {
        fprintf(stderr, "HARNESS: expected %d Streams checks, observed %d\n", HTML_STREAMS_CHECKS, checks);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    open_ctx("https://streams.example/page");
    int complete = exercise(argc > 1 ? argv[1] : "tests/fixtures/html-priority/streams.js");
    close_ctx();
    printf("html-streams: %d checks, %d failures\n", checks, failures);
    return complete ? (failures ? 1 : 0) : 2;
}
