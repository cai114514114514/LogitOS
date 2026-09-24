/* The JS probe (c/apps/gui/browser/js/runtime/probe.c), on the host.
 *
 * A bare QuickJS context stands in for the page: before the probe is
 * installed, the "platform" publishes a constructor FakeElement and an object
 * fakeNavigator, exactly as the browser's install functions publish
 * HTMLElement and navigator. Then a script does what Cloudflare's does --
 * feature-tests things that are not there, some inside try/catch -- and the
 * probe's report must say, by name:
 *
 *   MISS ON A PLATFORM SINGLETON   fakeNavigator.pdfViewerEnabled get
 *   MISS ON AN INSTANCE            FakeElement.movementX get  (via the prototype walk)
 *   `in` MISS                      window.OfflineAudioContext has
 *   UNDECLARED GLOBAL (typeof)     window.visualViewportX get
 *   CAUGHT ENGINE ERROR            not an object [js_object_getPrototypeOf]
 *   toString OF A PLATFORM FN      tostring click <platform> -> SOURCE
 *   NOT REPORTED                   misses on a script's own {} and on arrays
 *                                  (their prototypes are ES intrinsics)
 *
 * -DPROBE_NEGCTL_NO_WALK looks at the receiver only, never its prototypes;
 * the instance miss then has no platform object to be attributed to, and
 * exactly that check must fail.
 *
 * EMBEDDED DOCUMENTS. A frame's document has its OWN runtime (frame/passive.c)
 * and installs the probe with the "frame:" prefix into the same table. Its
 * misses must be named as frame misses, its close must release exactly its
 * own references through its own context -- a reference left behind keeps
 * objects alive in a runtime about to be freed, and one dropped through the
 * top page's context lands in the wrong runtime -- and neither order of
 * closing may disturb the other document's labels.
 * -DPROBE_NEGCTL_OWNER_ONLY_RESET restores the old reset (only the first
 * installer releases anything, and then everything, through itself); the
 * embedded runtime then still holds objects after its context is freed, and
 * exactly that check must fail. The test stops there rather than execute the
 * old code's wrong-runtime release. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "quickjs.h"
#include "js/runtime/probe.h"

static char out[131072];
static const char *platform =
    "globalThis.FakeElement = function FakeElement(){};"
    "FakeElement.prototype.tagName = 'DIV';"
    "FakeElement.prototype.click = function click() {};"
    "globalThis.fakeNavigator = { userAgent: 'x' };";
static void run(JSContext *ctx, const char *src)
{
    JSValue r = JS_Eval(ctx, src, strlen(src), "<script>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, r);
}
/* A runtime is only safe to free once nothing references its objects. */
static int runtime_empty(JSRuntime *rt)
{
    JSMemoryUsage mu;
    JS_RunGC(rt);
    JS_ComputeMemoryUsage(rt, &mu);
    return mu.obj_count == 0;
}
static JSContext *embedded(JSRuntime **rt_out)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    run(ctx, platform);
    js_probe_install(ctx, "frame:");
    run(ctx, "globalThis.window = globalThis;");
    *rt_out = rt;
    return ctx;
}
static int checks, fails;
static void check(int ok, const char *what)
{ checks++; if (!ok) { fails++; printf("FAIL %s\n", what); } }

