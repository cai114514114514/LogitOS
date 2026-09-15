/* SPDX-License-Identifier: MIT */
#include "stream_net.h"

static void router(struct fakesock *s, const char *method, const char *target)
{
    (void)method;
    if (!strcmp(target, "/redirect"))
        rsp_add(s, "HTTP/1.1 302 Found\r\nLocation: /payload\r\nContent-Length: 0\r\n\r\n");
    else
        rsp_add(s, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                   "X-Secret: hidden\r\nContent-Length: 14\r\n\r\nsecret payload");
}

static JSValue record(JSContext *c, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    const char *label = argc > 1 ? JS_ToCString(c, argv[1]) : NULL;
    ck(argc > 0 && JS_ToBool(c, argv[0]), label ? label : "missing assertion label");
    if (label) JS_FreeCString(c, label);
    return JS_UNDEFINED;
}

int main(void)
{
    fake_now = 1000;
    fs_reset();
    fs_set_router(router);
    open_ctx("http://page.example/index.html");
    JSValue g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "responseBoundaryRecord", JS_NewCFunction(ctx, record, "record", 2));
    JS_FreeValue(ctx, g);
    run("var responseBoundaryOrigin = 'http://other.example';");
    FILE *f = fopen("tests/fixtures/browser/response-boundary.js", "rb");
    if (!f) { fprintf(stderr, "HARNESS: missing shared response fixture\n"); return 2; }
    char source[16384];
    size_t n = fread(source, 1, sizeof source - 1, f);
    int ended = feof(f); fclose(f); source[n] = 0;
    if (!ended) { fprintf(stderr, "HARNESS: response fixture exceeds buffer\n"); return 2; }
    run(source);
    settle(400);
    ckjs("responseBoundary.done === true", "shared page reaches completion");
    ck(req_count() == 5, "real transport saw opaque redirect same origin and denied CORS requests");
    ck(fs_live() < 0, "all response sockets released");
    close_ctx();
    printf("response_boundary: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
