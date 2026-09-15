/* Real browser_load, parser, module loader, QuickJS and DOM. Only the window
 * and transport are the existing loader fixtures. In particular res_fetch
 * serves actual module source: successful normalization without evaluation
 * would miss the exact integration failure this component must prevent. */
#define main loader_original_main
#define res_fetch loader_unused_image_fetch
#include "loader_test.c"
#undef main
#undef res_fetch
#include "js_module.h"
#include "bfetch.h"
int res_fetch(const char *u, uint8_t **b, int *n) { return bfetch_sync(u, b, n); }
static int number(const char *s)
{
    JSContext *ctx = js_page_ctx(); int32_t n = -999;
    JSValue v = JS_Eval(ctx, s, strlen(s), "https://map.test/check.js", JS_EVAL_TYPE_GLOBAL);
    if (!JS_IsException(v)) JS_ToInt32(ctx, &n, v);
    else { JSValue e = JS_GetException(ctx); JS_FreeValue(ctx, e); }
    JS_FreeValue(ctx, v); return n;
}
static int map(const char *s) { return js_module_importmap(s, strlen(s), "https://map.test/index.html"); }
static int module(const char *s, const char *name) { return js_module_eval(s, strlen(s), name); }
int main(void)
{
    fake_site_reset();
    fake_site_add("https://map.test/index.html",
        "<!doctype html><body><p id='result'>pending</p>"
        "<script type='importmap'>{\"imports\":{\"pkg\":\"./lib.js\",\"parts/\":\"./parts/\"},"
        "\"scopes\":{\"./scoped/\":{\"pkg\":\"./scoped.js\"},\"./index.html\":{\"inline-only\":\"./late.js\"}}}</script>"
        "<script type='module'>import {v} from 'pkg'; import {w} from 'parts/leaf.js';"
        "import {z} from 'inline-only'; globalThis.exact=z;globalThis.inlineMeta=import.meta.url;"
        "globalThis.total=v+w; document.getElementById('result').textContent='map mounted';</script></body>");
    fake_site_add("https://map.test/lib.js", "export const v=20;");
    fake_site_add("https://map.test/parts/leaf.js", "export const w=22;");
    fake_site_add("https://map.test/scoped.js", "export const v=99;");
    fake_site_add("https://map.test/late.js", "export const z=7;");
    fake_site_add("https://map.test/other.js", "export const v=500;");
    browser_load("https://map.test/index.html");
    CHECK(number("globalThis.total") == 42, "mapped graph executes through browser loader");
    CHECK(number("document.getElementById('result').textContent==='map mounted'") == 1, "mapped module mounts DOM");
    CHECK(fake_site_fetched("/lib.js") > 0 && fake_site_fetched("/parts/leaf.js") > 0, "mapped source URLs fetched");
    CHECK(number("globalThis.exact") == 7, "exact document scope applies to inline module");
    CHECK(number("globalThis.inlineMeta==='https://map.test/index.html'") == 1, "inline import meta has document URL");
    CHECK(module("import {v} from 'pkg'; globalThis.scoped=v;", "https://map.test/scoped/main.js"), "scoped module evaluates");
    CHECK(number("globalThis.scoped") == 99, "longest scope wins");
    CHECK(map("{\"imports\":{\"pkg\":\"./other.js\",\"late\":\"./late.js\"}}"), "late map merges");
    CHECK(module("import {v} from 'pkg'; import {z} from 'late'; globalThis.merged=v+z;", "https://map.test/merge.js"), "merged map module evaluates");
    CHECK(number("globalThis.merged") == 27, "existing mapping wins while unrelated late mapping works");
    CHECK(map("{\"imports\":{\"blocked\":null,\"bad/\":\"./lib.js\"}}"), "null and invalid package entries parse");
    CHECK(!module("import 'blocked';", "https://map.test/blocked-main.js"), "null mapping rejects module");
    CHECK(!module("import 'bad/file.js';", "https://map.test/bad-main.js"), "package address without slash rejects module");
    CHECK(!module("import 'parts/../lib.js';", "https://map.test/escape-main.js"), "package traversal rejects module");
    CHECK(!map("{\"imports\":{\"rollback\":\"./late.js\"},\"scopes\":[]} "), "malformed scopes reject entire map");
    CHECK(!module("import 'rollback';", "https://map.test/rollback-main.js"), "failed map installs no partial entries");
    CHECK(!map("{\"imports\":{\"integrity-bypass\":\"./late.js\"},\"integrity\":{}}"), "unsupported integrity is explicit refusal");
    CHECK(!map("[]") && !map("null") && !map("{"), "invalid top-level JSON rejected");
    CHECK(number("globalThis.mapGetterHit=0;Object.defineProperty(Object.prototype,'imports',{configurable:true,get:function(){globalThis.mapGetterHit++;return {ghost:'./late.js'}}});1") == 1, "prototype getter fixture installed");
    CHECK(map("{}") && number("mapGetterHit") == 0, "map parser reads own fields without prototype getter");
    CHECK(!module("import 'ghost';", "https://map.test/ghost-main.js"), "inherited mapping not installed");
    CHECK(number("delete Object.prototype.imports;1") == 1, "prototype fixture removed");
    CHECK(map("{\"imports\":{\"https://map.test/alias.js\":\"./late.js\"}}"), "URL remapping installs");
    CHECK(module("import {z} from './alias.js';globalThis.alias=z;", "https://map.test/url-main.js") && number("alias") == 7, "URL-like specifier remaps");
    CHECK(module("import {z} from './late.js';globalThis.plain=z;", "https://map.test/plain-main.js"), "unmapped URL still works");
    CHECK(map("{\"imports\":{\"./late.js\":\"./other.js\"}}"), "late resolved-URL map processed");
    CHECK(module("import {z} from './late.js';globalThis.unchanged=z;", "https://map.test/after-main.js") && number("unchanged") == 7, "already resolved URL cannot be remapped");
    /* Dynamic import from a classic entry uses the same graph loader. */
    JSContext *ctx = js_page_ctx();
    const char *dynamic = "import('late').then(m=>globalThis.dynamic=m.z)";
    JSValue v = JS_Eval(ctx, dynamic, strlen(dynamic),
                        "https://map.test/dynamic.js", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) { JSValue e = JS_GetException(ctx); JS_FreeValue(ctx, e); }
    JS_FreeValue(ctx, v); js_page_pump();
    CHECK(number("dynamic") == 7, "classic dynamic import uses map");
    js_page_close(); js_module_reset();
    fake_site_add("https://map.test/next.html", "<!doctype html><p>new document</p>");
    browser_load("https://map.test/next.html");
    CHECK(!module("import 'pkg';", "https://map.test/new-main.js"), "navigation clears previous map");
    js_page_close(); js_module_reset();
    printf("importmap: %s\n", fail ? "FAIL" : "PASS"); return fail;
}