int main(void)
{
    /* Capture stdout (the probe prints with printf) into `out`. */
    FILE *cap = tmpfile();
    if (!cap) return 2;
    fflush(stdout);
    int saved = dup(fileno(stdout));
    dup2(fileno(cap), fileno(stdout));

    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    JSValue r = JS_Eval(ctx, platform, strlen(platform), "<platform>", JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(ctx, r);
    js_probe_install(ctx, "");
    const char *page =
        "var a = fakeNavigator.pdfViewerEnabled;"
        "var e = new FakeElement(); var b = e.movementX;"
        "var c = ('OfflineAudioContext' in window);"
        "var d = typeof visualViewportX;"
        "try { Object.getPrototypeOf(undefined); } catch (err) {}"
        "var own = {}; var f = own.nothingHere; var g = [1,2].alsoNothing;"
        "var h = fakeNavigator.userAgent;"
        "var t1 = Function.prototype.toString.call(FakeElement.prototype.click);"
        "var t2 = Function.prototype.toString.call(Math.max);";
    JSValue w = JS_Eval(ctx, "globalThis.window = globalThis;", 31, "<w>", JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(ctx, w);
    r = JS_Eval(ctx, page, strlen(page), "<page>", JS_EVAL_TYPE_GLOBAL);
    int threw = JS_IsException(r);
    JS_FreeValue(ctx, r);

    /* An embedded document closes FIRST, while the top page lives on. */
    JSRuntime *frt;
    JSContext *fctx = embedded(&frt);
    run(fctx, "var fa = fakeNavigator.frameOnlyFirst;");
    js_probe_reset(fctx);
    JS_FreeContext(fctx);
    int first_clean = runtime_empty(frt);
    check(first_clean, "an embedded document's reset releases every probe reference into its own runtime");
    if (!first_clean) {
        /* Freeing frt would assert; the owner's reset would now release the
         * embedded references through the WRONG runtime. Report and stop. */
        fflush(stdout);
        dup2(saved, fileno(stdout));
        rewind(cap);
        size_t m = fread(out, 1, sizeof out - 1, cap);
        out[m] = 0;
        fputs(out, stdout);
        printf("jsprobe: %d checks, %d failed\n", checks, fails);
        return 1;
    }
    JS_FreeRuntime(frt);
    run(ctx, "var afterFrame = fakeNavigator.afterFrameClose;");

    /* A second embedded document outlives the top page's reset. */
    JSContext *gctx = embedded(&frt);
    js_probe_reset(ctx);                 /* the owner: prints, clears counts */
    run(gctx, "var fb = fakeNavigator.frameOnlySecond;");
    js_probe_reset(gctx);
    JS_FreeContext(gctx);
    check(runtime_empty(frt), "an embedded document outliving the top page still releases through itself");
    JS_FreeRuntime(frt);

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    fflush(stdout);
    dup2(saved, fileno(stdout));
    rewind(cap);
    size_t n = fread(out, 1, sizeof out - 1, cap);
    out[n] = 0;
    fputs(out, stdout);

    check(!threw, "the page script ran to completion");
    check(strstr(out, "[probe] installed page:") != NULL, "the probe reported its installation");
    check(strstr(out, "miss fakeNavigator.pdfViewerEnabled get") != NULL,
          "a miss on a platform singleton is reported by name");
    check(strstr(out, "miss FakeElement.movementX get") != NULL,
          "a miss on an INSTANCE is attributed to its platform prototype");
    check(strstr(out, "miss window.OfflineAudioContext has") != NULL, "an `in` miss on window is reported");
    check(strstr(out, "miss window.visualViewportX get") != NULL, "a typeof of an undeclared global is reported");
    check(strstr(out, "throw TypeError: not an object [js_object_getPrototypeOf]") != NULL,
          "a CAUGHT engine error is reported with the C function that threw it");
    check(strstr(out, "nothingHere") == NULL, "a miss on the script's own {} is not reported");
    check(strstr(out, "alsoNothing") == NULL, "a miss on an array is not reported");
    check(strstr(out, "userAgent") == NULL, "a HIT is not reported");
    check(strstr(out, "tostring click <platform> -> SOURCE") != NULL,
          "a platform function stringified to its SOURCE is reported");
    check(strstr(out, "tostring max native") != NULL, "a native function stringified is reported as native");
    check(strstr(out, "[probe] n=1 miss fakeNavigator.pdfViewerEnabled get") != NULL,
          "the summary carries counts");
    check(strstr(out, "[probe] installed frame:") != NULL, "an embedded document's install is labelled");
    check(strstr(out, "miss frame:fakeNavigator.frameOnlyFirst get") != NULL,
          "a miss inside an embedded document names the frame realm");
    check(strstr(out, "miss fakeNavigator.frameOnlyFirst") == NULL,
          "an embedded document's miss is not attributed to the top page");
    check(strstr(out, "miss fakeNavigator.afterFrameClose get") != NULL,
          "the top page's labels survive an embedded document's reset");
    check(strstr(out, "miss frame:fakeNavigator.frameOnlySecond get") != NULL,
          "an embedded document's labels survive the top page's reset");
    printf("jsprobe: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
