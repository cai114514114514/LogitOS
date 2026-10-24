/* SPDX-License-Identifier: MIT */
/* Host verification of the shipping clone and Blob/File implementations.
 * stream_net.h supplies the existing WebAPI transport and lifecycle apparatus;
 * this fixture never opens a socket. No clone, byte store, or brand is faked.
 *
 * The sole bootstrap substitute is WorkerSelf's DOMException constructor shape,
 * copied here because linking platform.c would drag in unrelated page/DOM/CSS
 * services. It supplies error construction and instanceof only. Consequently
 * this is evidence of clone/Fetch-realm semantics, not page bootstrap or guest
 * integration; the unified guest fixture is responsible for those boundaries. */
#include "stream_net.h"
#include "js/runtime/clone.h"
#include "js/net/url.h"

#define CLONE_CHECKS 58
#define CLONE_JOBS 10000

static const char ERROR_BOOTSTRAP[] =
"(function () {\n"
"  function DOMException(message, name) {\n"
"    var e = Error.call(this, message);\n"
"    this.message = message === undefined ? '' : String(message);\n"
"    this.name = name === undefined ? 'Error' : String(name);\n"
"    this.stack = e.stack;\n"
"  }\n"
"  DOMException.prototype = Object.create(Error.prototype);\n"
"  DOMException.prototype.constructor = DOMException;\n"
"  globalThis.DOMException = DOMException;\n"
"})();";

static void show_exception(JSValueConst error)
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

static int eval_bootstrap(const char *source, const char *file)
{
    JSValue value = JS_Eval(ctx, source, strlen(source), file, JS_EVAL_TYPE_GLOBAL);
    int success = !JS_IsException(value);
    JS_FreeValue(ctx, value);
    if (!success) {
        JSValue error = JS_GetException(ctx);
        show_exception(error);
        JS_FreeValue(ctx, error);
    }
    return success;
}

static JSValue fixture_check(JSContext *context, JSValueConst self, int argc,
                             JSValueConst *argv)
{
    (void)self;
    if (argc != 2) return JS_ThrowTypeError(context, "fixture check needs condition and label");
    const char *label = JS_ToCString(context, argv[1]);
    if (!label) return JS_EXCEPTION;
    ck(JS_ToBool(context, argv[0]), label);
    JS_FreeCString(context, label);
    return JS_UNDEFINED;
}

static char *read_fixture(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) { fprintf(stderr, "HARNESS: cannot open %s\n", path); return NULL; }
    if (fseek(file, 0, SEEK_END) || ftell(file) < 0) {
        fprintf(stderr, "HARNESS: cannot size %s\n", path);
        fclose(file);
        return NULL;
    }
    long length = ftell(file);
    if (fseek(file, 0, SEEK_SET)) { fclose(file); return NULL; }
    char *source = malloc((size_t)length + 1);
    if (!source || fread(source, 1, (size_t)length, file) != (size_t)length) {
        fprintf(stderr, "HARNESS: cannot read %s\n", path);
        free(source);
        fclose(file);
        return NULL;
    }
    source[length] = 0;
    fclose(file);
    return source;
}

static int run_fixture(const char *path)
{
    char *source = read_fixture(path);
    if (!source) return 0;
    int loaded = eval_bootstrap(source, path);
    free(source);
    if (!loaded) return 0;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue test = JS_GetPropertyStr(ctx, global, "testHTMLClone");
    JS_FreeValue(ctx, global);
    if (!JS_IsFunction(ctx, test)) {
        fprintf(stderr, "HARNESS: fixture did not define testHTMLClone\n");
        JS_FreeValue(ctx, test);
        return 0;
    }
    JSValue check = JS_NewCFunction(ctx, fixture_check, "check", 2);
    JSValue promise = JS_Call(ctx, test, JS_UNDEFINED, 1, (JSValueConst *)&check);
    JS_FreeValue(ctx, check);
    JS_FreeValue(ctx, test);
    if (JS_IsException(promise)) {
        JSValue error = JS_GetException(ctx);
        show_exception(error);
        JS_FreeValue(ctx, error);
        return 0;
    }
    int settled = 0;
    for (int jobs = 0; jobs < CLONE_JOBS; jobs++) {
        JSPromiseStateEnum state = JS_PromiseState(ctx, promise);
        if (state == JS_PROMISE_FULFILLED) { settled = 1; break; }
        if (state == JS_PROMISE_REJECTED) {
            JSValue error = JS_PromiseResult(ctx, promise);
            show_exception(error);
            JS_FreeValue(ctx, error);
            break;
        }
        JSContext *job_context = NULL;
        int ran = JS_ExecutePendingJob(rt, &job_context);
        if (ran < 0) {
            JSValue error = JS_GetException(job_context ? job_context : ctx);
            show_exception(error);
            JS_FreeValue(ctx, error);
            break;
        }
        if (!ran) { fprintf(stderr, "HARNESS: fixture promise has no runnable job\n"); break; }
    }
    JS_FreeValue(ctx, promise);
    if (!settled) return 0;
    if (checks != CLONE_CHECKS) {
        fprintf(stderr, "HARNESS: expected %d public checks, observed %d\n", CLONE_CHECKS, checks);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "tests/fixtures/html-priority/clone.js";
    open_ctx("https://clone.example/page");
    int ready = eval_bootstrap(ERROR_BOOTSTRAP, "<WorkerSelf DOMException apparatus>");
    if (ready) ready = js_url_install_core(ctx) == 0;
    JSValue hosts = ready ? js_webapi_clone_hosts(ctx) : JS_UNDEFINED;
    if (ready && JS_IsArray(ctx, hosts) != 1) {
        fprintf(stderr, "HARNESS: shipping WebAPI did not return private clone hosts\n");
        ready = 0;
    }
    if (ready) ready = js_clone_install_host(ctx, hosts);
    JS_FreeValue(ctx, hosts);
    if (!ready) {
        fprintf(stderr, "HARNESS: clone bootstrap failed\n");
        JSValue error = JS_GetException(ctx);
        show_exception(error);
        JS_FreeValue(ctx, error);
    }
    int complete = ready && run_fixture(path);
    close_ctx();
    printf("html-clone: %d checks, %d failures\n", checks, failures);
    return complete ? (failures ? 1 : 0) : 2;
}
