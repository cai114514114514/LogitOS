/* Diagnostic-only Promise observation. QuickJS calls rejection trackers when
 * a promise first rejects and when a handler is attached later; the former is
 * PROVISIONAL, not proof of an unhandled error. No handler is added here.
 * Capture pristine native prototypes per worker, before any worker script,
 * and inspect only native Error own data properties. Looking up reason.name,
 * stack, message or a Proxy descriptor would execute user code in a hook that
 * must be observational. Missing-global labels mean an exact standard message
 * pattern, not a claim that arbitrary user-created ReferenceErrors are true.
 */
static const char *const wpd_errors[] = {
    "Error", "EvalError", "RangeError", "ReferenceError", "SyntaxError",
    "TypeError", "URIError", "InternalError", "AggregateError"
};
#define WPD_ERRORS ((int)(sizeof wpd_errors / sizeof wpd_errors[0]))
struct worker_promise_diag {
    int id;
    JSValue protos[WPD_ERRORS];
    JSAtom message, length;
};
static const char *wpd_type(JSContext *ctx, JSValueConst v,
                            struct worker_promise_diag *d)
{
    if (JS_IsError(ctx, v)) {
        JSValue p = JS_GetPrototype(ctx, v); /* native Error, never Proxy */
        const char *type = "Error";
        for (int i = 0; i < WPD_ERRORS; ++i)
            if (JS_IsObject(p) && JS_IsObject(d->protos[i]) &&
                JS_VALUE_GET_PTR(p) == JS_VALUE_GET_PTR(d->protos[i])) {
                type = wpd_errors[i]; break;
            }
        JS_FreeValue(ctx, p);
        return type;
    }
    if (JS_IsObject(v)) return "object";
    if (JS_IsString(v)) return "string";
    if (JS_IsNumber(v)) return "number";
    if (JS_IsBool(v)) return "boolean";
    if (JS_IsNull(v)) return "null";
    if (JS_IsUndefined(v)) return "undefined";
    return "other";
}
static const char *wpd_missing(JSContext *ctx, JSValueConst reason,
                               struct worker_promise_diag *d)
{
    static const char *const names[] = {
        "fetch", "WebAssembly", "URL", "URLSearchParams", "TextEncoder",
        "TextDecoder", "crypto", "atob", "btoa", "Response", "Request", "Headers"
    };
    const char *result = "none";
    JSPropertyDescriptor p;
    int rc = JS_GetOwnProperty(ctx, &p, reason, d->message);
    if (rc <= 0) return result;
    if (!(p.flags & JS_PROP_GETSET) && JS_IsString(p.value)) {
        /* Primitive-string length is intrinsic in QuickJS, with no prototype
         * lookup. Bound before UTF-8 conversion: even a deliberately huge
         * synthetic error cannot allocate an unbounded diagnostic copy. */
        JSValue length = JS_GetProperty(ctx, p.value, d->length);
        int n = -1;
        if (JS_IsNumber(length)) JS_ToInt32(ctx, &n, length);
        JS_FreeValue(ctx, length);
        if (n >= 0 && n < 96) {
            size_t count = 0;
            const char *s = JS_ToCStringLen(ctx, &count, p.value);
            if (s) {
                for (unsigned i = 0; i < sizeof names / sizeof names[0]; ++i) {
                    char pattern[96];
                    int size = snprintf(pattern, sizeof pattern, "'%s' is not defined", names[i]);
                    if (size >= 0 && count == (size_t)size && !memcmp(s, pattern, count)) {
                        result = names[i]; break;
                    }
                }
                JS_FreeCString(ctx, s);
            }
        }
    }
    JS_FreeValue(ctx, p.value);
    JS_FreeValue(ctx, p.getter);
    JS_FreeValue(ctx, p.setter);
    return result;
}
static void worker_promise_rejection(JSContext *ctx, JSValueConst promise,
                                      JSValueConst reason, JS_BOOL handled,
                                      void *opaque)
{
    (void)promise;
    struct worker_promise_diag *d = opaque;
    if (worker_diag_lines >= 256) return;
    /* Diagnostic allocation failure must not replace a pending exception in
     * the promise machinery. GetException transfers ownership without JS. */
    JSValue saved = JS_GetException(ctx);
    const char *type = wpd_type(ctx, reason, d);
    const char *missing = !strcmp(type, "ReferenceError") ? wpd_missing(ctx, reason, d) : "none";
    worker_diag_lines++;
    printf("[runtime-diag] worker id=%d phase=%s kind=%s missing-pattern=%s\n",
           d->id, handled ? "promise-handled" : "promise-rejected-provisional", type, missing);
    JS_FreeValue(ctx, JS_GetException(ctx));
    (void)JS_Throw(ctx, saved);
}
static void worker_promise_diag_install(JSContext *ctx,
                                         struct worker_promise_diag *d, int id)
{
    d->id = id;
    JSValue saved = JS_GetException(ctx);
    JSValue g = JS_GetGlobalObject(ctx);
    for (int i = 0; i < WPD_ERRORS; ++i) {
        JSValue ctor = JS_GetPropertyStr(ctx, g, wpd_errors[i]);
        d->protos[i] = JS_IsObject(ctor) ? JS_GetPropertyStr(ctx, ctor, "prototype") : JS_UNDEFINED;
        JS_FreeValue(ctx, ctor);
    }
    JS_FreeValue(ctx, g);
    d->message = JS_NewAtom(ctx, "message");
    d->length = JS_NewAtom(ctx, "length");
    JS_FreeValue(ctx, JS_GetException(ctx));
    (void)JS_Throw(ctx, saved);
#ifndef JS_WORKER_NO_PROMISE_DIAGNOSTICS
    JS_SetHostPromiseRejectionTracker(JS_GetRuntime(ctx), worker_promise_rejection, d);
#endif
}
static void worker_promise_diag_close(JSContext *ctx, struct worker_promise_diag *d)
{
    JS_SetHostPromiseRejectionTracker(JS_GetRuntime(ctx), NULL, NULL);
    for (int i = 0; i < WPD_ERRORS; ++i) JS_FreeValue(ctx, d->protos[i]);
    JS_FreeAtom(ctx, d->message);
    JS_FreeAtom(ctx, d->length);
}
