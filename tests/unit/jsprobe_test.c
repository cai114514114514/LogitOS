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
 * exactly that check must fail. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "quickjs.h"
#include "js/runtime/probe.h"

static char out[65536];
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
    const char *platform =
        "globalThis.FakeElement = function FakeElement(){};"
        "FakeElement.prototype.tagName = 'DIV';"
        "FakeElement.prototype.click = function click() {};"
        "globalThis.fakeNavigator = { userAgent: 'x' };";
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
    js_probe_reset(ctx);
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
    printf("jsprobe: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
